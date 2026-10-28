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
#include "../tools/bridge_common.h"
#include "RingBuffer.h"
#include "../tools/ADF7021.h"   /* ADF7021_PFD and ADF7021_REG1_UHF2, for the RX offsets */
#include <netinet/tcp.h>
#define VERSION "2026-10-07"
#define MAX_LINE_LENGTH 256
#define BUFFER_LENGTH 5000
#define MAX_CLIENT_CONNECTIONS 14
#define TX_TIMEOUT 300000

const char* TYPE_ACK          = "ACK ";
const char* TYPE_NACK         = "NACK";
const char* TYPE_MODE         = "MODE";

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
#define OPENMT_MEAS 0xD0U       // RX frequency measurement request and reply (openmths)
#define OPENMT_ACK 0xE0U
#define OPENMT_NAK 0xF0U

#define COMM_SET_DUPLEX 0x00
#define COMM_SET_SIMPLEX 0x01
#define COMM_SET_MODE 0x02
#define COMM_SET_IDLE 0x03

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
pthread_t cfgwatchid;

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
char callsign[10] = "N0CALL";

/* Kind of packet stream the modem (or the bridge in front of it) delivers.
 * A bridge always publishes it in the database; the host sends the bridge
 * control frames on the same serial port. */
static StreamType streamType = STREAM_NONE;
static bool       creditPacing = false;  /* published by the bridge: poll the modem for free buffer space */
static void       sendBridgeControl(uint8_t type, const char* payload, size_t plen);
static void       refreshBridgedSettings(void);


void setProtocol(const char* protocol, bool enabled);

/*
 * setCurrentMode()
 *
 * Stores the name of the current radio mode, such as "idle" or the name a service registered with,
 * under currentModeMutex.  Names longer than the storage are cut.
 *
 * Parameters:
 *   mode - New mode name.
 *
 * Returns: nothing.
 */
static void setCurrentMode(const char* mode)
{
    pthread_mutex_lock(&currentModeMutex);
    strncpy(currentMode, mode, sizeof(currentMode) - 1);
    currentMode[sizeof(currentMode) - 1] = '\0';
    pthread_mutex_unlock(&currentModeMutex);
}

/*
 * isCurrentMode()
 *
 * Tests, under currentModeMutex, whether the current radio mode is a given
 * one.
 *
 * Parameters:
 *   mode - Mode name to compare with (case insensitive).
 *
 * Returns: true if it is the current mode.
 */
static bool isCurrentMode(const char* mode)
{
    pthread_mutex_lock(&currentModeMutex);
    bool match = (strcasecmp(currentMode, mode) == 0);
    pthread_mutex_unlock(&currentModeMutex);
    return match;
}

/* How long the buffer-polling threads sleep when they have nothing to do.
 * While data is waiting they keep their old, much shorter, interval. */
#define MODEM_IDLE_POLL_US 5000U

/*
 * isCurrentModeIdle()
 *
 * Tests whether the host is currently idle, meaning no protocol has the
 * radio.
 *
 * Parameters: none.
 *
 * Returns: true if the current mode is "idle".
 */
static bool isCurrentModeIdle(void)
{
    return isCurrentMode("idle");
}

/*
 * sigintHandler()
 *
 * Handles Ctrl+C so the program can exit cleanly.  Every protocol client
 * that is connected is switched off in the modem with setProtocol(), then
 * after a one second pause the exit flags are set so that all threads stop.
 * The default SIGINT action is restored so that a second Ctrl+C ends the
 * program at once.
 *
 * Parameters:
 *   signum - Signal number (not used).
 *
 * Returns: nothing.
 */
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


/*
 * setSpace()
 *
 * Records whether the modem has room for more data, by setting the
 * isModemSpace flag that the TX threads wait on.
 *
 * Parameters:
 *   space - Free space reported by the modem; 0 means none.
 *
 * Returns: nothing.
 */
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
 * frame per loop pass, which is barely real time and starves a
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

/*
 * openmtCreditFromStatus()
 *
 * Turns a GET_STATUS reply into the number of frames the host may send.  The
 * reply describes the modem at the moment it read the request, so the sample
 * frames queued since that request are subtracted from the free count.  This
 * consumes the oldest outstanding request record.
 *
 * Parameters:
 *   freeFrames - Free frame count from the low nibble of the reply (0 to
 *                15).
 *
 * Returns: Free frames minus frames still in flight.  It can be negative.
 */
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

/*
 * openmtResyncPacing()
 *
 * Forgets all outstanding status requests, so the next reply is taken as it
 * stands with nothing in flight.  Used when replies and requests have
 * drifted apart.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void openmtResyncPacing(void)
{
    pthread_mutex_lock(&modemTxBufMutex);
    openmtReqHead  = 0;
    openmtReqCount = 0;
    pthread_mutex_unlock(&modemTxBufMutex);
}

/*
 * openmtPacing()
 *
 * Tells whether credit based flow control applies.  It is used only for
 * modem type "openmt" (SAMPLES mode); the "openmths" and "openmtsx" types
 * are not paced.
 *
 * Parameters: none.
 *
 * Returns: true if the bridge published "creditPacing" as true: its modem
 *   has a fixed TX buffer that has to be asked about.
 */
static bool openmtPacing(void)
{
    return creditPacing;
}

/*
 * requestModemStatus()
 *
 * Queues a GET_STATUS request for the modem in the modem TX buffer, and
 * records how many sample frames had been queued at that point so the reply
 * can be matched to them.  If the request list is full, the oldest request
 * is dropped, since its reply must have been lost.  The request is not
 * queued if the TX buffer is full.
 *
 * Parameters:
 *   force - If true, send the request even if one was sent less than
 *           OPENMT_POLL_MS (4 ms) ago.
 *
 * Returns: nothing.
 */
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

/*
 * setProtocol()
 *
 * Tells the bridge that a mode is wanted or not wanted any more.  The bridge
 * knows what the name means for its modem and does whatever that needs.
 *
 * Parameters:
 *   protocol - The mode name the service registered with.
 *   enabled  - true to enable the protocol, false to disable it.
 *
 * Returns: nothing.
 */
void setProtocol(const char* protocol, bool enabled)
{
    /* the bridge knows what the mode name means and does the rest */
    sendBridgeControl(enabled ? BRIDGE_CTL_MODE_ON : BRIDGE_CTL_MODE_OFF, protocol, strlen(protocol));
}

/*
 * error()
 *
 * Prints the system error text for the current errno, with a message in
 * front of it, and ends the program with exit status 1.
 *
 * Parameters:
 *   msg - Text to print before the error text.
 *
 * Returns: nothing.
 */
void error(char* msg)
{
    reportError(modemName, "Modem", "%s: %s", msg, strerror(errno));
    exit(1);
}

;

/*
 * openSerial()
 *
 * Opens and sets up the modem serial port for raw 8N1 data without flow
 * control.  The speed comes from the global modemBaud: 921600, 460800,
 * 230400 or 115200, and 115200 for any other value.  Reads time out after
 * one second.
 *
 * Parameters:
 *   serial - Device name of the port, such as /dev/ttyAMA0.
 *
 * Returns: The open file descriptor, or a negative number if the port could
 *   not be opened or configured.
 */
int openSerial(char* serial)
{
    struct termios tty;
    unsigned int speed = 0;
    int fd;

    fd = open(serial, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0)
    {
        reportError(modemName, "Modem", "Modem_Host: error when opening %s, errno=%d", serial, errno);
        return fd;
    }

    if (tcgetattr(fd, &tty) != 0)
    {
        reportError(modemName, "Modem", "Modem_Host: error %d from tcgetattr", errno);
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
        reportError(modemName, "Modem", "Modem_Host: error %d from tcsetattr", errno);
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

/*
 * modemRxFill()
 *
 * Does one read from the modem serial port into the receive buffer, moving
 * the waiting bytes to the start of the buffer first if needed.
 *
 * Parameters: none.
 *
 * Returns: The number of bytes added; 0 on a timeout or an interrupted read;
 *   -1 on a read error.
 */
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

/*
 * modemRxNeed()
 *
 * Reads from the serial port until at least n bytes are in the receive
 * buffer.  The port's own one second read timeout applies.
 *
 * Parameters:
 *   n - Number of bytes needed.
 *
 * Returns: 1 if n bytes are available, 0 if the read timed out first, -1 on
 *   an error.
 */
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

/*
 * modemRxNeedShort()
 *
 * Like modemRxNeed(), but waits at most a given time for the bytes, using
 * poll().
 *
 * Parameters:
 *   n  - Number of bytes needed.
 *   ms - Longest time to wait, in milliseconds, for each wait for data.
 *
 * Returns: 1 if n bytes are available, 0 if they did not arrive in time, -1
 *   on an error.
 */
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

/*
 * modemRead()
 *
 * Reads up to n bytes from the receive buffer, which is refilled from the
 * serial port if it is empty.  It works like read() on the port.
 *
 * Parameters:
 *   dst - Output buffer.
 *   n   - Largest number of bytes to read.
 *
 * Returns: The number of bytes copied; 0 if nothing arrived within the read
 *   timeout; -1 on an error.
 */
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

/*
 * modemDiscard()
 *
 * Throws away bytes from the front of the receive buffer while the reader is
 * out of step with the modem's frames.  The first discard of a loss of sync
 * prints one "lost frame sync" message and counts it; later ones are only
 * counted.
 *
 * Parameters:
 *   n - Number of bytes to throw away.
 *
 * Returns: nothing.
 */
static void modemDiscard(size_t n)
{
    if (!modemHunting)
    {
        modemHunting = true;
        modemSkipped = 0;
        modemResyncs++;
        fprintf(stderr, "Modem_Host: lost frame sync with the modem (0x%02X where a frame should start), resyncing\n",
                modemRxBuf[modemRxHead]);
        reportError(modemName, "Modem", "lost frame sync with the modem, resyncing");
    }
    modemSkipped += (uint32_t)n;
    modemSkippedTotal += (uint32_t)n;
    modemRxHead += n;
    modemRxCount -= n;
}

/*
 * modemGetFrame()
 *
 * Gets the next whole frame from the modem: 0x61, length (3 to 255, covering
 * the whole frame), type, payload.  A frame is only accepted if the byte
 * behind it, when it is there, is the start of the next frame; if it is not,
 * one byte is dropped and the search goes on.  This resynchronises the
 * stream after bytes were lost.
 *
 * Parameters:
 *   frame    - Output buffer of at least 255 bytes.
 *   frameLen - Receives the length of the frame.
 *
 * Returns: 1 if a frame is ready, 2 if nothing was received, 3 if bytes were
 *   thrown away (call again), 0 on a read error.
 */
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

/*
 * setMode()
 *
 * Tells the modem to change its operating mode (a SET_MODE command on the
 * BIT and SAMPLE streams).  A bridge whose modem has no such command drops
 * it.  FRAME streams never get here: the bridge changes its modem's mode
 * itself when the service asks for it.
 *
 * Parameters:
 *   mode - Mode value to set.
 *
 * Returns: nothing.
 */
void setMode(const uint8_t mode)
{
    uint8_t buffer[3];

    buffer[0] = 0x61;
    buffer[1] = 0x03;
    buffer[2] = (uint8_t)(OPENMT_SET_MODE | mode);
    pthread_mutex_lock(&modemComBufMutex);
    RingBuffer_addData(&modemCommandBuffer, buffer, 3);
    pthread_mutex_unlock(&modemComBufMutex);
}


/*
 * timerThread()
 *
 * Runs the timer tick of the host: advances all timers by the real time that
 * has passed since the last tick.  If the txTimeout timer fires while a
 * transmission is on (BIT and SAMPLE streams; the bridge of a FRAME modem
 * watches the transmit time itself), the transmission is ended: txOn is
 * cleared, the mode becomes idle, rxModeBuffer is emptied and the modem is
 * told to release its mode.  The thread stops when the running flag is
 * cleared.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL; the thread ends by calling pthread_exit().
 */
void* timerThread(void* arg)
{
    uint64_t lastTick = 0;          /* timers are advanced by the real time that passed */

    while (atomic_load(&running))
    {
        delay(HOST_TIMER_PERIOD_US);
        uint32_t elapsed = timerElapsedMs(&lastTick);

        for (uint8_t i = 0; i < 10; i++)
        {
            timerTickN(i, elapsed);
        }

        if (isTimerTriggered("txTimeout") && atomic_load(&txOn))
        {
            atomic_store(&txOn, false);
            setCurrentMode("idle");
            setStatus(modemName, "main", "active_mode", "idle");
            pthread_mutex_lock(&rxBufModeMutex);
            RingBuffer_clear(&rxModeBuffer);
            pthread_mutex_unlock(&rxBufModeMutex);
            /* release the mode the modem holds for the host */
            setMode(0x0f);
            resetTimer("txTimeout", 0);
            fprintf(stderr, "TX timeout. Setting mode to idle.\n");
        }
        else if (!atomic_load(&txOn))
            resetTimer("txTimeout", 0);
    }

    fprintf(stderr, "Timer thread exited.\n");
    pthread_exit(NULL);
    return NULL;
}

// Send data to protocol service.
// One thread per service connection.
/*
 * logBadTxHeader()
 *
 * Prints a diagnostic line for an invalid frame header found in
 * txModeBuffer: the byte seen, the byte that was dropped, the modem type,
 * the current mode, how many bytes are left and the next eight of them.  The
 * caller holds txBufModeMutex and has already removed the bad byte.
 *
 * Parameters:
 *   first   - The byte found where a 0x61 frame start was expected.
 *   dropped - The byte that was removed from the buffer.
 *
 * Returns: nothing.
 */
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
    fprintf(stderr, "txModeBuffer: invalid header [%02X]  [%02X]  stream=%s mode=%s left=%u next: %s\n",
            first, dropped, bridgeStreamName(streamType), currentMode, (unsigned)avail, hex);
}

/*
 * modeRxThread()
 *
 * Sends data from the modem to one protocol service.  There is one thread
 * for each connected service.  Twice a millisecond it takes a complete frame
 * from txModeBuffer and writes it to the service's socket, without its
 * trailing connection id byte: on a FRAME stream the frame whose connection
 * id is this service's (the bridge chose it); on the other streams a frame if
 * the service's mode is the current mode.  Frames with a bad header or length are dropped one byte at
 * a time to find the next frame.  It also sends the commands queued for this
 * service in its command buffer.  When a write fails or the service goes
 * away the connection is marked inactive and the thread ends.
 *
 * Parameters:
 *   arg - Connection index into hostClient[], passed as a pointer sized
 *         integer.
 *
 * Returns: NULL; the thread ends by calling pthread_exit().
 */
void* modeRxThread(void* arg)
{
    uint8_t conn_id = (intptr_t)arg;
    int sockfd      = hostClient[conn_id].sockfd;
    bool found      = false;
    bool fast       = false;    /* true: a frame was just handled, look again soon */

    while (hostClient[conn_id].active && running)
    {
        delay(fast ? 500 : MODEM_IDLE_POLL_US);
        fast = false;

        pthread_mutex_lock(&txBufModeMutex);
        if (RingBuffer_dataSize(&txModeBuffer) >= 4)
        {
            uint8_t first[3];

            RingBuffer_peek(&txModeBuffer, first, 1);
            if (first[0] != 0x61)
            {
                uint8_t tmp[1];
                RingBuffer_getData(&txModeBuffer, tmp, 1);
                fast = true;
                logBadTxHeader(first[0], tmp[0]);   /* caller holds txBufModeMutex */
                pthread_mutex_unlock(&txBufModeMutex);
                continue;
            }

            /* A frame in txModeBuffer is the service frame followed by the
             * connection id byte. */
            RingBuffer_peek(&txModeBuffer, first, 3);
            uint16_t len = (first[1] << 8) + first[2] + 1;
            if (len < 5 || len > 1024)
            {
                /* corrupt header: drop a byte and resync instead of building a
                 * huge on-stack array / reading past the data in the ring */
                uint8_t junk[1];
                RingBuffer_getData(&txModeBuffer, junk, 1);
                fast = true;
                fprintf(stderr, "txModeBuffer: bad length %u, resync\n", (unsigned)len);
                pthread_mutex_unlock(&txBufModeMutex);
                continue;
            }
            if (RingBuffer_dataSize(&txModeBuffer) >= len)
            {
                uint8_t buf[len];
                bool    mine;

                RingBuffer_peek(&txModeBuffer, buf, len);
                if (streamType == STREAM_FRAME)
                {
                    /* the bridge decided who gets the frame and said so */
                    uint8_t tag = buf[len - 1];

                    mine = (tag == conn_id);
                    if (!mine && (tag >= MAX_CLIENT_CONNECTIONS || !hostClient[tag].active))
                    {
                        RingBuffer_getData(&txModeBuffer, buf, len);    /* nobody to give it to */
                        fast = true;
                    }
                }
                else
                {
                    mine = strcasecmp(hostClient[conn_id].mode, currentMode) == 0;
                }

                if (mine)
                {
                    RingBuffer_getData(&txModeBuffer, buf, len);
                    fast = true;
                    len--;  // do not include conn id byte.
                    if (write(sockfd, buf, len) < 0)
                    {
                        pthread_mutex_unlock(&txBufModeMutex);
                        reportError(modemName, "Modem", "remote disconnect");
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
                fast = true;
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
                fast = true;
                pthread_mutex_unlock(&hostClient[conn_id].commandMutex);
                fprintf(stderr, "modeTx bad command length %u\n", (unsigned)len);
                continue;
            }
            if (RingBuffer_dataSize(&hostClient[conn_id].command) >= len)
            {
                uint8_t buf[len];
                RingBuffer_getData(&hostClient[conn_id].command, buf, len);
                fast = true;

                if (write(sockfd, buf, len) < 0)
                {
                    pthread_mutex_unlock(&hostClient[conn_id].commandMutex);
                    reportError(modemName, "Modem", "remote disconnect");
                    break;
                }
            }
        }
        pthread_mutex_unlock(&hostClient[conn_id].commandMutex);
    }

    hostClient[conn_id].active = false;
    delay(50000);
    fprintf(stderr, "Mode RX thread exited. Connection Id: %d\n", conn_id);
    pthread_exit(NULL);
    return NULL;
}

/*
 * modeTxThread()
 *
 * Moves data from the protocol services to the modem's transmit buffer.  It
 * takes whole frames from rxModeBuffer and copies them to modemTxBuffer when
 * there is room.  For modem type "openmt" the sample data frames are paced
 * by credit: each frame sent uses one credit, and when it is used up the
 * thread waits for a status reply from the modem, which gives fresh credit.
 * If the wait lasts more than 250 ms the request bookkeeping is reset and
 * one line "openmt pacing stall" is printed per second.  Other frames (end
 * of transmission, mode and configuration) are never held back.  Frames from
 * modemCommandBuffer are copied to modemTxBuffer as well.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL; the thread ends by calling pthread_exit().
 */
void* modeTxThread(void* arg)
{
    while (atomic_load(&running))
    {
        /* Short interval while there is something to send or a transmission
         * is on; otherwise look only every MODEM_IDLE_POLL_US. */
        bool busy = atomic_load(&txOn) || RingBuffer_dataSize(&rxModeBuffer) != 0 ||
                    RingBuffer_dataSize(&modemCommandBuffer) != 0;

        delay(busy ? (openmtPacing() ? 500 : 3000) : MODEM_IDLE_POLL_US);

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
        }

        if (RingBuffer_dataSize(&modemCommandBuffer) >= 3)
        {
            pthread_mutex_lock(&modemComBufMutex);
            uint8_t buf[3];
            RingBuffer_peek(&modemCommandBuffer, buf, 1);
            if (buf[0] != 0x61 && buf[0] != BRIDGE_CTL_START)
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
    pthread_exit(NULL);
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

/*
 * modemTxThread()
 *
 * Writes queued frames to the modem's serial port.  It takes one complete
 * frame at a time from modemTxBuffer and writes it.  A frame with a bad header or length is dropped a
 * byte at a time.  A warning is printed if a sample data frame is not 196
 * bytes long.  If the environment variable OPENMT_TXLOG is set, a copy of
 * every byte written is saved in that file, with the time of each frame in a
 * companion file, and a summary line is printed every five seconds.  The
 * optional gaps after status requests and sample frames
 * (OPENMT_STATUS_GAP_US, OPENMT_FRAME_GAP_US) are off by default.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL; the thread ends by calling pthread_exit().
 */
void* modemTxThread(void* arg)
{
    while (atomic_load(&running))
    {
        /* The old 10 microsecond poll is kept while a transmission is on or
         * anything is queued for the modem, so frames leave as before.  With
         * everything idle the thread looks only every MODEM_IDLE_POLL_US. */
        bool busy = atomic_load(&txOn) || RingBuffer_dataSize(&modemTxBuffer) != 0 ||
                    RingBuffer_dataSize(&rxModeBuffer) != 0 || RingBuffer_dataSize(&modemCommandBuffer) != 0;

        delay(busy ? 10 : MODEM_IDLE_POLL_US);

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
        if (buf[0] != 0x61 && buf[0] != BRIDGE_CTL_START)
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
        int ret = write(serialModemFd, buf, len);
        if (ret != len)
        {
            reportError(modemName, "Modem", "modem TX write failed. (%d of %u)", ret, (unsigned)len);
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
    pthread_exit(NULL);
    return NULL;
}

/*
 * commandThread()
 *
 * Handles commands that are not part of the voice data.  While the host is
 * idle, and the stream is not a FRAME stream, frames from the modem in
 * txModeBuffer are sent to all connected services.  When the "chkComTimeout"
 * timer fires, it reads the dashboard command "modemHost" from the database.
 * "setConfig" tells the bridge to read its settings again, tells every
 * connected service the duplex or simplex setting and acknowledges the
 * command as "success".  "reloadOffsets", "calibrate" and "probe" are passed
 * to the bridge, which acknowledges them itself.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL; the thread ends by calling pthread_exit().
 */
void* commandThread(void* arg)
{
    bool fast = false;      /* true: a frame was just forwarded, look again soon */

    while (atomic_load(&running))
    {
        if (isCurrentModeIdle())
            delay(fast ? 500 : MODEM_IDLE_POLL_US);
        else
            delay(50000);
        fast = false;

        if (isCurrentModeIdle())
        {
            if (streamType != STREAM_FRAME)
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
                        fast = true;
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
                        fast = true;
                        len--;  // do not include conn id byte.
                        for (uint8_t i = 0; i < MAX_CLIENT_CONNECTIONS; i++)
                        {
                            if (hostClient[i].active)
                            {
                                if (write(hostClient[i].sockfd, buf, len) < 0)
                                {
                                    pthread_mutex_unlock(&txBufModeMutex);
                                    reportError(modemName, "Modem", "remote disconnect");
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
            if (strcasecmp(parameter, "reloadOffsets") == 0 || strncasecmp(parameter, "calibrate ", 10) == 0 ||
                strncasecmp(parameter, "probe ", 6) == 0)
            {
                /* the bridge knows what these mean for its modem and acknowledges the command itself */
                sendBridgeControl(BRIDGE_CTL_COMMAND, parameter, strlen(parameter));
            }
            else if (strcasecmp(parameter, "setConfig") == 0)
            {
                /* the bridge reads its settings again; the duplex or simplex setting reaches the services below */
                refreshBridgedSettings();
                sendBridgeControl(BRIDGE_CTL_RELOAD, NULL, 0);

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
    pthread_exit(NULL);
    return NULL;
}

/*
 * relayToBridge()
 *
 * Passes a frame of a service to the bridge of a FRAME modem, wrapped in a
 * relay frame (the bridge knows what the modem needs).  The frame goes into
 * the command buffer, so it keeps its order relative to the control frames.
 * A frame that does not fit is dropped.
 *
 * Parameters:
 *   conn   - Connection id of the service.
 *   tlv    - The service frame.
 *   tlvLen - Its length.
 *
 * Returns: nothing.
 */
static void relayToBridge(uint8_t conn, const uint8_t* tlv, size_t tlvLen)
{
    uint8_t frame[BUFFER_LENGTH + BRIDGE_RELAY_HEADER];
    size_t  n = bridgeRelayBuild(conn, tlv, tlvLen, frame, sizeof(frame));

    if (n == 0)
        return;
    pthread_mutex_lock(&modemComBufMutex);
    if (RingBuffer_freeSpace(&modemCommandBuffer) >= n)
        RingBuffer_addData(&modemCommandBuffer, frame, n);
    else
        fprintf(stderr, "Modem_Host: bridge buffer full, frame dropped\n");
    pthread_mutex_unlock(&modemComBufMutex);
}

/*
 * modeRelayTxThread()
 *
 * Writes the frames in modemCommandBuffer to the bridge of a FRAME modem:
 * relay frames (service frames) and control frames, one whole frame at a
 * time and in the order they were queued.  Nothing is paced here; the bridge
 * does that.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL; the thread ends by calling pthread_exit().
 */
void* modeRelayTxThread(void* arg)
{
    static uint8_t buf[BUFFER_LENGTH + BRIDGE_RELAY_HEADER];

    while (atomic_load(&running))
    {
        bool wrote = false;

        for (;;)
        {
            uint8_t head[3];
            size_t  len;

            pthread_mutex_lock(&modemComBufMutex);
            if (RingBuffer_dataSize(&modemCommandBuffer) < 3)
            {
                pthread_mutex_unlock(&modemComBufMutex);
                break;
            }
            RingBuffer_peek(&modemCommandBuffer, head, 3);
            if (head[0] == BRIDGE_RELAY_START)
                len = ((size_t)head[1] << 8) | head[2];
            else if (head[0] == BRIDGE_CTL_START)
                len = head[1];
            else
                len = 0;
            if (len < 3 || len > sizeof(buf))
            {
                uint8_t junk[1];

                RingBuffer_getData(&modemCommandBuffer, junk, 1);   /* resync */
                pthread_mutex_unlock(&modemComBufMutex);
                fprintf(stderr, "modem command: invalid header [%02X]\n", head[0]);
                continue;
            }
            if (RingBuffer_dataSize(&modemCommandBuffer) < len)
            {
                pthread_mutex_unlock(&modemComBufMutex);
                break;
            }
            RingBuffer_getData(&modemCommandBuffer, buf, len);
            pthread_mutex_unlock(&modemComBufMutex);

            for (size_t done = 0; done < len;)
            {
                ssize_t w = write(serialModemFd, buf + done, len - done);

                if (w < 0)
                {
                    if (errno == EINTR || errno == EAGAIN)
                        continue;
                    reportError(modemName, "Modem", "bridge write failed.");
                    break;
                }
                done += (size_t)w;
            }
            wrote = true;
        }
        delay(wrote ? 500 : MODEM_IDLE_POLL_US);
    }

    fprintf(stderr, "Mode TX thread exited.\n");
    pthread_exit(NULL);
    return NULL;
}

/*
 * processClientSocket()
 *
 * Serves one protocol service connected to the host's TCP port.  It reads frames of the form 0x61,
 * 16 bit length, type, and handles the MODE frame that registers the
 * service: the name and transmit level are stored, addMode() and
 * setProtocol() are called, the bridge is told the connection's name and the
 * duplex or simplex command is queued for the service.  On a FRAME stream
 * every other frame is passed to the bridge unchanged (relayToBridge()); on
 * the BIT and SAMPLE streams sample data, bit data and their end frames are
 * put into rxModeBuffer, after checking that the buffer has room (frames
 * that do not fit are dropped), and set or clear txOn, and the mode commands
 * change the current mode.  When the service disconnects, it marks the
 * protocol inactive, removes the gateway and mode with delGateway() and
 * delMode(), tells the bridge and closes the socket.
 *
 * Parameters:
 *   arg - Connection index into hostClient[], passed as a pointer sized
 *         integer.
 *
 * Returns: NULL; the thread ends by calling pthread_exit().
 */
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
    bzero(buffer, BUFFER_LENGTH);

    int flags = fcntl(sockFd, F_GETFL, 0);
    if (flags == -1)
    {
        reportError(modemName, "Modem", "fcntl F_GETFL: %s", strerror(errno));
    }

    while (hostClient[conn_id].active && running)
    {
        len = read(sockFd, buffer, 1);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            /* a host went away (restart, crash): drop this connection only,
             * never exit() the whole Modem_Host */
            reportError(modemName, "Modem", "client connection closed remotely: %s", strerror(errno));
            break;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            delay(5);
            //       continue;
        }

        if (len != 1)
        {
            reportError(modemName, "Modem", "Modem_Host: error when reading from client, errno=%d", errno);
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
                hexDump((char*)"Modem mode data:", (unsigned char*)buffer, 62);

            {
                /* The bridge needs the name that goes with the connection: the
                 * mode's frames and settings are found by it. */
                char    conn[BRIDGE_CTL_MAX];
                size_t  nlen = strnlen(mode[conn_id].name, sizeof(mode[conn_id].name));

                conn[0] = (char)conn_id;
                memcpy(conn + 1, mode[conn_id].name, nlen);
                sendBridgeControl(BRIDGE_CTL_MODE_CONN, conn, nlen + 1);

                if (streamType != STREAM_FRAME)
                {
                    /* the modem of these streams holds a block per mode */
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

        if (streamType == STREAM_FRAME)
        {
            /* the bridge turns the frame into what its modem speaks */
            relayToBridge(conn_id, buffer, respLen);
            continue;
        }

        // *********** Convert from OpenMT type to the stream's frame type ***********
        if (typeLen != 1)
            type = 0;
        if (memcmp(buffer + 4, TYPE_ACK, typeLen) == 0)
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

            case COMM_SET_MODE:
                if (isCurrentModeIdle())
                {
                    setCurrentMode(hostClient[conn_id].mode);
                    setProtocol(hostClient[conn_id].mode, true);
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
       //                 hexDump((char*)"Bits", (unsigned char*)buffer, respLen);
                }
                else
                {
                    fprintf(stderr, "rxModeBuffer out of space. [%d]\n", RingBuffer_freeSpace(&rxModeBuffer));
                }
                pthread_mutex_unlock(&rxBufModeMutex);
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
    if (streamType != STREAM_FRAME)
    {
        uint8_t buf[3] = {0x61, 0x03, (uint8_t)(OPENMT_DEL_MODE | (conn_id & 0x0f))};
        pthread_mutex_lock(&modemComBufMutex);
        RingBuffer_addData(&modemCommandBuffer, buf, 3);
        pthread_mutex_unlock(&modemComBufMutex);
    }
    /* FRAME: the bridge sent the modem its new configuration when it was told the mode was off */

    delay(50000);
    close(sockFd);
    fprintf(stderr, "Client thread exited. Connection Id: %d\n", conn_id);
    pthread_mutex_unlock(&shutDownMutex);

    pthread_exit(NULL);
    return NULL;
}

/*
 * startTCPServer()
 *
 * Runs the TCP server that the protocol services connect to.  It listens on
 * host_port with SO_REUSEADDR and TCP_NODELAY, accepts connections, finds a
 * free hostClient[] slot for each one and starts a processClientSocket
 * thread and a modeRxThread for it.  When the accept loop ends the running
 * flag is cleared so the rest of the host shuts down.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL; the thread ends by calling pthread_exit().
 */
void* startTCPServer(void* arg)
{
    struct hostent* hostp; /* client host info */
    int childfd;           /* child socket */
    int sockFd;

    sockFd = socket(AF_INET, SOCK_STREAM, 0);

    setNoDelay(sockFd);
    if (sockFd < 0)
    {
        reportError(modemName, "Modem", "Modem_Host: error when creating the socket: %s", strerror(errno));
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
        reportError(modemName, "Modem", "Modem_Host: error when binding the socket to port %u: %s", host_port, strerror(errno));
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
            reportError(modemName, "Modem", "accept failed: %s", strerror(errno));
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
            reportError(modemName, "Modem", "ERROR on inet_ntoa");
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
            reportError(modemName, "Modem", "Can't create client process thread :[%s]", strerror(err));
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
            reportError(modemName, "Modem", "Can't create mode rx thread :[%s]", strerror(err));
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
    delay(500000);
    pthread_mutex_unlock(&shutDownMutex);
    atomic_store(&running, false);
    pthread_exit(NULL);
    return NULL;
}

/*
 * modemReadFull()
 *
 * Reads exactly n bytes from the modem port, waiting a while for slow ones.
 *
 * Parameters:
 *   dst - Output buffer.
 *   n   - Number of bytes wanted.
 *
 * Returns: true if all n bytes were read; false on a read error or when they
 *   did not arrive within about two seconds.
 */
static bool modemReadFull(uint8_t* dst, size_t n)
{
    size_t got   = 0;
    int    waits = 0;

    while (got < n)
    {
        ssize_t len = modemRead(dst + got, n - got);

        if (len < 0)
            return false;
        if (len == 0)
        {
            if (++waits > 400 || !atomic_load(&running))
                return false;
            delay(5000);
        }
        else
        {
            got += (size_t)len;
            waits = 0;
        }
    }
    return true;
}

/*
 * bridgeActive()
 *
 * The bridge says which connection owns the modem now.  The host only keeps
 * the name for the dashboard and for the services that ask.
 *
 * Parameters:
 *   conn - Connection id, BRIDGE_CONN_NONE for idle.
 *
 * Returns: nothing.
 */
static void bridgeActive(uint8_t conn)
{
    const char* name = "idle";

    if (conn < MAX_CLIENT_CONNECTIONS && hostClient[conn].active)
        name = mode[conn].name;
    setCurrentMode(name);
    setStatus(modemName, "main", "active_mode", name);
    if (debugM)
        fprintf(stderr, "Current mode set to %s.\n", name);
}

/*
 * processRelaySerial()
 *
 * Reads and handles one frame from the bridge of a FRAME modem.  A relay
 * frame (0x62, length, connection id, service frame) is put into
 * txModeBuffer, service frame first and the connection id behind it, for the
 * connection's thread to send; a control frame tells which connection owns
 * the modem.  Anything else is skipped.  The host does not look inside the
 * service frames.
 *
 * Parameters: none.
 *
 * Returns: 0 on a serial error, 1 when something was handled, 2 when there
 *   was nothing to read.
 */
int processRelaySerial(void)
{
    uint8_t buffer[BUFFER_LENGTH + 8];
    ssize_t len;

    len = modemRead(buffer, 1);
    if (len == 0)
        return 2;
    if (len != 1)
    {
        reportError(modemName, "Modem", "Modem_Host: error when reading from the bridge, errno=%d", errno);
        return 0;
    }

    if (buffer[0] == BRIDGE_RELAY_START)
    {
        size_t total;

        if (!modemReadFull(buffer + 1, 2))
            return 1;
        total = ((size_t)buffer[1] << 8) | buffer[2];
        if (total < BRIDGE_RELAY_HEADER + 5 || total > sizeof(buffer))
        {
            fprintf(stderr, "Modem_Host: bad relay frame length %zu from the bridge\n", total);
            return 1;
        }
        if (!modemReadFull(buffer + 3, total - 3))
            return 1;
        if (buffer[BRIDGE_RELAY_HEADER] != 0x61)
        {
            fprintf(stderr, "Modem_Host: relay frame without a service frame from the bridge\n");
            return 1;
        }

        size_t tlvLen = total - BRIDGE_RELAY_HEADER;

        pthread_mutex_lock(&txBufModeMutex);
        if (RingBuffer_freeSpace(&txModeBuffer) < tlvLen + 1)
        {
            fprintf(stderr, "txModeBuffer out of space.\n");
            RingBuffer_clear(&txModeBuffer);
        }
        RingBuffer_addData(&txModeBuffer, buffer + BRIDGE_RELAY_HEADER, tlvLen);
        RingBuffer_addData(&txModeBuffer, buffer + 3, 1);       /* the connection id */
        pthread_mutex_unlock(&txBufModeMutex);
        return 1;
    }

    if (buffer[0] == BRIDGE_CTL_START)
    {
        if (!modemReadFull(buffer + 1, 1))
            return 1;
        if (buffer[1] < BRIDGE_CTL_LENGTH || buffer[1] > BRIDGE_CTL_MAX)
            return 1;
        if (!modemReadFull(buffer + 2, buffer[1] - 2U))
            return 1;
        if (buffer[2] == BRIDGE_CTL_ACTIVE && buffer[1] >= BRIDGE_CTL_LENGTH + 1)
            bridgeActive(buffer[3]);
        return 1;
    }

    fprintf(stderr, "Modem_Host: unknown byte from the bridge, 0x%02X\n", buffer[0]);
    return 1;
}

/*
 * frameHasText()
 *
 * Looks for a text string inside a block of bytes.
 *
 * Parameters:
 *   data   - Bytes to search.
 *   n      - Number of bytes in data.
 *   needle - Zero terminated text to find.
 *
 * Returns: true if needle is found in data, false if not.
 */
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

/*
 * processSerial()
 *
 * Reads and handles one frame from an OpenMT modem.  The frame type is taken
 * from the upper four bits of byte 2.  NAK, ACK and version frames are
 * logged or noted.  Sample and bit data are put into txModeBuffer with the
 * connection id added at the end.  Timing frames are forwarded as they
 * are; the bridge sends them only when they are wanted.  A status reply gives the free space; for the paced "openmt" modem
 * openmtCreditFromStatus() turns it into credit, openmtLastFree and
 * openmtCredit are stored and setSpace() is called.  A debug frame (type
 * 0xC0) with a firmware version text containing " id=" is printed once for
 * each different text.
 *
 * Parameters: none.
 *
 * Returns: 0 on a serial error, 1 when a frame was handled, 2 when there was
 *   nothing to read, 3 when the frame was invalid and skipped.
 */
int processSerial(void)
{
    uint8_t buffer[BUFFER_LENGTH];
    uint16_t respLen = 0;
    uint8_t type;
    int rc;

    rc = modemGetFrame(buffer, &respLen);
    if (rc == 0)
    {
        reportError(modemName, "Modem", "Modem_Host: error when reading from Modem, errno=%d", errno);
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
            hexDump((char*)"SAMP", buffer, respLen);
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
    //        hexDump((char*)"BITS", buffer, respLen);
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
        /* TX/RX sample alignment report (8-byte payload).  The bridge sends
         * it only when the active mode wants it, so it can be forwarded as it
         * is, through txModeBuffer in arrival order with the SAMP records --
         * its position in the sample stream is part of what it means. */
        if (respLen == 3U + 8U)
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
            hexDump((char*)"STATUS:", buffer, respLen);
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
        hexDump((char*)"Modem rec data:", buffer, respLen);
    }
    delay(1000);
    return 1;
}


/* ------------------------------------------------------------------ */
/* Say why the process died: fatal signal name + a backtrace on stderr */
/*
 * fatalSignalHandler()
 *
 * Reports a fatal signal.  It prints the signal name and a backtrace to
 * stderr and ends the process with exit status 128 plus the signal number.
 *
 * Parameters:
 *   sig - Signal number received.
 *
 * Returns: nothing.
 */
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

/*
 * installCrashHandlers()
 *
 * Installs fatalSignalHandler() for SIGSEGV, SIGBUS, SIGFPE, SIGABRT,
 * SIGILL, SIGTERM and SIGHUP.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void installCrashHandlers(void)
{
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGABRT, SIGILL, SIGTERM, SIGHUP};
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        signal(sigs[i], fatalSignalHandler);
}

/*
 * resolveStreamType()
 *
 * Finds out which kind of packet stream the modem delivers.  The bridge in
 * front of the modem publishes it in the database (setting "streamType"),
 * with a few facts about its modem the host needs for pacing.  The bridge
 * may still be starting, so the host waits for it.
 *
 * Parameters: none.
 *
 * Returns: true when a bridge has published its stream type; sets
 *   streamType and creditPacing.
 */
static bool resolveStreamType(void)
{
    StreamType published = STREAM_NONE;

    for (int tries = 0; tries < 60 && atomic_load(&running); tries++)
    {
        published = bridgeReadStreamType(modemName);
        if (published != STREAM_NONE)
            break;
        if (tries == 0)
            fprintf(stderr, "Modem_Host: waiting for a bridge to publish its stream type...\n");
        sleep(1);
    }
    if (published == STREAM_NONE)
    {
        reportError(modemName, "Modem", "Modem_Host: no bridge has published a stream type. Start the bridge for your modem first.");
        return false;
    }
    streamType = published;

    char tmp[16] = "";

    readHostConfigN(modemName, "config", "creditPacing", tmp, sizeof(tmp));
    creditPacing = (strcasecmp(tmp, "true") == 0);

    fprintf(stderr, "Modem_Host: stream type %s (published by the bridge)\n", bridgeStreamName(streamType));
    return true;
}

/*
 * sendBridgeControl()
 *
 * Queues a control frame for the bridge on the serial port, through the
 * command buffer the serial writer thread already empties, so the frame
 * cannot land in the middle of another frame.
 *
 * Parameters:
 *   type    - Control frame type, for example BRIDGE_CTL_RELOAD.
 *   payload - Payload bytes, NULL when there are none.
 *   plen    - Number of payload bytes.
 *
 * Returns: nothing.
 */
static void sendBridgeControl(uint8_t type, const char* payload, size_t plen)
{
    uint8_t frame[BRIDGE_CTL_MAX];
    size_t  len = bridgeCtlBuild(type, (const uint8_t*)payload, plen, frame);

    if (len == 0)
        return;
    /* the command buffer also carries the ADD_MODE and mode frames, so the order of the frames is kept */
    pthread_mutex_lock(&modemComBufMutex);
    RingBuffer_addData(&modemCommandBuffer, frame, len);
    pthread_mutex_unlock(&modemComBufMutex);
}

/*
 * refreshBridgedSettings()
 *
 * Reads again the settings Modem_Host itself needs when a bridge looks
 * after the modem: whether the repeater runs duplex, which the services are
 * told about.  Everything else is the bridge's business.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void refreshBridgedSettings(void)
{
    char tmp[16] = "";

    readHostConfigN(modemName, "config", "mode", tmp, sizeof(tmp));
    modem_duplex = (strcasecmp(tmp, "duplex") == 0);
}

/*
 * configWatchThread()
 *
 * Looks at the "config" settings of the modem every two seconds.  When
 * something was added or changed, and a bridge is in front of the modem, it
 * sends the bridge the reload control frame so the bridge reads its settings
 * again.  The first reading after the start only sets the baseline.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL; the thread ends when the running flag is cleared.
 */
void* configWatchThread(void* arg)
{
    uint64_t last     = 0;
    bool     haveLast = false;

    while (atomic_load(&running))
    {
        for (int i = 0; i < 20 && atomic_load(&running); i++)
            delay(100000);

        uint64_t now;

        if (!hostConfigDigest(modemName, "config", &now))
            continue;

        if (!haveLast)
        {
            last     = now;
            haveLast = true;
        }
        else if (now != last)
        {
            last = now;
            fprintf(stderr, "Modem_Host: settings changed, asking the bridge to read them again.\n");
            refreshBridgedSettings();
            sendBridgeControl(BRIDGE_CTL_RELOAD, NULL, 0);
        }
    }
    pthread_exit(NULL);
    return NULL;
}

/*
 * main()
 *
 * Starts the modem host.  Options: -m n (modem 1 to 10, sets the modem name
 * and TCP port), -d (run as a daemon), -v (verbose), -x (debug) and -r
 * (real-time priority for the serial reader).  It sets up the signal
 * handlers, the software timers (status, txTimeout, chkComTimeout) and the
 * ring buffers, loads or creates the default configuration in the database,
 * opens and flushes the serial port, and starts the threads:
 * modeRelayTxThread for FRAME streams, or modemTxThread and modeTxThread for
 * the others, then timerThread, startTCPServer and commandThread.  It sends
 * the start-up configuration for the modem type and, for the "openmt" modem,
 * also asks for the firmware version.  The main thread then reads the serial
 * port in a loop until the running flag is cleared or a read fails.  On exit
 * it records that the host stopped and releases the buffers.
 *
 * Parameters:
 *   argc - Number of command line arguments.
 *   argv - Command line arguments.
 *
 * Returns: 0 on exit.
 */
int main(int argc, char** argv)
{
    bool daemon = false;
    bool realtime = false;
    int ret;
    int c;

    while ((c = getopt(argc, argv, ":m:s:u:p:c:dxvr")) != -1)
    {
        switch (c)
        {
            case 'm':
            {
                modemId = atoi(optarg);
                if (modemId < 1 || modemId > 10)
                {
                    reportError(modemName, "Modem", "Invalid modem number.");
                    return 1;
                }
                sprintf(modemName, "modem%d", modemId);
                host_port = 18000 + modemId - 1;
                fprintf(stderr, "Modem name: %s\n", modemName);
            }
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
                fprintf(stdout, "Modem_Host: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            case 'r':
                realtime = true;
                break;
            default:
                fprintf(stderr, "Usage: Modem_Host [-m modem_number (1-10)] [-s dbserver] [-u dbuser] [-p dbpassword] [-c configfile] [-d] [-v] [-x] [-r]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            reportError(modemName, "Modem", "Modem_Host: error in fork(), exiting");
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
    RingBuffer_Init(&modemCommandBuffer, 16384);
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

    /* The settings of the modem itself ("config") belong to its bridge, which creates them.  These are the
       host's own: created on a new install without touching values already saved. */
    addHostConfigDefault(modemName, "main", "gateways", "none", "none");
    addHostConfigDefault(modemName, "main", "activeModes", "none", "none");
    addHostConfigDefault(modemName, "main", "username", "none", "admin");
    addHostConfigDefault(modemName, "main", "callsign", "none", "N0CALL");
    addHostConfigDefault(modemName, "main", "title", "none", "Repeater Dashboard");
    addHostConfigDefault(modemName, "main", "latitude", "none", "0.0");
    addHostConfigDefault(modemName, "main", "longitude", "none", "0.0");

    /* Settings added after the first release: create them on older installs
       without touching values already saved. */
    addHostConfigDefault(modemName, "main", "location", "none", "");
    addHostConfigDefault(modemName, "main", "description", "none", "");

    if (!resolveStreamType())
        return 1;

    {
        /* "port" and "baud" now belong to the bridge; the host opens its virtual port */
        char bridgePort[20] = "";

        readHostConfigN(modemName, "config", "bridgePort", bridgePort, sizeof(bridgePort));
        if (bridgePort[0] != '\0')
            snprintf(commPort, sizeof(commPort), "%s", bridgePort);
        refreshBridgedSettings();
    }
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

    if (streamType == STREAM_FRAME)
    {
        int err = pthread_create(&(modetxid), NULL, &modeRelayTxThread, NULL);
        if (err != 0)
        {
            reportError(modemName, "Modem", "Can't create FRAME mode tx thread:[%s]", strerror(err));
            return 1;
        }
        else
        {
            if (debugM)
                fprintf(stderr, "FRAME mode tx thread created successfully\n");
        }
    }
    else
    {
        int err = pthread_create(&(modemTxid), NULL, &modemTxThread, NULL);
        if (err != 0)
        {
            reportError(modemName, "Modem", "Can't create modem tx thread:[%s]", strerror(err));
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
            reportError(modemName, "Modem", "Can't create mode tx thread:[%s]", strerror(err));
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
        reportError(modemName, "Modem", "Can't create timer thread:[%s]", strerror(err));
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
        reportError(modemName, "Modem", "Can't create TCP server thread:[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM)
            fprintf(stderr, "TCP server thread created successfully\n");
    }

    err = pthread_create(&(cfgwatchid), NULL, &configWatchThread, NULL);
    if (err != 0)
        reportError(modemName, "Modem", "Can't create config watch thread:[%s]", strerror(err));

    err = pthread_create(&(commandid), NULL, &commandThread, NULL);
    if (err != 0)
    {
        reportError(modemName, "Modem", "Can't create command thread:[%s]", strerror(err));
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

    /* The bridge configures the modem; tell it that the host has started. */
    sendBridgeControl(BRIDGE_CTL_HOST_UP, NULL, 0);

    if (realtime)
    {
        /* -r: serve the serial port ahead of ordinary processes, so a busy
         * Pi is less likely to let the UART buffer overflow (needs root) */
        struct sched_param sp;

        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = 20;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
            reportError(modemName, "Modem", "Modem_Host: can't get real-time priority (errno=%d), continuing without", errno);
        else
            fprintf(stderr, "Modem_Host: serial reader runs with real-time priority\n");
    }

    while (atomic_load(&running))
    {
        if (streamType == STREAM_FRAME)
            ret = processRelaySerial();
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
