/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 *                                                                         *
 ***************************************************************************/

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
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

#include "fm_dsp.h"

#define VERSION "2026-10-20"
#define BUFFER_SIZE 600

#define AUD_RING 65536U
#define AUD_MASK (AUD_RING - 1U)
#define FM_FRAME_SAMPLES 96U
#define FM_TICK_NS 4000000L
#define FM_TICK_MS 4U
#define FM_DRAIN_TICKS 100U
#define FM_MODE_WAIT_TICKS 250U
#define FM_EVENTS 16U

/* EchoLink gateway link */
#define LINK_RING 32768U
#define LINK_MASK (LINK_RING - 1U)
#define LINK_CHUNK 160U
#define LINK_DELAY 680U
#define LINK_WATCHDOG_TICKS 500U
#define LINK_NAME_LEN 24U
#define GW_FIFO_PACKETS 64U
#define GW_PACKET_MAX 340U
#define COMM_UPDATE_CONF 0x04
#define TYPE_CONNECT "CONN"
#define TYPE_DISCONNECT "DISC"
#define TYPE_NACK "NACK"
#define TYPE_LINK_AUDIO "ELAU"
#define TYPE_LINK_RX "ELRX"
#define TYPE_LINK_PTT "ELPT"
#define TYPE_LINK_DTMF "ELDT"

#ifndef FM_TEST
#define FM_STATIC static
#else
#define FM_STATIC
#endif

char MODE_NAME[11] = "FM";
char MODEM_TYPE[6] = "FM";

const char* TYPE_MODE    = "MODE";
const char* TYPE_COMMAND = "COMM";
const char* TYPE_SAMPLE  = "SAMP";
const char* TYPE_TO_MODEM = "2MOD";

const uint8_t PACKET_TYPE_SAMP  = 1;
const uint8_t PACKET_TYPE_FRAME = 2;

const uint8_t COMM_SET_DUPLEX  = 0x00;
const uint8_t COMM_SET_SIMPLEX = 0x01;
const uint8_t COMM_SET_MODE    = 0x02;
const uint8_t COMM_SET_IDLE    = 0x03;

#define MMDVM_FM_PARAMS1 0x60U
#define MMDVM_FM_PARAMS2 0x61U
#define MMDVM_FM_PARAMS3 0x62U

const uint8_t SETMODE[] = {0x61, 0x00, 0x05, 0x01, 0x02};
const uint8_t SETIDLE[] = {0x61, 0x00, 0x05, 0x01, 0x03};

typedef enum
{
    TXS_IDLE,
    TXS_WAITMODE,
    TXS_KEYUP,
    TXS_ACTIVE,
    TXS_ID,
    TXS_DRAIN,
    TXS_HOLD
} FmTxState;

typedef enum
{
    EV_START,
    EV_END
} FmEventType;

typedef struct
{
    FmEventType type;
    time_t      when;
} FmEvent;

typedef struct
{
    float    rxLevel;
    float    txLevel;
    uint8_t  rfPower;
    char     accessMode[12];
    float    ctcssRx;
    float    ctcssTx;
    float    ctcssThreshold;
    uint32_t ctcssHold;
    float    ctcssTxLevel;
    float    noiseOpen;
    float    noiseClose;
    float    kerchunk;
    float    hangTime;
    float    modeHold;
    uint32_t timeout;
    uint32_t audioDelay;
    uint32_t tailMute;
    uint32_t txDelay;
    bool     idEnable;
    uint32_t idInterval;
    uint8_t  idSpeed;
    float    idFreq;
    float    idLevel;
    uint32_t fwTimeoutLevel;
    uint32_t fwCtcssHigh;
    uint32_t fwCtcssLow;
    int      fwAccessMode;
    uint32_t fwRxAudioBoost;
    uint32_t fwMaxDev;
    uint32_t fwSquelchHigh;
    uint32_t fwSquelchLow;
    char     fwAckText[12];
    uint32_t fwAckSpeed;
    uint32_t fwAckFreq;
    uint32_t fwAckMinTime;
    uint32_t fwAckDelay;
    uint32_t fwAckLevel;
    uint32_t fwCallTime;
    uint32_t fwCallHoldoff;
    uint32_t fwCallHigh;
    uint32_t fwCallLow;
    bool     fwCallAtStart;
    bool     fwCallAtEnd;
    bool     fwCallAtLatch;
    bool     fwCosInvert;
    float    linkRxLevel;
    float    linkTxLevel;
    uint32_t linkPrebuf;
    bool     dtmfEnable;
} FmConfig;

uint8_t  packetType    = 0;
uint8_t  modemId       = 1;
char     modemName[10] = "modem1";
char     station_call[12] = "";
bool     debugM        = false;
bool     connected     = true;
volatile sig_atomic_t running = 1;
bool     modem_duplex  = false;
uint16_t clientPort    = 18000;
char     modemHost[80] = "127.0.0.1";
int      sockfd        = -1;

FmConfig cfg;

pthread_mutex_t fmMutex    = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t writeMutex = PTHREAD_MUTEX_INITIALIZER;

FmCtcssDet ctcss;
FmNoiseSq  noiseSq;
FmAudio    audio;
FmTone     txTone;
FmCw       cw;

float     aud[AUD_RING];
uint64_t  wIdx = 0;
uint64_t  rIdx = 0;

FmTxState txState        = TXS_IDLE;
bool      rxOpen         = false;
bool      audioGate      = false;
bool      lockout        = false;
bool      modeConfirmed  = false;
uint32_t  holdTicks      = 0;
bool      modeCheckReq   = false;
bool      hangActive     = false;
uint32_t  qualSamples    = 0;
uint32_t  waitTicks      = 0;
uint32_t  keyTicks       = 0;
uint32_t  hangTicks      = 0;
uint32_t  drainTicks     = 0;
uint32_t  rxOpenTicks    = 0;
uint64_t  fmTicks        = 0;
time_t    lastIdTime     = 0;
uint64_t  lastRxTick     = 0;
uint64_t  audioEndIdx    = 0;
float     rxGain         = 1.0f;
float     txGain         = 1.0f;
bool      useCtcss       = false;
bool      useNoise       = false;
float     dbgPeak        = 0.0f;
double    dbgSum         = 0.0;
double    dbgDc          = 0.0;
FmBiquad  dbgLp[3];
double    dbgBand[3];
double    dbgInit        = 0.0;

FmEvent   evq[FM_EVENTS];
uint8_t   evHead = 0;
uint8_t   evTail = 0;
time_t    callStart = 0;
char      callSuffix[24] = "";

bool      fwActive       = false;
time_t    fwLastData     = 0;
bool      fwModeOk       = false;
bool      fwParamsDue    = false;

uint32_t  dbgSamples     = 0;

/* EchoLink link: audio and control to and from fm_gateway */
uint16_t  serverPort     = 18700;
volatile bool gwConnected = false;
volatile bool gwLinked    = false;
volatile int  gwFd        = -1;
char      linkName[LINK_NAME_LEN] = "";
pthread_mutex_t gwMutex  = PTHREAD_MUTEX_INITIALIZER;
uint8_t   gwOut[GW_FIFO_PACKETS][GW_PACKET_MAX];
uint16_t  gwOutLen[GW_FIFO_PACKETS];
unsigned int gwHead      = 0;
unsigned int gwTail      = 0;

FmDecim   linkDecim;
FmInterp  linkInterp;
FmDtmf    dtmf;
bool      dtmfBusy       = false;
int16_t   linkOut[LINK_CHUNK];
unsigned int linkOutCnt  = 0;
int16_t   linkDly[LINK_DELAY];
unsigned int linkDlyPos  = 0;
bool      linkRxActive   = false;
float     lnk[LINK_RING];
uint64_t  lwIdx          = 0;
uint64_t  lrIdx          = 0;
bool      linkTx         = false;
bool      linkPend       = false;
bool      linkEnding     = false;
uint64_t  linkLastTick   = 0;

/*
 * error()
 *
 * Reports msg with reportError() and exits the process with status 1.
 *
 * Parameters:
 *   msg - Text printed before the system error message.
 *
 * Returns: nothing.
 */
void error(char* msg)
{
    reportError(modemName, "FM", "%s: %s", msg, strerror(errno));
    exit(1);
}

/*
 * sigHandler()
 *
 * Signal handler for SIGINT and SIGTERM. Asks every loop to finish so the
 * transmitter is released cleanly.
 *
 * Parameters:
 *   sig - The signal number (SIGINT or SIGTERM).
 *
 * Returns: nothing.
 */
FM_STATIC void sigHandler(int sig)
{
    (void)sig;
    running = 0;
}

/*
 * fmClampf()
 *
 * Returns v limited to the range lo to hi.
 *
 * Parameters:
 *   v  - The value.
 *   lo - Lower limit.
 *   hi - Upper limit.
 *
 * Returns: v, or lo or hi if v is outside the range.
 */
static float fmClampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/*
 * fmRoundByte()
 *
 * Converts a float to the nearest byte value, limited to 0 to 255.
 *
 * Parameters:
 *   v - The value.
 *
 * Returns: The nearest byte value, 0 to 255.
 */
static uint8_t fmRoundByte(float v)
{
    return (uint8_t)fmClampf(v + 0.5f, 0.0f, 255.0f);
}

/*
 * fmWriteAll()
 *
 * Writes len bytes to the modem host socket under the write lock. Returns
 * false if the connection failed.
 *
 * Parameters:
 *   data - The bytes to write.
 *   len  - Number of bytes in data.
 *
 * Returns: true if all bytes were written, false if the connection failed.
 */
FM_STATIC bool fmWriteAll(const uint8_t* data, size_t len)
{
    bool ok = true;

    if (sockfd < 0)
        return false;

    pthread_mutex_lock(&writeMutex);
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = write(sockfd, data + sent, len - sent);
        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                delay(500);
                continue;
            }
            ok = false;
            break;
        }
        sent += (size_t)n;
    }
    pthread_mutex_unlock(&writeMutex);

    if (!ok)
    {
        reportError(modemName, "FM", "modem host connection lost.");
        connected = false;
    }
    return ok;
}

/*
 * fmConfigText()
 *
 * Reads the FM setting key into out (size outLen) and, when it is missing,
 * stores def as its default and returns that instead.
 *
 * Parameters:
 *   key    - Name of the FM setting.
 *   def    - Default stored and used when the setting is missing.
 *   out    - Receives the value as a string.
 *   outLen - Size of out, in bytes.
 *
 * Returns: nothing.
 */
static void fmConfigText(const char* key, const char* def, char* out, size_t outLen)
{
    char tmp[64] = "";

    readHostConfig(modemName, "FM", key, tmp);
    if (strlen(tmp) == 0)
    {
        setHostConfig(modemName, "FM", key, "input", def);
        snprintf(out, outLen, "%s", def);
    }
    else
        snprintf(out, outLen, "%s", tmp);
}

/*
 * fmConfigFloat()
 *
 * Reads the numeric FM setting key, creating it with def when missing.
 *
 * Parameters:
 *   key - Name of the FM setting.
 *   def - Default, as text, stored and used when the setting is missing.
 *
 * Returns: The value as a number.
 */
static float fmConfigFloat(const char* key, const char* def)
{
    char tmp[32];

    fmConfigText(key, def, tmp, sizeof(tmp));
    return (float)atof(tmp);
}

/*
 * fmCreateDefaults()
 *
 * Stores the default FM settings in the database on the first run so they
 * appear in the dashboard Mode tab. Settings for the sample based modems and
 * for the MMDVM firmware path are only created for the modem in use.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
FM_STATIC void fmCreateDefaults(void)
{
    char tmp[16];

    readHostConfig(modemName, "FM", "txLevel", tmp);
    if (strlen(tmp) != 0)
        return;

    setHostConfig(modemName, "FM", "rxLevel", "input", "50");
    setHostConfig(modemName, "FM", "txLevel", "input", "50");
    setHostConfig(modemName, "FM", "rfPower", "input", "128");
    setHostConfig(modemName, "FM", "accessMode", "input", "ctcss");
    setHostConfig(modemName, "FM", "ctcssRx", "input", "88.5");
    setHostConfig(modemName, "FM", "ctcssTx", "input", "88.5");
    setHostConfig(modemName, "FM", "ctcssThreshold", "input", "15");
    setHostConfig(modemName, "FM", "ctcssTxLevel", "input", packetType == PACKET_TYPE_SAMP ? "3" : "15");
    setHostConfig(modemName, "FM", "kerchunk", "input", "0");
    setHostConfig(modemName, "FM", "hangTime", "input", "2");
    setHostConfig(modemName, "FM", "modeHold", "input", "30");
    setHostConfig(modemName, "FM", "timeout", "input", "180");
    setHostConfig(modemName, "FM", "idEnable", "input", "1");
    setHostConfig(modemName, "FM", "idInterval", "input", "10");
    setHostConfig(modemName, "FM", "idSpeed", "input", "20");
    setHostConfig(modemName, "FM", "idFreq", "input", "1000");
    setHostConfig(modemName, "FM", "idLevel", "input", packetType == PACKET_TYPE_SAMP ? "6" : "25");

    if (packetType == PACKET_TYPE_SAMP)
    {
        setHostConfig(modemName, "FM", "linkRxLevel", "input", "50");
        setHostConfig(modemName, "FM", "linkTxLevel", "input", "50");
        setHostConfig(modemName, "FM", "linkPrebuf", "input", "120");
        setHostConfig(modemName, "FM", "dtmfEnable", "input", "1");
        setHostConfig(modemName, "FM", "ctcssHold", "input", "400");
        setHostConfig(modemName, "FM", "noiseOpen", "input", "8");
        setHostConfig(modemName, "FM", "noiseClose", "input", "14");
        setHostConfig(modemName, "FM", "audioDelay", "input", "180");
        setHostConfig(modemName, "FM", "tailMute", "input", "120");
        setHostConfig(modemName, "FM", "txDelay", "input", "100");
    }
    else
    {
        setHostConfig(modemName, "FM", "fwTimeoutLevel", "input", "80");
        setHostConfig(modemName, "FM", "fwCtcssHigh", "input", "30");
        setHostConfig(modemName, "FM", "fwCtcssLow", "input", "20");
        setHostConfig(modemName, "FM", "fwAccessMode", "input", "-1");
        setHostConfig(modemName, "FM", "fwRxAudioBoost", "input", "1");
        setHostConfig(modemName, "FM", "fwMaxDev", "input", "90");
        setHostConfig(modemName, "FM", "fwSquelchHigh", "input", "30");
        setHostConfig(modemName, "FM", "fwSquelchLow", "input", "20");
        setHostConfig(modemName, "FM", "fwAckText", "input", "K");
        setHostConfig(modemName, "FM", "fwAckSpeed", "input", "20");
        setHostConfig(modemName, "FM", "fwAckFreq", "input", "1750");
        setHostConfig(modemName, "FM", "fwAckMinTime", "input", "4");
        setHostConfig(modemName, "FM", "fwAckDelay", "input", "1000");
        setHostConfig(modemName, "FM", "fwAckLevel", "input", "50");
        setHostConfig(modemName, "FM", "fwCallTime", "input", "10");
        setHostConfig(modemName, "FM", "fwCallHoldoff", "input", "0");
        setHostConfig(modemName, "FM", "fwCallHigh", "input", "50");
        setHostConfig(modemName, "FM", "fwCallLow", "input", "20");
        setHostConfig(modemName, "FM", "fwCallAtStart", "input", "1");
        setHostConfig(modemName, "FM", "fwCallAtEnd", "input", "1");
        setHostConfig(modemName, "FM", "fwCallAtLatch", "input", "0");
        setHostConfig(modemName, "FM", "fwCosInvert", "input", "0");
    }
}

/*
 * fmLoadConfig()
 *
 * Reads every FM setting from the database into cfg, filling in defaults for
 * any that are missing, then applies them: gains, detector settings, tone
 * generator and ID timing. Called at start up and when the dashboard saves
 * the FM section. The CTCSS detector and noise squelch are only restarted
 * when their settings changed, so a call in progress is not disturbed.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
FM_STATIC void fmLoadConfig(void)
{
    FmConfig n;

    memset(&n, 0, sizeof(n));

    n.rxLevel = fmClampf(fmConfigFloat("rxLevel", "50"), 1.0f, 100.0f);
    n.txLevel = fmClampf(fmConfigFloat("txLevel", "50"), 0.0f, packetType == PACKET_TYPE_SAMP ? 250.0f : 100.0f);
    n.rfPower = (uint8_t)fmClampf(fmConfigFloat("rfPower", "128"), 0.0f, 255.0f);
    fmConfigText("accessMode", "ctcss", n.accessMode, sizeof(n.accessMode));
    n.ctcssRx        = fmClampf(fmConfigFloat("ctcssRx", "88.5"), 0.0f, 300.0f);
    n.ctcssTx        = fmClampf(fmConfigFloat("ctcssTx", "88.5"), 0.0f, 300.0f);
    n.ctcssThreshold = fmClampf(fmConfigFloat("ctcssThreshold", "15"), 1.0f, 95.0f);
    n.ctcssTxLevel   = fmClampf(fmConfigFloat("ctcssTxLevel", packetType == PACKET_TYPE_SAMP ? "3" : "15"), 0.0f, 50.0f);
    n.kerchunk       = fmClampf(fmConfigFloat("kerchunk", "0"), 0.0f, 10.0f);
    n.hangTime       = fmClampf(fmConfigFloat("hangTime", "2"), 0.0f, 30.0f);
    n.modeHold       = fmClampf(fmConfigFloat("modeHold", "30"), 0.0f, 600.0f);
    n.timeout        = (uint32_t)fmClampf(fmConfigFloat("timeout", "180"), 0.0f, 3600.0f);
    n.idEnable       = fmConfigFloat("idEnable", "1") != 0.0f;
    n.idInterval     = (uint32_t)fmClampf(fmConfigFloat("idInterval", "10"), 1.0f, 60.0f);
    n.idSpeed        = (uint8_t)fmClampf(fmConfigFloat("idSpeed", "20"), 5.0f, 40.0f);
    n.idFreq         = fmClampf(fmConfigFloat("idFreq", "1000"), 300.0f, 3000.0f);
    n.idLevel        = fmClampf(fmConfigFloat("idLevel", packetType == PACKET_TYPE_SAMP ? "6" : "25"), 0.0f, 60.0f);

    if (packetType == PACKET_TYPE_SAMP)
    {
        n.ctcssHold  = (uint32_t)fmClampf(fmConfigFloat("ctcssHold", "400"), 100.0f, 5000.0f);
        n.noiseOpen  = fmClampf(fmConfigFloat("noiseOpen", "8"), 0.0f, 100.0f);
        n.noiseClose = fmClampf(fmConfigFloat("noiseClose", "14"), 0.0f, 100.0f);
        n.audioDelay = (uint32_t)fmClampf(fmConfigFloat("audioDelay", "180"), 0.0f, 1500.0f);
        n.tailMute   = (uint32_t)fmClampf(fmConfigFloat("tailMute", "120"), 0.0f, 1000.0f);
        n.txDelay    = (uint32_t)fmClampf(fmConfigFloat("txDelay", "100"), 0.0f, 1000.0f);
        if (n.tailMute > n.audioDelay)
            n.tailMute = n.audioDelay;
        n.linkRxLevel = fmClampf(fmConfigFloat("linkRxLevel", "50"), 0.0f, 200.0f);
        n.linkTxLevel = fmClampf(fmConfigFloat("linkTxLevel", "50"), 0.0f, 200.0f);
        n.linkPrebuf  = (uint32_t)fmClampf(fmConfigFloat("linkPrebuf", "120"), 0.0f, 600.0f);
        n.dtmfEnable  = fmConfigFloat("dtmfEnable", "1") != 0.0f;
    }
    else
    {
        n.fwTimeoutLevel = (uint32_t)fmClampf(fmConfigFloat("fwTimeoutLevel", "80"), 0.0f, 100.0f);
        n.fwCtcssHigh    = (uint32_t)fmClampf(fmConfigFloat("fwCtcssHigh", "30"), 0.0f, 255.0f);
        n.fwCtcssLow     = (uint32_t)fmClampf(fmConfigFloat("fwCtcssLow", "20"), 0.0f, 255.0f);
        n.fwAccessMode   = (int)fmConfigFloat("fwAccessMode", "-1");
        n.fwRxAudioBoost = (uint32_t)fmClampf(fmConfigFloat("fwRxAudioBoost", "1"), 0.0f, 255.0f);
        n.fwMaxDev       = (uint32_t)fmClampf(fmConfigFloat("fwMaxDev", "90"), 0.0f, 100.0f);
        n.fwSquelchHigh  = (uint32_t)fmClampf(fmConfigFloat("fwSquelchHigh", "30"), 0.0f, 255.0f);
        n.fwSquelchLow   = (uint32_t)fmClampf(fmConfigFloat("fwSquelchLow", "20"), 0.0f, 255.0f);
        fmConfigText("fwAckText", "K", n.fwAckText, sizeof(n.fwAckText));
        n.fwAckSpeed     = (uint32_t)fmClampf(fmConfigFloat("fwAckSpeed", "20"), 5.0f, 40.0f);
        n.fwAckFreq      = (uint32_t)fmClampf(fmConfigFloat("fwAckFreq", "1750"), 300.0f, 2550.0f);
        n.fwAckMinTime   = (uint32_t)fmClampf(fmConfigFloat("fwAckMinTime", "4"), 0.0f, 255.0f);
        n.fwAckDelay     = (uint32_t)fmClampf(fmConfigFloat("fwAckDelay", "1000"), 0.0f, 2550.0f);
        n.fwAckLevel     = (uint32_t)fmClampf(fmConfigFloat("fwAckLevel", "50"), 0.0f, 100.0f);
        n.fwCallTime     = (uint32_t)fmClampf(fmConfigFloat("fwCallTime", "10"), 0.0f, 255.0f);
        n.fwCallHoldoff  = (uint32_t)fmClampf(fmConfigFloat("fwCallHoldoff", "0"), 0.0f, 255.0f);
        n.fwCallHigh     = (uint32_t)fmClampf(fmConfigFloat("fwCallHigh", "50"), 0.0f, 100.0f);
        n.fwCallLow      = (uint32_t)fmClampf(fmConfigFloat("fwCallLow", "20"), 0.0f, 100.0f);
        n.fwCallAtStart  = fmConfigFloat("fwCallAtStart", "1") != 0.0f;
        n.fwCallAtEnd    = fmConfigFloat("fwCallAtEnd", "1") != 0.0f;
        n.fwCallAtLatch  = fmConfigFloat("fwCallAtLatch", "0") != 0.0f;
        n.fwCosInvert    = fmConfigFloat("fwCosInvert", "0") != 0.0f;
    }

    readHostConfig(modemName, "main", "callsign", station_call);

    pthread_mutex_lock(&fmMutex);

    bool ctcssChanged = (n.ctcssRx != cfg.ctcssRx) || (n.ctcssThreshold != cfg.ctcssThreshold) ||
                        (n.ctcssHold != cfg.ctcssHold);
    bool noiseChanged = (n.noiseOpen != cfg.noiseOpen) || (n.noiseClose != cfg.noiseClose);
    bool toneChanged  = (n.ctcssTx != cfg.ctcssTx);

    cfg = n;

    rxGain = cfg.rxLevel / 50.0f;
    txGain = cfg.txLevel / 50.0f;

    useCtcss = cfg.ctcssRx > 0.0f && strcasecmp(cfg.accessMode, "carrier") != 0;
    useNoise = strcasecmp(cfg.accessMode, "ctcss") != 0 || cfg.ctcssRx <= 0.0f;

    if (ctcssChanged)
        fmCtcssInit(&ctcss, cfg.ctcssRx, cfg.ctcssThreshold, cfg.ctcssHold);
    if (noiseChanged)
        fmNoiseInit(&noiseSq, cfg.noiseOpen, cfg.noiseClose);
    if (toneChanged)
        fmToneInit(&txTone, cfg.ctcssTx);

    pthread_mutex_unlock(&fmMutex);

    if (debugM)
        fprintf(stderr, "FM config: access %s  rx tone %.1f  tx tone %.1f  hang %.1fs  timeout %us\n",
                cfg.accessMode, cfg.ctcssRx, cfg.ctcssTx, cfg.hangTime, cfg.timeout);
}

/*
 * fmPostEvent()
 *
 * Queues a call start or end event for the service loop, which writes it to
 * the database. The audio threads never wait on the database.
 *
 * Parameters:
 *   type - The event, EV_START or EV_END.
 *
 * Returns: nothing.
 */
FM_STATIC void fmPostEvent(FmEventType type)
{
    uint8_t next = (uint8_t)((evHead + 1U) % FM_EVENTS);

    if (next == evTail)
        return;
    evq[evHead].type = type;
    evq[evHead].when = time(NULL);
    evHead           = next;
}

/*
 * fmToneText()
 *
 * Builds the text that identifies how the current call was accessed, used as
 * the suffix in the dashboard call record.
 *
 * Parameters:
 *   out - Receives the text.
 *   len - Size of out, in bytes.
 *
 * Returns: nothing.
 */
static void fmToneText(char* out, size_t len)
{
    if (strcasecmp(cfg.accessMode, "carrier") == 0 || cfg.ctcssRx <= 0.0f)
        snprintf(out, len, "Carrier");
    else
        snprintf(out, len, "CTCSS %.1f", cfg.ctcssRx);
}

/*
 * fmRxAccess()
 *
 * Combines the CTCSS and noise squelch results into the access decision for
 * the configured access mode.
 *
 * Parameters:
 *   ct - true if the CTCSS tone was detected.
 *   nq - true if the noise squelch is open.
 *
 * Returns: true if the signal has the access that is configured: the tone,
 *   the noise squelch or both (carrier access uses the noise squelch only).
 */
static bool fmRxAccess(bool ct, bool nq)
{
    if (!useCtcss)
        return nq;
    if (!useNoise)
        return ct;
    return ct && nq;
}

/*
 * gwQueue()
 *
 * Puts one packet in the queue for the gateway thread, which writes it to the
 * gateway. The packet is dropped if the queue is full. Safe to call with
 * fmMutex held; it only takes gwMutex.
 *
 * Parameters:
 *   data   - The packet.
 *   length - Number of bytes in the packet (at most GW_PACKET_MAX).
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
        gwOutLen[gwHead] = (uint16_t)length;
        gwHead           = next;
    }

    pthread_mutex_unlock(&gwMutex);
}

/*
 * fmLinkSend()
 *
 * Wraps a payload in a four character packet type and queues it for the
 * gateway. Does nothing when no gateway is connected.
 *
 * Parameters:
 *   type    - Four character packet type.
 *   payload - The bytes after the type (may be NULL when n is 0).
 *   n       - Number of bytes in payload.
 *
 * Returns: nothing.
 */
static void fmLinkSend(const char* type, const uint8_t* payload, unsigned int n)
{
    uint8_t pkt[GW_PACKET_MAX];

    if (!gwConnected || n + 8U > sizeof(pkt))
        return;

    pkt[0] = 0x61;
    pkt[1] = (uint8_t)((8U + n) >> 8);
    pkt[2] = (uint8_t)((8U + n) & 0xff);
    pkt[3] = 4;
    memcpy(pkt + 4, type, 4);
    if (n > 0)
        memcpy(pkt + 8, payload, n);
    gwQueue(pkt, 8U + n);
}

/*
 * fmLinkFlushRx()
 *
 * Sends the received audio collected so far to the gateway as one ELAU
 * packet of 8 kSa/s samples. The caller holds fmMutex.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void fmLinkFlushRx(void)
{
    uint8_t buf[LINK_CHUNK * 2U];

    if (linkOutCnt == 0)
        return;
    for (unsigned int i = 0; i < linkOutCnt; i++)
    {
        buf[i * 2U]      = (uint8_t)(linkOut[i] & 0xff);
        buf[i * 2U + 1U] = (uint8_t)((linkOut[i] >> 8) & 0xff);
    }
    fmLinkSend(TYPE_LINK_AUDIO, buf, linkOutCnt * 2U);
    linkOutCnt = 0;
}

/*
 * fmLinkEmit()
 *
 * Adds one sample to the packet of received audio for the gateway and sends
 * the packet when it is full. The caller holds fmMutex.
 *
 * Parameters:
 *   v - The sample.
 *
 * Returns: nothing.
 */
static void fmLinkEmit(int16_t v)
{
    linkOut[linkOutCnt++] = v;
    if (linkOutCnt >= LINK_CHUNK)
        fmLinkFlushRx();
}

/*
 * fmLinkRx()
 *
 * Handles one 8 kSa/s sample of the received audio for the EchoLink link:
 * runs the DTMF decoder, mutes the tones, and passes the audio to the
 * gateway while the squelch is open and a station is linked. The audio goes
 * through a delay of LINK_DELAY samples (85 ms), longer than the decoder needs
 * to recognise a digit, so the start of the tones can still be muted. A
 * squelch edge sends an ELRX packet so the gateway knows where a transmission
 * starts and ends. The caller holds fmMutex.
 *
 * Parameters:
 *   d8   - The 8 kSa/s sample.
 *   gate - true if the receiver audio gate is open.
 *
 * Returns: nothing.
 */
static void fmLinkRx(float d8, bool gate)
{
    bool send = gwLinked && gate;

    if (cfg.dtmfEnable && rxOpen)
    {
        char dg = fmDtmfPush(&dtmf, d8);

        dtmfBusy = true;
        if (dg != 0)
        {
            uint64_t avail = wIdx - rIdx;
            uint64_t back  = 3U * FM_DTMF_N * FM_LINK_DECIM;

            if (back > avail)
                back = avail;
            for (uint64_t k = 1; k <= back; k++)
                aud[(wIdx - k) & AUD_MASK] = 0.0f;
            memset(linkDly, 0, sizeof(linkDly));
            fmLinkSend(TYPE_LINK_DTMF, (const uint8_t*)&dg, 1);
        }
    }
    else if (dtmfBusy)
    {
        fmDtmfInit(&dtmf);
        dtmfBusy = false;
    }

    if (send && !linkRxActive)
    {
        uint8_t on = 1;

        linkRxActive = true;
        linkOutCnt   = 0;
        linkDlyPos   = 0;
        memset(linkDly, 0, sizeof(linkDly));
        fmLinkSend(TYPE_LINK_RX, &on, 1);
    }
    else if (!send && linkRxActive)
    {
        uint8_t off = 0;

        for (unsigned int k = 0; k < LINK_DELAY; k++)
            fmLinkEmit(linkDly[(linkDlyPos + k) % LINK_DELAY]);
        fmLinkFlushRx();
        linkRxActive = false;
        fmLinkSend(TYPE_LINK_RX, &off, 1);
    }

    if (linkRxActive)
    {
        float   v   = dtmf.active ? 0.0f : d8 * (cfg.linkRxLevel / 50.0f);
        int16_t old = linkDly[linkDlyPos];

        linkDly[linkDlyPos] = (int16_t)lrintf(fmClampf(v, -32000.0f, 32000.0f));
        linkDlyPos          = (linkDlyPos + 1U) % LINK_DELAY;
        fmLinkEmit(old);
    }
}

/*
 * fmRxSamples()
 *
 * Processes a block of discriminator samples from the modem host (24 kSa/s).
 * Each sample goes through the DC blocker, the CTCSS detector and the noise
 * squelch to decide whether a signal with the right access is present, and
 * through the voice band filter into the delay ring that feeds the
 * transmitter. When the tone drops, the most recent audio is muted so the
 * squelch tail is not repeated.
 *
 * Parameters:
 *   samples - The discriminator samples.
 *   count   - Number of samples.
 *
 * Returns: nothing.
 */
FM_STATIC void fmRxSamples(const int16_t* samples, unsigned int count)
{
    pthread_mutex_lock(&fmMutex);

    lastRxTick = fmTicks;
    uint32_t kerchunkSamples = (uint32_t)(cfg.kerchunk * (float)FM_FS);

    for (unsigned int i = 0; i < count; i++)
    {
        float x  = (float)samples[i] * rxGain;
        float xd = fmAudioDc(&audio, x);
        bool  ct = useCtcss ? fmCtcssProcess(&ctcss, xd) : false;
        bool  nq = fmNoiseProcess(&noiseSq, xd);
        if (!(useNoise || !useCtcss))
            nq = false;

        if (fabsf(x) > dbgPeak)
            dbgPeak = fabsf(x);
        dbgSum += (double)x * (double)x;
        dbgDc  += (double)x;
        if (dbgInit == 0.0)
        {
            memset(dbgLp, 0, sizeof(dbgLp));
            fmBiquadLowPass(&dbgLp[0], 60.0f, 0.7071f);
            fmBiquadLowPass(&dbgLp[1], 300.0f, 0.7071f);
            fmBiquadLowPass(&dbgLp[2], 3000.0f, 0.7071f);
            dbgInit = 1.0;
        }
        for (int b = 0; b < 3; b++)
        {
            float v = fmBiquadRun(&dbgLp[b], x);

            dbgBand[b] += (double)v * (double)v;
        }
        float sh = fmAudioShape(&audio, xd);
        float y  = sh * txGain;

        bool access = fmRxAccess(ct, nq);

        if (access)
        {
            if (qualSamples < 0xffffffffU)
                qualSamples++;
        }
        else
            qualSamples = 0;

        bool wasOpen = rxOpen;
        if (!rxOpen && access && qualSamples >= kerchunkSamples)
            rxOpen = true;
        else if (rxOpen && !access)
            rxOpen = false;

        bool gate = rxOpen && (useCtcss ? ctcss.weak : true);

        if (gwConnected)
        {
            float d8;

            if (fmDecimPush(&linkDecim, sh, &d8))
                fmLinkRx(d8, gate);
            if (dtmf.active && rxOpen)
                y = 0.0f;
        }

        if (audioGate && !gate)
        {
            uint64_t avail = wIdx - rIdx;
            uint64_t mute  = (uint64_t)cfg.tailMute * FM_FS / 1000U;

            if (txState == TXS_IDLE)
                mute = 0;
            if (mute > avail)
                mute = avail;
            for (uint64_t k = 1; k <= mute; k++)
                aud[(wIdx - k) & AUD_MASK] = 0.0f;
        }
        audioGate = gate;

        if (!wasOpen && rxOpen)
        {
            uint64_t back = (uint64_t)cfg.audioDelay * FM_FS / 1000U;
            if (!useCtcss && back > (uint64_t)(FM_FS / 16U))
                back = FM_FS / 16U;
            rIdx          = wIdx > back ? wIdx - back : 0;
            rxOpenTicks   = 0;
        }

        if (txState != TXS_IDLE && !gate)
            aud[wIdx & AUD_MASK] = 0.0f;
        else
            aud[wIdx & AUD_MASK] = y;
        wIdx++;
        if (gate)
            audioEndIdx = wIdx;
    }

    dbgSamples += count;
    if (debugM && dbgSamples >= FM_FS)
    {
        double nS = (double)dbgSamples;
        dbgSamples = 0;
        float bestF, bestFrac;

        fmCtcssScan(&ctcss, &bestF, &bestFrac);
        fprintf(stderr, "FM rx: in peak %.0f rms %.0f dc %.0f  noise %.1f%%  tone frac %.2f ratio %.1f  best %.1f Hz (%.2f)  pwr<60 %.0f%% <300 %.0f%% <3k %.0f%%  det %d  open %d  gate %d  state %d\n",
                dbgPeak, sqrt(dbgSum / nS), dbgDc / nS, noiseSq.level,
                ctcss.lastFraction, ctcss.lastRatio, bestF, bestFrac, 100.0 * dbgBand[0] / (dbgSum + 1e-9), 100.0 * dbgBand[1] / (dbgSum + 1e-9), 100.0 * dbgBand[2] / (dbgSum + 1e-9), ctcss.detected, rxOpen, audioGate, txState);
        dbgPeak = 0.0f;
        dbgSum  = 0.0;
        dbgDc   = 0.0;
        dbgBand[0] = dbgBand[1] = dbgBand[2] = 0.0;
    }

    pthread_mutex_unlock(&fmMutex);
}

/*
 * fmBuildFrame()
 *
 * Fills out with one transmit frame of audio, 96 samples. The audio comes
 * from the delay ring when withAudio is set; the CTCSS tone is added when
 * one is configured, and the CW ID is mixed in while it is running. The
 * caller holds fmMutex.
 *
 * Parameters:
 *   out       - Receives FM_FRAME_SAMPLES samples.
 *   withAudio - true to take the audio from the delay ring, false to make
 *               only the tone and the ID.
 *
 * Returns: nothing.
 */
static void fmBuildFrame(int16_t* out, bool withAudio)
{
    float toneAmp = cfg.ctcssTxLevel * 327.67f;
    float cwAmp   = cfg.idLevel * 327.67f;
    float linkGain = txGain * cfg.linkTxLevel / 50.0f;

    for (unsigned int i = 0; i < FM_FRAME_SAMPLES; i++)
    {
        float a = 0.0f;

        if (withAudio && rIdx < wIdx)
        {
            a = aud[rIdx & AUD_MASK];
            rIdx++;
        }

        if (withAudio && linkTx && lrIdx < lwIdx)
        {
            a += lnk[lrIdx & LINK_MASK] * linkGain;
            lrIdx++;
        }

        float c = 0.0f;
        if (cw.active)
        {
            fmCwNext(&cw, &c);
            a *= 0.3f;
        }

        float v = a + c * cwAmp + fmToneNext(&txTone) * toneAmp;
        v       = fmClampf(v, -32000.0f, 32000.0f);
        out[i]  = (int16_t)lrintf(v);
    }
}

/*
 * fmSendSamples()
 *
 * Sends one 96 sample frame to the modem host. With eot set the frame is the
 * end of transmission marker instead of audio.
 *
 * Parameters:
 *   samples - The FM_FRAME_SAMPLES samples (not used when eot is set).
 *   eot     - true to send the end of transmission marker instead of audio.
 *
 * Returns: true if the frame was sent, false if the connection failed.
 */
FM_STATIC bool fmSendSamples(const int16_t* samples, bool eot)
{
    uint8_t bytes[200] = {0x61, 0x00, 0xc8, 0x04, 'S', 'A', 'M', 'P'};

    if (eot)
    {
        bytes[5] = 'E';
        bytes[6] = 'O';
        bytes[7] = 'T';
        memset(bytes + 8, 0, 192);
    }
    else
    {
        for (unsigned int i = 0; i < FM_FRAME_SAMPLES; i++)
        {
            bytes[8 + i * 2]     = (uint8_t)(samples[i] & 0xff);
            bytes[8 + i * 2 + 1] = (uint8_t)((samples[i] >> 8) & 0xff);
        }
    }
    return fmWriteAll(bytes, 200);
}

/*
 * fmIdDue()
 *
 * Returns true when the CW ID is enabled and the ID interval has passed
 * since the last one. The caller holds fmMutex.
 *
 * Parameters: none.
 *
 * Returns: true if the ID is due.
 */
static bool fmIdDue(void)
{
    if (!cfg.idEnable || strlen(station_call) == 0)
        return false;
    return (time(NULL) - lastIdTime) >= (time_t)(cfg.idInterval * 60U);
}

/*
 * fmStartId()
 *
 * Starts sending the station call as CW. The caller holds fmMutex.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void fmStartId(void)
{
    fmCwStart(&cw, station_call, cfg.idSpeed, cfg.idFreq);
    lastIdTime = time(NULL);
}

/*
 * fmTxTick()
 *
 * Advances the transmitter by one 4 ms frame. It runs the state machine that
 * takes the modem when a signal opens, waits for the modem host to confirm
 * the mode, keys up, repeats the delayed audio with the CTCSS tone and the
 * CW ID, holds the carrier for the hang time, then ends the transmission and
 * releases the modem. Network writes happen after the lock is released.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
FM_STATIC void fmTxTick(void)
{
    int16_t out[FM_FRAME_SAMPLES];
    bool    sendFrame   = false;
    bool    sendEot     = false;
    bool    sendSetMode = false;
    bool    sendIdle    = false;

    memset(out, 0, sizeof(out));

    pthread_mutex_lock(&fmMutex);
    fmTicks++;

    if (fmTicks - lastRxTick > 25U)
    {
        rxOpen    = false;
        audioGate = false;
    }

    if (linkTx && ((linkEnding && lrIdx >= lwIdx) || fmTicks - linkLastTick > LINK_WATCHDOG_TICKS))
        linkTx = false;

    if (!rxOpen && !linkTx && lockout && (txState == TXS_IDLE || txState == TXS_HOLD))
        lockout = false;

    switch (txState)
    {
        case TXS_IDLE:
            if ((rxOpen || linkTx) && !lockout)
            {
                txState       = TXS_WAITMODE;
                waitTicks     = 0;
                modeConfirmed = false;
                modeCheckReq  = true;
                sendSetMode   = true;
            }
            break;

        case TXS_WAITMODE:
            waitTicks++;
            if (modeConfirmed)
            {
                txState      = TXS_KEYUP;
                keyTicks     = cfg.txDelay / FM_TICK_MS;
                hangActive   = false;
                rxOpenTicks  = 0;
                fmPostEvent(EV_START);
                if (fmIdDue())
                    fmStartId();
            }
            else if (waitTicks > FM_MODE_WAIT_TICKS)
            {
                txState      = TXS_IDLE;
                modeCheckReq = false;
                lockout      = true;
                fprintf(stderr, "FM: modem not available (another mode is active), not transmitting.\n");
            }
            break;

        case TXS_KEYUP:
            fmBuildFrame(out, false);
            sendFrame = true;
            if (keyTicks > 0)
                keyTicks--;
            if (keyTicks == 0)
                txState = TXS_ACTIVE;
            break;

        case TXS_ACTIVE:
        {
            bool drained = !audioGate && rIdx >= audioEndIdx && !linkTx;
            uint64_t maxLag = (uint64_t)(cfg.audioDelay + cfg.txDelay + 120U) * FM_FS / 1000U;

            if (rxOpen || linkTx)
            {
                if (hangActive)
                {
                    hangActive = false;
                    if (rxOpen)
                    {
                        uint64_t back = (uint64_t)cfg.audioDelay * FM_FS / 1000U;
                        rIdx = wIdx > back ? wIdx - back : 0;
                    }
                }
                rxOpenTicks++;
                if (!lockout && cfg.timeout > 0 && rxOpenTicks * FM_TICK_MS > cfg.timeout * 1000U)
                {
                    lockout = true;
                    fprintf(stderr, "FM: transmit timeout after %us, dropping the repeater.\n", cfg.timeout);
                }
                if (rxOpen && wIdx - rIdx > maxLag + (uint64_t)FM_FRAME_SAMPLES * 4U)
                    rIdx = wIdx - maxLag;
            }
            else if (drained)
            {
                if (!hangActive)
                {
                    hangActive = true;
                    hangTicks  = (uint32_t)(cfg.hangTime * 1000.0f) / FM_TICK_MS;
                }
                else if (hangTicks > 0)
                    hangTicks--;
            }

            if (lockout)
            {
                if (fmIdDue())
                {
                    fmStartId();
                    txState = TXS_ID;
                }
                else
                {
                    txState    = TXS_DRAIN;
                    sendEot    = true;
                    drainTicks = FM_DRAIN_TICKS;
                }
                break;
            }

            if (hangActive && hangTicks == 0)
            {
                if (fmIdDue())
                {
                    fmStartId();
                    txState = TXS_ID;
                }
                else
                {
                    txState    = TXS_DRAIN;
                    sendEot    = true;
                    drainTicks = FM_DRAIN_TICKS;
                }
                break;
            }

            if (!cw.active && fmIdDue())
                fmStartId();

            fmBuildFrame(out, !lockout);
            sendFrame = true;
        }
        break;

        case TXS_ID:
            fmBuildFrame(out, false);
            sendFrame = true;
            if (!cw.active)
            {
                txState    = TXS_DRAIN;
                sendEot    = true;
                drainTicks = FM_DRAIN_TICKS;
            }
            break;

        case TXS_DRAIN:
            if (drainTicks > 0)
                drainTicks--;
            if (drainTicks == 0)
            {
                hangActive = false;
                fmPostEvent(EV_END);
                if (cfg.modeHold > 0.0f)
                {
                    txState   = TXS_HOLD;
                    holdTicks = (uint32_t)(cfg.modeHold * 1000.0f) / FM_TICK_MS;
                }
                else
                {
                    txState  = TXS_IDLE;
                    sendIdle = true;
                }
            }
            break;

        case TXS_HOLD:
            if ((rxOpen || linkTx) && !lockout)
            {
                txState      = TXS_KEYUP;
                keyTicks     = cfg.txDelay / FM_TICK_MS;
                hangActive   = false;
                rxOpenTicks  = 0;
                fmPostEvent(EV_START);
                if (fmIdDue())
                    fmStartId();
            }
            else if (holdTicks > 0)
                holdTicks--;
            else
            {
                txState  = TXS_IDLE;
                sendIdle = true;
            }
            break;
    }

    pthread_mutex_unlock(&fmMutex);

    if (sendSetMode)
        fmWriteAll(SETMODE, 5);
    if (sendFrame)
        fmSendSamples(out, false);
    if (sendEot)
        fmSendSamples(out, true);
    if (sendIdle)
        fmWriteAll(SETIDLE, 5);
}

/*
 * fmTxThread()
 *
 * Thread that calls fmTxTick() every 4 ms, the duration of one 96 sample
 * frame, from an absolute clock so the transmit stream keeps real time.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL when the thread ends.
 */
FM_STATIC void* fmTxThread(void* arg)
{
    struct timespec next;

    (void)arg;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (running && connected)
    {
        next.tv_nsec += FM_TICK_NS;
        if (next.tv_nsec >= 1000000000L)
        {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long late = (now.tv_sec - next.tv_sec) * 1000L + (now.tv_nsec - next.tv_nsec) / 1000000L;
        if (late > 100L)
            next = now;
        else
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        fmTxTick();
    }
    return NULL;
}

/*
 * fmSendToModem()
 *
 * Wraps an MMDVM serial command (cmd byte plus payload) in a modem host
 * "2MOD" packet. The modem host forwards it to the MMDVM modem as a serial
 * frame.
 *
 * Parameters:
 *   cmd     - The MMDVM serial command byte.
 *   payload - The bytes after the command (may be NULL when n is 0).
 *   n       - Number of bytes in payload.
 *
 * Returns: true if the packet was written, false if it does not fit or the
 *   connection failed.
 */
FM_STATIC bool fmSendToModem(uint8_t cmd, const uint8_t* payload, unsigned int n)
{
    uint8_t  buf[128];
    uint16_t len = (uint16_t)(9U + n);

    if (len > sizeof(buf))
        return false;

    buf[0] = 0x61;
    buf[1] = (uint8_t)(len >> 8);
    buf[2] = (uint8_t)(len & 0xff);
    buf[3] = 0x04;
    memcpy(buf + 4, TYPE_TO_MODEM, 4);
    buf[8] = cmd;
    if (n > 0)
        memcpy(buf + 9, payload, n);
    return fmWriteAll(buf, len);
}

/*
 * fmSendFirmwareParams()
 *
 * Sends the FM settings to the MMDVM firmware: the callsign ID, the
 * acknowledgement tone and the miscellaneous parameters (timeout, CTCSS,
 * kerchunk and hang times, access mode, levels, squelch). Used with the
 * plain MMDVM modem, whose firmware does the CTCSS and repeater work itself.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
FM_STATIC void fmSendFirmwareParams(void)
{
    FmConfig c;
    char     call[12];
    uint8_t  p[64];
    unsigned int n;

    pthread_mutex_lock(&fmMutex);
    c = cfg;
    snprintf(call, sizeof(call), "%s", station_call);
    pthread_mutex_unlock(&fmMutex);

    n       = 0;
    p[n++]  = (uint8_t)c.idSpeed;
    p[n++]  = fmRoundByte((float)c.idFreq / 10.0f);
    p[n++]  = (uint8_t)c.fwCallTime;
    p[n++]  = (uint8_t)c.fwCallHoldoff;
    p[n++]  = fmRoundByte((float)c.fwCallHigh * 2.55f);
    p[n++]  = fmRoundByte((float)c.fwCallLow * 2.55f);
    p[n++]  = (uint8_t)((c.fwCallAtStart ? 0x01 : 0) | (c.fwCallAtEnd ? 0x02 : 0) | (c.fwCallAtLatch ? 0x04 : 0));
    for (size_t i = 0; i < strlen(call) && n < sizeof(p); i++)
        p[n++] = (uint8_t)call[i];
    fmSendToModem(MMDVM_FM_PARAMS1, p, n);

    n      = 0;
    p[n++] = (uint8_t)c.fwAckSpeed;
    p[n++] = fmRoundByte((float)c.fwAckFreq / 10.0f);
    p[n++] = (uint8_t)c.fwAckMinTime;
    p[n++] = fmRoundByte((float)c.fwAckDelay / 10.0f);
    p[n++] = fmRoundByte((float)c.fwAckLevel * 2.55f);
    for (size_t i = 0; i < strlen(c.fwAckText) && n < sizeof(p); i++)
        p[n++] = (uint8_t)c.fwAckText[i];
    fmSendToModem(MMDVM_FM_PARAMS2, p, n);

    int access = c.fwAccessMode;
    if (access < 0)
    {
        if (strcasecmp(c.accessMode, "carrier") == 0)
            access = 0;
        else if (strcasecmp(c.accessMode, "both") == 0)
            access = 2;
        else
            access = 1;
    }

    n      = 0;
    p[n++] = fmRoundByte((float)c.timeout / 5.0f);
    p[n++] = fmRoundByte((float)c.fwTimeoutLevel * 2.55f);
    p[n++] = fmRoundByte(c.ctcssRx);
    p[n++] = (uint8_t)c.fwCtcssHigh;
    p[n++] = (uint8_t)c.fwCtcssLow;
    p[n++] = fmRoundByte(c.ctcssTxLevel * 2.55f);
    p[n++] = fmRoundByte(c.kerchunk);
    p[n++] = fmRoundByte(c.hangTime);
    p[n++] = (uint8_t)((access & 0x0f) | (c.fwCosInvert ? 0x80 : 0));
    p[n++] = (uint8_t)c.fwRxAudioBoost;
    p[n++] = fmRoundByte((float)c.fwMaxDev * 2.55f);
    p[n++] = fmRoundByte(c.rxLevel * 2.55f);
    p[n++] = (uint8_t)c.fwSquelchHigh;
    p[n++] = (uint8_t)c.fwSquelchLow;
    fmSendToModem(MMDVM_FM_PARAMS3, p, n);

    if (debugM)
        fprintf(stderr, "FM: firmware parameters sent.\n");
}

/*
 * fmFirmwareFrame()
 *
 * Handles an FM packet that the modem host forwarded from the MMDVM
 * firmware: "FMDA" audio data, "FMST" status, "FMEO" end of transmission or
 * "FMRS" RSSI. Audio data marks a call as active; the end of transmission
 * ends it. The firmware does the repeating, so the host only tracks the call
 * for the dashboard.
 *
 * Parameters:
 *   type    - The four character packet type (FMDA, FMST, FMEO or FMRS).
 *   payload - The bytes of the packet after the type.
 *   len     - Number of bytes in payload.
 *
 * Returns: nothing.
 */
FM_STATIC void fmFirmwareFrame(const char* type, const uint8_t* payload, unsigned int len)
{
    pthread_mutex_lock(&fmMutex);

    if (memcmp(type, "FMDA", 4) == 0)
    {
        fwLastData = time(NULL);
        if (!fwActive)
        {
            fwActive = true;
            fmPostEvent(EV_START);
        }
    }
    else if (memcmp(type, "FMEO", 4) == 0)
    {
        if (fwActive)
        {
            fwActive = false;
            fmPostEvent(EV_END);
        }
    }
    else if (memcmp(type, "FMST", 4) == 0)
    {
        if (debugM && len > 0)
            fprintf(stderr, "FM status: %02X (%u bytes)\n", payload[0], len);
    }

    pthread_mutex_unlock(&fmMutex);
}

/*
 * fmReadFully()
 *
 * Reads exactly n bytes from the modem host socket. Returns false when the
 * connection closed or failed.
 *
 * Parameters:
 *   buf - Receives the bytes.
 *   n   - Number of bytes to read.
 *
 * Returns: true if n bytes were read, false if the connection closed or
 *   failed.
 */
FM_STATIC bool fmReadFully(uint8_t* buf, size_t n)
{
    size_t got = 0;

    while (got < n)
    {
        ssize_t r = read(sockfd, buf + got, n - got);
        if (r == 0)
            return false;
        if (r < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (!running)
                    return false;
                delay(2000);
                continue;
            }
            return false;
        }
        got += (size_t)r;
    }
    return true;
}

/*
 * fmRegister()
 *
 * Sends the MODE packet that registers the FM host with the modem host. The
 * modem radio settings block of the packet is not used for FM and is sent as
 * zeros.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
FM_STATIC void fmRegister(void)
{
    uint8_t buffer[84];

    memset(buffer, 0, sizeof(buffer));
    buffer[0] = 0x61;
    buffer[1] = 0x00;
    buffer[2] = sizeof(buffer);
    buffer[3] = 0x04;
    memcpy(buffer + 4, TYPE_MODE, 4);
    memcpy(buffer + 8, MODE_NAME, strlen(MODE_NAME));
    memcpy(buffer + 19, MODEM_TYPE, strlen(MODEM_TYPE));
    buffer[25] = (uint8_t)cfg.txLevel;
    fmWriteAll(buffer, sizeof(buffer));
}

/*
 * fmClientThread()
 *
 * Thread that reads the modem host connection. Discriminator samples go to
 * fmRxSamples(); with the MMDVM modem the forwarded firmware FM packets go
 * to fmFirmwareFrame(). Command packets update the duplex flag.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL when the thread ends.
 */
FM_STATIC void* fmClientThread(void* arg)
{
    uint8_t buffer[BUFFER_SIZE];

    (void)arg;

    while (running && connected)
    {
        if (!fmReadFully(buffer, 1))
            break;

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "FM_Service: unknown byte from server, 0x%02X\n", buffer[0]);
            continue;
        }

        if (!fmReadFully(buffer + 1, 3))
            break;

        uint16_t respLen = (uint16_t)((buffer[1] << 8) + buffer[2]);
        if (respLen < 5 || respLen > BUFFER_SIZE)
        {
            fprintf(stderr, "FM_Service: bad packet length %u\n", respLen);
            continue;
        }

        if (!fmReadFully(buffer + 4, respLen - 4))
            break;

        uint8_t typeLen = buffer[3];
        if (typeLen != 4 || respLen < 8)
            continue;

        if (memcmp(buffer + 4, TYPE_COMMAND, 4) == 0)
        {
            if (respLen > 8 && buffer[8] == COMM_SET_DUPLEX)
                modem_duplex = true;
            else if (respLen > 8 && buffer[8] == COMM_SET_SIMPLEX)
                modem_duplex = false;
        }
        else if (memcmp(buffer + 4, TYPE_SAMPLE, 4) == 0 && packetType == PACKET_TYPE_SAMP)
        {
            int16_t smp[BUFFER_SIZE / 2];
            unsigned int count = (respLen - 8U) / 2U;

            for (unsigned int i = 0; i < count; i++)
                smp[i] = (int16_t)(buffer[8 + i * 2] | (buffer[9 + i * 2] << 8));
            fmRxSamples(smp, count);
        }
        else if (buffer[4] == 'F' && buffer[5] == 'M' && packetType == PACKET_TYPE_FRAME)
        {
            fmFirmwareFrame((const char*)buffer + 4, buffer + 8, respLen - 8U);
        }
    }

    fprintf(stderr, "Disconnected from host.\n");
    connected = false;
    return NULL;
}

/*
 * fmLinkAudioIn()
 *
 * Takes 8 kSa/s audio from the gateway (a station on the EchoLink link
 * talking), converts it to the 24 kSa/s transmit rate and puts it in the link
 * ring, from which the transmitter mixes it with the receiver audio. The
 * transmitter is keyed once the configured pre-buffer has filled.
 *
 * Parameters:
 *   data  - The samples, 16 bit little endian.
 *   count - Number of samples.
 *
 * Returns: nothing.
 */
static void fmLinkAudioIn(const uint8_t* data, unsigned int count)
{
    uint64_t prebuf = (uint64_t)cfg.linkPrebuf * FM_FS / 1000U;

    pthread_mutex_lock(&fmMutex);

    for (unsigned int i = 0; i < count; i++)
    {
        float x = (float)(int16_t)(data[i * 2U] | (data[i * 2U + 1U] << 8));
        float o[FM_LINK_DECIM];

        fmInterpRun(&linkInterp, x, o);
        for (unsigned int k = 0; k < FM_LINK_DECIM; k++)
            lnk[(lwIdx++) & LINK_MASK] = o[k];
    }

    if (lwIdx - lrIdx > LINK_RING * 3U / 4U)
        lrIdx = lwIdx - LINK_RING / 2U;

    linkLastTick = fmTicks;
    if (!linkTx && linkPend && lwIdx - lrIdx >= prebuf)
    {
        linkTx      = true;
        linkEnding  = false;
    }

    pthread_mutex_unlock(&fmMutex);
}

/*
 * fmLinkPtt()
 *
 * Handles the gateway telling that a station on the link started or stopped
 * talking. A start clears the link ring and waits for the pre-buffer; a stop
 * lets the transmitter play out what is buffered and then drop the link
 * audio (a short transmission is played even if the pre-buffer never
 * filled).
 *
 * Parameters:
 *   on - true when the station started talking, false when it stopped.
 *
 * Returns: nothing.
 */
static void fmLinkPtt(bool on)
{
    pthread_mutex_lock(&fmMutex);

    if (on)
    {
        lrIdx        = lwIdx;
        linkPend     = true;
        linkEnding   = false;
        linkTx       = false;
        linkLastTick = fmTicks;
        rxOpenTicks  = 0;
        fmInterpInit(&linkInterp);
    }
    else
    {
        linkPend   = false;
        linkEnding = true;
        if (lwIdx > lrIdx)
            linkTx = true;
    }

    pthread_mutex_unlock(&fmMutex);
}

/*
 * gwHandle()
 *
 * Handles one packet from the gateway: link status replies update the
 * dashboard, audio from the linked station goes to the link ring and the
 * talk start and stop messages control the transmitter.
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

    char name[LINK_NAME_LEN];

    memset(name, 0x00, sizeof(name));
    if (respLen > 8)
        memcpy(name, buffer + 8, (respLen - 8U) < sizeof(name) - 1U ? (respLen - 8U) : sizeof(name) - 1U);

    if (memcmp(buffer + 4, TYPE_LINK_AUDIO, 4) == 0)
        fmLinkAudioIn(buffer + 8, (respLen - 8U) / 2U);
    else if (memcmp(buffer + 4, TYPE_LINK_PTT, 4) == 0)
    {
        if (respLen > 8)
            fmLinkPtt(buffer[8] != 0);
    }
    else if (memcmp(buffer + 4, TYPE_CONNECT, 4) == 0)
    {
        snprintf(linkName, sizeof(linkName), "%s", name);
        gwLinked = true;
        ackDashbCommand(modemName, "reflLinkFM", "success");
        setReflectorStatus(modemName, "FM", name, true);
    }
    else if (memcmp(buffer + 4, TYPE_NACK, 4) == 0)
    {
        gwLinked = false;
        ackDashbCommand(modemName, "reflLinkFM", "failed");
        setReflectorStatus(modemName, "FM", name, false);
    }
    else if (memcmp(buffer + 4, TYPE_DISCONNECT, 4) == 0)
    {
        gwLinked = false;
        fmLinkPtt(false);
        ackDashbCommand(modemName, "reflLinkFM", "unlinked");
        setReflectorStatus(modemName, "FM", name, false);
    }
}

/*
 * fmFdReadFully()
 *
 * Reads exactly n bytes from a socket. Returns false when the connection
 * closed or failed.
 *
 * Parameters:
 *   fd  - The socket.
 *   buf - Receives the bytes.
 *   n   - Number of bytes to read.
 *
 * Returns: true if n bytes were read, false if the connection closed or
 *   failed.
 */
static bool fmFdReadFully(int fd, uint8_t* buf, size_t n)
{
    size_t got = 0;

    while (got < n && running)
    {
        ssize_t r = read(fd, buf + got, n - got);

        if (r == 0)
            return false;
        if (r < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                delay(2000);
                continue;
            }
            return false;
        }
        got += (size_t)r;
    }
    return got == n;
}

/*
 * fmFdWriteAll()
 *
 * Writes len bytes to a socket. Returns false if the connection failed.
 *
 * Parameters:
 *   fd   - The socket.
 *   data - The bytes to write.
 *   len  - Number of bytes in data.
 *
 * Returns: true if all bytes were written, false if the connection failed.
 */
static bool fmFdWriteAll(int fd, const uint8_t* data, size_t len)
{
    size_t sent = 0;

    while (sent < len)
    {
        ssize_t n = write(fd, data + sent, len - sent);

        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                delay(500);
                continue;
            }
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

/*
 * gatewayThread()
 *
 * Serves the connected EchoLink gateway: sends what the audio path and the
 * dashboard queued for it and reads what it sends. Registers the gateway with
 * the dashboard while it is connected and clears the link state when it goes.
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

    pthread_mutex_lock(&gwMutex);
    gwHead = gwTail = 0U;
    pthread_mutex_unlock(&gwMutex);

    pthread_mutex_lock(&fmMutex);
    gwFd         = fd;
    gwLinked     = false;
    gwConnected  = true;
    linkRxActive = false;
    linkOutCnt   = 0;
    dtmfBusy     = true;
    fmDecimInit(&linkDecim);
    pthread_mutex_unlock(&fmMutex);

    addGateway(modemName, "main", "FM");
    fprintf(stderr, "FM: EchoLink gateway connected.\n");

    while (running && connected && gwFd == fd)
    {
        fd_set         rfds;
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
            if (!fmFdReadFully(fd, buffer, 4))
                break;

            if (buffer[0] != 0x61)
            {
                fprintf(stderr, "FM: unknown byte from gateway, 0x%02X\n", buffer[0]);
                continue;
            }

            uint16_t respLen = (uint16_t)((buffer[1] << 8) + buffer[2]);

            if (respLen < 5 || respLen > BUFFER_SIZE)
            {
                fprintf(stderr, "FM: bad packet length %u from gateway\n", respLen);
                continue;
            }

            if (respLen > 4 && !fmFdReadFully(fd, buffer + 4, respLen - 4))
                break;

            if (debugM && memcmp(buffer + 4, TYPE_LINK_AUDIO, 4) != 0)
                hexDump("FM gateway data", buffer, respLen);

            gwHandle(buffer, respLen);
        }

        for (;;)
        {
            uint8_t      pkt[GW_PACKET_MAX];
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

            if (!fmFdWriteAll(fd, pkt, len))
            {
                reportError(modemName, "FM", "FM: gateway write failed.");
                r = -1;
                break;
            }
        }

        if (r < 0)
            break;
    }

    fprintf(stderr, "FM: EchoLink gateway disconnected.\n");

    close(fd);

    if (gwFd == fd)
    {
        pthread_mutex_lock(&fmMutex);
        gwFd        = -1;
        gwConnected = false;
        gwLinked    = false;
        linkPend    = false;
        linkEnding  = true;
        pthread_mutex_unlock(&fmMutex);
        delGateway(modemName, "main", "FM");
        clearReflLinkStatus(modemName, "FM");
    }

    return NULL;
}

/*
 * serverThread()
 *
 * Listens for the EchoLink gateway on the server port. A new connection
 * replaces an earlier one.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL.
 */
static void* serverThread(void* arg)
{
    int                on = 1;
    struct sockaddr_in addr;

    (void)arg;

    int lfd = socket(AF_INET, SOCK_STREAM, 0);

    if (lfd < 0)
    {
        reportError(modemName, "FM", "FM: error creating the gateway socket: %s", strerror(errno));
        return NULL;
    }

    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&addr, 0x00, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(serverPort);

    if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        reportError(modemName, "FM", "FM: error binding port %u: %s", serverPort, strerror(errno));
        close(lfd);
        return NULL;
    }

    if (listen(lfd, 4) < 0)
    {
        reportError(modemName, "FM", "listen: %s", strerror(errno));
        close(lfd);
        return NULL;
    }

    while (running && connected)
    {
        int cfd = accept(lfd, NULL, NULL);

        if (cfd < 0)
        {
            if (errno == EINTR)
                continue;
            reportError(modemName, "FM", "accept: %s", strerror(errno));
            break;
        }

        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

        if (gwFd >= 0)
            shutdown(gwFd, SHUT_RDWR);

        pthread_t id;

        if (pthread_create(&id, NULL, gatewayThread, (void*)(intptr_t)cfd) != 0)
        {
            reportError(modemName, "FM", "FM: can't create gateway thread");
            close(cfd);
            continue;
        }

        pthread_detach(id);
    }

    close(lfd);

    return NULL;
}

/*
 * fmLinkDashboard()
 *
 * Handles the dashboard's EchoLink commands: "link,<station>,<x>" connects
 * the link to a station by node number or callsign, "unlink" disconnects it.
 * Both are passed on to the gateway, which answers with a link status packet
 * that acknowledges the command. Without a gateway the command is answered
 * at once.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void fmLinkDashboard(void)
{
    char parameter[31] = "";

    readDashbCommand(modemName, "reflLinkFM", parameter);
    if (strlen(parameter) == 0)
        return;

    if (!gwConnected)
    {
        ackDashbCommand(modemName, "reflLinkFM", "No gateway");
        return;
    }

    if (strcasecmp(parameter, "unlink") == 0)
        fmLinkSend(TYPE_DISCONNECT, NULL, 0);
    else
    {
        char* token = strtok(parameter, ",");

        if (token != NULL && strcasecmp(token, "link") == 0 && (token = strtok(NULL, ",")) != NULL && strlen(token) > 0)
        {
            uint8_t name[13];

            memset(name, 0x00, sizeof(name));
            strncpy((char*)name, token, 12);
            fmLinkSend(TYPE_CONNECT, name, 12);
        }
        else
            ackDashbCommand(modemName, "reflLinkFM", "failed");
    }
}

/*
 * fmServiceEvents()
 *
 * Writes the queued call start and end events to the dashboard database: the
 * live call record when a call starts, and the finished record plus a
 * history entry with the duration when it ends.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
FM_STATIC void fmServiceEvents(void)
{
    for (;;)
    {
        FmEvent ev;

        pthread_mutex_lock(&fmMutex);
        if (evTail == evHead)
        {
            pthread_mutex_unlock(&fmMutex);
            return;
        }
        ev     = evq[evTail];
        evTail = (uint8_t)((evTail + 1U) % FM_EVENTS);
        pthread_mutex_unlock(&fmMutex);

        if (ev.type == EV_START)
        {
            callStart = ev.when;
            fmToneText(callSuffix, sizeof(callSuffix));
            saveLastCall(1, modemName, "FM", "RF", "FM", callSuffix, "", "", NULL, "", true);
        }
        else
        {
            long secs = (long)(ev.when - callStart);
            if (secs < 0)
                secs = 0;
            saveLastCall(1, modemName, "FM", "RF", "FM", callSuffix, "", "", NULL, "", false);
            saveHistory(modemName, "FM", "RF", "FM", callSuffix, "", 0.0f, "", (uint16_t)secs);
        }
    }
}

/*
 * fmCheckMode()
 *
 * Looks up the modem host's active mode in the database. Returns true when
 * it is FM, meaning this host owns the modem.
 *
 * Parameters: none.
 *
 * Returns: true if the active mode is FM.
 */
FM_STATIC bool fmCheckMode(void)
{
    char active[16] = "";

    readStatus(modemName, "main", "active_mode", active);
    return strcasecmp(active, "FM") == 0;
}

/*
 * fmServiceOnce()
 *
 * One pass of the service work done outside the audio threads: database
 * events, confirming the modem mode for the transmitter, the MMDVM firmware
 * start up and call timeout, and the dashboard update command.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
FM_STATIC void fmServiceOnce(void)
{
    static time_t lastCmd  = 0;
    static time_t lastTry  = 0;
    time_t        now      = time(NULL);

    fmServiceEvents();

    if (packetType == PACKET_TYPE_SAMP)
    {
        bool check;

        pthread_mutex_lock(&fmMutex);
        check = modeCheckReq;
        pthread_mutex_unlock(&fmMutex);

        if (check && fmCheckMode())
        {
            pthread_mutex_lock(&fmMutex);
            modeConfirmed = true;
            modeCheckReq  = false;
            pthread_mutex_unlock(&fmMutex);
        }

        static time_t lastHoldCheck = 0;
        pthread_mutex_lock(&fmMutex);
        bool holding = txState == TXS_HOLD;
        pthread_mutex_unlock(&fmMutex);
        if (holding && now - lastHoldCheck >= 2)
        {
            lastHoldCheck = now;
            if (!fmCheckMode())
            {
                pthread_mutex_lock(&fmMutex);
                if (txState == TXS_HOLD)
                    txState = TXS_IDLE;
                pthread_mutex_unlock(&fmMutex);
            }
        }
    }
    else
    {
        if (!fwModeOk && now - lastTry >= 2)
        {
            lastTry = now;
            if (fmCheckMode())
            {
                fwModeOk    = true;
                fwParamsDue = true;
                fprintf(stderr, "FM: modem is in FM mode.\n");
            }
            else
                fmWriteAll(SETMODE, 5);
        }

        if (fwParamsDue)
        {
            fwParamsDue = false;
            fmSendFirmwareParams();
        }

        pthread_mutex_lock(&fmMutex);
        if (fwActive && now - fwLastData > 2)
        {
            fwActive = false;
            fmPostEvent(EV_END);
        }
        pthread_mutex_unlock(&fmMutex);
    }

    static time_t lastLink = 0;

    if (packetType == PACKET_TYPE_SAMP && now != lastLink)
    {
        lastLink = now;
        fmLinkDashboard();
    }

    if (now - lastCmd >= 2)
    {
        char parameter[31];

        lastCmd = now;
        readDashbCommand(modemName, "updateConfFM", parameter);
        if (strlen(parameter) > 0)
        {
            fmLoadConfig();
            if (gwConnected)
            {
                uint8_t conf = COMM_UPDATE_CONF;

                fmLinkSend(TYPE_COMMAND, &conf, 1);
            }
            if (packetType == PACKET_TYPE_FRAME && fwModeOk)
                fwParamsDue = true;
            ackDashbCommand(modemName, "updateConfFM", "success");
        }
    }
}

/*
 * fmConnect()
 *
 * Opens the TCP connection to the modem host on the modem's port. Returns
 * false if it could not be made.
 *
 * Parameters: none.
 *
 * Returns: true if connected, false if the connection could not be made.
 */
FM_STATIC bool fmConnect(void)
{
    struct sockaddr_in serv_addr;
    int on = 1;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        reportError(modemName, "FM", "Socket creation error: %s", strerror(errno));
        return false;
    }

    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(clientPort);

    if (!hostToAddress(modemHost, &serv_addr.sin_addr))
    {
        reportError(modemName, "FM", "Invalid address/ Address not supported");
        return false;
    }

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        reportError(modemName, "FM", "connection failed: %s", strerror(errno));
        return false;
    }
    return true;
}

/*
 * fmInitState()
 *
 * Initialises the DSP state and puts values in cfg that differ from any real
 * setting, so the first fmLoadConfig() sets every detector up.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
FM_STATIC void fmInitState(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.ctcssRx   = -1.0f;
    cfg.ctcssTx   = -1.0f;
    cfg.noiseOpen = -1.0f;
    cfg.ctcssThreshold = -1.0f;

    fmAudioInit(&audio);
    fmCtcssInit(&ctcss, 0.0f, 15.0f, 400);
    fmNoiseInit(&noiseSq, 8.0f, 14.0f);
    fmToneInit(&txTone, 0.0f);
    memset(&cw, 0, sizeof(cw));
    memset(aud, 0, sizeof(aud));
    wIdx = rIdx = 0;

    fmDecimInit(&linkDecim);
    fmInterpInit(&linkInterp);
    fmDtmfInit(&dtmf);
    memset(lnk, 0, sizeof(lnk));
    lwIdx = lrIdx = 0;
    linkTx = linkPend = linkEnding = false;
}

#ifndef FM_TEST
/*
 * main()
 *
 * Program entry. Parses the command line (-m modem number 1-10, -d daemon,
 * -v version, -x debug), checks the modem type (FM is not available on the
 * hotspot modems), reads the FM settings, connects to the modem host and
 * registers the FM mode, then runs the receive and transmit threads and the
 * service loop until it is stopped.
 *
 * Parameters:
 *   argc - Number of command line arguments.
 *   argv - Command line arguments.
 *
 * Returns: 0 on normal exit, 1 on a command line or startup error.
 */
int main(int argc, char** argv)
{
    bool daemon = false;
    int  c;

    while ((c = getopt(argc, argv, ":m:a:s:u:p:c:dxv")) != -1)
    {
        switch (c)
        {
            case 'm':
            {
                modemId = (uint8_t)atoi(optarg);
                if (modemId < 1 || modemId > 10)
                {
                    reportError(modemName, "FM", "Invalid modem number.");
                    return 1;
                }
                sprintf(modemName, "modem%d", modemId);
                clientPort = (uint16_t)(18000 + modemId - 1);
                serverPort = (uint16_t)(18700 + modemId - 1);
                fprintf(stderr, "Modem name: %s\n", modemName);
            }
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
                daemon = true;
                break;
            case 'v':
                fprintf(stdout, "FM_Service: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: FM_Service [-m modem_number (1-10)] [-a address] [-s dbserver] [-u dbuser] [-p dbpassword] [-c configfile] [-d] [-v] [-x]\n");
                return 1;
        }
    }

    char streamName[20] = "";
    readHostConfig(modemName, "config", "streamType", streamName);   /* published by the bridge: FRAME, BIT or SAMPLE */

    if (strcasecmp(streamName, "SAMPLE") == 0)
        packetType = PACKET_TYPE_SAMP;
    else if (strcasecmp(streamName, "FRAME") == 0)
        packetType = PACKET_TYPE_FRAME;
    else
    {
        fprintf(stderr, "FM_Service: FM mode is not available for a '%s' stream.\n", streamName);
        return 1;
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            reportError(modemName, "FM", "FM_Service: error in fork(), exiting");
            return 1;
        }
        if (pid > 0)
            return 0;
        setsid();
        umask(0);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigHandler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fmInitState();
    fmCreateDefaults();
    fmLoadConfig();

    if (!fmConnect())
        return 1;

    pthread_t clientId;
    pthread_t txId;

    if (pthread_create(&clientId, NULL, &fmClientThread, NULL) != 0)
    {
        reportError(modemName, "FM", "Can't create modem host thread.");
        return 1;
    }

    sleep(1);
    fmRegister();
    fprintf(stderr, "Connected to host. FM mode registered (%s path).\n",
            packetType == PACKET_TYPE_SAMP ? "sample" : "MMDVM firmware");

    if (packetType == PACKET_TYPE_SAMP)
    {
        pthread_t serverId;

        clearReflLinkStatus(modemName, "FM");
        /* tells the dashboard to show a box for a typed link target (see getLinkInput in data_query.php) */
        addHostConfigDefault(modemName, "FM", "linkLabel", "linkinput", "EchoLink node or callsign");
        addHostConfigDefault(modemName, "FM", "linkPattern", "linkinput", "^[A-Za-z0-9\\-/*_]{1,12}$");
        if (pthread_create(&txId, NULL, &fmTxThread, NULL) != 0)
        {
            reportError(modemName, "FM", "Can't create transmit thread.");
            return 1;
        }
        if (pthread_create(&serverId, NULL, &serverThread, NULL) != 0)
            reportError(modemName, "FM", "Can't create the EchoLink gateway thread.");
    }

    while (running && connected)
    {
        fmServiceOnce();
        delay(20000);
    }

    if (packetType == PACKET_TYPE_SAMP)
    {
        pthread_mutex_lock(&fmMutex);
        bool wasTx   = txState != TXS_IDLE && txState != TXS_HOLD;
        bool wasHold = txState == TXS_HOLD;
        pthread_mutex_unlock(&fmMutex);
        if (wasHold)
            fmWriteAll(SETIDLE, 5);
        else if (wasTx)
        {
            int16_t silence[FM_FRAME_SAMPLES];
            memset(silence, 0, sizeof(silence));
            fmSendSamples(silence, true);
            delay(300000);
            fmWriteAll(SETIDLE, 5);
        }
    }
    else if (fwModeOk)
        fmWriteAll(SETIDLE, 5);

    connected = false;
    fmServiceEvents();
    close(sockfd);
    fprintf(stderr, "FM service terminated.\n");
    logError(modemName, "main", "FM host terminated.");
    return 0;
}
#endif
