/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
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
#include <pthread.h>
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
#include <unistd.h>

#include "../tools/RingBuffer.h"
#include "../tools/tools.h"
#include "RingBuffer.h"
#include "mmdvm.h"

#define VERSION "2025-10-12"
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
const char* TYPE_HDR_P25      = "P25H";
const char* TYPE_LDU_P25      = "P25L";
const char* TYPE_LOST_P25     = "P25E";
const char* TYPE_DATA1_DMR    = "DMD1";
const char* TYPE_LOST1_DMR    = "DML1";
const char* TYPE_DATA2_DMR    = "DMD2";
const char* TYPE_LOST2_DMR    = "DML2";
const char* TYPE_SHORTLC_DMR  = "DMLC";
const char* TYPE_START_DMR    = "DMST";
const char* TYPE_ABORT_DMR    = "DMAB";

const char* TYPE_TO_MODEM = "2MOD";
const char* TYPE_COMMAND  = "COMM";
const char* TYPE_SAMPLE   = "SAMP";
const char* TYPE_BITS     = "BITS";

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
    pthread_mutex_t commandMutex;  /* Protect command buffer access */
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

int serialModemFd   = 0;           //< File descriptor for modem serial port.
uint8_t modemId     = 1;           //< Modem Id used to create modem name.
char modemName[10]  = "modem1";    //< Modem name that this program is associated with.
char modemtty[50]   = "";          //< Modem serial port.
uint8_t connections = 0;           //< Number of clients currently connected.
uint16_t host_port  = 18000;       //< All protocol services connect on this TCP port.
uint32_t txTimeout  = TX_TIMEOUT;  //< Station TX timeout. FIX-ME: Should be user configurable.
bool duplex         = false;       //< Indicates station TX operation mode.
bool debugM         = false;       //< If true print debug info.
bool txOn           = false;       //< If true we are processing frame data.
bool running        = true;        //< Set false to kill all processes and exit program.
bool exitRequested  = false;       //< Set to true with ctrl + c keyboard input.
bool isModemSpace   = true;        //< Flag indicating that modem buffer space is available.
bool bufReady       = false;       //< Signal when rxModeBuffer has adequate number of bytes.

unsigned int clientlen;         //< byte size of client's address
char* hostaddrp;                //< dotted decimal host addr string
int optval;                     //< flag value for setsockopt
struct sockaddr_in serveraddr;  //< server's addr
struct sockaddr_in clientaddr;  //< client addr

RingBuffer modemCommandBuffer; /* Modem command buffer */
RingBuffer txBuffer;           /* Modem TX buffer */
RingBuffer rxModeBuffer;       /* Client RX buffer */
RingBuffer txModeBuffer;       /* Client TX buffer */

pthread_mutex_t txBufMutex       = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t modemComBufMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t rxBufModeMutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t txBufModeMutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t shutDownMutex    = PTHREAD_MUTEX_INITIALIZER;

/* General settings */
char currentMode[11] = "idle";
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

//  SIGINT handler, so we can gracefully exit when the user hits ctrl+c.
static void sigintHandler(int signum)
{
    signal(SIGINT, SIG_DFL);
    for (uint8_t x = 0; x < MAX_CLIENT_CONNECTIONS; x++)
    {
        if (hostClient[x].active)
            setProtocol(mode[x].name, false);
    }
    sleep(1);
    exitRequested = true;
}

uint8_t getModemSpace(const char* protocol, const uint8_t slot)
{
    if (strncasecmp(modem, "openmt", 6) != 0)
    {
        if (strcasecmp(protocol, "M17") == 0)
            return getM17Space();
        else if (strcasecmp(protocol, "DSTAR") == 0)
            return getDSTARSpace();
        else if (strcasecmp(protocol, "P25") == 0)
            return getP25Space();
        else if (strcasecmp(protocol, "DMR") == 0)
            return getDMRSpace(slot);
    }
    return 20;  // default
}

void setSpace(uint8_t space)
{
    if (space == 0)
        isModemSpace = true;
    else
        isModemSpace = false;
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
    tty.c_iflag &= ~IGNBRK;
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
    else if (strcasecmp(modem, "OPENMTHS") == 0)
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

// Simple timer thread.
// Each loop through the while statement takes 1 millisecond.
void* timerThread(void* arg)
{
    uint32_t loop[4] = {10000, 0, 0, 0};

    if (getTimer("frameDelay", 20) < 0)
    {
        if (debugM) fprintf(stderr, "Timer thread exited.\n");
        int iRet = 600;
        pthread_exit(&iRet);
        return NULL;
    }

    while (running)
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

            if (isTimerTriggered("frameDelay"))
            {
                pthread_mutex_lock(&rxBufModeMutex);
                if (RingBuffer_dataSize(&rxModeBuffer) == 0)
                    bufReady = false;
                pthread_mutex_unlock(&rxBufModeMutex);

                if (strncasecmp(modem, "mmdvm", 5) == 0)
                {
                    if (strcmp(currentMode, "DSTAR") == 0)
                    {
                        uint8_t space = getModemSpace(currentMode, 1);
                        if (space < 10)
                            resetTimer("frameDelay", 18);
                        else if (space > 17)
                            resetTimer("frameDelay", 13);
                        else
                            resetTimer("frameDelay", 17);
                    }
                    else if (strcmp(currentMode, "M17") == 0)
                    {
                        uint8_t space = getModemSpace(currentMode, 1);
                        if (space < 30)
                            resetTimer("frameDelay", 18);
                        else if (space > 60)
                            resetTimer("frameDelay", 12);
                        else
                            resetTimer("frameDelay", 14);
                    }
                    else if (strcmp(currentMode, "P25") == 0)
                    {
                        uint8_t space = getModemSpace(currentMode, 1);
                        if (space < 10)
                            resetTimer("frameDelay", 19);
                        else if (space > 12)
                            resetTimer("frameDelay", 8);
                        else
                            resetTimer("frameDelay", 12);
                    }
                    else if (strcmp(currentMode, "DMR") == 0)
                    {
                        uint8_t space1 = getModemSpace(currentMode, 1);
                        uint8_t space2 = getModemSpace(currentMode, 2);
                        uint8_t space  = MIN(space1, space2);
                        if (space < 5)
                            resetTimer("frameDelay", 60);
                        else if (space > 8)
                            resetTimer("frameDelay", 45);
                        else
                            resetTimer("frameDelay", 57);
                    }
                }
                else
                    resetTimer("frameDelay", 0);
            }

            if (isTimerTriggered("txTimeout") && txOn)
            {
                txOn = false;
                strcpy(currentMode, "idle");
                setStatus(modemName, "main", "active_mode", currentMode);
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
                if (debugM)
                    fprintf(stderr, "TX timeout. Setting mode to idle.\n");
            }
            else if (isTimerTriggered("txTimeout") && !txOn)
                resetTimer("txTimeout", 0);
        }

        if (exitRequested)
        {
            fprintf(stderr, "Program shutdown requested.\n");
            running = false;
        }
    }

    fprintf(stderr, "Timer thread exited.\n");
    int iRet = 600;
    pthread_exit(&iRet);
    return NULL;
}

// Send data to protocol service.
// One thread per service connection.
void* modeRxThread(void* arg)
{
    uint8_t conn_id = (intptr_t)arg;
    int sockfd      = hostClient[conn_id].sockfd;
    bool found      = false;

    while (hostClient[conn_id].active && running)
    {
        delay(100);

        pthread_mutex_lock(&txBufModeMutex);
        if (RingBuffer_dataSize(&txModeBuffer) >= 4)
        {
            uint8_t buf[1];
            RingBuffer_peek(&txModeBuffer, buf, 1);
            if (buf[0] != 0x61)
            {
                uint8_t tmp[1];
                RingBuffer_getData(&txModeBuffer, tmp, 1);
                fprintf(stderr, "txModeBuffer: invalid header [%02X]  [%02X]\n", buf[0], tmp[0]);
                pthread_mutex_unlock(&txBufModeMutex);
                continue;
            }
            if (strncasecmp(modem, "mmdvm", 5) == 0)
            {
                uint8_t byte[6];
                uint16_t len = 0;
                RingBuffer_peek(&txModeBuffer, byte, 6);
                len = (byte[1] << 8) + byte[2];
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
                uint8_t tmp[len];
                RingBuffer_peek(&txModeBuffer, tmp, len);
                if (RingBuffer_dataSize(&txModeBuffer) >= len && conn_id == tmp[len - 1])
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
            if (RingBuffer_dataSize(&hostClient[conn_id].command) >= len)
            {
                uint8_t buf[len];
                RingBuffer_getData(&hostClient[conn_id].command, buf, len);
                pthread_mutex_unlock(&hostClient[conn_id].commandMutex);

                if (write(sockfd, buf, len) < 0)
                {
                    fprintf(stderr, "ERROR: remote disconnect\n");
                    break;
                }
            }
            else
            {
                pthread_mutex_unlock(&hostClient[conn_id].commandMutex);
            }
        }
        else
        {
            pthread_mutex_unlock(&hostClient[conn_id].commandMutex);
        }
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
    while (running)
    {
        delay(100);

        if (isTimerTriggered("status"))
        {
            if (RingBuffer_freeSpace(&txBuffer) >= 3 && RingBuffer_dataSize(&rxModeBuffer) < 3)
            {
                uint8_t buf[3];
                buf[0] = 0x61;
                buf[1] = 0x03;
                buf[2] = OPENMT_GET_STATUS;
                //     pthread_mutex_lock(&txBufMutex);
                //     RingBuffer_addData(&txBuffer, buf, 3);
                //     pthread_mutex_unlock(&txBufMutex);
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
            if (RingBuffer_dataSize(&rxModeBuffer) >= len)
            {
                if (isModemSpace && RingBuffer_freeSpace(&txBuffer) >= len + 3)
                {
                    uint8_t buf[len];
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_getData(&rxModeBuffer, buf, len);
                    pthread_mutex_unlock(&rxBufModeMutex);
                    pthread_mutex_lock(&txBufMutex);
                    RingBuffer_addData(&txBuffer, buf, len);
                    pthread_mutex_unlock(&txBufMutex);
                }
                if (RingBuffer_freeSpace(&txBuffer) >= 3)
                {
                    uint8_t buf[3];
                    buf[0] = 0x61;
                    buf[1] = 0x03;
                    buf[2] = OPENMT_GET_STATUS;
                    //       pthread_mutex_lock(&txBufMutex);
                    //       RingBuffer_addData(&txBuffer, buf, 3);
                    //       pthread_mutex_unlock(&txBufMutex);
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
            if (RingBuffer_dataSize(&modemCommandBuffer) >= len)
            {
                if (RingBuffer_freeSpace(&txBuffer) >= len)
                {
                    uint8_t buf[len];
                    RingBuffer_getData(&modemCommandBuffer, buf, len);
                    pthread_mutex_lock(&txBufMutex);
                    RingBuffer_addData(&txBuffer, buf, len);
                    pthread_mutex_unlock(&txBufMutex);
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
    while (running)
    {
        delay(200);

        if (isTimerTriggered("status"))
        {
            if (RingBuffer_freeSpace(&txBuffer) >= 3 && RingBuffer_dataSize(&rxModeBuffer) < 3)
            {
                uint8_t buf[3];
                buf[0] = 0xE0;
                buf[1] = 0x03;
                buf[2] = MODEM_STATUS;
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_addData(&txBuffer, buf, 3);
                pthread_mutex_unlock(&txBufMutex);
            }
            resetTimer("status", 0);
        }

        if (RingBuffer_dataSize(&rxModeBuffer) >= 3 && isTimerTriggered("frameDelay") && bufReady)
        {
            uint8_t buf[3];
            pthread_mutex_lock(&rxBufModeMutex);
            RingBuffer_peek(&rxModeBuffer, buf, 1);
            if (buf[0] != 0xE0)
            {
                uint8_t tmp[1];
                RingBuffer_getData(&rxModeBuffer, tmp, 1);
                pthread_mutex_unlock(&rxBufModeMutex);
                fprintf(stderr, "txBufMutex invalid header [%02X]\n", tmp[0]);
                resetTimer("frameDelay", 0);
                continue;
            }
            RingBuffer_peek(&rxModeBuffer, buf, 2);
            pthread_mutex_unlock(&rxBufModeMutex);
            uint8_t len = buf[1];
            if (RingBuffer_dataSize(&rxModeBuffer) >= len)
            {
                uint8_t space = 0;
                RingBuffer_peek(&rxModeBuffer, buf, 3);
                if (buf[2] == TYPE_DMR_DATA2)
                    space = getModemSpace(currentMode, 2);
                else
                    space = getModemSpace(currentMode, 1);
                if (space > 1 && RingBuffer_freeSpace(&txBuffer) >= (len + 3))
                {
                    uint8_t buf[len];
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_getData(&rxModeBuffer, buf, len);
                    pthread_mutex_unlock(&rxBufModeMutex);
                    pthread_mutex_lock(&txBufMutex);
                    RingBuffer_addData(&txBuffer, buf, len);
                    pthread_mutex_unlock(&txBufMutex);
                }
                if (RingBuffer_freeSpace(&txBuffer) >= 3)
                {
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = 0x03;
                    buf[2] = MODEM_STATUS;
                    pthread_mutex_lock(&txBufMutex);
                    RingBuffer_addData(&txBuffer, buf, 3);
                    pthread_mutex_unlock(&txBufMutex);
                }
            }
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
                if (RingBuffer_freeSpace(&txBuffer) >= len)
                {
                    uint8_t buf[len];
                    RingBuffer_getData(&modemCommandBuffer, buf, len);
                    pthread_mutex_lock(&txBufMutex);
                    RingBuffer_addData(&txBuffer, buf, len);
                    pthread_mutex_unlock(&txBufMutex);
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

// Send queued up bytes to MMDVM modem.
void* modemMMDVMTxThread(void* arg)
{
    while (running)
    {
        delay(50);

        if (RingBuffer_dataSize(&txBuffer) >= 3)
        {
            pthread_mutex_lock(&txBufMutex);
            uint8_t buf[2];
            RingBuffer_peek(&txBuffer, buf, 1);
            if (buf[0] != 0xE0)
            {
                uint8_t tmp[1];
                RingBuffer_getData(&txBuffer, tmp, 1);
                fprintf(stderr, "modem TX invalid header [%02X]  [%02X]\n", buf[0], tmp[0]);
                pthread_mutex_unlock(&txBufMutex);
                continue;
            }
            if (!RingBuffer_peek(&txBuffer, buf, 2))
                fprintf(stderr, "npeek failed\n");
            pthread_mutex_unlock(&txBufMutex);
            uint8_t len = buf[1];
            if (len < 1)
            {
                uint8_t tmp[2];
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_getData(&txBuffer, tmp, 2);
                fprintf(stderr, "modem length invalid. [%02X]  [%02X]\n", len, tmp[1]);
                pthread_mutex_unlock(&txBufMutex);
                continue;
            }
            if (RingBuffer_dataSize(&txBuffer) >= len)
            {
                uint8_t buf[len];
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_getData(&txBuffer, buf, len);
                pthread_mutex_unlock(&txBufMutex);
                int ret = write(serialModemFd, buf, len);
                if (ret != len)
                {
                    fprintf(stderr, "modem TX write failed.\n");
                }
        //        if (buf[2] != 0x01)     dump((char*)"Modem", buf, len);
            }
        }
    }

    fprintf(stderr, "Modem TX thread exited.\n");
    int iRet = 300;
    pthread_exit(&iRet);
    return NULL;
}

// Send queued up bytes to modem.
void* modemTxThread(void* arg)
{
    while (running)
    {
        delay(1000);

        if (RingBuffer_dataSize(&txBuffer) >= 3)
        {
            pthread_mutex_lock(&txBufMutex);
            uint8_t buf[3];
            RingBuffer_peek(&txBuffer, buf, 1);
            if (buf[0] != 0x61)
            {
                uint8_t tmp[1];
                RingBuffer_getData(&txBuffer, tmp, 1);
                fprintf(stderr, "modem TX invalid header [%02X]  [%02X]\n", buf[0], tmp[0]);
                pthread_mutex_unlock(&txBufMutex);
                continue;
            }
            if (!RingBuffer_peek(&txBuffer, buf, 3))
                fprintf(stderr, "peek failed\n");
            uint8_t len = buf[1];
            if (len < 1)
            {
                fprintf(stderr, "modem length invalid. [%02X]\n", len);
                uint8_t tmp[1];
                RingBuffer_getData(&txBuffer, tmp, 1);
                pthread_mutex_unlock(&txBufMutex);
                continue;
            }
            pthread_mutex_unlock(&txBufMutex);
            if (RingBuffer_dataSize(&txBuffer) >= len)
            {
                uint8_t buf[len];
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_getData(&txBuffer, buf, len);
                pthread_mutex_unlock(&txBufMutex);
                int ret = write(serialModemFd, buf, len);
                if (ret != len)
                {
                    fprintf(stderr, "modem TX write failed.\n");
                }
                //      dump((char*)"Modem:", buf, len);
            }
        }
    }

    fprintf(stderr, "Modem TX thread exited.\n");
    int iRet = 300;
    pthread_exit(&iRet);
    return NULL;
}

// Read commands queued up in database from dashboard.
void* commandThread(void* arg)
{
    while (running)
    {
        delay(50000);

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
            error((char*)"ERROR: client connection closed remotely.");
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
                    strcpy(currentMode, "idle");
                    pthread_mutex_lock(&hostClient[conn_id].commandMutex);
                    RingBuffer_addData(&hostClient[conn_id].command, buffer, 9);
                    pthread_mutex_unlock(&hostClient[conn_id].commandMutex);
                    setStatus(modemName, "main", "active_mode", currentMode);
                }
            }
            else
            {
                if (strcasecmp(modem, "openmths") == 0)
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
                pthread_mutex_lock(&txBufModeMutex);
                RingBuffer_addData(&txModeBuffer, buffer, 9);
                buffer[0] = conn_id;
                RingBuffer_addData(&txModeBuffer, buffer, 1);
                pthread_mutex_unlock(&txBufModeMutex);
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
        else if (memcmp(buffer + 4, TYPE_BITS, typeLen) == 0)
            type = OPENMT_BIT_DATA;
        else if (memcmp(buffer + 4, TYPE_TO_MODEM, typeLen) == 0)
            type = OPENMT_TO_MODEM;

        if (debugM)
            dump((char*)"Protocol service data:", (unsigned char*)buffer, respLen);

        uint8_t space = getModemSpace(currentMode, 1);

        if (RingBuffer_freeSpace(&rxModeBuffer) < respLen)
        {
            fprintf(stderr, "rxModeBuffer out of space. [%d]\n", RingBuffer_freeSpace(&rxModeBuffer));
            pthread_mutex_lock(&rxBufModeMutex);
            RingBuffer_clear(&rxModeBuffer);
            pthread_mutex_unlock(&rxBufModeMutex);
        }

        if (strcmp(currentMode, "DSTAR") == 0)
        {
            if (RingBuffer_dataSize(&rxModeBuffer) >= 200)
                bufReady = true;
        }
        else if (strcmp(currentMode, "M17") == 0)
        {
            if (RingBuffer_dataSize(&rxModeBuffer) >= 500)
                bufReady = true;
        }
        else if (strcmp(currentMode, "P25") == 0)
        {
            //    if (RingBuffer_dataSize(&rxModeBuffer) >= 2400)
            bufReady = true;
        }
        else if (strcmp(currentMode, "DMR") == 0)
        {
            //    if (RingBuffer_dataSize(&rxModeBuffer) >= 2400)
            bufReady = true;
            if (type == TYPE_DMR_DATA2)
                space = getModemSpace(currentMode, 2);
        }

        currType = type;
        // This debug statement to be removed after debugging
        //        if (debugM)
  //      if (currType != lastType || RingBuffer_dataSize(&rxModeBuffer) > 1500)
        fprintf(stderr, "TM: %llu  Type: %02X  Mode: %4s  Conn: %2d  Mode Sp: %2d  Modem Sp: %4d  RX buf: %4d  BR: %d\n",
                (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000, type, currentMode, conn_id,
                space, RingBuffer_freeSpace(&txBuffer), RingBuffer_dataSize(&rxModeBuffer), bufReady);

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
                if (strcmp(currentMode, "idle") == 0)
                {
                    strcpy(currentMode, hostClient[conn_id].mode);
                    setProtocol(currentMode, true);
                    if (strncasecmp(modem, "mmdvm", 5) != 0)
                        setMode(conn_id);
                    //           if (debugM)
                    fprintf(stderr, "Current mode set to %s.\n", currentMode);
                    setStatus(modemName, "main", "active_mode", currentMode);
                }
                break;

            case COMM_SET_IDLE:
                if (strcmp(currentMode, "idle") != 0 && strcasecmp(currentMode, hostClient[conn_id].mode) == 0)
                {
                    txOn = false;
                    strcpy(currentMode, "idle");
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_clear(&rxModeBuffer);
                    pthread_mutex_unlock(&rxBufModeMutex);
                    if (strncasecmp(modem, "mmdvm", 5) == 0)
                        setMode(0x00);
                    else
                        setMode(0x0f);
                    //           if (debugM)
                    fprintf(stderr, "Current mode set to IDLE.\n");
                    setStatus(modemName, "main", "active_mode", currentMode);
                }
                break;

            case OPENMT_SAMP_DATA:
            {
                uint8_t buf[4] = {0x61, 0xc4, 0x70, mode[conn_id].txLevel};
                pthread_mutex_lock(&rxBufModeMutex);
                if (RingBuffer_freeSpace(&rxModeBuffer) >= 0xc4)
                {
                    RingBuffer_addData(&rxModeBuffer, buf, 4);
                    RingBuffer_addData(&rxModeBuffer, buffer + 8, respLen - 8);
                }
                else
                {
                    fprintf(stderr, "rxModeBuffer out of space. [%d]\n", RingBuffer_freeSpace(&rxModeBuffer));
                }
                pthread_mutex_unlock(&rxBufModeMutex);
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
                    //   if (debugM)
                    dump((char*)"Bits:", (unsigned char*)buffer, respLen);
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
                if (hostClient[conn_id].active && strcasecmp(currentMode, hostClient[conn_id].mode) == 0)
                {
                    txOn = true;
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
                    txOn = false;
                }
            }
            break;

            case TYPE_DSTAR_HEADER:
            {
                if (hostClient[conn_id].active && strcasecmp(currentMode, hostClient[conn_id].mode) == 0)
                {
                    txOn = true;
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
                if (hostClient[conn_id].active && strcasecmp(currentMode, hostClient[conn_id].mode) == 0)
                {
                    txOn = true;
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
                if (hostClient[conn_id].active && strcasecmp(currentMode, hostClient[conn_id].mode) == 0)
                {
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = 0x03;
                    buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                    pthread_mutex_unlock(&rxBufModeMutex);
                    txOn = false;
                }
            }
            break;

            case TYPE_P25_HDR:
            {
                if (hostClient[conn_id].active && strcasecmp(currentMode, hostClient[conn_id].mode) == 0)
                {
                    txOn = true;
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
                if (hostClient[conn_id].active && strcasecmp(currentMode, hostClient[conn_id].mode) == 0)
                {
                    txOn = true;
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
                if (hostClient[conn_id].active && strcasecmp(currentMode, hostClient[conn_id].mode) == 0)
                {
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = 0x03;
                    buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 3);
                    pthread_mutex_unlock(&rxBufModeMutex);
                    txOn = false;
                }
            }
            break;

            case TYPE_DMR_DATA1:
            case TYPE_DMR_DATA2:
            case TYPE_DMR_LOST1:
            case TYPE_DMR_LOST2:
            {
                if (hostClient[conn_id].active && strcasecmp(currentMode, hostClient[conn_id].mode) == 0)
                {
                    txOn = true;
                    uint8_t buf[3];
                    buf[0] = 0xE0;
                    buf[1] = respLen - 8 + 2;
                    // buf[2] = type;
                    pthread_mutex_lock(&rxBufModeMutex);
                    RingBuffer_addData(&rxModeBuffer, buf, 2);
                    RingBuffer_addData(&rxModeBuffer, &buffer[4 + typeLen], respLen - 8);
                    pthread_mutex_unlock(&rxBufModeMutex);

                    if (buffer[8] == 0x42 || type == TYPE_DMR_LOST1 || type == TYPE_DMR_LOST2)
                        txOn = false;
                }
            }
            break;
        }
        delay(10);
    }

    if (connections > 0)
        connections--;
    hostClient[conn_id].active = false;
    setProtocol(mode[conn_id].name, false);

    pthread_mutex_lock(&shutDownMutex);
    delGateway(modemName, "main", mode[conn_id].name);
    delMode(modemName, "main", mode[conn_id].name);
    if (strcasecmp(modem, "openmths") == 0)
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

    while (running)
    {
        /*
         * accept: wait for a connection request
         */
        childfd = accept(sockFd, (struct sockaddr*)&clientaddr, &clientlen);
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
        hostp = gethostbyaddr((const char*)&clientaddr.sin_addr.s_addr, sizeof(clientaddr.sin_addr.s_addr), AF_INET);
        if (hostp == NULL)
        {
            error((char*)"ERROR on gethostbyaddr");
            break;
        }

        hostaddrp = inet_ntoa(clientaddr.sin_addr);
        if (hostaddrp == NULL)
        {
            error((char*)"ERROR on inet_ntoa\n");
            break;
        }

        if (debugM)
            fprintf(stderr, "Server established connection with %s (%s)\n", hostp->h_name, hostaddrp);

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
    running = false;
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

    len = read(serialModemFd, buffer, 1);
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
        len = read(serialModemFd, buffer + 1 + offset, 2 - offset);
        if (len == 0)
            delay(5);
        else
            offset += len;
    }

    respLen = buffer[1];
    type    = buffer[2];

    offset += 1;
    while (offset < respLen)
    {
        len = read(serialModemFd, buffer + offset, respLen - offset);

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
    }
    else if (type == TYPE_M17_LSF && respLen == 54)
    {
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "M17") == 0))
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
    else if (type == TYPE_M17_STREAM && respLen == 54)
    {
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "M17") == 0))
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
    else if (type == TYPE_M17_PACKET && respLen == 54)
    {
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "M17") == 0))
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
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "M17") == 0))
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
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "DSTAR") == 0))
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
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "DSTAR") == 0))
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
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "DSTAR") == 0))
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
    else if (type == TYPE_P25_HDR && buffer[3] == 1 && (respLen >= 21 && respLen <= 105))
    {
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "P25") == 0))
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
    else if (type == TYPE_P25_LDU && buffer[3] == 1 && respLen == 222)
    {
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "P25") == 0))
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
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "P25") == 0))
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
    else if (type == TYPE_DMR_DATA1 && (respLen == 0x25 || respLen == 0x27))
    {
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "DMR") == 0))
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
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "DMR") == 0))
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
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "DMR") == 0))
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
        if (connections > 0 && (strcmp(currentMode, "idle") == 0 || strcmp(currentMode, "DMR") == 0))
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

// Process incoming modem bytes.
int processSerial(void)
{
    uint8_t buffer[BUFFER_LENGTH];
    uint16_t respLen;
    uint16_t offset;
    uint8_t type;
    uint16_t len;
    uint8_t typeLen;

    len = read(serialModemFd, buffer, 1);
    if (len == 0) return 2;

    if (len != 1)
    {
        fprintf(stderr, "Modem_Host: error when reading from Modem, errno=%d\n", errno);
        return 0;
    }

    if (buffer[0] != 0x61)
    {
        fprintf(stderr, "Modem_Host: unknown byte from Modem, 0x%02X\n", buffer[0]);
        return 1;
    }

    offset = 0;
    while (offset < 2)
    {
        len = read(serialModemFd, buffer + 1 + offset, 2 - offset);
        if (len == 0)
            delay(5);
        else
            offset += len;
    }

    respLen = buffer[1];

    offset += 1;
    while (offset < respLen)
    {
        len = read(serialModemFd, buffer + offset, respLen - offset);

        if (len == 0)
            delay(5);
        else
            offset += len;
    }

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
    }
    else if (type == OPENMT_BIT_DATA)
    {
        if (debugM)
            dump((char*)"BITS", buffer, respLen);
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
    }
    else if (type == OPENMT_GET_STATUS)
    {
        //     if (debugM)
        dump((char*)"STATUS:", buffer, respLen);
        setSpace(buffer[2] & 0x0f);
    }
    else
    {
        if (respLen > BUFFER_LENGTH)
            respLen = BUFFER_LENGTH;
        //       if (debugM)
        dump((char*)"Modem rec data:", buffer, respLen);
    }

    return 1;
}

int main(int argc, char** argv)
{
    bool daemon = false;
    int ret;
    int c;

    while ((c = getopt(argc, argv, ":m:dxv")) != -1)
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
            default:
                fprintf(stderr, "Usage: Modem_Host [-m modem_number (1-10)] [-d] [-v] [-x]\n");
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

    initTimers();
    if (getTimer("status", 500) < 0) return 0;
    if (getTimer("txTimeout", txTimeout) < 0) return 0;
    if (getTimer("chkComTimeout", 2000) < 0) return 0;

    /* Initialize RingBuffers */
    RingBuffer_Init(&modemCommandBuffer, 300);
    RingBuffer_Init(&txBuffer, 1000);
    RingBuffer_Init(&rxModeBuffer, 12000);
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
    if (serialModemFd < 0)
        return 1;

    sleep(1);

    if (strncasecmp(modem, "mmdvm", 5) == 0)
    {
        int err = pthread_create(&(modemTxid), NULL, &modemMMDVMTxThread, NULL);
        if (err != 0)
        {
            fprintf(stderr, "Can't create MMDVM modem tx thread:[%s]", strerror(err));
            return 1;
        }
        else
        {
            if (debugM)
                fprintf(stderr, "MMDVM modem tx thread created successfully\n");
        }

        err = pthread_create(&(modetxid), NULL, &modeMMDVMTxThread, NULL);
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
        //write(serialModemFd, buf, 3);
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
    }

    while (running)
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
    RingBuffer_Destroy(&txBuffer);
    RingBuffer_Destroy(&rxModeBuffer);
    RingBuffer_Destroy(&txModeBuffer);
    for (uint8_t i = 0; i < MAX_CLIENT_CONNECTIONS; i++)
    {
        RingBuffer_Destroy(&hostClient[i].command);
        pthread_mutex_destroy(&hostClient[i].commandMutex);
    }

    return 0;
}
