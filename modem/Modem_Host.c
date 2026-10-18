/***************************************************************************
 *   Copyright (C) 2025,2026 by Rick KD0OSS                                *
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
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <execinfo.h>
#include <unistd.h>
#include "../tools/RingBuffer.h"
#include "../tools/tools.h"
#include "RingBuffer.h"
#include "mmdvm.h"
#include <netinet/tcp.h>
/* Disable Nagle: these programs talk over loopback TCP in small frames
 * (200 bytes of samples, 3 byte requests).  With Nagle on, a small write
 * waits for the ACK of the previous one, and the peer delays that ACK for
 * ~40 ms, so the stream arrives in bursts with 40 ms gaps. */
static void setNoDelay(int fd)
{
    int one = 1;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

#define VERSION "2026-10-07"
#define MAX_LINE_LENGTH 256
#define BUFFER_LENGTH 5000
#define MAX_CLIENT_CONNECTIONS 14
#define TX_TIMEOUT 300000

const char* TYPE_ACK          = "ACK ";
const char* TYPE_NACK         = "NACK";
const char* TYPE_MODE         = "MODE";
const char* TYPE_LSF_M17      = "M17L";
const char* TYPE_STREAM_M17   = "M17S";
const char* TYPE_PACKET_M17   = "M17P";
const char* TYPE_EOT_M17      = "M17E";
const char* TYPE_HEADER_DSTAR = "DSTH";
const char* TYPE_DATA_DSTAR   = "DSTD";
const char* TYPE_EOT_DSTAR    = "DSTE";
const char* TYPE_DATA_FM      = "FMDA";
const char* TYPE_STATUS_FM    = "FMST";
const char* TYPE_EOT_FM       = "FMEO";
const char* TYPE_RSSI_FM      = "FMRS";
const char* TYPE_HDR_P25      = "P25H";
const char* TYPE_LDU_P25      = "P25L";
const char* TYPE_LOST_P25     = "P25E";
const char* TYPE_DATA_YSF     = "YSFD";
const char* TYPE_LOST_YSF     = "YSFL";
const char* TYPE_DATA_NXDN    = "NXDD";
const char* TYPE_LOST_NXDN    = "NXDL";
const char* TYPE_DATA1_DMR    = "DMD1";
const char* TYPE_LOST1_DMR    = "DML1";
const char* TYPE_DATA2_DMR    = "DMD2";
const char* TYPE_LOST2_DMR    = "DML2";
const char* TYPE_SHORTLC_DMR  = "DMLC";
const char* TYPE_START_DMR    = "DMST";
const char* TYPE_ABORT_DMR    = "DMAB";

const char* TYPE_TO_MODEM     = "2MOD";
const char* TYPE_COMMAND      = "COMM";
const char* TYPE_SAMPLE       = "SAMP";
const char* TYPE_SAMPLE_EOT   = "SEOT";
const char* TYPE_BITS         = "BITS";
const char* TYPE_BITS_EOT     = "BEOT";
const char* TYPE_TX_TIMING    = "TXTM";  // OPENMT_TX_TIMING, forwarded to the DMR host only

#define NET_ACK 0xF0
#define NET_NACK 0xF1

#define OPENMT_GET_VERSION 0x00U
#define OPENMT_GET_STATUS 0x10U
#define OPENMT_SET_CONFIG 0x20U
#define OPENMT_SET_MODE 0x30U
#define OPENMT_ADD_MODE 0x40U
#define OPENMT_DEL_MODE 0x50U
#define OPENMT_BIT_DATA 0x60U
#define OPENMT_SAMP_DATA 0x70U
#define OPENMT_TO_MODEM 0x80U
#define OPENMT_SAMP_EOT 0x90U
#define OPENMT_BIT_EOT 0xA0U
#define OPENMT_TX_TIMING 0xB0U  // bridge -> host: TX/RX sample alignment (DMR slot timing)
#define OPENMT_ACK 0xE0U
#define OPENMT_NAK 0xF0U

#define COMM_SET_DUPLEX 0x00
#define COMM_SET_SIMPLEX 0x01
#define COMM_SET_MODE 0x02
#define COMM_SET_IDLE 0x03
#define TYPE_M17_SAMPLE 0x04
#define TYPE_DSTAR_SAMPLE 0x05
#define TYPE_DSTAR_BITS 0x06

// ****** Modem specific parameters ********************
bool modem_trace           = false;
bool modem_debug           = false;
bool modem_duplex          = false;
bool modem_rxInvert        = false;
bool modem_txInvert        = true;
bool modem_pttInvert       = false;
bool modem_useCOSAsLockout = false;
unsigned int modem_txDelay = 100U;
char modem_rxFrequency[11] = "435000000";
char modem_txFrequency[11] = "435000000";
uint8_t modem_rxLevel      = 100U;
uint8_t modem_rfLevel      = 100U;
int modem_rxDCOffset       = 0U;
int modem_txDCOffset       = 0U;
// ******************************************************

pthread_t modemTxid;
pthread_t modetxid;
pthread_t tcpid;
pthread_t timerid;
pthread_t commandid;

// Protocol service client descriptor
typedef struct
{
    pthread_t clientid;
    pthread_t rxid;
    int sockfd;
    char mode[11];
    bool active;
    bool isTx;
    RingBuffer command;
    pthread_mutex_t commandMutex; /* Protect command buffer access */
} Client;

Client hostClient[MAX_CLIENT_CONNECTIONS];

// Descriptor for protocol
// Data received from protocol service
typedef struct
{
    char name[11];          //< Protocol name (10 char max)
    char modem_type[6];     //< Modem type (5 char max) (4FSK, GMSK, etc.)
    bool use_rx_dc_filter;  //< Use DC filter
    bool use_lp_filter;     //< Use LP filter
    uint8_t txLevel;        //< TX mod level
} Protocol;

Protocol mode[MAX_CLIENT_CONNECTIONS];

int serialModemFd         = 0;           //< File descriptor for modem serial port.
uint8_t modemId           = 1;           //< Modem Id used to create modem name.
char modemName[10]        = "modem1";    //< Modem name that this program is associated with.
char modemtty[50]         = "";          //< Modem serial port.
uint8_t connections       = 0;           //< Number of clients currently connected.
uint16_t host_port        = 18000;       //< All protocol services connect on this TCP port.
uint32_t txTimeout        = TX_TIMEOUT;  //< Station TX timeout. FIX-ME: Should be user configurable.
bool duplex               = false;       //< Indicates station TX operation mode.
bool debugM               = false;       //< If true print debug info.
atomic_bool txOn          = false;       //< If true we are processing frame data.
atomic_bool running       = true;        //< Set false to kill all processes and exit program.
atomic_bool exitRequested = false;       //< Set to true with ctrl + c keyboard input.
atomic_bool isModemSpace  = true;        //< Flag indicating that modem buffer space is available.
atomic_bool bufReady      = false;       //< Signal when rxModeBuffer has adequate number of bytes.

unsigned int clientlen;         //< byte size of client's address
char* hostaddrp;                //< dotted decimal host addr string
int optval;                     //< flag value for setsockopt
struct sockaddr_in serveraddr;  //< server's addr
struct sockaddr_in clientaddr;  //< client addr

RingBuffer modemCommandBuffer; /* Modem command buffer */
RingBuffer modemTxBuffer;      /* Modem TX buffer */
RingBuffer rxModeBuffer;       /* Client RX buffer */
RingBuffer txModeBuffer;       /* Client TX buffer */

pthread_mutex_t modemTxBufMutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t modemComBufMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t rxBufModeMutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t txBufModeMutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t shutDownMutex    = PTHREAD_MUTEX_INITIALIZER;

/* Current active radio mode - shared across threads, protected by currentModeMutex */
char currentMode[11]             = "idle";
pthread_mutex_t currentModeMutex = PTHREAD_MUTEX_INITIALIZER;
char commPort[20];
char modemBaud[10];
char modem[20];
char callsign[10] = "N0CALL";

typedef struct TIMERS
{
    char name[20];
    bool valid;
    bool enabled;
    uint32_t duration;
    uint32_t count;
    bool triggered;
} TIMERS;

pthread_mutex_t timerMutex = PTHREAD_MUTEX_INITIALIZER;
TIMERS timer[10];

void setProtocol(const char* protocol, bool enabled);

/* Safe accessors for currentMode string */
static void getCurrentMode(char* buf, size_t len)
{
    pthread_mutex_lock(&currentModeMutex);
    strncpy(buf, currentMode, len - 1);
    buf[len - 1] = '\0';
    pthread_mutex_unlock(&currentModeMutex);
}

static void setCurrentMode(const char* mode)
{
    pthread_mutex_lock(&currentModeMutex);
    strncpy(currentMode, mode, sizeof(currentMode) - 1);
    currentMode[sizeof(currentMode) - 1] = '\0';
    pthread_mutex_unlock(&currentModeMutex);
}

static bool isCurrentMode(const char* mode)
{
    pthread_mutex_lock(&currentModeMutex);
    bool match = (strcasecmp(currentMode, mode) == 0);
    pthread_mutex_unlock(&currentModeMutex);
    return match;
}

static bool isCurrentModeIdle(void)
{
    return isCurrentMode("idle");
}

//  SIGINT handler, so we can gracefully exit when the user hits ctrl+c.
static void sigintHandler(int signum)
{
    fprintf(stderr, "Program shutdown requested.\n");

    for (uint8_t x = 0; x < MAX_CLIENT_CONNECTIONS; x++)
    {
        if (hostClient[x].active)
            setProtocol(mode[x].name, false);
    }
    sleep(1);
    atomic_store(&exitRequested, true);
    atomic_store(&running, false);
    signal(SIGINT, SIG_DFL);
}

uint8_t getModemSpace(const char* protocol, const uint8_t slot)
{
    if (strncasecmp(modem, "openmt", 6) != 0)
    {
        char mode[11];
        if (protocol != NULL)
            strncpy(mode, protocol, sizeof(mode) - 1);
        else
            getCurrentMode(mode, sizeof(mode));
        if (strcasecmp(mode, "M17") == 0)
            return getM17Space();
        else if (strcasecmp(mode, "DSTAR") == 0)
            return getDSTARSpace();
        else if (strcasecmp(mode, "P25") == 0)
            return getP25Space();
        else if (strcasecmp(mode, "YSF") == 0)
            return getYSFSpace();
        else if (strcasecmp(mode, "NXDN") == 0)
            return getNXDNSpace();
        else if (strcasecmp(mode, "DMR") == 0)
            return getDMRSpace(slot);
    }
    return 20;  // default
}

void setSpace(uint8_t space)
{
    if (space == 0)
        atomic_store(&isModemSpace, false); /* no space in modem buffer */
    else
        atomic_store(&isModemSpace, true); /* space available */
}

/* "openmt" (SAMPLE mode) flow control.  The modem has a fixed TX sample
 * buffer and NAKs (error 6) any SAMP_DATA that does not fit, so the host
 * must not send faster than the modem plays.  After every SAMP_DATA frame
 * we wait for a GET_STATUS reply that says at least OPENMT_MIN_FREE frames
 * are free before sending the next one. */
#define OPENMT_MIN_FREE 2U       /* free frames (96 samples each) needed to send */
#define OPENMT_POLL_MS  4U       /* status poll interval while waiting          */

/* Free TX frames the modem last reported, minus frames sent since.  The
 * status nibble is capped at 15, so with a large TX buffer it never shows
 * how full the buffer really is; waiting for a status reply after EVERY
 * frame (as an earlier version did) limits throughput to about one 4 ms
 * frame per loop pass, which is barely real time and starves a DMR
 * downlink.  Instead send while credit remains and refresh it from the
 * periodic status replies. */
static atomic_int openmtCredit = 0;

/* A status reply describes the modem's buffer at the moment the modem read
 * the request.  Sample frames queued after that request were still on their
 * way (in modemTxBuffer or the serial driver), so they are not in the free
 * count yet and must be subtracted from it, or the host overfills the modem
 * and it NAKs (error 6) the frames that do not fit.
 *
 * openmtSampQueued counts SAMP_DATA frames put into modemTxBuffer.  Each
 * status request records that count when it is queued (both under
 * modemTxBufMutex, so the order matches the byte stream); replies come back
 * in request order. */
#define OPENMT_REQ_FIFO 16U
static uint32_t openmtSampQueued = 0;                 /* under modemTxBufMutex */
static uint32_t openmtReqSnap[OPENMT_REQ_FIFO];       /* under modemTxBufMutex */
static uint32_t openmtReqHead = 0;
static uint32_t openmtReqCount = 0;

/* Status reply arrived: free frames in the modem minus frames still in flight. */
static int openmtCreditFromStatus(uint8_t freeFrames)
{
    int inFlight = 0;

    pthread_mutex_lock(&modemTxBufMutex);
    if (openmtReqCount > 0)
    {
        inFlight = (int)(openmtSampQueued - openmtReqSnap[openmtReqHead]);
        openmtReqHead = (openmtReqHead + 1U) % OPENMT_REQ_FIFO;
        openmtReqCount--;
    }
    pthread_mutex_unlock(&modemTxBufMutex);

    return (int)freeFrames - inFlight;
}

static bool       openmtStalled;         /* modeTxThread only */
static atomic_int openmtLastFree = -1;   /* last raw free-frame nibble from the modem */

/* Forget outstanding status requests so the next reply is taken as-is
 * (inFlight = 0).  Used when replies and requests have drifted apart. */
static void openmtResyncPacing(void)
{
    pthread_mutex_lock(&modemTxBufMutex);
    openmtReqHead  = 0;
    openmtReqCount = 0;
    pthread_mutex_unlock(&modemTxBufMutex);
}

static bool openmtPacing(void)
{
    return strcasecmp(modem, "openmt") == 0;
}

/* Queue a GET_STATUS request for the modem.  Unless 'force' is set it is
 * rate limited to one per OPENMT_POLL_MS. */
static void requestModemStatus(bool force)
{
    static struct timespec last;
    struct timespec        now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t ms = (int64_t)(now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000;
    if (!force && ms < (int64_t)OPENMT_POLL_MS)
        return;
    uint8_t buf[3] = {0x61, 0x03, OPENMT_GET_STATUS};
    pthread_mutex_lock(&modemTxBufMutex);
    if (RingBuffer_freeSpace(&modemTxBuffer) > 3)
    {
        RingBuffer_addData(&modemTxBuffer, buf, 3);
        last = now;
        if (openmtReqCount == OPENMT_REQ_FIFO)
        {
            /* a reply went missing: forget the oldest request */
            openmtReqHead = (openmtReqHead + 1U) % OPENMT_REQ_FIFO;
            openmtReqCount--;
        }
        openmtReqSnap[(openmtReqHead + openmtReqCount) % OPENMT_REQ_FIFO] = openmtSampQueued;
        openmtReqCount++;
    }
    pthread_mutex_unlock(&modemTxBufMutex);
}

void setProtocol(const char* protocol, bool enabled)
{
    if (strncasecmp(modem, "openmt", 6) != 0)
    {
        if (strcasecmp(protocol, "M17") == 0)
        {
            if (enabled)
                enableM17(modemName);
            else
                disableM17(modemName);
        }
        else if (strcasecmp(protocol, "DSTAR") == 0)
        {
            if (enabled)
                enableDSTAR(modemName);
            else
                disableDSTAR(modemName);
        }
        else if (strcasecmp(protocol, "P25") == 0)
        {
            if (enabled)
                enableP25(modemName);
            else
                disableP25(modemName);
        }
        else if (strcasecmp(protocol, "YSF") == 0)
        {
            if (enabled)
                enableYSF(modemName);
            else
                disableYSF(modemName);
        }
        else if (strcasecmp(protocol, "NXDN") == 0)
        {
            if (enabled)
                enableNXDN(modemName);
            else
                disableNXDN(modemName);
        }
        else if (strcasecmp(protocol, "FM") == 0)
        {
            if (enabled)
                enableFM(modemName);
            else
                disableFM(modemName);
        }
        else if (strcasecmp(protocol, "DMR") == 0)
        {
            if (enabled)
                enableDMR(modemName);
            else
                disableDMR(modemName);
        }
    }
}

// error - wrapper for perror
void error(char* msg)
{
    perror(msg);
    exit(1);
}

// Wait for 'delay' microseconds
void delay(uint32_t delay)
{
    struct timespec req, rem;
    req.tv_sec  = 0;
    req.tv_nsec = delay * 1000;
    nanosleep(&req, &rem);
};

// Print debug data.
// From MMDVM project by Jonathan Naylor G4KLX.
void dump(char* text, unsigned char* data, unsigned int length)
{
    struct timespec ts;
    uint64_t milliseconds;
    unsigned int offset = 0U;
    unsigned int i;

    // Get the current time with nanosecond precision
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
    {
        perror("clock_gettime");
        return;
    }

    // Convert to milliseconds
    milliseconds = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;

    fprintf(stdout, "%s: %llu\n", text, milliseconds);

    while (length > 0U)
    {
        unsigned int bytes = (length > 16U) ? 16U : length;

        fprintf(stdout, "%04X:  ", offset);

        for (i = 0U; i < bytes; i++)
            fprintf(stdout, "%02X ", data[offset + i]);

        for (i = bytes; i < 16U; i++)
            fputs("   ", stdout);

        fputs("   *", stdout);

        for (i = 0U; i < bytes; i++)
        {
            unsigned char c = data[offset + i];

            if (isprint(c))
                fputc(c, stdout);
            else
                fputc('.', stdout);
        }

        fputs("*\n", stdout);

        offset += 16U;

        if (length >= 16U)
            length -= 16U;
        else
            length = 0U;
    }
}

// Setup seial port
int openSerial(char* serial)
{
    struct termios tty;
    unsigned int speed = 0;
    int fd;

    fd = open(serial, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0)
    {
        fprintf(stderr, "Modem_Host: error when opening %s, errno=%d\n", serial, errno);
        return fd;
    }

    if (tcgetattr(fd, &tty) != 0)
    {
        fprintf(stderr, "Modem_Host: error %d from tcgetattr\n", errno);
        return -1;
    }

    switch (atol(modemBaud))
    {
        case 921600:
            speed = B921600;
            break;
        case 460800:
            speed = B460800;
            break;

        case 230400:
            speed = B230400;
            break;

        case 115200:
            speed = B115200;
            break;

        default:
            speed = B115200;
    }

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    /* binary data: no break handling, parity marking, CR/LF mapping,
     * stripping or flow control */
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | INPCK | IGNPAR);
    tty.c_lflag = 0;

    tty.c_oflag     = 0;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 10;

    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);

    tty.c_cflag |= (CLOCAL | CREAD);

    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0)
    {
        fprintf(stderr, "Modem_Host: error %d from tcsetattr\n", errno);
        return -1;
    }

    if (debugM)
        fprintf(stdout, "opened %s\n", serial);
    return fd;
}


/* ---- modem receive layer begin ------------------------------------------
 *
 * The serial port used to be read one byte per read() call, about 90,000
 * system calls a second at 921600 baud.  The bytes are now read in blocks
 * into a buffer and taken from there.
 *
 * Frames from the modem look like  0x61, length (whole frame, 3..255),
 * type, payload...  and follow each other back to back.  A byte lost on the
 * way (the Pi's UART overrunning, the kernel buffer overflowing while the
 * host is stalled) shifts everything after it; the next frame header is then
 * swallowed and the rest of that frame turns up as stray bytes.  So a frame
 * start is only believed when its length is plausible and, where the next
 * bytes are already there, when the next frame starts right behind it.  (The
 * type byte is not checked: the modem also sends DEBUG and version frames.)  After a loss the reader skips to the next confirmed frame in one go
 * and reports it once, instead of printing a line for every stray byte.
 */
#define MODEM_START       0x61U
#define MODEM_RX_BUF      8192U
#define MODEM_CONFIRM_MS  20

static uint8_t  modemRxBuf[MODEM_RX_BUF];
static size_t   modemRxHead;
static size_t   modemRxCount;
static bool     modemHunting;          /* out of step, looking for a frame start */
static uint32_t modemSkipped;          /* bytes thrown away in this hunt         */
static uint32_t modemResyncs;          /* hunts since start                      */
static uint32_t modemSkippedTotal;     /* bytes thrown away since start          */

/* One read into the buffer.  >0 bytes added, 0 nothing (timeout, signal),
 * -1 error. */
static int modemRxFill(void)
{
    ssize_t r;

    if (modemRxCount == 0)
        modemRxHead = 0;
    else if (modemRxHead + modemRxCount > MODEM_RX_BUF / 2)
    {
        memmove(modemRxBuf, modemRxBuf + modemRxHead, modemRxCount);
        modemRxHead = 0;
    }

    r = read(serialModemFd, modemRxBuf + modemRxHead + modemRxCount,
             MODEM_RX_BUF - modemRxHead - modemRxCount);
    if (r < 0)
        return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    modemRxCount += (size_t)r;
    return (int)r;
}

/* Wait until n bytes are buffered (the port's own 1 s read timeout applies).
 * 1 = there, 0 = timed out, -1 = error. */
static int modemRxNeed(size_t n)
{
    while (modemRxCount < n)
    {
        int r = modemRxFill();

        if (r < 0)
            return -1;
        if (r == 0)
            return 0;
    }
    return 1;
}

/* Same, but for at most ms milliseconds.  1 = there, 0 = not in time, -1 = error. */
static int modemRxNeedShort(size_t n, int ms)
{
    struct pollfd pfd;

    while (modemRxCount < n)
    {
        int r;

        pfd.fd     = serialModemFd;
        pfd.events = POLLIN;
        r = poll(&pfd, 1, ms);
        if (r < 0)
            return (errno == EINTR) ? 0 : -1;
        if (r == 0)
            return 0;
        r = modemRxFill();
        if (r < 0)
            return -1;
    }
    return 1;
}

/* Same as read(), but from the buffer: up to n bytes, 0 when nothing came
 * within the read timeout, -1 on error. */
static ssize_t modemRead(uint8_t* dst, size_t n)
{
    if (modemRxCount == 0)
    {
        int r = modemRxFill();

        if (r <= 0)
            return r;
    }
    if (n > modemRxCount)
        n = modemRxCount;
    memcpy(dst, modemRxBuf + modemRxHead, n);
    modemRxHead += n;
    modemRxCount -= n;
    return (ssize_t)n;
}

/* Throw away n buffered bytes while out of step. */
static void modemDiscard(size_t n)
{
    if (!modemHunting)
    {
        modemHunting = true;
        modemSkipped = 0;
        modemResyncs++;
        fprintf(stderr, "Modem_Host: lost frame sync with the modem (0x%02X where a frame should start), resyncing\n",
                modemRxBuf[modemRxHead]);
    }
    modemSkipped += (uint32_t)n;
    modemSkippedTotal += (uint32_t)n;
    modemRxHead += n;
    modemRxCount -= n;
}

/* Next frame from the modem into frame[] (whole frame, 0x61 first).
 * Returns 1 = frame ready, 2 = nothing received, 3 = bytes were thrown away
 * (call again), 0 = read error. */
static int modemGetFrame(uint8_t* frame, uint16_t* frameLen)
{
    const uint8_t* p;
    size_t len;
    int r;

    r = modemRxNeed(1);
    if (r < 0)
        return 0;
    if (r == 0)
        return 2;

    p = modemRxBuf + modemRxHead;
    if (p[0] != MODEM_START)
    {
        const uint8_t* q = memchr(p, MODEM_START, modemRxCount);

        modemDiscard(q ? (size_t)(q - p) : modemRxCount);
        return 3;
    }

    r = modemRxNeed(3);
    if (r < 0)
        return 0;
    p = modemRxBuf + modemRxHead;
    if (r == 0)                         /* header never completed */
    {
        modemDiscard(1);
        return 3;
    }

    len = p[1];
    if (len < 3)
    {
        modemDiscard(1);
        return 3;
    }

    r = modemRxNeed(len);
    if (r < 0)
        return 0;
    p = modemRxBuf + modemRxHead;
    if (r == 0)                         /* frame never completed */
    {
        modemDiscard(1);
        return 3;
    }

    /* The byte behind the frame must be the next frame start.  After a
     * loss, wait a moment for it to be sure; in step, check it only if it
     * is already there. */
    if (modemHunting)
    {
        if (modemRxNeedShort(len + 1, MODEM_CONFIRM_MS) < 0)
            return 0;
        p = modemRxBuf + modemRxHead;
    }
    if (modemRxCount > len && p[len] != MODEM_START)
    {
        modemDiscard(1);
        return 3;
    }

    memcpy(frame, p, len);
    *frameLen = (uint16_t)len;
    modemRxHead += len;
    modemRxCount -= len;

    if (modemHunting)
    {
        fprintf(stderr, "Modem_Host: back in step after skipping %u bytes (%u resyncs, %u bytes skipped since start)\n",
                (unsigned int)modemSkipped, (unsigned int)modemResyncs, (unsigned int)modemSkippedTotal);
        modemHunting = false;
    }
    return 1;
}
/* ---- modem receive layer end -------------------------------------------- */

void setMode(const uint8_t mode)
{
    uint8_t buffer[4];

    if (strcasecmp(modem, "MMDVM") == 0 || strcasecmp(modem, "MMDVMHS") == 0)
    {
        buffer[0] = 0xE0;
        buffer[1] = 0x04;
        buffer[2] = MODEM_MODE;
        buffer[3] = mode;
        pthread_mutex_lock(&modemComBufMutex);
        RingBuffer_addData(&modemCommandBuffer, buffer, 4);
        pthread_mutex_unlock(&modemComBufMutex);
    }
    else if (strcasecmp(modem, "OPENMTHS") == 0 || strcasecmp(modem, "OPENMTSX") == 0)
    {
        buffer[0] = 0x61;
        buffer[1] = 0x03;
        buffer[2] = (uint8_t)(OPENMT_SET_MODE | mode);
        pthread_mutex_lock(&modemComBufMutex);
        RingBuffer_addData(&modemCommandBuffer, buffer, 3);
        pthread_mutex_unlock(&modemComBufMutex);
    }
}

void initTimers()
{
    for (uint8_t i = 0; i < 10; i++)
    {
        bzero(timer[i].name, 20);
        timer[i].valid     = false;
        timer[i].duration  = 0;
        timer[i].count     = 0;
        timer[i].enabled   = false;
        timer[i].triggered = false;
    }
}

int8_t getTimer(const char* name, uint32_t duration)
{
    for (uint8_t i = 0; i < 10; i++)
    {
        pthread_mutex_lock(&timerMutex);
        if (!timer[i].valid)
        {
            strcpy(timer[i].name, name);
            timer[i].valid     = true;
            timer[i].enabled   = true;
            timer[i].triggered = false;
            timer[i].count     = 0;
            timer[i].duration  = duration;
            pthread_mutex_unlock(&timerMutex);
            return i;
        }
        pthread_mutex_unlock(&timerMutex);
    }
    return -1;
}

bool isTimerTriggered(const char* name)
{
    for (uint8_t i = 0; i < 10; i++)
    {
        pthread_mutex_lock(&timerMutex);
        if (timer[i].valid && timer[i].triggered && strcasecmp(timer[i].name, name) == 0)
        {
            pthread_mutex_unlock(&timerMutex);
            return true;
        }
        pthread_mutex_unlock(&timerMutex);
    }
    return false;
}

uint32_t getTimerDuration(const char* name)
{
    for (uint8_t i = 0; i < 10; i++)
    {
        pthread_mutex_lock(&timerMutex);
        if (strcasecmp(timer[i].name, name) == 0)
        {
            pthread_mutex_unlock(&timerMutex);
            return timer[i].duration;
        }
        pthread_mutex_unlock(&timerMutex);
    }
    return 0;
}

void resetTimer(const char* name, const uint32_t duration)
{
    pthread_mutex_lock(&timerMutex);
    for (uint8_t i = 0; i < 10; i++)
    {
        if (timer[i].valid && strcasecmp(timer[i].name, name) == 0)
        {
            /* Set new duration if greater than zero. */
            if (duration > 0)
                timer[i].duration = duration;
            else
            {
                timer[i].count     = 0;
                timer[i].triggered = false;
            }
            break;
        }
    }
    pthread_mutex_unlock(&timerMutex);
}

void disableTimer(uint8_t id)
{
    pthread_mutex_lock(&timerMutex);
    bzero(timer[id].name, 20);
    timer[id].valid     = false;
    timer[id].enabled   = false;
    timer[id].triggered = false;
    timer[id].duration  = 0;
    timer[id].count     = 0;
    pthread_mutex_unlock(&timerMutex);
}

/*
 * NXDN frame pacing.
 *
 * One TYPE_NXDN_DATA frame is a whole 80 ms air frame (48 bytes), unlike the
 * 20 ms frames of the other modes, and the modem's NXDN FIFO holds dozens of
 * them. The generic controller below steers the free space to 10..12 frames,
 * which for NXDN would park well over a second of speech in the modem. Here
 * the aim is a small, steady queue instead: enough to ride out network jitter
 * and keep the transmitter fed, little enough to keep the delay short.
 *
 * The depth of the queue is the largest free space the modem has reported
 * (its empty FIFO) minus the free space now.
 */
#define NXDN_QUEUE_MIN      3U     /* frames queued before the call is paced in real time */
#define NXDN_QUEUE_MAX      5U     /* frames queued above which the sender holds back */
#define NXDN_FRAME_MS       80U    /* one frame on the air */
#define NXDN_FILL_MS        10U    /* pause between frames while building the queue */

static uint32_t nxdnFrameDelay(const uint8_t space)
{
    static uint8_t peak = 0U;

    if (space > peak)
        peak = space;

    uint8_t queued = peak > space ? (uint8_t)(peak - space) : 0U;

    if (queued < NXDN_QUEUE_MIN)
        return NXDN_FILL_MS;                 /* start of a call or after a gap: fill quickly */
    if (queued > NXDN_QUEUE_MAX)
        return NXDN_FRAME_MS + 10U;          /* running ahead: let the modem drain a little */
    return NXDN_FRAME_MS - 10U;              /* in the band: just under real time keeps the queue topped up */
}

/*
 * YSF frame pacing.
 *
 * One TYPE_YSF_DATA frame is a whole 100 ms air frame (120 bytes) and the
 * modem's YSF FIFO holds ten of them. As for NXDN the aim is a small, steady
 * queue: enough to ride out network jitter and keep the transmitter fed, little
 * enough to keep the delay short. The depth of the queue is the largest free
 * space the modem has reported (its empty FIFO) minus the free space now.
 */
#define YSF_QUEUE_MIN       2U     /* frames queued before the call is paced in real time */
#define YSF_QUEUE_MAX       4U     /* frames queued above which the sender holds back */
#define YSF_FRAME_MS        100U   /* one frame on the air */
#define YSF_FILL_MS         10U    /* pause between frames while building the queue */

static uint32_t ysfFrameDelay(const uint8_t space)
{
    static uint8_t peak = 0U;

    if (space > peak)
        peak = space;

    uint8_t queued = peak > space ? (uint8_t)(peak - space) : 0U;

    if (queued < YSF_QUEUE_MIN)
        return YSF_FILL_MS;                  /* start of a call or after a gap: fill quickly */
    if (queued > YSF_QUEUE_MAX)
        return YSF_FRAME_MS + 10U;           /* running ahead: let the modem drain a little */
    return YSF_FRAME_MS - 10U;               /* in the band: just under real time keeps the queue topped up */
}

// Simple timer thread.
// Each loop through the while statement takes 1 millisecond.
void* timerThread(void* arg)
{
    uint8_t currDelay = 19;

    if (getTimer("frameDelay", 19) < 0)
    {
        if (debugM) fprintf(stderr, "Timer thread exited.\n");
        int iRet = 600;
        pthread_exit(&iRet);
        return NULL;
    }

    while (atomic_load(&running))
    {
        delay(1000);  // 1ms

        for (uint8_t i = 0; i < 10; i++)
        {
            pthread_mutex_lock(&timerMutex);
            if (timer[i].valid && timer[i].enabled)
            {
                if (timer[i].count >= timer[i].duration)
                {
                    timer[i].triggered = true;
                    timer[i].count     = 0;
                }
                else if (!timer[i].triggered)
                {
                    timer[i].count++;
                }
            }
            pthread_mutex_unlock(&timerMutex);
        }

        pthread_mutex_lock(&rxBufModeMutex);
        if (RingBuffer_dataSize(&rxModeBuffer) == 0)
            atomic_store(&bufReady, false);
        pthread_mutex_unlock(&rxBufModeMutex);

        if (strncasecmp(modem, "mmdvm", 5) == 0 && RingBuffer_dataSize(&rxModeBuffer) != 0)
        {
            //      if (strcmp(currentMode, "DSTAR") == 0)
            //      {
            if (isCurrentMode("NXDN"))
            {
                resetTimer("frameDelay", nxdnFrameDelay(getNXDNSpace()));
            }
            else if (isCurrentMode("YSF"))
            {
                resetTimer("frameDelay", ysfFrameDelay(getYSFSpace()));
            }
            else
            {
                uint8_t space1 = getModemSpace(NULL, 1);
                uint8_t space2 = getModemSpace(NULL, 2);
                uint8_t space  = MIN(space1, space2);
                //       fprintf(stderr, "***************** Modem space. %u   Delay: %u\n", space, currDelay);
                if (space < 10)
                {
                    if (currDelay > 19)
                        currDelay = 19;

                    resetTimer("frameDelay", ++currDelay);
                }
                else if (space > 12)
                {
                    if (currDelay <= 1)
                        currDelay = 2;

                    resetTimer("frameDelay", --currDelay);
                }
            }
            /*
            if (space < 10)
                resetTimer("frameDelay", 18);
            else if (space > 17)
                resetTimer("frameDelay", 12);
            else
                resetTimer("frameDelay", 17);
        }
        else if (isCurrentMode("M17"))
        {
            uint8_t space = getModemSpace(NULL, 1);
            if (space < 30)
                resetTimer("frameDelay", 16);
            else if (space > 60)
                resetTimer("frameDelay", 11);
            else
                resetTimer("frameDelay", 14);
        }
        else if (isCurrentMode("P25"))
        {
            uint8_t space = getModemSpace(NULL, 1);
            if (space < 10)
                resetTimer("frameDelay", 19);
            else if (space > 12)
                resetTimer("frameDelay", 8);
            else
                resetTimer("frameDelay", 12);
        }
        else if (isCurrentMode("DMR"))
        {
            uint8_t space1 = getModemSpace(NULL, 1);
            uint8_t space2 = getModemSpace(NULL, 2);
            uint8_t space  = MIN(space1, space2);
            if (space < 5)
                resetTimer("frameDelay", 60);
            else if (space > 8)
                resetTimer("frameDelay", 45);
            else
                resetTimer("frameDelay", 57);
        } */
        }
        else
        {
            if (getTimerDuration("frameDelay") != 19)
                resetTimer("frameDelay", 19);

            resetTimer("frameDelay", 0);
        }
    }

    if (isTimerTriggered("txTimeout") && atomic_load(&txOn))
    {
        atomic_store(&txOn, false);
        setCurrentMode("idle");
        setStatus(modemName, "main", "active_mode", "idle");
        pthread_mutex_lock(&rxBufModeMutex);
        RingBuffer_clear(&rxModeBuffer);
        pthread_mutex_unlock(&rxBufModeMutex);
        if (strncasecmp(modem, "mmdvm", 5) == 0)
        {
            uint8_t buffer[4];
            buffer[0] = 0xE0;
            buffer[1] = 0x04;
            buffer[2] = MODEM_MODE;
            buffer[3] = 0x00;  // IDLE_MODE
            pthread_mutex_lock(&modemComBufMutex);
            RingBuffer_addData(&modemCommandBuffer, buffer, 4);
            pthread_mutex_unlock(&modemComBufMutex);
        }
        resetTimer("txTimeout", 0);
        //       if (debugM)
        fprintf(stderr, "TX timeout. Setting mode to idle.\n");
    }
    else if (!atomic_load(&txOn))
        resetTimer("txTimeout", 0);

    fprintf(stderr, "Timer thread exited.\n");
    int iRet = 600;
    pthread_exit(&iRet);
    return NULL;
}

// Send data to protocol service.
// One thread per service connection.
/* "invalid header" in txModeBuffer: say what the buffer held, so the cause
 * (misaligned frame length, stray zeros, wrong producer) can be seen.
 * The caller holds txBufModeMutex and has already dropped the bad byte. */
static void logBadTxHeader(uint8_t first, uint8_t dropped)
{
    uint8_t next[8];
    uint16_t avail = (uint16_t)RingBuffer_dataSize(&txModeBuffer);
    uint16_t n = avail < sizeof next ? avail : (uint16_t)sizeof next;
    char hex[8 * 3 + 1] = "";

    if (n)
        RingBuffer_peek(&txModeBuffer, next, n);
    for (uint16_t i = 0; i < n; i++)
        snprintf(hex + i * 3, 4, "%02X ", next[i]);
    fprintf(stderr, "txModeBuffer: invalid header [%02X]  [%02X]  modem=%s mode=%s left=%u next: %s\n",
            first, dropped, modem, currentMode, (unsigned)avail, hex);
}

void* modeRxThread(void* arg)
{
    uint8_t conn_id = (intptr_t)arg;
    int sockfd      = hostClient[conn_id].sockfd;
    bool found      = false;

    while (hostClient[conn_id].active && running)
    {
        delay(500);

        pthread_mutex_lock(&txBufModeMutex);
        if (RingBuffer_dataSize(&txModeBuffer) >= 4)
        {
            uint8_t buf[1];
            RingBuffer_peek(&txModeBuffer, buf, 1);
            if (buf[0] != 0x61)
            {
                uint8_t tmp[1];
                RingBuffer_getData(&txModeBuffer, tmp, 1);
                logBadTxHeader(buf[0], tmp[0]);   /* caller holds txBufModeMutex */
                pthread_mutex_unlock(&txBufModeMutex);
                continue;
            }
            if (strncasecmp(modem, "mmdvm", 5) == 0)
            {
                uint8_t byte[6];
                uint16_t len = 0;
                RingBuffer_peek(&txModeBuffer, byte, 6);
                len = (byte[1] << 8) + byte[2];
                if (len < 6 || len > 1024)
                {
                    uint8_t junk[1];
                    RingBuffer_getData(&txModeBuffer, junk, 1);
                    fprintf(stderr, "txModeBuffer: bad length %u, resync\n", (unsigned)len);
                    pthread_mutex_unlock(&txBufModeMutex);
                    continue;
                }
                if (RingBuffer_dataSize(&txModeBuffer) >= len && strncasecmp(hostClient[conn_id].mode, (char*)byte + 4, 2) == 0)
                {
                    uint8_t buf[len];
                    RingBuffer_getData(&txModeBuffer, buf, len);
                    if (write(sockfd, buf, len) < 0)
                    {
                        pthread_mutex_unlock(&txBufModeMutex);
                        fprintf(stderr, "ERROR: remote disconnect\n");
                        break;
                    }
                }
            }
            else
            {
                uint8_t byte[3];
                uint16_t len = 0;
                RingBuffer_peek(&txModeBuffer, byte, 3);
                len = (byte[1] << 8) + byte[2] + 1;  // length plus conn id byte.
                if (len < 4 || len > 1024)
                {
                    /* corrupt header: drop a byte and resync instead of building a
                     * huge on-stack array / reading past the data in the ring */
                    uint8_t junk[1];
                    RingBuffer_getData(&txModeBuffer, junk, 1);
                    fprintf(stderr, "txModeBuffer: bad length %u, resync\n", (unsigned)len);
                    pthread_mutex_unlock(&txBufModeMutex);
                    continue;
                }
                if (RingBuffer_dataSize(&txModeBuffer) >= len && strcasecmp(hostClient[conn_id].mode, currentMode) == 0)
                {
                    uint8_t buf[len];
                    RingBuffer_getData(&txModeBuffer, buf, len);
                    len--;  // do not include conn id byte.
                    if (write(sockfd, buf, len) < 0)
                    {
                        pthread_mutex_unlock(&txBufModeMutex);
                        fprintf(stderr, "ERROR: remote disconnect\n");
                        break;
                    }
                }
            }
        }
        pthread_mutex_unlock(&txBufModeMutex);

        // check for out going commands.
        pthread_mutex_lock(&hostClient[conn_id].commandMutex);
        if (RingBuffer_dataSize(&hostClient[conn_id].command) >= 5)
        {
            uint8_t buf[1];
            RingBuffer_peek(&hostClient[conn_id].command, buf, 1);
            if (buf[0] != 0x61)
            {
                RingBuffer_getData(&hostClient[conn_id].command, buf, 1);
                pthread_mutex_unlock(&hostClient[conn_id].commandMutex);
                fprintf(stderr, "modeTx invalid header.\n");
                continue;
            }
            uint8_t byte[3];
            uint16_t len = 0;
            RingBuffer_peek(&hostClient[conn_id].command, byte, 3);
            len = (byte[1] << 8) + byte[2];
            if (len < 3 || len > 1024)
            {
                RingBuffer_getData(&hostClient[conn_id].command, byte, 1);   /* resync */
                pthread_mutex_unlock(&hostClient[conn_id].commandMutex);
                fprintf(stderr, "modeTx bad command length %u\n", (unsigned)len);
                continue;
            }
            if (RingBuffer_dataSize(&hostClient[conn_id].command) >= len)
            {
                uint8_t buf[len];
                RingBuffer_getData(&hostClient[conn_id].command, buf, len);

                if (write(sockfd, buf, len) < 0)
                {
                    pthread_mutex_unlock(&hostClient[conn_id].commandMutex);
                    fprintf(stderr, "ERROR: remote disconnect\n");
                    break;
                }
            }
        }
        pthread_mutex_unlock(&hostClient[conn_id].commandMutex);
    }

    hostClient[conn_id].active = false;
    delay(50000);
    fprintf(stderr, "Mode RX thread exited. Connection Id: %d\n", conn_id);
    int iRet = 500;
    pthread_exit(&iRet);
    return NULL;
}

// Queue up out going bytes for modem.
void* modeTxThread(void* arg)
{
    while (atomic_load(&running))
    {
        delay(openmtPacing() ? 500 : 3000);

        if (isTimerTriggered("status"))
        {
            if (RingBuffer_freeSpace(&modemTxBuffer) >= 3 && RingBuffer_dataSize(&rxModeBuffer) < 3)
            {
                uint8_t buf[3];
                buf[0] = 0x61;
                buf[1] = 0x03;
                buf[2] = OPENMT_GET_STATUS;
                //     pthread_mutex_lock(&modemTxBufMutex);
                //     RingBuffer_addData(&modemTxBuffer, buf, 3);
                //     pthread_mutex_unlock(&modemTxBufMutex);
            }
            resetTimer("status", 0);
        }

        if (RingBuffer_dataSize(&rxModeBuffer) >= 3)  // && frameTimeout && bufReady)
        {
            uint8_t buf[3];
            pthread_mutex_lock(&rxBufModeMutex);
            RingBuffer_peek(&rxModeBuffer, buf, 1);
            if (buf[0] != 0x61)
            {
                uint8_t tmp[1];
                RingBuffer_getData(&rxModeBuffer, tmp, 1);
                pthread_mutex_unlock(&rxBufModeMutex);
                fprintf(stderr, "RX Mode buffer invalid header [%02X]\n", tmp[0]);
                resetTimer("frameDelay", 0);
                continue;
            }
            RingBuffer_peek(&rxModeBuffer, buf, 3);
            pthread_mutex_unlock(&rxBufModeMutex);
            uint8_t len = buf[1];
            if (len < 3)
            {
                uint8_t junk[1];                   /* corrupt length: resync */
                pthread_mutex_lock(&rxBufModeMutex);
                RingBuffer_getData(&rxModeBuffer, junk, 1);
                pthread_mutex_unlock(&rxBufModeMutex);
                fprintf(stderr, "RX Mode buffer bad length %u, resync\n", (unsigned)len);
                continue;
            }
            if (RingBuffer_dataSize(&rxModeBuffer) >= len)
            {
                /* Only SAMP_DATA is paced by the modem's free space; EOT,
                 * mode and config frames always go out (in order). */
                bool paced = openmtPacing() && ((buf[2] & 0xf0) == OPENMT_SAMP_DATA);
                bool okToSend = paced ? atomic_load(&isModemSpace) : (openmtPacing() || atomic_load(&isModemSpace));
                if (okToSend && RingBuffer_freeSpace(&modemTxBuffer) > len + 6 &&
                    (!paced || RingBuffer_dataSize(&modemTxBuffer) < 600))
                {
                    uint8_t buf[len];
                    openmtStalled = false;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_getData(&rxModeBuffer, buf, len);
                    pthread_mutex_unlock(&rxBufModeMutex);
                    pthread_mutex_lock(&modemTxBufMutex);
                    RingBuffer_addData(&modemTxBuffer, buf, len);
                    if (paced)
                        openmtSampQueued++;
                    pthread_mutex_unlock(&modemTxBufMutex);
                    if (paced)
                    {
                        if (atomic_fetch_sub(&openmtCredit, 1) - 1 <= (int)OPENMT_MIN_FREE)
                        {
                            /* credit used up: wait for fresh free-space info */
                            atomic_store(&isModemSpace, false);
                            requestModemStatus(true);
                        }
                        else
                            requestModemStatus(false);  /* rate-limited refresh */
                    }
                }
                else if (paced && !atomic_load(&isModemSpace))
                {
                    /* Stall watchdog: waiting for credit.  If it takes more
                     * than 250 ms the request/reply bookkeeping has probably
                     * drifted (a lost status reply makes every later credit
                     * look too small), so drop it and ask afresh; report once
                     * a second so the cause shows in the log. */
                    static struct timespec stallT0;
                    static time_t          lastRpt;
                    struct timespec        nowT;
                    clock_gettime(CLOCK_MONOTONIC, &nowT);
                    if (!openmtStalled)
                    {
                        openmtStalled = true;
                        stallT0 = nowT;
                    }
                    else if ((nowT.tv_sec - stallT0.tv_sec) * 1000L + (nowT.tv_nsec - stallT0.tv_nsec) / 1000000L > 250)
                    {
                        if (time(NULL) != lastRpt)
                        {
                            lastRpt = time(NULL);
                            fprintf(stderr, "openmt pacing stall: lastFree=%d credit=%d reqPending=%u queued=%u rxBuf=%u\n",
                                    atomic_load(&openmtLastFree), atomic_load(&openmtCredit), openmtReqCount,
                                    openmtSampQueued, (unsigned)RingBuffer_dataSize(&rxModeBuffer));
                        }
                        openmtResyncPacing();
                        stallT0 = nowT;
                    }
                    requestModemStatus(false);  /* keep polling until space frees up */
                }
                if (RingBuffer_freeSpace(&modemTxBuffer) >= 3)
                {
                    uint8_t buf[3];
                    buf[0] = 0x61;
                    buf[1] = 0x03;
                    buf[2] = OPENMT_GET_STATUS;
                    //       pthread_mutex_lock(&modemTxBufMutex);
                    //       RingBuffer_addData(&modemTxBuffer, buf, 3);
                    //       pthread_mutex_unlock(&modemTxBufMutex);
                }
            }
            resetTimer("frameDelay", 0);
        }

        if (RingBuffer_dataSize(&modemCommandBuffer) >= 3)
        {
            pthread_mutex_lock(&modemComBufMutex);
            uint8_t buf[3];
            RingBuffer_peek(&modemCommandBuffer, buf, 1);
            if (buf[0] != 0x61)
            {
                uint8_t tmp[1];
                RingBuffer_getData(&modemCommandBuffer, tmp, 1);
                fprintf(stderr, "modem command: invalid header [%02X]\n", buf[0]);
                pthread_mutex_unlock(&modemComBufMutex);
                continue;
            }
            RingBuffer_peek(&modemCommandBuffer, buf, 2);
            uint16_t len = buf[1];
            if (len < 3)
            {
                uint8_t junk[1];
                RingBuffer_getData(&modemCommandBuffer, junk, 1);   /* resync */
                pthread_mutex_unlock(&modemComBufMutex);
                fprintf(stderr, "modem command: bad length %u\n", (unsigned)len);
                continue;
            }
            if (RingBuffer_dataSize(&modemCommandBuffer) >= len)
            {
                if (RingBuffer_freeSpace(&modemTxBuffer) >= len)
                {
                    uint8_t buf[len];
                    RingBuffer_getData(&modemCommandBuffer, buf, len);
                    pthread_mutex_lock(&modemTxBufMutex);
                    RingBuffer_addData(&modemTxBuffer, buf, len);
                    pthread_mutex_unlock(&modemTxBufMutex);
                }
            }
            pthread_mutex_unlock(&modemComBufMutex);
        }
    }
    fprintf(stderr, "Mode TX thread exited.\n");
    int iRet = 400;
    pthread_exit(&iRet);
    return NULL;
}

// MMDVM modem specific function.
// Queue up out going bytes for modem.
void* modeMMDVMTxThread(void* arg)
{
    while (atomic_load(&running))
    {
        delay(200);

        if (isTimerTriggered("status"))
        {
            if (RingBuffer_dataSize(&rxModeBuffer) < 3)
            {
                uint8_t buf[3];
                buf[0]  = 0xE0;
                buf[1]  = 0x03;
                buf[2]  = MODEM_STATUS;
                int ret = write(serialModemFd, buf, 3);
                if (ret != 3)
                {
                    fprintf(stderr, "modem TX write failed.\n");
                }
            }
            resetTimer("status", 0);
        }

        if (RingBuffer_dataSize(&rxModeBuffer) >= 3 && isTimerTriggered("frameDelay") && atomic_load(&bufReady))
        {
            uint8_t buf[3];
            pthread_mutex_lock(&rxBufModeMutex);
            RingBuffer_peek(&rxModeBuffer, buf, 1);
            if (buf[0] != 0xE0)
            {
                uint8_t tmp[1];
                RingBuffer_getData(&rxModeBuffer, tmp, 1);
                pthread_mutex_unlock(&rxBufModeMutex);
                fprintf(stderr, "rxModeBuffer invalid header [%02X]\n", tmp[0]);
                resetTimer("frameDelay", 0);
                continue;
            }
            RingBuffer_peek(&rxModeBuffer, buf, 2);
            uint8_t len = buf[1];
            if (RingBuffer_dataSize(&rxModeBuffer) >= len)
            {
                uint8_t space = 0;
                RingBuffer_peek(&rxModeBuffer, buf, 3);
                if (buf[2] == TYPE_DMR_DATA2)
                    space = getModemSpace(NULL, 2);
                else
                    space = getModemSpace(NULL, 1);

                if (space > 1 && RingBuffer_freeSpace(&modemTxBuffer) >= (len + 3))
                {
                    uint8_t buf[len];
                    RingBuffer_getData(&rxModeBuffer, buf, len);
                    int ret = write(serialModemFd, buf, len);
                    if (ret != len)
                    {
                        fprintf(stderr, "modem TX write failed.\n");
                    }
                }
                uint8_t buf[3];
                buf[0]  = 0xE0;
                buf[1]  = 0x03;
                buf[2]  = MODEM_STATUS;
                int ret = write(serialModemFd, buf, 3);
                if (ret != 3)
                {
                    fprintf(stderr, "modem TX write failed.\n");
                }
            }
            else
            {
                pthread_mutex_unlock(&rxBufModeMutex);
                continue;
            }
            pthread_mutex_unlock(&rxBufModeMutex);
            resetTimer("frameDelay", 0);
        }

        pthread_mutex_lock(&modemComBufMutex);
        if (RingBuffer_dataSize(&modemCommandBuffer) >= 3)
        {
            uint8_t buf[2];
            RingBuffer_peek(&modemCommandBuffer, buf, 1);
            if (buf[0] != 0xE0)
            {
                fprintf(stderr, "modem command: invalid header [%02X]\n", buf[0]);
                pthread_mutex_unlock(&modemComBufMutex);
                continue;
            }
            RingBuffer_peek(&modemCommandBuffer, buf, 2);
            uint8_t len = buf[1];
            if (RingBuffer_dataSize(&modemCommandBuffer) >= len)
            {
                if (RingBuffer_freeSpace(&modemTxBuffer) >= len)
                {
                    uint8_t buf[len];
                    RingBuffer_getData(&modemCommandBuffer, buf, len);
                    int ret = write(serialModemFd, buf, len);
                    if (ret != len)
                    {
                        fprintf(stderr, "modem TX write failed.\n");
                    }
                }
            }
        }
        pthread_mutex_unlock(&modemComBufMutex);
    }

    fprintf(stderr, "Mode TX thread exited.\n");
    int iRet = 400;
    pthread_exit(&iRet);
    return NULL;
}

#ifndef OPENMT_STATUS_GAP_US
#define OPENMT_STATUS_GAP_US 0
#endif

/* Idle time left on the line after every sample frame, so the modem's UART
 * never sees a 100% back-to-back stream (a frame is 2.1 ms on the wire, the
 * frame period 4 ms).  0 = off. */
#ifndef OPENMT_FRAME_GAP_US
#define OPENMT_FRAME_GAP_US 0
#endif

/* Raspberry Pi PL011 (ttyAMA0) TX race workaround.
 *
 * The kernel's amba-pl011 interrupt handler reads the interrupt status once,
 * handles RX first -- dropping the port lock while it pushes the RX data to
 * the tty layer -- and then, on the TX status it read before, writes up to 8
 * characters into the TX FIFO without checking whether it is full.  A write()
 * from this process on another CPU in that window fills the FIFO through
 * pl011_start_tx(), and the handler's characters then land in a full FIFO
 * and are lost, although the kernel counts them as sent.  The modem received
 * frames 4-7 bytes short (and replaced them by silence: the M17 drop outs).
 *
 * The handler can only act on stale TX status while the port's queue still
 * holds data, so a write() made when the queue is empty is safe.  Wait for
 * that before every write.  TIOCOUTQ reads the queue length under the same
 * port lock.  The 16 byte FIFO still holds ~170 us of data when the queue
 * runs empty, so the line hardly ever idles. */
#ifndef MODEM_TXQ_GATE
#define MODEM_TXQ_GATE 1
#endif
#define MODEM_TXQ_POLL_US    50U        /* poll interval while waiting     */
#define MODEM_TXQ_TIMEOUT_US 50000U     /* give up waiting after 50 ms     */

static void waitModemTxQueueEmpty(void)
{
#if MODEM_TXQ_GATE
    static uint32_t timeouts;
    uint32_t waited = 0;
    int      pending = 0;

    if (strncasecmp(modem, "openmt", 6) != 0 || strcasecmp(modem, "openmtsx") == 0)
        return;                         /* only the real UART link          */

    while (ioctl(serialModemFd, TIOCOUTQ, &pending) == 0 && pending > 0)
    {
        if (waited >= MODEM_TXQ_TIMEOUT_US)
        {
            if ((timeouts++ % 100U) == 0U)
                fprintf(stderr, "Modem_Host: serial TX queue not draining (%d bytes), writing anyway\n", pending);
            return;
        }
        delay(MODEM_TXQ_POLL_US);
        waited += MODEM_TXQ_POLL_US;
    }
#endif
}

// Send queued up bytes to modem.
void* modemTxThread(void* arg)
{
    while (atomic_load(&running))
    {
        delay(10);

        if (RingBuffer_dataSize(&modemTxBuffer) < 3)
            continue;

        uint8_t buf[256];
        uint8_t len = 0;

        /* Header peek and frame removal happen under ONE lock hold so a
         * producer can never change the buffer in between. */
        pthread_mutex_lock(&modemTxBufMutex);
        if (!RingBuffer_peek(&modemTxBuffer, buf, 3))
        {
            pthread_mutex_unlock(&modemTxBufMutex);
            continue;
        }
        if (buf[0] != 0x61)
        {
            uint8_t tmp[1];
            RingBuffer_getData(&modemTxBuffer, tmp, 1);
            pthread_mutex_unlock(&modemTxBufMutex);
            fprintf(stderr, "modem TX invalid header [%02X]\n", tmp[0]);
            continue;
        }
        len = buf[1];
        if (len < 3)
        {
            uint8_t tmp[1];
            RingBuffer_getData(&modemTxBuffer, tmp, 1);
            pthread_mutex_unlock(&modemTxBufMutex);
            fprintf(stderr, "modem length invalid. [%02X]\n", len);
            continue;
        }
        if (RingBuffer_dataSize(&modemTxBuffer) < len)
        {
            pthread_mutex_unlock(&modemTxBufMutex);
            continue;
        }
        RingBuffer_getData(&modemTxBuffer, buf, len);
        pthread_mutex_unlock(&modemTxBufMutex);

        /* sanity: a SAMP_DATA frame is always 196 bytes (4 + 96 q15) */
        if ((buf[2] & 0xf0) == OPENMT_SAMP_DATA && len != 0xC4)
            fprintf(stderr, "Modem_Host: odd frame to modem: len=%u type=%02X\n", (unsigned)len, (unsigned)buf[2]);
        waitModemTxQueueEmpty();        /* PL011 driver race, see above */
        int ret = write(serialModemFd, buf, len);
        if (ret != len)
        {
            fprintf(stderr, "modem TX write failed. (%d of %u)\n", ret, (unsigned)len);
        }
        /* Diagnostic: OPENMT_TXLOG=/path keeps a copy of every byte written
         * to the modem, and a summary is printed every 5 s. */
        {
            static FILE*    txlog;
            static int      txlogInit;
            static uint32_t nSamp, nOther, nBytes;
            static time_t   lastRep;

            if (!txlogInit)
            {
                const char* path = getenv("OPENMT_TXLOG");
                txlogInit        = 1;
                if (path && *path)
                    txlog = fopen(path, "wb");
            }
            if (txlog)
            {
                /* companion file <path>.t: one uint32 per frame, microseconds
                 * since the first frame, to replay the exact timing */
                static FILE*           txt;
                static struct timespec t0;
                struct timespec        tn;
                uint32_t               us;

                if (!txt)
                {
                    char tp[512];
                    snprintf(tp, sizeof tp, "%s.t", getenv("OPENMT_TXLOG"));
                    txt = fopen(tp, "wb");
                    clock_gettime(CLOCK_MONOTONIC, &t0);
                }
                clock_gettime(CLOCK_MONOTONIC, &tn);
                us = (uint32_t)((tn.tv_sec - t0.tv_sec) * 1000000L + (tn.tv_nsec - t0.tv_nsec) / 1000L);
                if (txt)
                {
                    fwrite(&us, 4, 1, txt);
                    fflush(txt);
                }
                fwrite(buf, 1, len, txlog);
                fflush(txlog);
            }
            if ((buf[2] & 0xf0) == OPENMT_SAMP_DATA)
                nSamp++;
            else
                nOther++;
            nBytes += len;
            if (txlog && time(NULL) - lastRep >= 5)
            {
                lastRep = time(NULL);
                fprintf(stderr, "Modem_Host: sent to modem: samp=%u other=%u bytes=%u\n", nSamp, nOther, nBytes);
            }
        }
        /* Experiment: the modem was seen losing bytes of the sample frame
         * that directly follows a status request.  Leave a gap after every
         * status request (OPENMT_STATUS_GAP_US, 0 = off). */
        if (len == 3 && (buf[2] & 0xf0) == OPENMT_GET_STATUS && OPENMT_STATUS_GAP_US)
            delay(OPENMT_STATUS_GAP_US);
        else if ((buf[2] & 0xf0) == OPENMT_SAMP_DATA && OPENMT_FRAME_GAP_US)
            delay(OPENMT_FRAME_GAP_US);
    }

    fprintf(stderr, "Modem TX thread exited.\n");
    int iRet = 300;
    pthread_exit(&iRet);
    return NULL;
}

// Read commands queued up in database from dashboard.
void* commandThread(void* arg)
{
    while (atomic_load(&running))
    {
        if (isCurrentModeIdle())
            delay(500);
        else
            delay(50000);

        if (isCurrentModeIdle())
        {
            if (strncasecmp(modem, "openmt", 6) == 0)
            {
                pthread_mutex_lock(&txBufModeMutex);
                if (RingBuffer_dataSize(&txModeBuffer) >= 4)
                {
                    uint8_t buf[1];
                    RingBuffer_peek(&txModeBuffer, buf, 1);
                    if (buf[0] != 0x61)
                    {
                        uint8_t tmp[1];
                        RingBuffer_getData(&txModeBuffer, tmp, 1);
                        logBadTxHeader(buf[0], tmp[0]);   /* caller holds txBufModeMutex */
                        pthread_mutex_unlock(&txBufModeMutex);
                        continue;
                    }
                    uint8_t byte[3];
                    uint16_t len = 0;
                    RingBuffer_peek(&txModeBuffer, byte, 3);
                    len = (byte[1] << 8) + byte[2] + 1;  // length plus conn id byte.
                    uint8_t tmp[len];
                    RingBuffer_peek(&txModeBuffer, tmp, len);
                    if (RingBuffer_dataSize(&txModeBuffer) >= len)
                    {
                        uint8_t buf[len];
                        RingBuffer_getData(&txModeBuffer, buf, len);
                        len--;  // do not include conn id byte.
                        for (uint8_t i = 0; i < MAX_CLIENT_CONNECTIONS; i++)
                        {
                            if (hostClient[i].active)
                            {
                                if (write(hostClient[i].sockfd, buf, len) < 0)
                                {
                                    pthread_mutex_unlock(&txBufModeMutex);
                                    fprintf(stderr, "ERROR: remote disconnect\n");
                                    break;
                                }
                            }
                        }
                    }
                }
                pthread_mutex_unlock(&txBufModeMutex);
            }
        }

        if (isTimerTriggered("chkComTimeout"))
        {
            char parameter[31];
            readDashbCommand(modemName, "modemHost", parameter);
            if (strcasecmp(parameter, "setConfig") == 0)
            {
                if (strcasecmp(modem, "mmdvmhs") == 0)
                    set_ConfigHS(modemName);
                else if (strcasecmp(modem, "mmdvm") == 0)
                    set_Config(modemName);
                else if (strcasecmp(modem, "openmtsx") == 0)
                {
                    char tmp[16];
                    uint32_t rx_freq_hz, tx_freq_hz;
                    uint8_t lna_gain_db, pga_gain_db;
                    int8_t mixer_gain_x2, dac_gain_db;
                    uint16_t rx_pll_bw_khz, tx_pll_bw_khz;
                    uint8_t invert_flags = 0;
                    uint8_t buf[21];

                    readHostConfig(modemName, "config", "rxFrequency", tmp);
                    rx_freq_hz = (uint32_t)strtoul(tmp, NULL, 10);
                    readHostConfig(modemName, "config", "txFrequency", tmp);
                    tx_freq_hz = (uint32_t)strtoul(tmp, NULL, 10);
                    readHostConfig(modemName, "config", "rxLnaGain", tmp);
                    lna_gain_db = (uint8_t)atoi(tmp);
                    readHostConfig(modemName, "config", "rxPgaGain", tmp);
                    pga_gain_db = (uint8_t)atoi(tmp);
                    readHostConfig(modemName, "config", "txMixerGain", tmp);
                    mixer_gain_x2 = (int8_t)(atof(tmp) * 2.0); /* 2dB steps */
                    readHostConfig(modemName, "config", "txDacGain", tmp);
                    dac_gain_db = (int8_t)atoi(tmp);
                    readHostConfig(modemName, "config", "rxPllBw", tmp);
                    rx_pll_bw_khz = (uint16_t)atoi(tmp);
                    readHostConfig(modemName, "config", "txPllBw", tmp);
                    tx_pll_bw_khz = (uint16_t)atoi(tmp);
                    readHostConfig(modemName, "config", "rxInvert", tmp);
                    if (strcasecmp(tmp, "true") == 0)
                        invert_flags |= 0x01U;
                    readHostConfig(modemName, "config", "txInvert", tmp);
                    if (strcasecmp(tmp, "true") == 0)
                        invert_flags |= 0x02U;
                    readHostConfig(modemName, "config", "mode", tmp);

                    buf[0] = 0x61;
                    buf[1] = 21; /* total frame length, including 0x61 itself */
                    buf[2] = OPENMT_SET_CONFIG;
                    buf[3] = 1; /* payload version */
                    buf[4] = (uint8_t)(rx_freq_hz >> 24);
                    buf[5] = (uint8_t)(rx_freq_hz >> 16);
                    buf[6] = (uint8_t)(rx_freq_hz >> 8);
                    buf[7] = (uint8_t)rx_freq_hz;
                    buf[8] = (uint8_t)(tx_freq_hz >> 24);
                    buf[9] = (uint8_t)(tx_freq_hz >> 16);
                    buf[10] = (uint8_t)(tx_freq_hz >> 8);
                    buf[11] = (uint8_t)tx_freq_hz;
                    buf[12] = lna_gain_db;
                    buf[13] = pga_gain_db;
                    buf[14] = (uint8_t)mixer_gain_x2;
                    buf[15] = (uint8_t)dac_gain_db;
                    buf[16] = (uint8_t)(rx_pll_bw_khz >> 8);
                    buf[17] = (uint8_t)rx_pll_bw_khz;
                    buf[18] = (uint8_t)(tx_pll_bw_khz >> 8);
                    buf[19] = (uint8_t)tx_pll_bw_khz;
                    buf[20] = invert_flags;
                    pthread_mutex_lock(&modemTxBufMutex);
                    RingBuffer_addData(&modemTxBuffer, buf, 21);
                    pthread_mutex_unlock(&modemTxBufMutex);
             //       write(serialModemFd, buf, 21);
                }

                uint8_t buffer[9];
                buffer[0] = 0x61;
                buffer[1] = 0x00;
                buffer[2] = 0x09;
                buffer[3] = 0x04;
                memcpy(buffer + 4, TYPE_COMMAND, 4);
                if (modem_duplex)
                    buffer[8] = COMM_SET_DUPLEX;
                else
                    buffer[8] = COMM_SET_SIMPLEX;
                for (uint8_t i = 0; i < MAX_CLIENT_CONNECTIONS; i++)
                {
                    if (hostClient[i].active)
                    {
                        pthread_mutex_lock(&hostClient[i].commandMutex);
                        RingBuffer_addData(&hostClient[i].command, buffer, 9);
                        pthread_mutex_unlock(&hostClient[i].commandMutex);
                    }
                }

                ackDashbCommand(modemName, "modemHost", "success");
            }
            resetTimer("chkComTimeout", 0);
        }
    }
    fprintf(stderr, "Command thread exited.\n");
    int iRet = 350;
    pthread_exit(&iRet);
    return NULL;
}

// Handle incoming bytes from protocol service.
// One thread per service.
void* processClientSocket(void* arg)
{
    uint8_t buffer[BUFFER_LENGTH];
    uint8_t conn_id  = (intptr_t)arg;
    int sockFd       = hostClient[conn_id].sockfd;
    uint16_t respLen = 0;
    uint8_t type     = 0;
    uint8_t typeLen  = 0;
    uint16_t offset  = 0;
    ssize_t len      = 0;
    struct timespec ts;
    uint8_t currType = 0;
    uint8_t lastType = 0;
    bzero(buffer, BUFFER_LENGTH);

    int flags = fcntl(sockFd, F_GETFL, 0);
    if (flags == -1)
    {
        perror("fcntl F_GETFL");
    }

    while (hostClient[conn_id].active && running)
    {
        len = read(sockFd, buffer, 1);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            /* a host went away (restart, crash): drop this connection only,
             * never exit() the whole Modem_Host */
            perror("ERROR: client connection closed remotely");
            break;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            delay(5);
            //       continue;
        }

        if (len != 1)
        {
            fprintf(stderr, "Modem_Host: error when reading from client, errno=%d\n", errno);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "Modem_Host: unknown byte from client, 0x%02X\n", buffer[0]);
            continue;
        }

        offset = 0;
        while (offset < 3)
        {
            len = read(sockFd, buffer + 1 + offset, 3 - offset);
            if (len == 0) break;
            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                delay(5);
            else
                offset += len;
        }

        if (len == 0) break;

        respLen = (buffer[1] << 8) + buffer[2];

        offset += 1;
        while (offset < respLen)
        {
            len = read(sockFd, buffer + offset, respLen - offset);
            if (len == 0) break;
            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                delay(5);
            else
                offset += len;
        }

        if (len == 0) break;

        typeLen = buffer[3];
        if (typeLen == 1)
            type = buffer[4];

        if (memcmp(buffer + 4, "MODE", typeLen) == 0)
        {
            memcpy(mode[conn_id].name, buffer + 8, 11);
            memcpy(mode[conn_id].modem_type, buffer + 19, 6);
            //            mode[conn_id].use_rx_dc_filter = buffer[25];
            //            mode[conn_id].use_lp_filter = buffer[26];
            mode[conn_id].txLevel = buffer[25];
            strcpy(hostClient[conn_id].mode, mode[conn_id].name);
            addMode(modemName, "main", mode[conn_id].name);
            fprintf(stderr, "Received mode data for [%s].\n", mode[conn_id].name);
            setProtocol(mode[conn_id].name, true);

            if (debugM)
                dump((char*)"Modem mode data:", (unsigned char*)buffer, 62);

            if (strncasecmp(modem, "mmdvm", 5) == 0)
            {
                //     if (strcmp(currentMode, "idle") != 0)
                {
                    buffer[0] = 0xE0;
                    buffer[1] = 0x04;
                    buffer[2] = MODEM_MODE;
                    buffer[3] = 0x00;  // IDLE_MODE
                    pthread_mutex_lock(&modemComBufMutex);
                    RingBuffer_addData(&modemCommandBuffer, buffer, 4);
                    pthread_mutex_unlock(&modemComBufMutex);

                    buffer[0] = 0x61;
                    buffer[1] = 0x00;
                    buffer[2] = 0x09;
                    buffer[3] = 0x04;
                    memcpy(buffer + 4, TYPE_COMMAND, 4);
                    if (modem_duplex)
                        buffer[8] = COMM_SET_DUPLEX;
                    else
                        buffer[8] = COMM_SET_SIMPLEX;
                    //    RingBuffer_addData(&rxModeBuffer, buffer, 9);
                    setCurrentMode("idle");
                    pthread_mutex_lock(&hostClient[conn_id].commandMutex);
                    RingBuffer_addData(&hostClient[conn_id].command, buffer, 9);
                    pthread_mutex_unlock(&hostClient[conn_id].commandMutex);
                    char curMode[11];
                    getCurrentMode(curMode, sizeof(curMode));
                    setStatus(modemName, "main", "active_mode", curMode);
                }
            }
            else
            {
                if (strcasecmp(modem, "openmths") == 0 || strcasecmp(modem, "openmtsx") == 0)
                {
                    buffer[0] = 0x61;
                    buffer[1] = 0x3e;
                    buffer[2] = OPENMT_ADD_MODE;
                    buffer[3] = conn_id;
                    memcpy(buffer + 4, buffer + 26, 58);
                    pthread_mutex_lock(&modemComBufMutex);
                    RingBuffer_addData(&modemCommandBuffer, buffer, 62);
                    pthread_mutex_unlock(&modemComBufMutex);
                }

                buffer[0] = 0x61;
                buffer[1] = 0x00;
                buffer[2] = 0x09;
                buffer[3] = 0x04;
                memcpy(buffer + 4, TYPE_COMMAND, 4);
                if (modem_duplex)
                    buffer[8] = COMM_SET_DUPLEX;
                else
                    buffer[8] = COMM_SET_SIMPLEX;
                pthread_mutex_lock(&hostClient[conn_id].commandMutex);
                RingBuffer_addData(&hostClient[conn_id].command, buffer, 9);
                pthread_mutex_unlock(&hostClient[conn_id].commandMutex);
            }
            continue;
        }

        clock_gettime(CLOCK_REALTIME, &ts);

        // *********** Convert from OpenMT type to modem specific type ***********
        if (memcmp(buffer + 4, TYPE_LSF_M17, typeLen) == 0)
            type = TYPE_M17_LSF;
        else if (memcmp(buffer + 4, TYPE_STREAM_M17, typeLen) == 0)
            type = TYPE_M17_STREAM;
        else if (memcmp(buffer + 4, TYPE_PACKET_M17, typeLen) == 0)
            type = TYPE_M17_PACKET;
        else if (memcmp(buffer + 4, TYPE_EOT_M17, typeLen) == 0)
            type = TYPE_M17_EOT;
        else if (memcmp(buffer + 4, TYPE_HEADER_DSTAR, typeLen) == 0)
            type = TYPE_DSTAR_HEADER;
        else if (memcmp(buffer + 4, TYPE_DATA_DSTAR, typeLen) == 0)
            type = TYPE_DSTAR_DATA;
        else if (memcmp(buffer + 4, TYPE_EOT_DSTAR, typeLen) == 0)
            type = TYPE_DSTAR_EOT;
        else if (memcmp(buffer + 4, TYPE_HDR_P25, typeLen) == 0)
            type = TYPE_P25_HDR;
        else if (memcmp(buffer + 4, TYPE_LDU_P25, typeLen) == 0)
            type = TYPE_P25_LDU;
        else if (memcmp(buffer + 4, TYPE_LOST_P25, typeLen) == 0)
            type = TYPE_P25_LOST;
        else if (memcmp(buffer + 4, TYPE_DATA_YSF, typeLen) == 0)
            type = TYPE_YSF_DATA;
        else if (memcmp(buffer + 4, TYPE_LOST_YSF, typeLen) == 0)
            type = TYPE_YSF_LOST;
        else if (memcmp(buffer + 4, TYPE_DATA_NXDN, typeLen) == 0)
            type = TYPE_NXDN_DATA;
        else if (memcmp(buffer + 4, TYPE_LOST_NXDN, typeLen) == 0)
            type = TYPE_NXDN_LOST;
        else if (memcmp(buffer + 4, TYPE_DATA1_DMR, typeLen) == 0)
            type = TYPE_DMR_DATA1;
        else if (memcmp(buffer + 4, TYPE_DATA2_DMR, typeLen) == 0)
            type = TYPE_DMR_DATA2;
        else if (memcmp(buffer + 4, TYPE_LOST1_DMR, typeLen) == 0)
            type = TYPE_DMR_LOST1;
        else if (memcmp(buffer + 4, TYPE_LOST2_DMR, typeLen) == 0)
            type = TYPE_DMR_LOST2;
        else if (memcmp(buffer + 4, TYPE_ACK, typeLen) == 0)
            type = NET_ACK;
        else if (memcmp(buffer + 4, TYPE_NACK, typeLen) == 0)
            type = NET_NACK;
        else if (memcmp(buffer + 4, TYPE_SAMPLE, typeLen) == 0)
            type = OPENMT_SAMP_DATA;
        else if (memcmp(buffer + 4, TYPE_SAMPLE_EOT, typeLen) == 0)
            type = OPENMT_SAMP_EOT;
        else if (memcmp(buffer + 4, TYPE_BITS, typeLen) == 0)
            type = OPENMT_BIT_DATA;
        else if (memcmp(buffer + 4, TYPE_BITS_EOT, typeLen) == 0)
            type = OPENMT_BIT_EOT;
        else if (memcmp(buffer + 4, TYPE_TO_MODEM, typeLen) == 0)
            type = OPENMT_TO_MODEM;

 //       if (debugM)
    //        dump((char*)"Protocol service data:", (unsigned char*)buffer, respLen);

        uint16_t space = getModemSpace(NULL, 1);

        /* Sample frames/EOT are checked (and dropped singly, which keeps the
         * stream frame-aligned) in their own cases below.  Flushing the whole
         * buffer here threw away up to half a second of queued audio and the
         * EOT with it, so only do that for non-sample traffic. */
        if (type != OPENMT_SAMP_DATA && type != OPENMT_SAMP_EOT && RingBuffer_freeSpace(&rxModeBuffer) < respLen)
        {
            fprintf(stderr, "rxModeBuffer out of space. [%d]\n", RingBuffer_freeSpace(&rxModeBuffer));
            pthread_mutex_lock(&rxBufModeMutex);
            RingBuffer_clear(&rxModeBuffer);
            pthread_mutex_unlock(&rxBufModeMutex);
        }

        if (isCurrentMode("DSTAR"))
        {
            if (RingBuffer_dataSize(&rxModeBuffer) >= 200 || type == TYPE_DSTAR_EOT)
                atomic_store(&bufReady, true);
        }
        else if (isCurrentMode("M17"))
        {
            if (RingBuffer_dataSize(&rxModeBuffer) >= 500 || type == TYPE_M17_EOT)
                atomic_store(&bufReady, true);
        }
        else if (isCurrentMode("P25"))
        {
            //    if (RingBuffer_dataSize(&rxModeBuffer) >= 2400)
            atomic_store(&bufReady, true);
        }
        else if (isCurrentMode("NXDN") || isCurrentMode("YSF"))
        {
            atomic_store(&bufReady, true);
        }
        else if (isCurrentMode("DMR"))
        {
            //    if (RingBuffer_dataSize(&rxModeBuffer) >= 2400)
            atomic_store(&bufReady, true);
            if (type == TYPE_DMR_DATA2)
                space = getModemSpace(NULL, 2);
        }

        currType = type;
        // This debug statement to be removed after debugging
        if (type != lastType || debugM)
        {
            char curMode[11];
            if (type == OPENMT_SAMP_DATA || type == OPENMT_BIT_DATA)
                space = RingBuffer_freeSpace(&modemTxBuffer);
            getCurrentMode(curMode, sizeof(curMode));
            fprintf(stderr, "TM: %llu  Type: %02X  Mode: %4s  Conn: %2d  Modem Sp: %4d  TX Modem: %4d  BR: %d  TXON: %1d  FD: %u\n",
                    (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000, type,
                    curMode, conn_id, space, RingBuffer_dataSize(&rxModeBuffer),
                    (int)atomic_load(&bufReady), (int)atomic_load(&txOn),
                    getTimerDuration("frameDelay"));
        }

        lastType = type;

        switch (type)
        {
            case NET_NACK:
                if (debugM)
                    fprintf(stderr, "cNack\n");
                break;

            case NET_ACK:
                if (debugM)
                    fprintf(stderr, "cAck\n");
                break;

            case OPENMT_TO_MODEM:
            {  // Packet type must be the first byte after type in buffer.
                uint8_t buf[3];
                buf[0] = 0xE0;
                buf[1] = respLen - 8 + 2;
                pthread_mutex_lock(&modemComBufMutex);
                RingBuffer_addData(&modemCommandBuffer, buf, 2);
                RingBuffer_addData(&modemCommandBuffer, buffer + 8, respLen - 8);
                pthread_mutex_unlock(&modemComBufMutex);
            }
            break;

            case COMM_SET_MODE:
                if (isCurrentModeIdle())
                {
                    setCurrentMode(hostClient[conn_id].mode);
                    setProtocol(hostClient[conn_id].mode, true);
                    if (strncasecmp(modem, "mmdvm", 5) != 0)
                        setMode(conn_id);
                    //           if (debugM)
                    fprintf(stderr, "Current mode set to %s.\n", hostClient[conn_id].mode);
                    setStatus(modemName, "main", "active_mode", hostClient[conn_id].mode);
                }
                break;

            case COMM_SET_IDLE:
                if (!isCurrentModeIdle() && isCurrentMode(hostClient[conn_id].mode))
                {
                    atomic_store(&txOn, false);
                    setCurrentMode("idle");
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_clear(&rxModeBuffer);
                    pthread_mutex_unlock(&rxBufModeMutex);
                    if (strncasecmp(modem, "mmdvm", 5) == 0)
                        setMode(0x00);
                    else
                        setMode(0x0f);
                    //           if (debugM)
                    fprintf(stderr, "Current mode set to IDLE.\n");
                    setStatus(modemName, "main", "active_mode", "idle");
                }
                break;

            case OPENMT_SAMP_DATA:
            {
                atomic_store(&txOn, true);
                uint8_t buf[4] = {0x61, 0xc4, 0x70, mode[conn_id].txLevel};
                pthread_mutex_lock(&rxBufModeMutex);
                if (RingBuffer_freeSpace(&rxModeBuffer) >= 0xc4)
                {
                    RingBuffer_addData(&rxModeBuffer, buf, 4);
                    RingBuffer_addData(&rxModeBuffer, buffer + 8, respLen - 8);
                }
                else
                {
                    fprintf(stderr, "#############################   rxModeBuffer out of space. [%d]\n", RingBuffer_freeSpace(&rxModeBuffer));
                }
                pthread_mutex_unlock(&rxBufModeMutex);
            }
            break;

            case OPENMT_SAMP_EOT:
            {
                uint8_t buf[3] = {0x61, 0x03, 0x0};
                pthread_mutex_lock(&rxBufModeMutex);
                if (RingBuffer_freeSpace(&rxModeBuffer) >= 0x03)
                {
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                }
                else
                {
                    fprintf(stderr, "#############################   rxModeBuffer out of space. [%d]\n", RingBuffer_freeSpace(&rxModeBuffer));
                }
                pthread_mutex_unlock(&rxBufModeMutex);
                atomic_store(&txOn, false);
            }
            break;

            case OPENMT_BIT_DATA:
            {
                uint8_t buf[3] = {0x61, 0xc3, (uint8_t)(OPENMT_BIT_DATA | conn_id)};
                pthread_mutex_lock(&rxBufModeMutex);
                if (RingBuffer_freeSpace(&rxModeBuffer) >= 0xc3)
                {
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                    RingBuffer_addData(&rxModeBuffer, buffer + 8, respLen - 8);
       //             if (debugM)
       //                 dump((char*)"Bits", (unsigned char*)buffer, respLen);
                }
                else
                {
                    fprintf(stderr, "rxModeBuffer out of space. [%d]\n", RingBuffer_freeSpace(&rxModeBuffer));
                }
                pthread_mutex_unlock(&rxBufModeMutex);
            }
            break;

            case TYPE_M17_LSF:
            case TYPE_M17_STREAM:
            case TYPE_M17_PACKET:
            case TYPE_M17_EOT:
            {
                if (hostClient[conn_id].active && isCurrentMode(hostClient[conn_id].mode))
                {
                    atomic_store(&txOn, true);
                    uint8_t buf[4];
                    buf[0] = 0xE0;
                    buf[1] = 0x34;
                    buf[2] = type;
                    buf[3] = 0x00;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 4);
                    RingBuffer_addData(&rxModeBuffer, &buffer[4 + typeLen], 48);
                    pthread_mutex_unlock(&rxBufModeMutex);
                }
                if (type == TYPE_M17_EOT)
                {
                    atomic_store(&txOn, false);
                }
            }
            break;

            case TYPE_DSTAR_HEADER:
            {
                if (hostClient[conn_id].active && isCurrentMode(hostClient[conn_id].mode))
                {
                    atomic_store(&txOn, true);
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = 0x2C;
                    buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                    RingBuffer_addData(&rxModeBuffer, &buffer[4 + typeLen], 41);
                    pthread_mutex_unlock(&rxBufModeMutex);
                }
            }
            break;

            case TYPE_DSTAR_DATA:
            {
                if (hostClient[conn_id].active && isCurrentMode(hostClient[conn_id].mode))
                {
                    atomic_store(&txOn, true);
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = 0x0F;
                    buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                    RingBuffer_addData(&rxModeBuffer, &buffer[4 + typeLen], 12);
                    pthread_mutex_unlock(&rxBufModeMutex);
                }
            }
            break;

            case TYPE_DSTAR_EOT:
            {
                if (hostClient[conn_id].active && isCurrentMode(hostClient[conn_id].mode))
                {
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = 0x03;
                    buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                    pthread_mutex_unlock(&rxBufModeMutex);
                    atomic_store(&txOn, false);
                }
            }
            break;

            case TYPE_P25_HDR:
            {
                if (hostClient[conn_id].active && isCurrentMode(hostClient[conn_id].mode))
                {
                    atomic_store(&txOn, true);
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = respLen - 8 + 3;
                    buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                    RingBuffer_addData(&rxModeBuffer, &buffer[4 + typeLen], respLen - 8);
                    pthread_mutex_unlock(&rxBufModeMutex);
                }
            }
            break;

            case TYPE_P25_LDU:
            {
                if (hostClient[conn_id].active && isCurrentMode(hostClient[conn_id].mode))
                {
                    atomic_store(&txOn, true);
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = 216 + 3;
                    buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                    RingBuffer_addData(&rxModeBuffer, &buffer[4 + typeLen], 216);
                    pthread_mutex_unlock(&rxBufModeMutex);
                }
            }
            break;

            case TYPE_P25_LOST:
            {
                if (hostClient[conn_id].active && isCurrentMode(hostClient[conn_id].mode))
                {
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = 0x03;
                    buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                    pthread_mutex_unlock(&rxBufModeMutex);
                    atomic_store(&txOn, false);
                }
            }
            break;

            case TYPE_YSF_DATA:
            {
                if (hostClient[conn_id].active && isCurrentMode(hostClient[conn_id].mode))
                {
                    atomic_store(&txOn, true);
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = respLen - 8 + 3;
                    buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                    RingBuffer_addData(&rxModeBuffer, &buffer[4 + typeLen], respLen - 8);
                    pthread_mutex_unlock(&rxBufModeMutex);
                }
            }
            break;

            case TYPE_YSF_LOST:
            {
                if (hostClient[conn_id].active && isCurrentMode(hostClient[conn_id].mode))
                {
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = 0x03;
                    buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                    pthread_mutex_unlock(&rxBufModeMutex);
                    atomic_store(&txOn, false);
                }
            }
            break;

            case TYPE_NXDN_DATA:
            {
                if (hostClient[conn_id].active && isCurrentMode(hostClient[conn_id].mode))
                {
                    atomic_store(&txOn, true);
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = respLen - 8 + 3;
                    buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                    RingBuffer_addData(&rxModeBuffer, &buffer[4 + typeLen], respLen - 8);
                    pthread_mutex_unlock(&rxBufModeMutex);
                }
            }
            break;

            case TYPE_NXDN_LOST:
            {
                if (hostClient[conn_id].active && isCurrentMode(hostClient[conn_id].mode))
                {
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = 0x03;
                    buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                    pthread_mutex_unlock(&rxBufModeMutex);
                    atomic_store(&txOn, false);
                }
            }
            break;

            case TYPE_DMR_DATA1:
            case TYPE_DMR_DATA2:
            case TYPE_DMR_LOST1:
            case TYPE_DMR_LOST2:
            {
                if (hostClient[conn_id].active && isCurrentMode(hostClient[conn_id].mode))
                {
                    atomic_store(&txOn, true);
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = respLen - 8 + 2;
                    // buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 2);
                    RingBuffer_addData(&rxModeBuffer, &buffer[4 + typeLen], respLen - 8);
                    pthread_mutex_unlock(&rxBufModeMutex);

                    if (buffer[8] == 0x42 || type == TYPE_DMR_LOST1 || type == TYPE_DMR_LOST2)
                        atomic_store(&txOn, false);
                }
            }
            break;
        }
        delay(2000);
    }

    if (connections > 0)
        connections--;

    hostClient[conn_id].active = false;
    setProtocol(mode[conn_id].name, false);

    pthread_mutex_lock(&shutDownMutex);
    delGateway(modemName, "main", mode[conn_id].name);
    delMode(modemName, "main", mode[conn_id].name);
    if (strcasecmp(modem, "openmths") == 0 || strcasecmp(modem, "openmtsx") == 0)
    {
        uint8_t buf[3] = {0x61, 0x03, (uint8_t)(OPENMT_DEL_MODE | (conn_id & 0x0f))};
        pthread_mutex_lock(&modemComBufMutex);
        RingBuffer_addData(&modemCommandBuffer, buf, 3);
        pthread_mutex_unlock(&modemComBufMutex);
    }
    else if (strcasecmp(modem, "mmdvmhs") == 0)
        set_ConfigHS(modemName);
    else if (strcasecmp(modem, "mmdvm") == 0)
        set_Config(modemName);

    delay(50000);
    close(sockFd);
    fprintf(stderr, "Client thread exited. Connection Id: %d\n", conn_id);
    int iRet = 200;
    pthread_mutex_unlock(&shutDownMutex);

    pthread_exit(&iRet);
    return NULL;
}

// This thread listens for incoming TCP connections from protocol services.
void* startTCPServer(void* arg)
{
    struct hostent* hostp; /* client host info */
    int childfd;           /* child socket */
    int sockFd;

    sockFd = socket(AF_INET, SOCK_STREAM, 0);

    setNoDelay(sockFd);
    if (sockFd < 0)
    {
        fprintf(stderr, "Modem_Host: error when creating the socket: %s\n", strerror(errno));
        exit(1);
    }

    /* setsockopt: Handy debugging trick that lets
     * us rerun the server immediately after we kill it;
     * otherwise we have to wait about 20 secs.
     * Eliminates "ERROR on binding: Address already in use" error.
     */
    optval = 1;
    setsockopt(sockFd, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char*)&serveraddr, sizeof(serveraddr));

    /* this is an Internet address */
    serveraddr.sin_family = AF_INET;

    /* let the system figure out our IP address */
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* this is the port we will listen on */
    serveraddr.sin_port = htons((unsigned short)host_port);

    if (bind(sockFd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
    {
        fprintf(stderr, "Modem_Host: error when binding the socket to port %u: %s\n", host_port, strerror(errno));
        exit(1);
    }

    if (debugM)
        fprintf(stdout, "Opened the TCP socket on port %u\n", host_port);

    /*
     * listen: make this socket ready to accept connection requests
     */
    if (listen(sockFd, 10) < 0) /* allow 10 requests to queue up */
        error((char*)"ERROR on listen");

    /*
     * main loop: wait for a connection request, echo input line,
     * then close connection.
     */
    clientlen = sizeof(clientaddr);

    if (sockFd < 0)
    {
        error((char*)"Client connect failed.");
        exit(1);
    }

    while (atomic_load(&running))
    {
        /*
         * accept: wait for a connection request
         */
        childfd = accept(sockFd, (struct sockaddr*)&clientaddr, &clientlen);
        if (childfd >= 0)
            setNoDelay(childfd);
        if (childfd < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            perror("accept failed");
            break;
        }

        if (childfd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;

        /*
         * gethostbyaddr: determine who sent the message
         */
        hostp = debugM ? gethostbyaddr((const char*)&clientaddr.sin_addr.s_addr, sizeof(clientaddr.sin_addr.s_addr), AF_INET) : NULL;

        hostaddrp = inet_ntoa(clientaddr.sin_addr);
        if (hostaddrp == NULL)
        {
            /* a bad peer address must not take Modem_Host down */
            fprintf(stderr, "ERROR on inet_ntoa\n");
            close(childfd);
            continue;
        }

        if (debugM)
            fprintf(stderr, "Server established connection with %s (%s)\n", hostp ? hostp->h_name : "?", hostaddrp);

        uint8_t newConn = 255;
        for (uint8_t i = 0; i < MAX_CLIENT_CONNECTIONS; i++)
        {  // look for free connection
            if (!hostClient[i].active)
            {
                bzero(mode[i].name, 11);
                bzero(mode[i].modem_type, 6);
                newConn = i;
                break;
            }
        }

        if (newConn == 255)  // check if max connections
            continue;

        // create threads
        hostClient[newConn].sockfd = childfd;
        hostClient[newConn].active = true;

        int err = pthread_create(&(hostClient[newConn].clientid), NULL, &processClientSocket, (void*)(intptr_t)newConn);
        if (err != 0)
        {
            fprintf(stderr, "Can't create client process thread :[%s]", strerror(err));
            hostClient[newConn].active = false;
            continue;
        }
        else
        {
            if (debugM)
                fprintf(stderr, "Client process thread created successfully\n");
        }

        err = pthread_create(&(hostClient[newConn].rxid), NULL, &modeRxThread, (void*)(intptr_t)newConn);
        if (err != 0)
        {
            hostClient[newConn].active = false;
            fprintf(stderr, "Can't create mode rx thread :[%s]", strerror(err));
        }
        else
        {
            if (debugM)
                fprintf(stderr, "Mode rx thread created successfully\n");
        }

        connections++;

        delay(1000);
    }
    pthread_mutex_lock(&shutDownMutex);
    fprintf(stderr, "TCP server exited.\n");
    int iRet = 100;
    delay(500000);
    pthread_mutex_unlock(&shutDownMutex);
    atomic_store(&running, false);
    pthread_exit(&iRet);
    return NULL;
}

// Mmdvm modem specific function
int processMMDVMSerial(void)
{
    uint8_t buffer[BUFFER_LENGTH];
    unsigned int respLen;
    unsigned int offset;
    unsigned int type;
    ssize_t len;

    len = modemRead(buffer, 1);
    if (len == 0) return 2;

    if (len != 1)
    {
        fprintf(stderr, "Modem_Host: error when reading from Modem, errno=%d\n", errno);
        return 0;
    }

    if (buffer[0] != 0xE0)
    {
        fprintf(stderr, "Modem_Host: unknown byte from Modem, 0x%02X\n", buffer[0]);
        return 1;
    }

    offset = 0;
    while (offset < 2)
    {
        len = modemRead(buffer + 1 + offset, 2 - offset);
        if (len == 0)
            delay(5);
        else
            offset += len;
    }

    respLen = buffer[1];
    if (respLen < 3)
    {
        fprintf(stderr, "Modem_Host: bad frame length %u from Modem, resyncing\n", (unsigned int)respLen);
        return 1;
    }
    type    = buffer[2];

    offset += 1;
    while (offset < respLen)
    {
        len = modemRead(buffer + offset, respLen - offset);

        if (len == 0)
            delay(5);
        else
            offset += len;
    }

    if (RingBuffer_freeSpace(&txModeBuffer) == 0)
    {
        fprintf(stderr, "txModeBuffer out of space.\n");
        pthread_mutex_lock(&txBufModeMutex);
        RingBuffer_clear(&txModeBuffer);
        pthread_mutex_unlock(&txBufModeMutex);
    }

    if (debugM)
    {
        dump((char*)"SERIAL", buffer, respLen);
        if (type == TYPE_DSTAR_HEADER)
        {
            if (respLen != 46)
                fprintf(stderr, "DSTAR header wrong length. [%d]\n", respLen);
        }
        if (type == TYPE_DSTAR_DATA)
        {
            if (respLen != 15 && respLen != 17)
                fprintf(stderr, "DSTAR data wrong length. [%d]\n", respLen);
        }
        if (type == TYPE_M17_LSF || type == TYPE_M17_STREAM || type == TYPE_M17_PACKET)
        {
            if (respLen != 54)
                fprintf(stderr, "M17 frame wrong length. [%d]\n", respLen);
        }
    }

    if (type == MODEM_NACK)
    {
        //     if (debugM)
        fprintf(stderr, "Nack == Type: %02X  Err: %02X\n", buffer[3], buffer[4]);
    }
    else if (type == MODEM_ACK)
    {
        if (debugM)
            fprintf(stderr, "Ack\n");
    }
    else if ((type & 0xf0) == 0xf0)
    {
        if (debugM)
            dump((char*)"DEBUG1", buffer, respLen);
        buffer[respLen] = 0;
        fprintf(stderr, "DEBUG: %s\n", buffer + 3);
    }
    else if (type == MODEM_VERSION)
    {
        dump((char*)"Version", buffer, respLen);
    }
    else if (type == MODEM_STATUS)
    {
        if (debugM)
            dump((char*)"STATUS", buffer, respLen);
        if (strcasecmp(modem, "mmdvmhs") == 0)
            setM17Space(buffer[13]);
        else
            setM17Space(buffer[12]);
        setDSTARSpace(buffer[6]);
        setDMRSpace(buffer[7], buffer[8]);
        setP25Space(buffer[10]);
        setYSFSpace(buffer[9]);
        setNXDNSpace(buffer[11]);
    }
    else if (type == TYPE_M17_LSF && (respLen == 54 || respLen == 52))
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("M17")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0x38;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_LSF_M17, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, &buffer[4], 48);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_M17_STREAM && (respLen == 54 || respLen == 52))
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("M17")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0x38;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_STREAM_M17, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, &buffer[4], 48);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_M17_PACKET && (respLen == 54 || respLen == 52))
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("M17")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0x38;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_PACKET_M17, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, &buffer[4], 48);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if ((type == TYPE_M17_EOT || type == TYPE_M17_LOST) && respLen == 3)
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("M17")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0x08;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_EOT_M17, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_DSTAR_HEADER && respLen == 46)
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("DSTAR")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0x31;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_HEADER_DSTAR, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, &buffer[3], 41);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_DSTAR_DATA && (respLen == 15 || respLen == 17))
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("DSTAR")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0x14;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_DATA_DSTAR, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, &buffer[3], 12);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if ((type == TYPE_DSTAR_EOT || type == TYPE_DSTAR_LOST) && respLen == 3)
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("DSTAR")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0x08;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_EOT_DSTAR, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_FM_DATA || type == TYPE_FM_STATUS || type == TYPE_FM_EOT || type == TYPE_FM_RSSI)
    {
        if (connections > 0 && respLen >= 3 && respLen <= 250 && (isCurrentModeIdle() || isCurrentMode("FM")))
        {
            const char* fmType = type == TYPE_FM_DATA ? TYPE_DATA_FM :
                                 type == TYPE_FM_STATUS ? TYPE_STATUS_FM :
                                 type == TYPE_FM_EOT ? TYPE_EOT_FM : TYPE_RSSI_FM;
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = respLen + 5;
            buf[3] = 0x04;
            memcpy(buf + 4, fmType, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            if (respLen > 3)
                RingBuffer_addData(&txModeBuffer, &buffer[3], respLen - 3);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_P25_HDR && buffer[3] == 1 && (respLen >= 21 && respLen <= 105))
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("P25")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = respLen + 4;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_HDR_P25, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, &buffer[4], respLen - 4);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_P25_LDU && buffer[3] == 1 && (respLen == 222 || respLen == 220))
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("P25")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0xE0;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_LDU_P25, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, &buffer[4], 216);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_P25_LOST && respLen == 3)
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("P25")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0x08;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_LOST_P25, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_YSF_DATA && (respLen == 124 || respLen == 126))
    {
        /* [placeholder] [120 byte frame] [optional 2 byte RSSI] */
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("YSF")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = respLen - 3 + 8;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_DATA_YSF, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, &buffer[3], respLen - 3);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_YSF_LOST)
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("YSF")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0x08;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_LOST_YSF, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_NXDN_DATA && (respLen == 52 || respLen == 54))
    {
        /* [placeholder] [48 byte frame] [optional 2 byte RSSI] */
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("NXDN")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = respLen - 3 + 8;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_DATA_NXDN, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, &buffer[3], respLen - 3);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_NXDN_LOST)
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("NXDN")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0x08;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_LOST_NXDN, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_DMR_DATA1 && (respLen == 0x25 || respLen == 0x27))
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("DMR")))
        {
            /*     uint8_t buf[10] = {0xE0, 0x04, 0x03, 0x02, 0x00};
                 setProtocol("DMR", true);
                 buf[2] = 0x1D;
                 buf[3] = 0x01;
                 write(serialModemFd, buf, 4);
                 delay(1000);
                 RingBuffer_addData(&modemCommandBuffer, buf, 4);
                 fprintf(stderr, "Done\n"); */
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = respLen - 3 + 8;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_DATA1_DMR, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, &buffer[3], respLen - 3);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_DMR_LOST1)
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("DMR")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = respLen - 3 + 8;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_LOST1_DMR, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, &buffer[3], respLen - 3);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_DMR_DATA2 && (respLen == 0x25 || respLen == 0x27))
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("DMR")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = respLen - 3 + 8;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_DATA2_DMR, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, &buffer[3], respLen - 3);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else if (type == TYPE_DMR_LOST2)
    {
        if (connections > 0 && (isCurrentModeIdle() || isCurrentMode("DMR")))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = respLen - 3 + 8;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_LOST2_DMR, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, &buffer[3], respLen - 3);
            pthread_mutex_unlock(&txBufModeMutex);
        }
    }
    else
    {
        //     if (debugM)
        dump((char*)"Modem rec data:", buffer, respLen);
    }

    return 1;
}

/* true if the n bytes at data contain the C string needle */
static bool frameHasText(const uint8_t* data, unsigned int n, const char* needle)
{
    size_t m = strlen(needle);
    unsigned int i;

    for (i = 0; i + m <= n; i++)
    {
        if (memcmp(data + i, needle, m) == 0)
            return true;
    }
    return false;
}

// Process incoming modem bytes.
int processSerial(void)
{
    uint8_t buffer[BUFFER_LENGTH];
    uint16_t respLen = 0;
    uint8_t type;
    int rc;

    rc = modemGetFrame(buffer, &respLen);
    if (rc == 0)
    {
        fprintf(stderr, "Modem_Host: error when reading from Modem, errno=%d\n", errno);
        return 0;
    }
    if (rc != 1)
        return rc;                      /* 2 = nothing yet, 3 = resyncing */

    type = buffer[2] & 0xf0;

    if (RingBuffer_freeSpace(&txModeBuffer) == 0)
    {
        fprintf(stderr, "txModeBuffer out of space.\n");
        pthread_mutex_lock(&txBufModeMutex);
        RingBuffer_clear(&txModeBuffer);
        pthread_mutex_unlock(&txBufModeMutex);
    }

    if (type == OPENMT_NAK)
    {
        //     if (debugM)
        fprintf(stderr, "Nack == Type: %02X  Err: %02X\n", buffer[3], buffer[4]);
    }
    else if (type == OPENMT_ACK)
    {
        if (debugM)
            fprintf(stderr, "Ack\n");
    }
    else if (type == OPENMT_GET_VERSION)
    {
        fprintf(stderr, "Version\n");
    }
    else if (type == OPENMT_SAMP_DATA)
    {
        if (debugM)
            dump((char*)"SAMP", buffer, respLen);
        uint8_t buf[8];
        buf[0] = 0x61;
        buf[1] = ((respLen + 5) & 0xff00) >> 8;
        buf[2] = (respLen + 5) & 0x00ff;
        buf[3] = 0x04;
        memcpy(buf + 4, TYPE_SAMPLE, 4);
        pthread_mutex_lock(&txBufModeMutex);
        RingBuffer_addData(&txModeBuffer, buf, 8);
        RingBuffer_addData(&txModeBuffer, buffer + 3, respLen - 3);
        buf[0] = buffer[2] & 0x0f;
        RingBuffer_addData(&txModeBuffer, buf, 1);  // add connection id
        pthread_mutex_unlock(&txBufModeMutex);
        return 1;
    }
    else if (type == OPENMT_BIT_DATA)
    {
   //     if (debugM)
    //        dump((char*)"BITS", buffer, respLen);
        uint8_t buf[8];
        buf[0] = 0x61;
        buf[1] = ((respLen + 5) & 0xff00) >> 8;
        buf[2] = (respLen + 5) & 0x00ff;
        buf[3] = 0x04;
        memcpy(buf + 4, TYPE_BITS, 4);
        pthread_mutex_lock(&txBufModeMutex);
        RingBuffer_addData(&txModeBuffer, buf, 8);
        RingBuffer_addData(&txModeBuffer, buffer + 3, respLen - 3);
        buf[0] = buffer[2] & 0x0f;
        RingBuffer_addData(&txModeBuffer, buf, 1);  // add connection id
        pthread_mutex_unlock(&txBufModeMutex);
        return 1;
    }
    else if (type == OPENMT_TX_TIMING)
    {
        /* TX/RX sample alignment report from the SX1255 bridge (8-byte
         * payload). Only the DMR host uses it (to tell TDMA timeslots
         * apart), and it must not reach any other host: m17_host, for
         * one, treats any packet type it doesn't recognise as an M17
         * frame and switches itself to FRAME mode. So forward it only
         * while DMR is the active mode, through txModeBuffer in arrival
         * order with the SAMP records -- its position in the sample
         * stream is part of what it means -- and drop it otherwise. */
        if (respLen == 3U + 8U && isCurrentMode("DMR"))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = ((respLen + 5) & 0xff00) >> 8;
            buf[2] = (respLen + 5) & 0x00ff;
            buf[3] = 0x04;
            memcpy(buf + 4, TYPE_TX_TIMING, 4);
            pthread_mutex_lock(&txBufModeMutex);
            RingBuffer_addData(&txModeBuffer, buf, 8);
            RingBuffer_addData(&txModeBuffer, buffer + 3, respLen - 3);
            buf[0] = buffer[2] & 0x0f;
            RingBuffer_addData(&txModeBuffer, buf, 1);  // add connection id
            pthread_mutex_unlock(&txBufModeMutex);
        }
        return 1;
    }
    else if (type == OPENMT_GET_STATUS)
    {
        //     if (debugM)
        if (openmtPacing())
        {
            uint8_t freeFrames = buffer[2] & 0x0f;
            int     credit     = openmtCreditFromStatus(freeFrames);

            atomic_store(&openmtLastFree, (int)freeFrames);

            atomic_store(&openmtCredit, credit);
            setSpace(credit >= (int)OPENMT_MIN_FREE ? (uint8_t)credit : 0);
        }
        else
        {
            dump((char*)"STATUS:", buffer, respLen);
            setSpace(buffer[2] & 0x0f);
        }
    }
    else if (type == 0xC0U && respLen > 3U && frameHasText(buffer + 3, respLen - 3U, " id="))
    {
        /* DEBUG text frame carrying the firmware version, e.g.
         * "OpenMT-SAMP-F446 v1.5.0 Oct  7 2026 id=10006421".  The modem
         * sends it on the first configuration after a reset and whenever it
         * is asked; show each distinct text once. */
        static char lastVersion[128];
        char text[128];
        unsigned int n = respLen - 3U;

        if (n >= sizeof(text))
            n = sizeof(text) - 1U;
        memcpy(text, buffer + 3, n);
        text[n] = '\0';
        if (strcmp(text, lastVersion) != 0)
        {
            strcpy(lastVersion, text);
            fprintf(stderr, "Modem_Host: modem firmware: %s\n", text);
        }
    }
    else
    {
        if (respLen > BUFFER_LENGTH)
            respLen = BUFFER_LENGTH;
        //       if (debugM)
        dump((char*)"Modem rec data:", buffer, respLen);
    }
    delay(1000);
    return 1;
}


/* ------------------------------------------------------------------ */
/* Say why the process died: fatal signal name + a backtrace on stderr */
/* ------------------------------------------------------------------ */
static void fatalSignalHandler(int sig)
{
    void* frames[32];
    const char* name = (sig == SIGSEGV) ? "SIGSEGV (bad memory access)" :
                       (sig == SIGBUS)  ? "SIGBUS" :
                       (sig == SIGFPE)  ? "SIGFPE" :
                       (sig == SIGABRT) ? "SIGABRT (abort/assert/heap corruption)" :
                       (sig == SIGILL)  ? "SIGILL" :
                       (sig == SIGTERM) ? "SIGTERM (killed)" :
                       (sig == SIGHUP)  ? "SIGHUP (terminal closed)" : "signal";
    char msg[96];
    int n = snprintf(msg, sizeof msg, "\nModem_Host: FATAL %s [%d]\n", name, sig);

    if (n > 0)
        (void)!write(2, msg, (size_t)n);
    int cnt = backtrace(frames, 32);
    backtrace_symbols_fd(frames, cnt, 2);
    _exit(128 + sig);
}

static void installCrashHandlers(void)
{
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGABRT, SIGILL, SIGTERM, SIGHUP};
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        signal(sigs[i], fatalSignalHandler);
}

int main(int argc, char** argv)
{
    bool daemon = false;
    bool realtime = false;
    int ret;
    int c;

    while ((c = getopt(argc, argv, ":m:dxvr")) != -1)
    {
        switch (c)
        {
            case 'm':
            {
                modemId = atoi(optarg);
                if (modemId < 1 || modemId > 10)
                {
                    fprintf(stderr, "Invalid modem number.\n");
                    return 1;
                }
                sprintf(modemName, "modem%d", modemId);
                host_port = 18000 + modemId - 1;
                fprintf(stderr, "Modem name: %s\n", modemName);
            }
            break;
            case ':':
                fprintf(stderr, "Option 'm' requires modem number.\n");
                return 1;
            case 'd':
                daemon = true;
                break;
            case 'v':
                fprintf(stdout, "Modem_Host: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            case 'r':
                realtime = true;
                break;
            default:
                fprintf(stderr, "Usage: Modem_Host [-m modem_number (1-10)] [-d] [-v] [-x] [-r]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            fprintf(stderr, "Modem_Host: error in fork(), exiting\n");
            return 1;
        }

        // If this is the parent, exit
        if (pid > 0)
            return 0;

        // We are the child from here onwards
        setsid();

        umask(0);
    }

    signal(SIGINT, sigintHandler);
    /* A client (dmr_host etc.) closing its socket must not kill the modem host:
     * the next write() to it would raise SIGPIPE and terminate the process.
     * Ignore it; write() then just returns -1/EPIPE, which is already handled. */
    signal(SIGPIPE, SIG_IGN);
    installCrashHandlers();

    initTimers();
    if (getTimer("status", 500) < 0) return 0;
    if (getTimer("txTimeout", txTimeout) < 0) return 0;
    if (getTimer("chkComTimeout", 2000) < 0) return 0;

    /* Initialize RingBuffers */
    RingBuffer_Init(&modemCommandBuffer, 300);
    RingBuffer_Init(&modemTxBuffer, 8192);
    RingBuffer_Init(&rxModeBuffer, 64000);  /* TX_AHEAD (400 ms = 19200 B) + jitter */
    RingBuffer_Init(&txModeBuffer, 10000);

    for (uint8_t i = 0; i < MAX_CLIENT_CONNECTIONS; i++)
    {
        hostClient[i].active = false;
        hostClient[i].isTx   = false;
        strcpy(hostClient[i].mode, "");
        RingBuffer_Init(&hostClient[i].command, 300);
        pthread_mutex_init(&hostClient[i].commandMutex, NULL);
    }

    readHostConfig(modemName, "config", "modem", modem);
    if (strlen(modem) == 0)
    {
        setHostConfig(modemName, "config", "modem", "none", "mmdvmhs");
        readHostConfig(modemName, "config", "modem", modem);
        setHostConfig(modemName, "config", "port", "none", "ttyAMA0");
        setHostConfig(modemName, "config", "mode", "none", "simplex");
        setHostConfig(modemName, "config", "rxInvert", "none", "false");
        setHostConfig(modemName, "config", "txInvert", "none", "true");
        setHostConfig(modemName, "config", "pttInvert", "none", "false");
        setHostConfig(modemName, "config", "useCOSAsLockout", "none", "false");
        setHostConfig(modemName, "config", "debug", "none", "false");
        setHostConfig(modemName, "config", "txDelay", "none", "100");
        setHostConfig(modemName, "config", "rxLevel", "none", "50");
        setHostConfig(modemName, "config", "rxTxLevel", "none", "50");
        setHostConfig(modemName, "config", "rxFrequency", "none", "449900000");
        setHostConfig(modemName, "config", "txFrequency", "none", "449900000");
        setHostConfig(modemName, "config", "rxDCOffset", "none", "0");
        setHostConfig(modemName, "config", "txDCOffset", "none", "0");
        setHostConfig(modemName, "config", "baud", "none", "460800");
        setHostConfig(modemName, "config", "fmTxLevel", "none", "50");
        setHostConfig(modemName, "config", "cwIdTxLevel", "none", "50");
        setHostConfig(modemName, "main", "gateways", "none", "none");
        setHostConfig(modemName, "main", "activeModes", "none", "none");
        setHostConfig(modemName, "main", "username", "none", "admin");
        setHostConfig(modemName, "main", "callsign", "none", "N0CALL");
        setHostConfig(modemName, "main", "title", "none", "Repeater Dashboard");
        setHostConfig(modemName, "main", "latitude", "none", "0.0");
        setHostConfig(modemName, "main", "longitude", "none", "0.0");
    }

    readHostConfig(modemName, "config", "port", commPort);
    readHostConfig(modemName, "config", "baud", modemBaud);

    readHostConfig(modemName, "main", "callsign", callsign);

    clearDashbCommands(modemName);

    sprintf(modemtty, "/dev/%s", commPort);
    serialModemFd = openSerial(modemtty);
    // Discard anything the modem/bridge queued before we opened the port:
    // a partly written frame at the front would misalign the first few
    // frames read ("txModeBuffer: invalid header [00] [00]" at start-up).
    tcflush(serialModemFd, TCIFLUSH);
    if (serialModemFd < 0)
        return 1;

    sleep(1);

    if (strncasecmp(modem, "mmdvm", 5) == 0)
    {
        int err = pthread_create(&(modetxid), NULL, &modeMMDVMTxThread, NULL);
        if (err != 0)
        {
            fprintf(stderr, "Can't create MMDVM mode tx thread:[%s]", strerror(err));
            return 1;
        }
        else
        {
            if (debugM)
                fprintf(stderr, "MMDVM mode tx thread created successfully\n");
        }
    }
    else
    {
        int err = pthread_create(&(modemTxid), NULL, &modemTxThread, NULL);
        if (err != 0)
        {
            fprintf(stderr, "Can't create modem tx thread:[%s]", strerror(err));
            return 1;
        }
        else
        {
            if (debugM)
                fprintf(stderr, "Modem tx thread created successfully\n");
        }

        err = pthread_create(&(modetxid), NULL, &modeTxThread, NULL);
        if (err != 0)
        {
            fprintf(stderr, "Can't create mode tx thread:[%s]", strerror(err));
            return 1;
        }
        else
        {
            if (debugM)
                fprintf(stderr, "Mode tx thread created successfully\n");
        }
    }

    int err = pthread_create(&(timerid), NULL, &timerThread, NULL);
    if (err != 0)
    {
        fprintf(stderr, "Can't create timer thread:[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM)
            fprintf(stderr, "Timer thread created successfully\n");
    }

    err = pthread_create(&(tcpid), NULL, &startTCPServer, NULL);
    if (err != 0)
    {
        fprintf(stderr, "Can't create TCP server thread:[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM)
            fprintf(stderr, "TCP server thread created successfully\n");
    }

    err = pthread_create(&(commandid), NULL, &commandThread, NULL);
    if (err != 0)
    {
        fprintf(stderr, "Can't create command thread:[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM)
            fprintf(stderr, "Command thread created successfully\n");
    }

    setHostConfig(modemName, "main", "gateways", "none", "none");
    setHostConfig(modemName, "main", "activeModes", "none", "none");
    delay(10000);

    if (strcasecmp(modem, "mmdvmhs") == 0)
    {
        readHostConfig(modemName, "config", "rxFrequency", modem_rxFrequency);
        readHostConfig(modemName, "config", "txFrequency", modem_txFrequency);

        set_ConfigHS(modemName);
        delay(10000);

        setFrequency(modem_rxFrequency, modem_txFrequency, modem_txFrequency, 255);
        delay(10000);
        uint8_t buf[3];
        buf[0] = 0xE0;
        buf[1] = 0x03;
        buf[2] = 0x00;
        // write(serialModemFd, buf, 3);
        RingBuffer_addData(&modemCommandBuffer, buf, 3);
    }
    else if (strcasecmp(modem, "mmdvm") == 0)
    {
        set_Config(modemName);
        delay(10000);
        uint8_t buf[3];
        buf[0] = 0xE0;
        buf[1] = 0x03;
        buf[2] = 0x00;
        write(serialModemFd, buf, 3);
    }
    else if (strcasecmp(modem, "openmths") == 0)
    {
        uint8_t buf[4];
        buf[0] = 0x61;
        buf[1] = 0x04;
        buf[2] = OPENMT_SET_CONFIG;
        buf[3] = 0x81;
        write(serialModemFd, buf, 4);
        char tmp[10];
        readHostConfig(modemName, "config", "mode", tmp);
        if (strcasecmp(tmp, "duplex") == 0)
            modem_duplex = true;
        else
            modem_duplex = false;
        readHostConfig(modemName, "config", "rxLevel", tmp);
        modem_rxLevel = atoi(tmp);
    }
    else if (strcasecmp(modem, "openmtsx") == 0)
    {
        /* Richer SET_CONFIG payload for the sx1255-openmt-bridge project
         * specifically: plain SX1255 hardware parameters (frequency,
         * gains, PLL bandwidth, RX/TX invert), overriding that bridge's
         * own command-line defaults, rather than the single-byte
         * "openmths" handshake above. Needs these additional keys under
         * this modem's "config" section: rxFrequency, txFrequency,
         * rxInvert, txInvert (already used by other modem types, see
         * the "openmt" branch below), plus rxLnaGain, rxPgaGain,
         * txMixerGain, txDacGain, rxPllBw, txPllBw (new, SX1255-
         * specific). See sx1255-openmt-bridge's own README for the
         * exact payload format and valid ranges for each field. */
        char tmp[16];
        uint32_t rx_freq_hz, tx_freq_hz;
        uint8_t lna_gain_db, pga_gain_db;
        int8_t mixer_gain_x2, dac_gain_db;
        uint16_t rx_pll_bw_khz, tx_pll_bw_khz;
        uint8_t invert_flags = 0;
        uint8_t buf[21];

        readHostConfig(modemName, "config", "rxFrequency", tmp);
        rx_freq_hz = (uint32_t)strtoul(tmp, NULL, 10);
        readHostConfig(modemName, "config", "txFrequency", tmp);
        tx_freq_hz = (uint32_t)strtoul(tmp, NULL, 10);
        readHostConfig(modemName, "config", "rxLnaGain", tmp);
        lna_gain_db = (uint8_t)atoi(tmp);
        readHostConfig(modemName, "config", "rxPgaGain", tmp);
        pga_gain_db = (uint8_t)atoi(tmp);
        readHostConfig(modemName, "config", "txMixerGain", tmp);
        mixer_gain_x2 = (int8_t)(atof(tmp) * 2.0); /* 2dB steps */
        readHostConfig(modemName, "config", "txDacGain", tmp);
        dac_gain_db = (int8_t)atoi(tmp);
        readHostConfig(modemName, "config", "rxPllBw", tmp);
        rx_pll_bw_khz = (uint16_t)atoi(tmp);
        readHostConfig(modemName, "config", "txPllBw", tmp);
        tx_pll_bw_khz = (uint16_t)atoi(tmp);
        readHostConfig(modemName, "config", "rxInvert", tmp);
        if (strcasecmp(tmp, "true") == 0)
            invert_flags |= 0x01U;
        readHostConfig(modemName, "config", "txInvert", tmp);
        if (strcasecmp(tmp, "true") == 0)
            invert_flags |= 0x02U;
        readHostConfig(modemName, "config", "mode", tmp);
        if (strcasecmp(tmp, "duplex") == 0)
            modem_duplex = true;
        else
            modem_duplex = false;

        buf[0] = 0x61;
        buf[1] = 21; /* total frame length, including 0x61 itself */
        buf[2] = OPENMT_SET_CONFIG;
        buf[3] = 1; /* payload version */
        buf[4] = (uint8_t)(rx_freq_hz >> 24);
        buf[5] = (uint8_t)(rx_freq_hz >> 16);
        buf[6] = (uint8_t)(rx_freq_hz >> 8);
        buf[7] = (uint8_t)rx_freq_hz;
        buf[8] = (uint8_t)(tx_freq_hz >> 24);
        buf[9] = (uint8_t)(tx_freq_hz >> 16);
        buf[10] = (uint8_t)(tx_freq_hz >> 8);
        buf[11] = (uint8_t)tx_freq_hz;
        buf[12] = lna_gain_db;
        buf[13] = pga_gain_db;
        buf[14] = (uint8_t)mixer_gain_x2;
        buf[15] = (uint8_t)dac_gain_db;
        buf[16] = (uint8_t)(rx_pll_bw_khz >> 8);
        buf[17] = (uint8_t)rx_pll_bw_khz;
        buf[18] = (uint8_t)(tx_pll_bw_khz >> 8);
        buf[19] = (uint8_t)tx_pll_bw_khz;
        buf[20] = invert_flags;
        write(serialModemFd, buf, 21);
    }
    else if (strcasecmp(modem, "openmt") == 0)
    {
        char tmp[10];
        readHostConfig(modemName, "config", "rxInvert", tmp);
        if (strcasecmp(tmp, "true") == 0)
            modem_rxInvert = true;
        else
            modem_rxInvert = false;
        readHostConfig(modemName, "config", "txInvert", tmp);
        if (strcasecmp(tmp, "true") == 0)
            modem_txInvert = true;
        else
            modem_txInvert = false;
        readHostConfig(modemName, "config", "pttInvert", tmp);
        if (strcasecmp(tmp, "true") == 0)
            modem_pttInvert = true;
        else
            modem_pttInvert = false;
        readHostConfig(modemName, "config", "mode", tmp);
        if (strcasecmp(tmp, "duplex") == 0)
            modem_duplex = true;
        else
            modem_duplex = false;
        readHostConfig(modemName, "config", "rxLevel", tmp);
        modem_rxLevel = atoi(tmp);

        uint8_t buf[196];
        memset(buf, 0, 196);
        buf[0] = 0x61;
        buf[1] = 0xc4;
        buf[2] = OPENMT_SET_CONFIG;
        buf[3] |= modem_duplex;
        buf[3] |= modem_rxInvert << 1;
        buf[3] |= modem_txInvert << 2;
        buf[3] |= modem_pttInvert << 3;
        buf[4] = modem_rxLevel;
        write(serialModemFd, buf, 196);

        /* Ask for the firmware version (61 03 D0).  The modem sends it by
         * itself only the first time it is configured after a reset, so a
         * restarted host would otherwise never see it. */
        {
            static const uint8_t getVersion[3] = {0x61, 0x03, 0xD0};

            if (write(serialModemFd, getVersion, 3) != 3)
                fprintf(stderr, "Modem_Host: could not request the modem firmware version\n");
        }
    }

    if (realtime)
    {
        /* -r: serve the serial port ahead of ordinary processes, so a busy
         * Pi is less likely to let the UART buffer overflow (needs root) */
        struct sched_param sp;

        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = 20;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
            fprintf(stderr, "Modem_Host: can't get real-time priority (errno=%d), continuing without\n", errno);
        else
            fprintf(stderr, "Modem_Host: serial reader runs with real-time priority\n");
    }

    while (atomic_load(&running))
    {
        if (strncasecmp(modem, "mmdvm", 5) == 0)
            ret = processMMDVMSerial();
        else
            ret = processSerial();

        if (!ret)
            break;
    }

    pthread_mutex_lock(&shutDownMutex);
    setHostConfig(modemName, "main", "gateways", "none", "none");
    setHostConfig(modemName, "main", "activeModes", "none", "none");
    fprintf(stderr, "Modem host terminated.\n");
    logError(modemName, "main", "Modem host terminated.");
    pthread_mutex_unlock(&shutDownMutex);

    /* Cleanup RingBuffers */
    RingBuffer_Destroy(&modemCommandBuffer);
    RingBuffer_Destroy(&modemTxBuffer);
    RingBuffer_Destroy(&rxModeBuffer);
    RingBuffer_Destroy(&txModeBuffer);
    for (uint8_t i = 0; i < MAX_CLIENT_CONNECTIONS; i++)
    {
        RingBuffer_Destroy(&hostClient[i].command);
        pthread_mutex_destroy(&hostClient[i].commandMutex);
    }

    return 0;
}
