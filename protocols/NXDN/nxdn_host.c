/* Created by Rick KD0OSS */
/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <tools.h>
#include <mariadb/mysql.h>
#include <fec_tools.h>
#include <ADF7021.h>

#include "nxdn_dsp.h"
#include "nxdn_func.h"

#define VERSION "2026-10-12"
#define BUFFER_SIZE 500

#define TX_FIFO_FRAMES 64U
#define GW_FIFO_PACKETS 64U
#define GW_PACKET_MAX 64U
#define EVENT_FIFO 32U
#define SAMPLES_PER_PACKET 96U
#define TX_AHEAD_MS 200U        /* samples queued ahead in the modem host; its 12000 byte buffer holds 240 ms */
#define TX_UNDERRUN_MS 400U     /* a frame this late (plus the jitter time) after it was due ends the transmission */
#define NXDN_FRAME_MS 80U       /* one NXDN frame: 384 bits at 4800 bit/s */

/*
 * OpenMT_HS (ADF7021, BIT packets): 4800 bit/s = 600 bytes/s, 192 bytes in a
 * 200 byte BITS packet (320 ms).
 */
#define BIT_BYTES_PER_PACKET 192U
#define BIT_BYTES_PER_SECOND 600U

/* ADF7021 register values for NXDN (4FSK, 2400 baud, 12.5 kHz IF), from the MMDVM_HS ADF7021.h */
#if defined(ADF7021_14_7456)
#define NXDN_ADF_DEV            13U         /* +1 symbol deviation at 100% level */
#define NXDN_ADF_REG3           0x2A4CC113U
#define NXDN_ADF_DISC_BW        295U
#define NXDN_ADF_POST_BW        7U
#define NXDN_ADF_REG10          0x049E472AU
#else
#define NXDN_ADF_DEV            8U
#define NXDN_ADF_REG3           0x29ECA113U
#define NXDN_ADF_DISC_BW        246U
#define NXDN_ADF_POST_BW        8U
#define NXDN_ADF_REG10          0x049E556AU
#endif

static const char* MODE_NAME  = "NXDN";
static const char* MODEM_TYPE = "4FSK";

static const char* TYPE_NXDN        = "NXDN";
static const char* TYPE_DATA        = "NXDD";
static const char* TYPE_LOST        = "NXDL";
static const char* TYPE_ACK         = "ACK ";
static const char* TYPE_NACK        = "NACK";
static const char* TYPE_DISCONNECT  = "DISC";
static const char* TYPE_CONNECT     = "CONN";
static const char* TYPE_STATUS      = "STAT";
static const char* TYPE_MODE        = "MODE";
static const char* TYPE_COMMAND     = "COMM";
static const char* TYPE_SAMPLE      = "SAMP";
static const char* TYPE_SAMPLE_EOT  = "SEOT";
static const char* TYPE_BITS        = "BITS";

static const uint8_t PACKET_TYPE_BIT   = 0;
static const uint8_t PACKET_TYPE_SAMP  = 1;
static const uint8_t PACKET_TYPE_FRAME = 2;

static const uint8_t COMM_SET_DUPLEX  = 0x00;
static const uint8_t COMM_SET_SIMPLEX = 0x01;
static const uint8_t COMM_SET_MODE    = 0x02;
static const uint8_t COMM_SET_IDLE    = 0x03;
static const uint8_t COMM_UPDATE_CONF = 0x04;

static const uint8_t SETMODE[] = {0x61, 0x00, 0x05, 0x01, 0x02};
static const uint8_t SETIDLE[] = {0x61, 0x00, 0x05, 0x01, 0x03};

typedef struct
{
    uint8_t  ran;
    uint16_t id;
    bool     selfOnly;
    bool     remoteGateway;
    uint8_t  rxLevel;
    uint8_t  txLevel;
    uint16_t txDelay;       /* ms of preamble before a transmission */
    uint8_t  rfPower;       /* OpenMT_HS RF power, 0 - 255 (MMDVM_HS scale)   */
    uint8_t  bitFlags0;     /* OpenMT_HS mode flags (registration byte 82)    */
    uint8_t  bitFlags1;     /* OpenMT_HS mode flags (registration byte 83)    */
    uint32_t modeHang;      /* s the modem stays in NXDN after a call, 0 = release at once */
    uint32_t timeout;       /* s, longest RF call forwarded */
    uint32_t jitterMs;      /* ms of frames to collect before a transmission starts (0 = off) */
    uint8_t  txShape;
    uint8_t  rxShape;
    float    rxMinAmp;
} NxdnConfig;

typedef struct
{
    uint8_t data[NXDN_FRAME_LENGTH_BYTES];
    bool    eot;
} TxFrame;

typedef struct
{
    NxdnCall call;
    bool     isNet;
    time_t   when;
} HostEvent;

static NxdnConfig cfg;
static NxdnBitRx  bitRx;
static char       rxFrequency[32] = "435000000";
static char       txFrequency[32] = "435000000";

static uint8_t  packetType        = 0;
static uint8_t  modemId           = 1;
static char     modemName[10]     = "modem1";
static char     station_call[9]   = "";
static bool     debugM            = false;
static volatile bool connected    = true;
static volatile bool modem_duplex = false;
static volatile bool gwConnected  = false;
static volatile bool gwLinked     = false;
static volatile int  gwFd         = -1;
static volatile bool modeHeld     = false;
static volatile bool txActive     = false;
static char     reflName[8]       = "";

static char     modemHost[80]     = "127.0.0.1";
static int      sockfd            = -1;
static uint16_t serverPort        = 18500;
static uint16_t clientPort        = 18000;

static NxdnCtrl ctrl;
static NxdnRx   rx;
static NxdnTx   tx;

static pthread_mutex_t ctrlMutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sockMutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t txMutex    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  txCond     = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t gwMutex    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t evMutex    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t modeMutex  = PTHREAD_MUTEX_INITIALIZER;

static TxFrame  txFifo[TX_FIFO_FRAMES];
static unsigned int txHead = 0U;
static unsigned int txTail = 0U;

static uint8_t  gwOut[GW_FIFO_PACKETS][GW_PACKET_MAX];
static uint8_t  gwOutLen[GW_FIFO_PACKETS];
static unsigned int gwHead = 0U;
static unsigned int gwTail = 0U;

static HostEvent events[EVENT_FIFO];
static unsigned int evHead = 0U;
static unsigned int evTail = 0U;

static uint64_t lastActivityMs = 0U;
static bool     callActive     = false;
static time_t   callStart      = 0;
static char     callSrc[16]    = "";
static char     callSuffix[16] = "";
static char     callDst[16]    = "";

/*
 * dump()
 *
 * Debug helper. Prints text, then a hex and ASCII dump of length bytes of
 * data, 16 bytes per line.
 *
 * Parameters:
 *   text   - Label printed above the dump.
 *   data   - Bytes to dump.
 *   length - Number of bytes in data.
 *
 * Returns: nothing.
 */
static void dump(const char* text, const uint8_t* data, unsigned int length)
{
    unsigned int offset = 0U;

    fprintf(stderr, "%s: %llu (ms)\n", text, (unsigned long long)nowMs());

    while (length > 0U)
    {
        unsigned int bytes = (length > 16U) ? 16U : length;

        fprintf(stderr, "%04X:  ", offset);

        for (unsigned int i = 0U; i < bytes; i++)
            fprintf(stderr, "%02X ", data[offset + i]);

        for (unsigned int i = bytes; i < 16U; i++)
            fputs("   ", stderr);

        fputs("   *", stderr);

        for (unsigned int i = 0U; i < bytes; i++)
            fputc(isprint(data[offset + i]) ? data[offset + i] : '.', stderr);

        fputs("*\n", stderr);

        offset += 16U;
        length = (length >= 16U) ? length - 16U : 0U;
    }
}

/*
 * writeAll()
 *
 * Writes a whole buffer to a socket, retrying on partial writes. Returns
 * false if the connection has failed.
 *
 * Parameters:
 *   fd     - Socket to write to.
 *   data   - Bytes to write.
 *   length - Number of bytes in data.
 *
 * Returns: true if everything was written, false if the connection failed.
 */
static bool writeAll(int fd, const uint8_t* data, size_t length)
{
    size_t done = 0U;

    while (done < length)
    {
        ssize_t n = write(fd, data + done, length - done);

        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                delay(1000);
                continue;
            }
            return false;
        }

        done += (size_t)n;
    }

    return true;
}

/*
 * modemWrite()
 *
 * Writes one packet to the modem host. The mutex keeps packets from
 * different threads from being mixed up.
 *
 * Parameters:
 *   data   - The packet.
 *   length - Number of bytes in data.
 *
 * Returns: true if the packet was written, false if the modem host is not
 *   connected or the write failed.
 */
static bool modemWrite(const uint8_t* data, size_t length)
{
    bool ok;

    pthread_mutex_lock(&sockMutex);
    ok = sockfd >= 0 && writeAll(sockfd, data, length);
    pthread_mutex_unlock(&sockMutex);

    return ok;
}

/*
 * readFully()
 *
 * Reads exactly length bytes. Returns false on end of file or error.
 *
 * Parameters:
 *   fd     - Socket to read from.
 *   data   - Receives the bytes.
 *   length - Number of bytes to read.
 *
 * Returns: true if all the bytes were read, false on end of file or error.
 */
static bool readFully(int fd, uint8_t* data, size_t length)
{
    size_t got = 0U;

    while (got < length)
    {
        ssize_t n = read(fd, data + got, length - got);

        if (n == 0)
            return false;

        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (!connected)
                    return false;
                delay(2000);
                continue;
            }
            return false;
        }

        got += (size_t)n;
    }

    return true;
}

/*
 * cfgValue()
 *
 * Reads a configuration value, writing the default to the configuration
 * database when the key does not exist yet.
 *
 * Parameters:
 *   key - Name of the NXDN configuration key.
 *   def - Default value, written to the database when the key does not
 *         exist.
 *   out  - Receives the value.
 *   size - Size of the out buffer; a longer value is cut.
 *
 * Returns: nothing.
 */
static void cfgValue(const char* key, const char* def, char* out, size_t size)
{
    out[0] = '\0';
    readHostConfigN(modemName, "NXDN", key, out, size);

    if (strlen(out) == 0)
    {
        setHostConfig(modemName, "NXDN", key, "input", def);
        snprintf(out, size, "%s", def);
    }
}

/*
 * loadConfig()
 *
 * Fills the configuration structure from the host configuration, creating
 * any missing keys with their defaults.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void loadConfig(void)
{
    char tmp[40];

    cfgValue("RAN", "1", tmp, sizeof(tmp));
    cfg.ran = (uint8_t)(atoi(tmp) & 0x3F);

    cfgValue("id", "0", tmp, sizeof(tmp));
    cfg.id = (uint16_t)atoi(tmp);

    cfgValue("selfOnly", "0", tmp, sizeof(tmp));
    cfg.selfOnly = atoi(tmp) != 0;

    cfgValue("remoteGateway", "0", tmp, sizeof(tmp));
    cfg.remoteGateway = atoi(tmp) != 0;

    cfgValue("rxLevel", "50", tmp, sizeof(tmp));
    cfg.rxLevel = (uint8_t)atoi(tmp);

    cfgValue("txLevel", "50", tmp, sizeof(tmp));
    cfg.txLevel = (uint8_t)atoi(tmp);

    cfgValue("txDelay", "100", tmp, sizeof(tmp));
    cfg.txDelay = (uint16_t)atoi(tmp);

    cfgValue("rfPower", "128", tmp, sizeof(tmp));
    cfg.rfPower = (uint8_t)atoi(tmp);

    /* Only used with an OpenMT_HS modem; the P25 and M17 values */
    cfgValue("bitFlags0", "0x20", tmp, sizeof(tmp));
    cfg.bitFlags0 = (uint8_t)strtol(tmp, NULL, 0);

    cfgValue("bitFlags1", "0x11", tmp, sizeof(tmp));
    cfg.bitFlags1 = (uint8_t)strtol(tmp, NULL, 0);

    cfgValue("modeHang", "30", tmp, sizeof(tmp));
    long hang = atol(tmp);
    cfg.modeHang = (uint32_t)(hang < 0 ? 0 : (hang > 600 ? 600 : hang));

    cfgValue("timeout", "180", tmp, sizeof(tmp));
    cfg.timeout = (uint32_t)atoi(tmp);

    /* The network delivers the frames in bursts and with stalls of up to a second.  A transmission starts when
       this many ms of frames have arrived and then goes out at the frame rate, so a stall shorter than this does
       not run the transmitter dry.  0 sends every frame as it comes. */
    cfgValue("jitterMs", "300", tmp, sizeof(tmp));
    long jit = atol(tmp);
    cfg.jitterMs = (uint32_t)(jit < 0 ? 0 : (jit > 2000 ? 2000 : jit));

    cfgValue("txShape", "0", tmp, sizeof(tmp));
    cfg.txShape = atoi(tmp) ? NXDN_SHAPE_RC : NXDN_SHAPE_RRC;

    cfgValue("rxShape", "0", tmp, sizeof(tmp));
    cfg.rxShape = atoi(tmp) ? NXDN_SHAPE_RC : NXDN_SHAPE_RRC;

    cfgValue("rxMinAmp", "30", tmp, sizeof(tmp));
    cfg.rxMinAmp = (float)atof(tmp);

    if (cfg.rxMinAmp < 1.0F)
        cfg.rxMinAmp = 1.0F;

    readHostConfigN(modemName, "main", "callsign", station_call, sizeof(station_call));

    fprintf(stderr, "NXDN config: RAN %u id %u selfOnly %d remoteGateway %d rx %u tx %u txDelay %u ms "
            "modeHang %u s timeout %u s jitterMs %u\n", cfg.ran, cfg.id, cfg.selfOnly, cfg.remoteGateway,
            cfg.rxLevel, cfg.txLevel, cfg.txDelay, cfg.modeHang, cfg.timeout, cfg.jitterMs);
}

/*
 * isActiveMode()
 *
 * True when the modem host's active mode is NXDN or idle, i.e. when this
 * host is allowed to handle frames. The answer is cached for 200 ms.
 *
 * Parameters: none.
 *
 * Returns: true if the active mode is NXDN or idle, false if another
 *   protocol owns the modem.
 */
static bool isActiveMode(void)
{
    static uint64_t checked = 0U;
    static bool     result  = true;
    uint64_t        now     = nowMs();
    bool            ret;

    pthread_mutex_lock(&modeMutex);
    if (now - checked < 200U && checked != 0U)
    {
        ret = result;
        pthread_mutex_unlock(&modeMutex);
        return ret;
    }
    pthread_mutex_unlock(&modeMutex);

    char active_mode[10] = "IDLE";
    readStatus(modemName, "main", "active_mode", active_mode);

    ret = strcasecmp(active_mode, "NXDN") == 0 || strcasecmp(active_mode, "IDLE") == 0;

    pthread_mutex_lock(&modeMutex);
    checked = nowMs();
    result  = ret;
    pthread_mutex_unlock(&modeMutex);

    return ret;
}

/*
 * touchActivity()
 *
 * Restarts the mode hang time.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void touchActivity(void)
{
    lastActivityMs = nowMs();
}

/*
 * requestMode()
 *
 * Asks the modem host to switch to NXDN. Only works while the modem host is
 * idle, which is also the case while this host holds the mode.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void requestMode(void)
{
    modemWrite(SETMODE, sizeof(SETMODE));
    modeHeld = true;
    touchActivity();
}

/*
 * releaseMode()
 *
 * Hands the modem back to the idle state.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void releaseMode(void)
{
    if (modeHeld)
    {
        modemWrite(SETIDLE, sizeof(SETIDLE));
        modeHeld = false;
        fprintf(stderr, "NXDN: mode released.\n");
    }
}

/* ------------------------------------------------------------------------ */
/* Control callbacks                                                         */
/* ------------------------------------------------------------------------ */

/*
 * onNet()
 *
 * Control callback: an NXDN message for the gateway. Queued for the gateway
 * thread; dropped when no gateway is connected. The gateway decides about
 * linking (a call to a talk group links to it), so frames are passed on
 * whether or not a reflector is linked yet.
 *
 * Parameters:
 *   c       - Callback context; not used.
 *   netData - The NXDN message for the gateway.
 *   type    - Message type.
 *
 * Returns: nothing.
 */
static void onNet(void* c, const uint8_t* netData, uint8_t type)
{
    (void)c;

    if (!gwConnected)
        return;

    pthread_mutex_lock(&gwMutex);

    unsigned int next = (gwHead + 1U) % GW_FIFO_PACKETS;

    if (next != gwTail)
    {
        uint8_t* p = gwOut[gwHead];

        p[0] = 0x61;
        p[1] = 0x00;
        p[2] = 8 + 1 + NXDN_NET_DATA_LENGTH;
        p[3] = 0x04;
        memcpy(p + 4, TYPE_NXDN, 4);
        p[8] = type;
        memcpy(p + 9, netData, NXDN_NET_DATA_LENGTH);
        gwOutLen[gwHead] = 8 + 1 + NXDN_NET_DATA_LENGTH;
        gwHead = next;
    }
    else
    {
        fprintf(stderr, "NXDN: gateway queue overflow.\n");
    }

    pthread_mutex_unlock(&gwMutex);
}

/*
 * onRf()
 *
 * Control callback: a frame to transmit. Queued for the transmit thread.
 *
 * Parameters:
 *   c     - Callback context; not used.
 *   frame - The frame to transmit.
 *   eot   - true if this is the last frame of the transmission.
 *
 * Returns: nothing.
 */
static void onRf(void* c, const uint8_t* frame, bool eot)
{
    (void)c;

    pthread_mutex_lock(&txMutex);

    unsigned int next = (txHead + 1U) % TX_FIFO_FRAMES;

    if (next != txTail)
    {
        memcpy(txFifo[txHead].data, frame, NXDN_FRAME_LENGTH_BYTES);
        txFifo[txHead].eot = eot;
        txHead = next;
        pthread_cond_signal(&txCond);
    }
    else
    {
        fprintf(stderr, "NXDN: transmit queue overflow.\n");
    }

    pthread_mutex_unlock(&txMutex);

    touchActivity();
}

/*
 * onEvent()
 *
 * Control callback: a call started, ended or was lost. The dashboard work is
 * queued so the receive thread is not held up by the database. The modem is
 * switched to NXDN here, before the first frame goes out.
 *
 * Parameters:
 *   c    - Callback context; not used.
 *   call - Description of the call event (start, end or lost, RF or
 *          network).
 *
 * Returns: nothing.
 */
static void onEvent(void* c, const NxdnCall* call)
{
    (void)c;

    if (call->event == NXDN_EV_RF_START || call->event == NXDN_EV_RF_LATE || call->event == NXDN_EV_NET_START)
        requestMode();
    else
        touchActivity();

    pthread_mutex_lock(&evMutex);

    unsigned int next = (evHead + 1U) % EVENT_FIFO;

    if (next != evTail)
    {
        events[evHead].call  = *call;
        events[evHead].isNet = call->event == NXDN_EV_NET_START || call->event == NXDN_EV_NET_END ||
                               call->event == NXDN_EV_NET_LOST || call->event == NXDN_EV_NET_SWITCH;
        events[evHead].when  = time(NULL);
        evHead = next;
    }

    pthread_mutex_unlock(&evMutex);
}

/*
 * onRxFrame()
 *
 * DSP callback and firmware frame entry: one received frame, or the loss of
 * the signal. While the control is listening the frame is only used when
 * this host may take the modem. The caller holds ctrlMutex.
 *
 * Parameters:
 *   c     - Callback context; not used.
 *   frame - The received frame.
 *   lost  - true if the signal was lost instead of a frame arriving.
 *
 * Returns: nothing.
 */
static void onRxFrame(void* c, const uint8_t* frame, bool lost)
{
    (void)c;

    if (ctrl.rfState == NXDN_RF_LISTENING && !lost && !isActiveMode())
        return;

    nxdnCtrlRf(&ctrl, frame, lost);
}

/* ------------------------------------------------------------------------ */
/* Dashboard                                                                 */
/* ------------------------------------------------------------------------ */

/*
 * findNxdnId()
 *
 * Looks an NXDN radio id up in modem_host.nxdn_ids (kept across the DMR id
 * updates, which empty dmr_ids), then in dmr_ids. Returns the callsign and
 * name.
 *
 * Parameters:
 *   id           - NXDN radio id to look up.
 *   callsign     - Receives the callsign.
 *   callsignSize - Size of the callsign buffer.
 *   name         - Receives the name.
 *   nameSize     - Size of the name buffer.
 *
 * Returns: true if the id was found, false if not.
 */
static bool findNxdnId(unsigned int id, char* callsign, size_t callsignSize, char* name, size_t nameSize)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    char query[128];
    bool found = false;
    MYSQL* conn;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    fprintf(stderr, "NXDN: looking up id %u (database host %s)\n", id, dbhost);

    conn = mysql_init(NULL);

    if (conn != NULL)
    {
        if (mysql_real_connect(conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) != NULL)
        {
            snprintf(query, sizeof(query), "SELECT Callsign, Name FROM modem_host.nxdn_ids WHERE NXDN_Id = %u LIMIT 1", id);

            if (mysql_query(conn, query) == 0)
            {
                MYSQL_RES* result = mysql_store_result(conn);

                if (result != NULL)
                {
                    MYSQL_ROW row = mysql_fetch_row(result);

                    if (row != NULL && row[0] != NULL)
                    {
                        snprintf(callsign, callsignSize, "%s", row[0]);
                        snprintf(name, nameSize, "%s", row[1] != NULL ? row[1] : "");
                        found = true;
                    }

                    mysql_free_result(result);
                }
            }
            else
            {
                fprintf(stderr, "NXDN: nxdn_ids query failed (%s), trying dmr_ids\n", mysql_error(conn));
            }
        }
        else
        {
            fprintf(stderr, "NXDN: id lookup cannot connect to the database: %s\n", mysql_error(conn));
        }

        mysql_close(conn);
    }

    if (found)
    {
        fprintf(stderr, "NXDN: id %u found in nxdn_ids: %s\n", id, callsign);
        return true;
    }

    if (findDMRId(id, callsign, callsignSize, name, nameSize))
    {
        fprintf(stderr, "NXDN: id %u found in dmr_ids: %s\n", id, callsign);
        return true;
    }

    return false;
}

/*
 * serviceEvents()
 *
 * Writes the queued call events to the dashboard database: the live record
 * when a call starts, the finished record and a history entry when it ends.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void serviceEvents(void)
{
    for (;;)
    {
        HostEvent ev;

        pthread_mutex_lock(&evMutex);
        if (evTail == evHead)
        {
            pthread_mutex_unlock(&evMutex);
            return;
        }
        ev     = events[evTail];
        evTail = (evTail + 1U) % EVENT_FIFO;
        pthread_mutex_unlock(&evMutex);

        const char* cType = ev.isNet ? "NET" : "RF";

        switch (ev.call.event)
        {
            case NXDN_EV_RF_START:
            case NXDN_EV_RF_LATE:
            case NXDN_EV_NET_START:
            {
                char callsign[16] = "";
                char name[100]    = "";

                snprintf(callDst, sizeof(callDst), ev.call.group ? "TG %u" : "%u", ev.call.dst);
                callStart = ev.when;

                if (findNxdnId(ev.call.src, callsign, sizeof(callsign), name, sizeof(name)))
                {
                    snprintf(callSrc, sizeof(callSrc), "%s", callsign);
                    snprintf(callSuffix, sizeof(callSuffix), "%u", ev.call.src);
                }
                else
                {
                    fprintf(stderr, "NXDN: id %u is not in nxdn_ids or dmr_ids.\n", ev.call.src);
                    snprintf(callSrc, sizeof(callSrc), "%u", ev.call.src);
                    callSuffix[0] = '\0';
                }

                callActive = true;
                fprintf(stderr, "NXDN %s %s: %s %s -> %s\n", cType,
                        ev.call.event == NXDN_EV_RF_LATE ? "late entry" : "call", callSrc, callSuffix, callDst);
                saveLastCall(1, modemName, "NXDN", cType, callSrc, callSuffix, callDst, "", NULL, "", true);
                break;
            }
            case NXDN_EV_RF_END:
            case NXDN_EV_NET_END:
            case NXDN_EV_RF_LOST:
            case NXDN_EV_NET_LOST:
            case NXDN_EV_NET_SWITCH:
            {
                if (!callActive)
                    break;

                long secs = (long)(ev.when - callStart);

                if (secs < 0)
                    secs = 0;

                fprintf(stderr, "NXDN %s %s: %s -> %s, %.1f s, BER %.2f%%\n", cType,
                        (ev.call.event == NXDN_EV_RF_LOST || ev.call.event == NXDN_EV_NET_LOST) ? "lost" : "end",
                        callSrc, callDst, ev.call.seconds, ev.call.ber);

                if (ev.call.event == NXDN_EV_NET_SWITCH)
                    fprintf(stderr, "NXDN NET: %s sent no release, another station started to talk\n", callSrc);

                saveLastCall(1, modemName, "NXDN", cType, callSrc, callSuffix, callDst, "", NULL, "", false);
                saveHistory(modemName, "NXDN", cType, callSrc, callSuffix, callDst, ev.call.ber / 100.0F, "",
                            (uint16_t)secs);
                callActive = false;
                break;
            }
            case NXDN_EV_RF_REJECTED:
                fprintf(stderr, "NXDN call from %u rejected (selfOnly).\n", ev.call.src);
                break;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Transmit                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct
{
    uint8_t  pkt[200];
    unsigned int count;
    uint64_t playEnd;
    bool     active;
} SampWriter;

/*
 * sampStart()
 *
 * Starts a new burst of transmit samples with a fresh pulse shaping filter.
 *
 * Parameters:
 *   w - The sample writer.
 *
 * Returns: nothing.
 */
static void sampStart(SampWriter* w)
{
    nxdnTxInit(&tx, NXDN_LEVEL_OUTER, cfg.txShape);
    w->count   = 0U;
    w->playEnd = nowMs();
    w->active  = true;
    memset(w->pkt, 0x00, sizeof(w->pkt));
}

/*
 * sampSend()
 *
 * Sends the packet being assembled to the modem host and waits when more
 * than TX_AHEAD_MS of samples are already queued, so the modem host's buffer
 * cannot overflow.
 *
 * Parameters:
 *   w    - The sample writer.
 *   type - Four character packet type to send.
 *
 * Returns: nothing.
 */
static void sampSend(SampWriter* w, const char* type)
{
    uint64_t now = nowMs();

    if (w->playEnd < now)
        w->playEnd = now;

    while (connected && w->playEnd - now > TX_AHEAD_MS)
    {
        delay(2000);
        now = nowMs();
    }

    w->pkt[0] = 0x61;
    w->pkt[1] = 0x00;
    w->pkt[2] = 0xc8;
    w->pkt[3] = 0x04;
    memcpy(w->pkt + 4, type, 4);

    modemWrite(w->pkt, sizeof(w->pkt));

    if (strcmp(type, TYPE_SAMPLE) == 0)
        w->playEnd += (SAMPLES_PER_PACKET * 1000U) / 24000U;

    w->count = 0U;
    memset(w->pkt + 8, 0x00, sizeof(w->pkt) - 8U);
}

/*
 * sampAdd()
 *
 * Appends samples to the packet being assembled.
 *
 * Parameters:
 *   w       - The sample writer.
 *   samples - Samples to add.
 *   length  - Number of samples.
 *
 * Returns: nothing.
 */
static void sampAdd(SampWriter* w, const int16_t* samples, unsigned int length)
{
    for (unsigned int i = 0U; i < length; i++)
    {
        uint8_t* p = w->pkt + 8 + w->count * 2U;

        p[0] = (uint8_t)(samples[i] & 0xFF);
        p[1] = (uint8_t)((samples[i] >> 8) & 0xFF);
        w->count++;

        if (w->count == SAMPLES_PER_PACKET)
            sampSend(w, TYPE_SAMPLE);
    }
}

/*
 * sampEnd()
 *
 * Flushes the filter, sends the last partly filled packet and the end of
 * transmission packet.
 *
 * Parameters:
 *   w - The sample writer.
 *
 * Returns: nothing.
 */
static void sampEnd(SampWriter* w)
{
    int16_t tail[NXDN_SAMPLES_PER_SYMBOL * (NXDN_FILTER_SPAN + 1U)];
    unsigned int n = nxdnTxFlush(&tx, tail);

    sampAdd(w, tail, n);

    if (w->count > 0U)
        sampSend(w, TYPE_SAMPLE);

    sampSend(w, TYPE_SAMPLE_EOT);
    w->active = false;
}

typedef struct
{
    uint8_t  pkt[200];
    unsigned int count;
    uint64_t playEnd;
    bool     active;
} BitWriter;

/*
 * bitStart()
 *
 * Starts a new burst of transmit bits.
 *
 * Parameters:
 *   w - The bit writer.
 *
 * Returns: nothing.
 */
static void bitStart(BitWriter* w)
{
    w->count   = 0U;
    w->playEnd = nowMs();
    w->active  = true;
    memset(w->pkt, 0x00, sizeof(w->pkt));
}

/*
 * bitSend()
 *
 * Sends the BITS packet being assembled (unused bytes are zero) and waits
 * when more than TX_AHEAD_MS of bits are already queued.
 *
 * Parameters:
 *   w - The bit writer.
 *
 * Returns: nothing.
 */
static void bitSend(BitWriter* w)
{
    uint64_t now = nowMs();

    if (w->playEnd < now)
        w->playEnd = now;

    while (connected && w->playEnd - now > TX_AHEAD_MS)
    {
        delay(2000);
        now = nowMs();
    }

    w->pkt[0] = 0x61;
    w->pkt[1] = 0x00;
    w->pkt[2] = 0xc8;
    w->pkt[3] = 0x04;
    memcpy(w->pkt + 4, TYPE_BITS, 4);

    modemWrite(w->pkt, sizeof(w->pkt));

    w->playEnd += (BIT_BYTES_PER_PACKET * 1000U) / BIT_BYTES_PER_SECOND;
    w->count = 0U;
    memset(w->pkt + 8, 0x00, sizeof(w->pkt) - 8U);
}

/*
 * bitAdd()
 *
 * Appends bytes of the bit stream, most significant bit first.
 *
 * Parameters:
 *   w      - The bit writer.
 *   data   - Bytes to add.
 *   length - Number of bytes.
 *
 * Returns: nothing.
 */
static void bitAdd(BitWriter* w, const uint8_t* data, unsigned int length)
{
    for (unsigned int i = 0U; i < length; i++)
    {
        w->pkt[8 + w->count] = data[i];
        w->count++;

        if (w->count == BIT_BYTES_PER_PACKET)
            bitSend(w);
    }
}

/*
 * bitEnd()
 *
 * Sends the last partly filled packet; its unused bytes are zero, as in the
 * other OpenMT hosts' end of transmission.
 *
 * Parameters:
 *   w - The bit writer.
 *
 * Returns: nothing.
 */
static void bitEnd(BitWriter* w)
{
    if (w->count > 0U)
        bitSend(w);

    w->active = false;
}

/*
 * txThread()
 *
 * Takes the frames queued by the control logic and sends them to the modem
 * host: modulated as samples for the SAMP modem, as bits for an OpenMT_HS
 * modem, as frames for an MMDVM modem. A transmission starts with a
 * preamble, ends with the frame marked end of transmission, or after
 * TX_UNDERRUN_MS without a frame.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL.
 */
static void* txThread(void* arg)
{
    static SampWriter w;
    static BitWriter  bw;
    static int16_t    buf[NXDN_SAMPLES_PER_FRAME];
    bool     burst      = false;
    uint64_t due        = 0U;       /* when the next frame of the transmission goes out */
    uint64_t prefillAt  = 0U;       /* when the first frame of the next transmission came */

    (void)arg;

    while (connected)
    {
        TxFrame f;
        bool    have = false;
        bool    ready = false;
        uint64_t now = nowMs();
        unsigned int queued;

        pthread_mutex_lock(&txMutex);
        queued = (txHead + TX_FIFO_FRAMES - txTail) % TX_FIFO_FRAMES;

        if (queued > 0U)
        {
            if (!burst)
            {
                /* A new transmission: collect cfg.jitterMs of frames first (or all of it, when the end frame is
                   already there), so that the frames that follow have a head start on a network that stalls. */
                bool eotQueued = false;

                for (unsigned int i = 0U; i < queued; i++)
                {
                    if (txFifo[(txTail + i) % TX_FIFO_FRAMES].eot)
                        eotQueued = true;
                }

                if (prefillAt == 0U)
                    prefillAt = now;

                ready = cfg.jitterMs == 0U || eotQueued || queued * NXDN_FRAME_MS >= cfg.jitterMs ||
                        now - prefillAt >= cfg.jitterMs + NXDN_FRAME_MS;
            }
            else
            {
                /* In a transmission the frames go out at the frame rate, whatever the way they came in */
                ready = now >= due;
            }
        }

        if (ready)
        {
            f      = txFifo[txTail];
            txTail = (txTail + 1U) % TX_FIFO_FRAMES;
            have   = true;
        }
        else
        {
            struct timespec ts;
            long waitMs = 20L;

            if (queued > 0U && burst && due > now && due - now < 20U)
                waitMs = (long)(due - now);

            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += waitMs * 1000000L;
            if (ts.tv_nsec >= 1000000000L)
            {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&txCond, &txMutex, &ts);
        }
        pthread_mutex_unlock(&txMutex);

        if (!have)
        {
            if (burst && queued == 0U && nowMs() > due + TX_UNDERRUN_MS + cfg.jitterMs)
            {
                fprintf(stderr, "NXDN: transmit underrun, ending transmission.\n");
                if (packetType == PACKET_TYPE_SAMP)
                    sampEnd(&w);
                else if (packetType == PACKET_TYPE_BIT)
                    bitEnd(&bw);
                else
                {
                    uint8_t lost[8] = {0x61, 0x00, 0x08, 0x04, 'N', 'X', 'D', 'L'};
                    modemWrite(lost, sizeof(lost));
                }
                burst    = false;
                txActive = false;
                prefillAt = 0U;
                touchActivity();
            }
            continue;
        }

        /* the schedule of the transmission: one frame every NXDN_FRAME_MS; when a frame is late the schedule starts
           again from now (a burst of late frames is not sent faster) */
        now = nowMs();

        if (!burst)
        {
            due = now;

            /* the mode request normally comes with the call start, but that can follow the first frames */
            if (!modeHeld)
                requestMode();
        }
        else if (now > due + NXDN_FRAME_MS)
            due = now;

        due += NXDN_FRAME_MS;
        prefillAt = 0U;
        txActive  = true;

        if (packetType == PACKET_TYPE_SAMP)
        {
            if (!burst)
            {
                sampStart(&w);
                unsigned int symbols = ((unsigned int)cfg.txDelay * 24U) / 10U;     /* 2400 symbols/s */
                symbols &= ~1U;

                while (symbols > 0U)
                {
                    unsigned int chunk = symbols > 48U ? 48U : symbols;
                    int16_t pre[48 * NXDN_SAMPLES_PER_SYMBOL];
                    unsigned int n = nxdnTxPreamble(&tx, chunk, pre);

                    sampAdd(&w, pre, n);
                    symbols -= chunk;
                }
                burst = true;
            }

            unsigned int n = nxdnTxFrame(&tx, f.data, buf);
            sampAdd(&w, buf, n);

            if (f.eot)
            {
                sampEnd(&w);
                burst = false;
            }
        }
        else if (packetType == PACKET_TYPE_BIT)
        {
            if (!burst)
            {
                uint8_t pre[64];
                unsigned int bytes = ((unsigned int)cfg.txDelay * BIT_BYTES_PER_SECOND) / 1000U;

                bitStart(&bw);

                while (bytes > 0U)
                {
                    unsigned int chunk = bytes > sizeof(pre) ? (unsigned int)sizeof(pre) : bytes;

                    nxdnBitTxPreamble(pre, chunk);
                    bitAdd(&bw, pre, chunk);
                    bytes -= chunk;
                }

                burst = true;
            }

            bitAdd(&bw, f.data, NXDN_FRAME_LENGTH_BYTES);

            if (f.eot)
            {
                bitEnd(&bw);
                burst = false;
            }
        }
        else if (packetType == PACKET_TYPE_FRAME)
        {
            uint8_t pkt[8 + 1 + NXDN_FRAME_LENGTH_BYTES];

            pkt[0] = 0x61;
            pkt[1] = 0x00;
            pkt[2] = sizeof(pkt);
            pkt[3] = 0x04;
            memcpy(pkt + 4, TYPE_DATA, 4);
            pkt[8] = 0x00;
            memcpy(pkt + 9, f.data, NXDN_FRAME_LENGTH_BYTES);
            modemWrite(pkt, sizeof(pkt));
            burst = true;

            if (f.eot)
            {
                uint8_t lost[8] = {0x61, 0x00, 0x08, 0x04, 'N', 'X', 'D', 'L'};
                modemWrite(lost, sizeof(lost));
                burst = false;
            }
        }

        if (!burst)
        {
            txActive = false;
            touchActivity();
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Gateway                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * gwQueue()
 *
 * Queues a small control packet (connect, disconnect, update configuration)
 * for the gateway.  Packets longer than GW_PACKET_MAX are dropped.
 *
 * Parameters:
 *   data   - The packet.
 *   length - Number of bytes in data.
 *
 * Returns: nothing.
 */
static void gwQueue(const uint8_t* data, unsigned int length)
{
    if (length > GW_PACKET_MAX)
        return;

    pthread_mutex_lock(&gwMutex);

    unsigned int next = (gwHead + 1U) % GW_FIFO_PACKETS;

    if (next != gwTail)
    {
        memcpy(gwOut[gwHead], data, length);
        gwOutLen[gwHead] = (uint8_t)length;
        gwHead = next;
    }

    pthread_mutex_unlock(&gwMutex);
}

/*
 * gwHandle()
 *
 * Handles one packet from the gateway: reflector replies update the link
 * status on the dashboard, commands are acknowledged and NXDN messages from
 * the network go to the control logic.
 *
 * Parameters:
 *   buffer  - The packet from the gateway.
 *   respLen - Length of the packet.
 *
 * Returns: nothing.
 */
static void gwHandle(const uint8_t* buffer, uint16_t respLen)
{
    uint8_t typeLen = buffer[3];

    if (typeLen != 4 || respLen < 8)
        return;

    char name[8];

    memset(name, 0x00, sizeof(name));

    if (respLen >= 14)
        memcpy(name, buffer + 8, 6);

    if (memcmp(buffer + 4, TYPE_NACK, 4) == 0)
    {
        gwLinked = false;
        ackDashbCommand(modemName, "reflLinkNXDN", "failed");
        setReflectorStatus(modemName, "NXDN", name, false);
    }
    else if (memcmp(buffer + 4, TYPE_CONNECT, 4) == 0)
    {
        gwLinked = true;
        strncpy(reflName, name, sizeof(reflName) - 1);
        ackDashbCommand(modemName, "reflLinkNXDN", "success");
        setReflectorStatus(modemName, "NXDN", name, true);
    }
    else if (memcmp(buffer + 4, TYPE_DISCONNECT, 4) == 0)
    {
        gwLinked = false;
        ackDashbCommand(modemName, "reflLinkNXDN", "unlinked");
        setReflectorStatus(modemName, "NXDN", name, false);
    }
    else if (memcmp(buffer + 4, TYPE_COMMAND, 4) == 0)
    {
        ackDashbCommand(modemName, "updateConfNXDN", "success");
    }
    else if (memcmp(buffer + 4, TYPE_STATUS, 4) == 0)
    {
    }
    else if (memcmp(buffer + 4, TYPE_NXDN, 4) == 0 && respLen == 8 + 1 + NXDN_NET_DATA_LENGTH)
    {
        if (isActiveMode())
        {
            pthread_mutex_lock(&ctrlMutex);
            nxdnCtrlNet(&ctrl, buffer + 9);
            pthread_mutex_unlock(&ctrlMutex);
        }
    }
}

/*
 * gatewayThread()
 *
 * Serves the connected NXDN gateway: sends what the control logic and the
 * dashboard queued for it and reads what it sends. Registers the gateway
 * with the dashboard while it is connected.
 *
 * Parameters:
 *   arg - Socket of the gateway connection, passed as a pointer sized
 *         integer.
 *
 * Returns: NULL.
 */
static void* gatewayThread(void* arg)
{
    int     fd = (int)(intptr_t)arg;
    uint8_t buffer[BUFFER_SIZE];

    gwFd        = fd;
    gwLinked    = false;
    gwConnected = true;

    pthread_mutex_lock(&gwMutex);
    gwHead = gwTail = 0U;
    pthread_mutex_unlock(&gwMutex);

    addGateway(modemName, "main", "NXDN");
    fprintf(stderr, "NXDN gateway connected.\n");

    while (connected && gwFd == fd)
    {
        fd_set rfds;
        struct timeval tv;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec  = 0;
        tv.tv_usec = 20000;

        int r = select(fd + 1, &rfds, NULL, NULL, &tv);

        if (r < 0 && errno != EINTR)
            break;

        if (r > 0 && FD_ISSET(fd, &rfds))
        {
            if (!readFully(fd, buffer, 4))
                break;

            if (buffer[0] != 0x61)
            {
                fprintf(stderr, "NXDN: unknown byte from gateway, 0x%02X\n", buffer[0]);
                continue;
            }

            uint16_t respLen = (uint16_t)((buffer[1] << 8) + buffer[2]);

            if (respLen < 5 || respLen > BUFFER_SIZE)
            {
                fprintf(stderr, "NXDN: bad packet length %u from gateway\n", respLen);
                continue;
            }

            if (respLen > 4 && !readFully(fd, buffer + 4, respLen - 4))
                break;

            if (debugM)
                dump("NXDN gateway data", buffer, respLen);

            gwHandle(buffer, respLen);
        }

        for (;;)
        {
            uint8_t pkt[GW_PACKET_MAX];
            unsigned int len;

            pthread_mutex_lock(&gwMutex);
            if (gwTail == gwHead)
            {
                pthread_mutex_unlock(&gwMutex);
                break;
            }
            len = gwOutLen[gwTail];
            memcpy(pkt, gwOut[gwTail], len);
            gwTail = (gwTail + 1U) % GW_FIFO_PACKETS;
            pthread_mutex_unlock(&gwMutex);

            if (!writeAll(fd, pkt, len))
            {
                reportError(modemName, "NXDN", "NXDN: gateway write failed.");
                r = -1;
                break;
            }
        }

        if (r < 0)
            break;
    }

    fprintf(stderr, "NXDN gateway disconnected.\n");

    close(fd);

    if (gwFd == fd)
    {
        gwFd        = -1;
        gwConnected = false;
        gwLinked    = false;
        delGateway(modemName, "main", "NXDN");
        clearReflLinkStatus(modemName, "NXDN");
    }

    return NULL;
}

/*
 * serverThread()
 *
 * Listens for the NXDN gateway on the server port. A new connection replaces
 * an earlier one.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL.
 */
static void* serverThread(void* arg)
{
    int on = 1;
    struct sockaddr_in addr;

    (void)arg;

    int lfd = socket(AF_INET, SOCK_STREAM, 0);

    if (lfd < 0)
    {
        reportError(modemName, "NXDN", "NXDN: error creating the gateway socket: %s", strerror(errno));
        exit(1);
    }

    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&addr, 0x00, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(serverPort);

    if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        reportError(modemName, "NXDN", "NXDN: error binding port %u: %s", serverPort, strerror(errno));
        exit(1);
    }

    if (listen(lfd, 4) < 0)
    {
        reportError(modemName, "NXDN", "listen: %s", strerror(errno));
        exit(1);
    }

    while (connected)
    {
        int cfd = accept(lfd, NULL, NULL);

        if (cfd < 0)
        {
            if (errno == EINTR)
                continue;
            reportError(modemName, "NXDN", "accept: %s", strerror(errno));
            break;
        }

        if (gwFd >= 0)
            shutdown(gwFd, SHUT_RDWR);

        pthread_t id;

        if (pthread_create(&id, NULL, gatewayThread, (void*)(intptr_t)cfd) != 0)
        {
            reportError(modemName, "NXDN", "NXDN: can't create gateway thread");
            close(cfd);
            continue;
        }

        pthread_detach(id);
    }

    close(lfd);

    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Modem host                                                                */
/* ------------------------------------------------------------------------ */

/*
 * registerMode()
 *
 * Sends the MODE packet that registers this host with the modem host. The
 * radio settings of the packet are not used with a SAMP or MMDVM modem.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void registerMode(void)
{
    uint8_t buffer[84];

    memset(buffer, 0x00, sizeof(buffer));
    buffer[0] = 0x61;
    buffer[1] = 0x00;
    buffer[2] = sizeof(buffer);
    buffer[3] = 0x04;
    memcpy(buffer + 4, TYPE_MODE, 4);
    memcpy(buffer + 8, MODE_NAME, strlen(MODE_NAME));
    memcpy(buffer + 19, MODEM_TYPE, strlen(MODEM_TYPE));
    buffer[25] = cfg.txLevel;

    if (packetType == PACKET_TYPE_BIT)
    {
        /*
         * OpenMT_HS: the modem firmware is configured with the ADF7021 registers
         * of the mode, as the P25, D-Star and DMR hosts do. The values are those
         * of the MMDVM_HS firmware for NXDN: 4FSK with the raised cosine filter,
         * 12.5 kHz IF, data inverted, slicer threshold 26.
         */
        uint8_t  bytes[40];
        uint32_t reg3  = NXDN_ADF_REG3;
        uint32_t reg10 = NXDN_ADF_REG10;
        uint32_t reg4  = (uint32_t)0b0100                << 0;   /* register 4          */
        uint32_t reg2  = (uint32_t)0b10                  << 28;  /* invert data         */
        uint32_t reg13 = (uint32_t)0b1101                << 0;   /* register 13         */
        uint64_t sync[2];
        uint32_t level = (uint32_t)(cfg.txLevel * 2.55F + 0.5F);    /* 0 - 255, 128 = nominal */
        uint32_t dev   = (NXDN_ADF_DEV * level) / 26U;             /* ifConf() scales by 26/128 */

        reg4  |= (uint32_t)0b011                         << 4;   /* mode, 4FSK          */
        reg4  |= (uint32_t)0b0                           << 7;
        reg4  |= (uint32_t)0b11                          << 8;
        reg4  |= (uint32_t)NXDN_ADF_DISC_BW              << 10;  /* discriminator BW    */
        reg4  |= (uint32_t)NXDN_ADF_POST_BW              << 20;  /* post demod BW       */
        reg4  |= (uint32_t)0b00                          << 30;  /* IF filter 12.5 kHz  */
        reg2  |= (uint32_t)0b111                         << 4;   /* modulation, RC 4FSK */
        reg13 |= (uint32_t)ADF7021_SLICER_TH_NXDN        << 4;   /* slicer threshold    */

        if (dev > 255U)
            dev = 255U;

        ifConf(bytes, reg2, reg3, reg4, reg10, reg13, (uint32_t)atol(rxFrequency), (uint32_t)atol(txFrequency),
               (uint8_t)dev, (uint8_t)(cfg.rfPower >> 2), 0, true);

        memcpy(buffer + 26, bytes, 40);

        sync[0] = 0x000CDF59ULL;        /* frame sync word, 20 bits */
        sync[1] = 0x000FFFFFULL;        /* and its mask             */
        memcpy(buffer + 66, (uint8_t*)sync, 16);

        buffer[82] = cfg.bitFlags0;
        buffer[83] = cfg.bitFlags1;
    }

    modemWrite(buffer, sizeof(buffer));
}

/*
 * rxSamples()
 *
 * Applies the receive level to a packet of demodulated samples and feeds
 * them to the receiver.
 *
 * Parameters:
 *   data   - Demodulated samples, two bytes each, least significant byte
 *            first.
 *   length - Number of bytes in data.
 *
 * Returns: nothing.
 */
static void rxSamples(const uint8_t* data, unsigned int length)
{
    int16_t smp[BUFFER_SIZE / 2];
    unsigned int count = length / 2U;

    if (count > BUFFER_SIZE / 2U)
        count = BUFFER_SIZE / 2U;

    for (unsigned int i = 0U; i < count; i++)
    {
        int32_t v = (int16_t)(data[i * 2U] | (data[i * 2U + 1U] << 8));

        v = (v * (int32_t)(cfg.rxLevel * 128)) >> 15;

        if (v > 32767)
            v = 32767;
        if (v < -32768)
            v = -32768;

        smp[i] = (int16_t)v;
    }

    pthread_mutex_lock(&ctrlMutex);
    nxdnRxSamples(&rx, smp, count);
    pthread_mutex_unlock(&ctrlMutex);
}

/*
 * clientThread()
 *
 * Thread that reads the modem host connection. Demodulated samples go to the
 * receiver, frames from the MMDVM firmware go to the control logic and
 * command packets update the duplex flag.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL.
 */
static void* clientThread(void* arg)
{
    uint8_t buffer[BUFFER_SIZE];

    (void)arg;

    while (connected)
    {
        if (!readFully(sockfd, buffer, 1))
            break;

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "NXDN: unknown byte from modem host, 0x%02X\n", buffer[0]);
            continue;
        }

        if (!readFully(sockfd, buffer + 1, 3))
            break;

        uint16_t respLen = (uint16_t)((buffer[1] << 8) + buffer[2]);

        if (respLen < 5 || respLen > BUFFER_SIZE)
        {
            fprintf(stderr, "NXDN: bad packet length %u\n", respLen);
            continue;
        }

        if (respLen > 4 && !readFully(sockfd, buffer + 4, respLen - 4))
            break;

        if (buffer[3] != 4 || respLen < 8)
            continue;

        if (memcmp(buffer + 4, TYPE_COMMAND, 4) == 0)
        {
            if (respLen > 8 && buffer[8] == COMM_SET_DUPLEX)
                modem_duplex = true;
            else if (respLen > 8 && buffer[8] == COMM_SET_SIMPLEX)
                modem_duplex = false;

            pthread_mutex_lock(&ctrlMutex);
            ctrl.duplex = modem_duplex;
            ctrl.localRepeat = modem_duplex;   /* RF is re-transmitted only in duplex */
            pthread_mutex_unlock(&ctrlMutex);
        }
        else if (memcmp(buffer + 4, TYPE_SAMPLE, 4) == 0 && packetType == PACKET_TYPE_SAMP)
        {
            rxSamples(buffer + 8, respLen - 8U);
        }
        else if (memcmp(buffer + 4, TYPE_BITS, 4) == 0 && packetType == PACKET_TYPE_BIT)
        {
            pthread_mutex_lock(&ctrlMutex);
            nxdnBitRxBytes(&bitRx, buffer + 8, respLen - 8U);
            pthread_mutex_unlock(&ctrlMutex);
        }
        else if (memcmp(buffer + 4, TYPE_DATA, 4) == 0 && packetType == PACKET_TYPE_FRAME)
        {
            /* [placeholder] [48 frame bytes] [optional 2 RSSI bytes] */
            if (respLen >= 8U + 1U + NXDN_FRAME_LENGTH_BYTES)
            {
                pthread_mutex_lock(&ctrlMutex);
                onRxFrame(NULL, buffer + 9, false);
                pthread_mutex_unlock(&ctrlMutex);
            }
        }
        else if (memcmp(buffer + 4, TYPE_LOST, 4) == 0 && packetType == PACKET_TYPE_FRAME)
        {
            pthread_mutex_lock(&ctrlMutex);
            onRxFrame(NULL, NULL, true);
            pthread_mutex_unlock(&ctrlMutex);
        }
    }

    fprintf(stderr, "Disconnected from host.\n");
    connected = false;

    return NULL;
}

/*
 * timerThread()
 *
 * Runs the control timers every 10 ms, releases the modem once the mode hang
 * time has passed without activity and prints receiver statistics when
 * debugging.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL.
 */
static void* timerThread(void* arg)
{
    uint64_t last      = nowMs();
    uint64_t lastStats = last;

    (void)arg;

    while (connected)
    {
        delay(10000);

        uint64_t now = nowMs();
        uint32_t ms  = (uint32_t)(now - last);

        last = now;

        pthread_mutex_lock(&ctrlMutex);
        nxdnCtrlClock(&ctrl, ms);

        bool busy = nxdnCtrlBusy(&ctrl);

        if (debugM && now - lastStats >= 5000U)
        {
            lastStats = now;
            fprintf(stderr, "NXDN rx: cand %u locks %u frames %u lost %u dropped %u lastR %.2f amp %.0f rf %d net %d\n",
                    rx.statCandidates, rx.statLocks, rx.statFrames, rx.statLost, rx.statDropped, rx.statLastR,
                    rx.statAmp, ctrl.rfState, ctrl.netState);
        }
        pthread_mutex_unlock(&ctrlMutex);

        if (busy || txActive)
            touchActivity();
        else if (modeHeld && now - lastActivityMs >= (uint64_t)cfg.modeHang * 1000U)
            releaseMode();
    }

    return NULL;
}

/*
 * connectModemHost()
 *
 * Connects to the modem host and registers the NXDN mode.
 *
 * Parameters: none.
 *
 * Returns: true if the connection was made and the mode registered, false if
 *   not.
 */
static bool connectModemHost(void)
{
    struct sockaddr_in serv_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0)
    {
        reportError(modemName, "NXDN", "Socket creation error: %s", strerror(errno));
        return false;
    }

    int on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));

    memset(&serv_addr, 0x00, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(clientPort);

    if (!hostToAddress(modemHost, &serv_addr.sin_addr))
    {
        reportError(modemName, "NXDN", "Invalid address/ Address not supported");
        return false;
    }

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        reportError(modemName, "NXDN", "connection failed: %s", strerror(errno));
        return false;
    }

    registerMode();

    return true;
}

/*
 * pollDashboard()
 *
 * Checks the dashboard for configuration and reflector link commands.
 * Commands go to the gateway when one is connected, otherwise they are
 * answered at once.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void pollDashboard(void)
{
    char parameter[31];

    if (gwConnected)
    {
        readDashbCommand(modemName, "updateConfNXDN", parameter);

        if (strcasecmp(parameter, "readConfig") == 0)
        {
            uint8_t buf[9];

            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0x09;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_COMMAND, 4);
            buf[8] = COMM_UPDATE_CONF;
            gwQueue(buf, 9);
        }

        parameter[0] = '\0';
        readDashbCommand(modemName, "reflLinkNXDN", parameter);

        if (strlen(parameter) == 0)
            return;

        if (strcasecmp(parameter, "unlink") == 0)
        {
            uint8_t buf[8];

            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0x08;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_DISCONNECT, 4);
            gwQueue(buf, 8);
            sleep(3);
        }
        else
        {
            char tmp[31];
            char* token;

            snprintf(tmp, sizeof(tmp), "%s", parameter);
            token = strtok(tmp, ",");

            if (token != NULL && strcasecmp(token, "link") == 0 && (token = strtok(NULL, ",")) != NULL)
            {
                char name[8];
                char callsign[9];
                uint8_t buf[20];

                memset(name, 0x00, sizeof(name));
                memset(callsign, 0x00, sizeof(callsign));
                strncpy(name, token, 6);
                readHostConfigN(modemName, "main", "callsign", callsign, sizeof(callsign));

                buf[0] = 0x61;
                buf[1] = 0x00;
                buf[2] = 0x14;
                buf[3] = 0x04;
                memcpy(buf + 4, TYPE_CONNECT, 4);
                memset(buf + 8, 0x00, 12);
                memcpy(buf + 8, callsign, strnlen(callsign, 6));
                memcpy(buf + 14, name, strnlen(name, 6));
                gwQueue(buf, 20);
                sleep(3);
            }
            else
            {
                ackDashbCommand(modemName, "reflLinkNXDN", "failed");
            }
        }
    }
    else
    {
        readDashbCommand(modemName, "updateConfNXDN", parameter);
        if (strlen(parameter) > 0)
            ackDashbCommand(modemName, "updateConfNXDN", "success");

        parameter[0] = '\0';
        readDashbCommand(modemName, "reflLinkNXDN", parameter);
        if (strlen(parameter) > 0)
            ackDashbCommand(modemName, "reflLinkNXDN", "No gateway");
    }
}

/*
 * main()
 *
 * Program entry. Parses the command line (-m modem number 1-10, which also
 * selects the TCP ports, -d daemon, -v version, -x debug), reads the
 * configuration, connects to the modem host and runs until the connection is
 * lost.
 *
 * Parameters:
 *   argc - Number of command line arguments.
 *   argv - Command line arguments.
 *
 * Returns: 0 on exit, 1 on a bad argument.
 */
int main(int argc, char** argv)
{
    bool daemonize = false;
    int  c;

    while ((c = getopt(argc, argv, ":m:a:s:u:p:c:dxv")) != -1)
    {
        switch (c)
        {
            case 'm':
                modemId = (uint8_t)atoi(optarg);
                if (modemId < 1 || modemId > 10)
                {
                    reportError(modemName, "NXDN", "Invalid modem number.");
                    return 1;
                }
                sprintf(modemName, "modem%d", modemId);
                clientPort = 18000 + modemId - 1;
                serverPort = 18500 + modemId - 1;
                fprintf(stderr, "Modem name: %s\n", modemName);
                break;
            case 'a':
                snprintf(modemHost, sizeof(modemHost), "%s", optarg);   /* where the modem host runs */
                break;
            case 's':
            case 'u':
            case 'p':
            case 'c':
                dbOption(c, optarg);   /* database server, user, password, configuration file */
                break;
            case ':':
                fprintf(stderr, "Option '-%c' requires a value.\n", optopt);
                return 1;
            case 'd':
                daemonize = true;
                break;
            case 'v':
                fprintf(stdout, "NXDN_Service: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: NXDN_Service [-m modem_number (1-10)] [-a address] [-s dbserver] [-u dbuser] [-p dbpassword] [-c configfile] [-d] [-v] [-x]\n");
                return 1;
        }
    }

    if (daemonize)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            reportError(modemName, "NXDN", "NXDN_Service: error in fork(), exiting");
            return 1;
        }

        if (pid > 0)
            return 0;

        setsid();
        umask(0);
    }

    char tmp[15];

    readHostConfigN(modemName, "config", "modem", tmp, sizeof(tmp));

    if (strcasecmp(tmp, "openmt") == 0 || strcasecmp(tmp, "openmtsx") == 0)
        packetType = PACKET_TYPE_SAMP;
    else if (strcasecmp(tmp, "openmths") == 0)
        packetType = PACKET_TYPE_BIT;
    else
        packetType = PACKET_TYPE_FRAME;

    loadConfig();

    readHostConfigN(modemName, "config", "rxFrequency", rxFrequency, sizeof(rxFrequency));
    readHostConfigN(modemName, "config", "txFrequency", txFrequency, sizeof(txFrequency));

    nxdnCtrlInit(&ctrl);
    ctrl.ran           = cfg.ran;
    ctrl.id            = cfg.id;
    ctrl.selfOnly      = cfg.selfOnly;
    ctrl.remoteGateway = cfg.remoteGateway;
    ctrl.localRepeat   = false;     /* follows the duplex command from Modem_Host */
    ctrl.rfTimeoutMs   = cfg.timeout * 1000U;
    ctrl.sendNet       = onNet;
    ctrl.sendRf        = onRf;
    ctrl.event         = onEvent;

    nxdnRxInit(&rx, onRxFrame, NULL, cfg.rxShape);
    nxdnBitRxInit(&bitRx, onRxFrame, NULL);
    rx.minAmp = cfg.rxMinAmp;
    nxdnTxInit(&tx, NXDN_LEVEL_OUTER, cfg.txShape);

    clearReflLinkStatus(modemName, "NXDN");

    if (!connectModemHost())
        return 1;

    pthread_t id;

    if (pthread_create(&id, NULL, serverThread, NULL) != 0 ||
        pthread_create(&id, NULL, txThread, NULL) != 0 ||
        pthread_create(&id, NULL, timerThread, NULL) != 0 ||
        pthread_create(&id, NULL, clientThread, NULL) != 0)
    {
        reportError(modemName, "NXDN", "NXDN_Service: can't create threads");
        return 1;
    }

    fprintf(stderr, "NXDN_Service: connected to host, %s modem.\n",
            packetType == PACKET_TYPE_SAMP ? "sample" : (packetType == PACKET_TYPE_BIT ? "OpenMT_HS bit" : "MMDVM"));

    uint64_t lastPoll = nowMs();

    while (connected)
    {
        serviceEvents();

        if (nowMs() - lastPoll >= 2000U)
        {
            pollDashboard();
            lastPoll = nowMs();
        }

        delay(50000);
    }

    releaseMode();
    clearReflLinkStatus(modemName, "NXDN");
    fprintf(stderr, "NXDN service terminated.\n");
    logError(modemName, "main", "NXDN host terminated.");

    return 0;
}
