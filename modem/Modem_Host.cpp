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

/* ########################## ATTENTION #################################  */
/***************************************************************************
 *  This program is currently specific to the MMDVM firmware written by    *
 *  Jonathan Naylor G4KLX. This is for proof of concept and as such does   *
 *  not completely fit into the OpenMT idea as the firmware is not         *
 *  programmable on the fly. The firmware only works with the protocols    *
 *  programed into it a compile time.                                      *
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <cstdint>
#include <unistd.h>
#include <string.h>
#include <string>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/param.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "../tools/RingBuffer.h"
#include "../tools/tools.h"
#include "mmdvm.h"

using namespace std;

#define    VERSION               "2025-8-14"
#define    MAX_LINE_LENGTH        256
#define    BUFFER_LENGTH          500
#define    MAX_CLIENT_CONNECTIONS 20
#define    TX_TIMEOUT             300000

const char      *TYPE_ACK               = "ACK ";
const char      *TYPE_NACK              = "NACK";
const char      *TYPE_MODE              = "MODE";
const char      *TYPE_LSF_M17           = "M17L";
const char      *TYPE_STREAM_M17        = "M17S";
const char      *TYPE_PACKET_M17        = "M17P";
const char      *TYPE_EOT_M17           = "M17E";
const char      *TYPE_COMMAND           = "COMM";
const char      *TYPE_HEADER_DSTAR      = "DSTH";
const char      *TYPE_DATA_DSTAR        = "DSTD";
const char      *TYPE_EOT_DSTAR         = "DSTE";

const uint8_t    DSTAR_END_SYNC_BYTES[] = {0x55, 0x55, 0x55, 0x55, 0xC8, 0x7A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t    NET_ACK                = 0xF0;
const uint8_t    NET_NACK               = 0xF1;

const uint8_t    COMM_SET_DUPLEX        = 0x00;
const uint8_t    COMM_SET_SIMPLEX       = 0x01;
const uint8_t    COMM_SET_MODE          = 0x02;
const uint8_t    COMM_SET_IDLE          = 0x03;

// ****** Modem specific parameters ********************
bool             modem_trace            = false;
bool             modem_debug            = false;
bool             modem_duplex           = true;
bool             modem_rxInvert         = false;
bool             modem_txInvert         = true;
bool             modem_pttInvert        = false;
bool             modem_useCOSAsLockout  = false;
unsigned int     modem_txDelay          = 100U;
char             modem_rxFrequency[11]  = "435000000";
char             modem_txFrequency[11]  = "435000000";
float            modem_rxLevel          = 100U;
float            modem_rfLevel          = 100U;
int              modem_rxDCOffset       = 0U;
int              modem_txDCOffset       = 0U;
// ******************************************************

pthread_t modemTxid;
pthread_t modetxid;
pthread_t tcpid;
pthread_t timerid;
pthread_t commandid;

sem_t txBufSem;
sem_t shutDownSem;

// Protocol service client descriptor
struct Client
{
    pthread_t clientid;
    pthread_t rxid;
    int       sockfd;
    char      mode[11];
    bool      active;
    bool      isTx;
    CRingBuffer<uint8_t> command;
};

Client hostClient[MAX_CLIENT_CONNECTIONS];

// Descriptor for protocol
// Data received from protocol service
// Downloaded to modem
struct Protocol
{
    char     name[11];                //< Protocol name (10 char max)
    char     modem_type[6];           //< Modem type (5 char max) (4FSK, GMSK, etc.)
    bool     use_rx_dc_filter;        //< Use DC filter
    bool     use_lp_filter;           //< Use LP filter
    uint8_t  rx_filter_pState_len;    //< RX filter pstate len
    uint8_t  rx_filter_numTaps;       //< Number of pcoeffs in RX filter
    int16_t *rx_filter_pCoeffs;       //< RX filter pcoeffs array
    int16_t *rx_filter_pState;        //< RX filterpstate array
    uint8_t  tx_filter_symbol_len;    //< Fir filter radio symbol length, upsample factor
    uint8_t  tx_filter_pState_len;    //< TX filter pstate length
    uint8_t  tx_filter_phaseLen;      //< Filter phase length
    uint8_t  tx_filter_Len;           //< TX Filter length
    int16_t *tx_filter_pCoeffs;       //< Pointer to array of values for fir filter
    int16_t *tx_filter_pState;        //< Pointer to filter state array
    uint8_t  tx_lp_filter_pState_len; //< LP filter pstate length
    uint8_t  tx_lp_filter_numTaps;    //< Number of pcoeffs in filter
    int16_t *tx_lp_filter_pCoeffs;    //< Pointer to array of values for fir filter
    int16_t *tx_lp_filter_pState;     //< Pointer to filter state array
};

Protocol mode[MAX_CLIENT_CONNECTIONS];

int      serialModemFd = 0;          //< File descriptor for modem serial port.
char     modemtty[50]  = "";         //< Modem serial port.
uint8_t  connections   = 0;          //< Number of clients currently connected.
uint16_t host_port     = 18000;      //< All protocol services connect on this TCP port.
uint32_t txTimeout     = TX_TIMEOUT; //< Station TX timeout. FIX-ME: Should be user configurable.
bool     statusTimeout = false;      //< On timeout modem is queried for status.
bool     chkComTimeout = false;      //< On timeout interprocess commands are sent, if any.
bool     frameTimeout  = false;      //< On timeout symbolframes are sent to modem.
bool     duplex        = false;      //< Indicates station TX operation mode.
bool     debugM        = false;      //< If true print debug info.
bool     txOn          = false;      //< If true we are processing frame data.
bool     running       = true;       //< Set false to kill all processes and exit program.
bool     exitRequested = false;      //< Set to true with ctrl + c keyboard input.

unsigned int       clientlen;        //< byte size of client's address
char              *hostaddrp;        //< dotted decimal host addr string
int                optval;           //< flag value for setsockopt
struct sockaddr_in serveraddr;       //< server's addr
struct sockaddr_in clientaddr;       //< client addr

CRingBuffer<uint8_t>  modemCommandBuffer(300);  //< Modem command buffer
CRingBuffer<uint8_t>  txBuffer(1300);           //< Modem TX buffer
CRingBuffer<uint8_t>  rxModeBuffer(3600);       //< Client RX buffer
CRingBuffer<uint8_t>  txModeBuffer(3600);       //< Client TX buffer

// ******** General settings
std::string username("");
std::string password("");
std::string commPort("");
std::string modemBaud("");
std::string modem("");
std::string currentMode("idle");
std::string callsign("N0CALL");


//  SIGINT handler, so we can gracefully exit when the user hits ctrl+c.
static void sigintHandler(int signum)
{
    exitRequested = true;
    signal(SIGINT, SIG_DFL);
}

uint8_t getModemSpace(const char* protocol)
{
    if (strcasecmp(protocol, "M17") == 0)
        return getM17Space();
    else if (strcasecmp(protocol, "DSTAR") == 0)
        return getDSTARSpace();
    return 0;
}

void setProtocol(const char* protocol, bool enabled)
{
    if (strcasecmp(protocol, "M17") == 0)
    {
        if (enabled)
            enableM17();
        else
            disableM17();
    }
    else if (strcasecmp(protocol, "DSTAR") == 0)
    {
        if (enabled)
            enableDSTAR();
        else
            disableDSTAR();
    }
}

// error - wrapper for perror
void error(char *msg)
{
  perror(msg);
  exit(1);
}

// Wait for 'delay' microseconds
void delay(uint32_t delay)
{
    struct timespec req, rem;
    req.tv_sec = 0;
    req.tv_nsec = delay * 1000;
    nanosleep(&req, &rem);
};

// Print debug data.
// From MMDVM project by Jonathan Naylor G4KLX.
void dump(char *text, unsigned char *data, unsigned int length)
{
    struct timespec ts;
    uint64_t milliseconds;
    unsigned int offset = 0U;
    unsigned int i;

    // Get the current time with nanosecond precision
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
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
int openSerial(char *serial)
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

    switch (atol(modemBaud.c_str()))
    {
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

    tty.c_oflag = 0;
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

// Simple timer thread.
// Each loop through the while statement takes 1 millisecond.
void* timerThread(void *arg)
{
    uint32_t loop[4] = {10000, 0, 0, 0};
    while (running)
    {
        delay(1000); // 1ms

        if (loop[0] >= 250)
        {
            statusTimeout = true;
            loop[0] = 0;
        }

        if (loop[1] >= 17)
        {
            frameTimeout = true;
            loop[1] = 0;
        }

        if (loop[2] >= txTimeout && txOn)
        {
            txOn = false;
            currentMode = "idle";
            uint8_t buffer[4];
            buffer[0] = 0xE0;
            buffer[1] = 0x04;
            buffer[2] = MODEM_MODE;
            buffer[3] = 0x00; // IDLE_MODE
            for (uint8_t i=0;i<4;i++)
                modemCommandBuffer.put(buffer[i]);
            loop[2] = 0;
            if (debugM)
                fprintf(stderr, "TX timeout. Setting mode to idle.\n");
        }

        if (loop[2] > 0 && !txOn)
            loop[2] = 0;

        if (loop[3] >= 2000)
        {
            chkComTimeout = true;
            loop[3] = 0;
        }

        if (!statusTimeout)
            loop[0]++;

        if (!frameTimeout)
            loop[1]++;

        if (currentMode != "idle" && txOn)
            loop[2]++;

        if (!chkComTimeout)
            loop[3]++;

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
void* modeRxThread(void *arg)
{
    uint8_t conn_id = (intptr_t)arg;
    int  sockfd = hostClient[conn_id].sockfd;
    bool found = false;

    while (hostClient[conn_id].active && running)
    {
        delay(100);

        sem_wait(&txBufSem);
        if (txModeBuffer.getData() >= 5)
        {
            if (txModeBuffer.peek() != 0x61)
            {
                uint8_t tmp[1];
                txModeBuffer.get(tmp[0]);
                fprintf(stderr, "txModeBuffer: invalid header. [%02X]\n", tmp[0]);
                if (tmp[0] != 0x61)
                {
                    sem_post(&txBufSem);
                    continue;
                }
                found = true;
            }
            uint8_t  byte[3];
            uint16_t len = 0;
            txModeBuffer.npeek(byte[0], 1);
            txModeBuffer.npeek(byte[1], 2);
            txModeBuffer.npeek(byte[2], 4);
            len = (byte[0] << 8) + byte[1];;
            if (txModeBuffer.getData() >= len && hostClient[conn_id].mode[0] == byte[2])
            {
                uint8_t buf[len];
                for (int i=0;i<len;i++)
                {
                    if (found && i == 0)
                        buf[0] = 0x61;
                    else
                        txModeBuffer.get(buf[i]);
                }
                if (write(sockfd, buf, len) < 0)
                {
                    fprintf(stderr, "ERROR: remote disconnect\n");
                    break;
                }
                found = false;
            }
        }
        sem_post(&txBufSem);

        // check for out going commands.
        if (hostClient[conn_id].command.getData() >= 5)
        {
            if (hostClient[conn_id].command.peek() != 0x61)
            {
         //       modeCommandBuffer.reset();
                fprintf(stderr, "modeTx invalid header.\n");
                continue;
            }
            uint8_t  byte[2];
            uint16_t len = 0;
            hostClient[conn_id].command.npeek(byte[0], 1);
            hostClient[conn_id].command.npeek(byte[1], 2);
            len = (byte[0] << 8) + byte[1];;
            if (hostClient[conn_id].command.getData() >= len)
            {
                uint8_t buf[len];
                for (int i=0;i<len;i++)
                {
                    hostClient[conn_id].command.get(buf[i]);
                }
                if (write(sockfd, buf, len) < 0)
                {
                    fprintf(stderr, "ERROR: remote disconnect\n");
                    break;
                }
            }
        }
    }
    hostClient[conn_id].active = false;
    delay(50000);
    fprintf(stderr, "Mode RX thread exited. Connection Id: %d\n", conn_id);
    int iRet = 500;
    pthread_exit(&iRet);
    return NULL;
}

// MMDVM modem specific function.
// Queue up out going bytes for modem.
void* modeTxThread(void *arg)
{
    while (running)
    {
        delay(100);

        if (statusTimeout)
        {
            if (txBuffer.getSpace() >= 3 && rxModeBuffer.getData() < 3)
            {
                txBuffer.put(0xE0);
                txBuffer.put(0x03);
                txBuffer.put(MODEM_STATUS);
            }
            statusTimeout = false;
        }

        if (rxModeBuffer.getData() >= 3 && frameTimeout)
        {
            if (rxModeBuffer.peek() != 0xE0)
            {
                fprintf(stderr, "modemTx invalid header.\n");
                frameTimeout = false;
                continue;
            }
            uint8_t len = 0;
            rxModeBuffer.npeek(len, 1);
            if (rxModeBuffer.getData() >= len)
            {
                if (getModemSpace(currentMode.c_str()) > 1 && txBuffer.getSpace() >= len + 3)
                {
                    uint8_t buf[1];
                    for (int i=0;i<len;i++)
                    {
                        rxModeBuffer.get(buf[0]);
                        txBuffer.put(buf[0]);
                    }
                }
                if (txBuffer.getSpace() >= 3)
                {
                    txBuffer.put(0xE0);
                    txBuffer.put(0x03);
                    txBuffer.put(MODEM_STATUS);
                }
            }
            frameTimeout = false;
        }

        if (modemCommandBuffer.getData() >= 3)
        {
            if (modemCommandBuffer.peek() != 0xE0)
            {
                fprintf(stderr, "modem command: invalid header [%02X].\n", modemCommandBuffer.peek());
                continue;
            }
            uint8_t len = 0;
            modemCommandBuffer.npeek(len, 1);
            if (modemCommandBuffer.getData() >= len)
            {
                if (txBuffer.getSpace() >= len)
                {
                    uint8_t buf[1];
                    for (int i=0;i<len;i++)
                    {
                        modemCommandBuffer.get(buf[0]);
                        txBuffer.put(buf[0]);
                    }
                }
            }
        }
    }
    fprintf(stderr, "Mode TX thread exited.\n");
    int iRet = 400;
    pthread_exit(&iRet);
    return NULL;
}

// Send queued up bytes to modem.
void* modemTxThread(void *arg)
{
    while (running)
    {
        delay(50);

        if (txBuffer.getData() >= 3)
        {
            if (txBuffer.peek() != 0xE0)
            {
                uint8_t tmp[1];
                txBuffer.get(tmp[0]);
                fprintf(stderr, "modem TX invalid header. [%02X]\n", tmp[0]);
                continue;
            }
            uint8_t len = 0;
            if (!txBuffer.npeek(len, 1))
                fprintf(stderr, "npeek failed\n");
            if (len < 1)
            {
                uint8_t tmp[2];
                txBuffer.npeek(tmp[0], 1);
                fprintf(stderr, "modem length invalid. [%02X]  [%02X]\n", tmp[0], tmp[1]);
                continue;
            }

            if (txBuffer.getData() >= len)
            {
                uint8_t buf[len];
                for (uint8_t i=0;i<len;i++)
                    txBuffer.get(buf[i]);
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
void* commandThread(void *arg)
{
    while (running)
    {
        delay(500000);

        if (chkComTimeout)
        {
            std::string parameter = readDashbCommand("modemHost");
            if (parameter == "setConfig")
            {
                if (modem == "mmdvmhs")
                    set_ConfigHS();
                else
                    set_Config();
                uint8_t buffer[9];
                buffer[0] = 0x61;
                buffer[1] = 0x00;
                buffer[2] = 0x09;
                buffer[3] = 0x04;
                memcpy(buffer+4, TYPE_COMMAND, 4);
                if (modem_duplex)
                    buffer[8] = COMM_SET_DUPLEX;
                else
                    buffer[8] = COMM_SET_SIMPLEX;
                for (uint8_t x=0;x<9;x++)
                {
                    for (uint8_t i=0;i<MAX_CLIENT_CONNECTIONS;i++)
                    {
                        if (hostClient[i].active)
                            hostClient[i].command .put(buffer[x]);
                    }
                }

                ackDashbCommand("modemHost", "success");
            }
            chkComTimeout = false;
        }
    }
    fprintf(stderr, "Command thread exited.\n");
    int iRet = 350;
    pthread_exit(&iRet);
    return NULL;
}

// Handle incoming bytes from protocol service.
// One thread per service.
void *processClientSocket(void *arg)
{
    char          buffer[BUFFER_LENGTH];
    uint8_t       conn_id = (intptr_t)arg;
    int           sockFd  = hostClient[conn_id].sockfd;
    uint16_t      respLen = 0;
    uint8_t       type    = 0;
    uint8_t       typeLen = 0;
    uint16_t      offset  = 0;
    ssize_t       len     = 0;
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

        if (memcmp(buffer+4, "MODE", typeLen) == 0)
        {
            memcpy(mode[conn_id].name, buffer+8, 11);
            memcpy(mode[conn_id].modem_type, buffer+19, 6);
            mode[conn_id].use_rx_dc_filter = buffer[25];
            mode[conn_id].use_lp_filter = buffer[26];
            mode[conn_id].rx_filter_pState_len = buffer[27];
            mode[conn_id].rx_filter_pState = (int16_t*)malloc(mode[conn_id].rx_filter_pState_len * 2);
            if (mode[conn_id].rx_filter_pState == NULL) break;
            memset(mode[conn_id].rx_filter_pState, 0x00, mode[conn_id].rx_filter_pState_len * 2);
            mode[conn_id].rx_filter_numTaps = buffer[28];
            mode[conn_id].rx_filter_pCoeffs = (int16_t*)malloc(mode[conn_id].rx_filter_numTaps * 2);
            if (mode[conn_id].rx_filter_pCoeffs == NULL) break;
            memcpy(mode[conn_id].rx_filter_pCoeffs, buffer+29, mode[conn_id].rx_filter_numTaps * 2);
            uint8_t pos = 29 + mode[conn_id].rx_filter_numTaps;
            mode[conn_id].tx_filter_symbol_len = buffer[pos];
            mode[conn_id].tx_filter_pState_len = buffer[pos+1];
            mode[conn_id].tx_filter_pState = (int16_t*)malloc(mode[conn_id].tx_filter_pState_len * 2);
            if (mode[conn_id].tx_filter_pState == NULL) break;
            memset(mode[conn_id].tx_filter_pState, 0x00, mode[conn_id].tx_filter_pState_len * 2);
            mode[conn_id].tx_filter_phaseLen = buffer[pos+2];
            mode[conn_id].tx_filter_Len = buffer[pos+3];
            mode[conn_id].tx_filter_pCoeffs = (int16_t*)malloc(mode[conn_id].tx_filter_Len);
            if (mode[conn_id].tx_filter_pCoeffs == NULL) break;
            memcpy(mode[conn_id].tx_filter_pCoeffs, buffer+pos+3, mode[conn_id].tx_filter_Len);
            if (mode[conn_id].use_lp_filter)
            {
                pos = pos + 3 + mode[conn_id].tx_filter_Len;
                mode[conn_id].tx_lp_filter_pState_len = buffer[pos];
                mode[conn_id].tx_filter_pState = (int16_t*)malloc(mode[conn_id].tx_filter_pState_len * 2);
                if (mode[conn_id].tx_filter_pState == NULL) break;
                memset(mode[conn_id].tx_filter_pState, 0x00, mode[conn_id].tx_filter_pState_len * 2);
                mode[conn_id].tx_lp_filter_numTaps = buffer[pos+1];
                mode[conn_id].tx_lp_filter_pCoeffs = (int16_t*)malloc(mode[conn_id].tx_lp_filter_numTaps * 2);
                if (mode[conn_id].tx_lp_filter_pCoeffs == NULL) break;
                memcpy(mode[conn_id].tx_lp_filter_pCoeffs, buffer+pos+2, mode[conn_id].tx_lp_filter_numTaps);
            }
            strcpy(hostClient[conn_id].mode, mode[conn_id].name);
            addMode("main", mode[conn_id].name);
            fprintf(stderr, "Received mode data for [%s].\n", mode[conn_id].name);
            // MMDVM specific command
            setProtocol(mode[conn_id].name, true);

            buffer[0] = 0xE0;
            buffer[1] = 0x04;
            buffer[2] = MODEM_MODE;
            buffer[3] = 0x00; // IDLE_MODE
            for (uint8_t i=0;i<4;i++)
                modemCommandBuffer.put(buffer[i]);
        }

// *********** Convert from OpenMT type to modem specific type ***********
        if (memcmp(buffer+4, TYPE_LSF_M17, typeLen) == 0)
            type = TYPE_M17_LSF;
        else if (memcmp(buffer+4, TYPE_STREAM_M17, typeLen) == 0)
            type = TYPE_M17_STREAM;
        else if (memcmp(buffer+4, TYPE_PACKET_M17, typeLen) == 0)
            type = TYPE_M17_PACKET;
        else if (memcmp(buffer+4, TYPE_EOT_M17, typeLen) == 0)
            type = TYPE_M17_EOT;
        else if (memcmp(buffer+4, TYPE_HEADER_DSTAR, typeLen) == 0)
            type = TYPE_DSTAR_HEADER;
        else if (memcmp(buffer+4, TYPE_DATA_DSTAR, typeLen) == 0)
            type = TYPE_DSTAR_DATA;
        else if (memcmp(buffer+4, TYPE_EOT_DSTAR, typeLen) == 0)
            type = TYPE_DSTAR_EOT;
        else if (memcmp(buffer+4, TYPE_ACK, typeLen) == 0)
            type = NET_ACK;
        else if (memcmp(buffer+4, TYPE_NACK, typeLen) == 0)
            type = NET_NACK;

        if (debugM)
           dump((char*)"Protocol service data:", (unsigned char*)buffer, respLen);

        if (rxModeBuffer.getSpace() < respLen)
        {
            fprintf(stderr, "rxModeBuffer out of space. [%02X]\n", rxModeBuffer.peek());
            rxModeBuffer.reset();
        }
// This line to be removed after debugging
if (debugM)
fprintf(stderr, "Type: %02X  Mode: %s  Conn: %d  Active: %d  Mode Space: %d\n", type, currentMode.c_str(), conn_id, hostClient[conn_id].active, getModemSpace(currentMode.c_str()));
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
                if (currentMode == "idle")
                {
                    currentMode = hostClient[conn_id].mode;
                    setProtocol(currentMode.c_str(), true);
        //           if (debugM)
                    fprintf(stderr, "Current mode set to %s.\n", currentMode.c_str());

                }
                break;

            case COMM_SET_IDLE:
                if (currentMode != "idle")
                {
                    currentMode = "idle";
                    buffer[0] = 0xE0;
                    buffer[1] = 0x04;
                    buffer[2] = MODEM_MODE;
                    buffer[3] = 0x00; // IDLE_MODE
                    for (uint8_t i=0;i<4;i++)
                        modemCommandBuffer.put(buffer[i]);
                    //           if (debugM)
                        fprintf(stderr, "Current mode set to IDLE.\n");
                }
                break;

            case TYPE_M17_LSF:
            case TYPE_M17_STREAM:
            case TYPE_M17_PACKET:
            case TYPE_M17_EOT:
            {
                if (hostClient[conn_id].active && currentMode == hostClient[conn_id].mode)
                {
                    txOn = true;
                    rxModeBuffer.put(0xE0);
                    rxModeBuffer.put(0x34);
                    rxModeBuffer.put(type);
                    rxModeBuffer.put(0x00);
                    for (int x=0;x<48;x++)
                    {
                        rxModeBuffer.put(buffer[x+4+typeLen]);
                    }
                }
                if (type == TYPE_M17_EOT)
                    txOn = false;
            }
            break;

            case TYPE_DSTAR_HEADER:
            {
                if (hostClient[conn_id].active && currentMode == hostClient[conn_id].mode)
                {
                    txOn = true;
                    rxModeBuffer.put(0xE0);
                    rxModeBuffer.put(0x2C);
                    rxModeBuffer.put(type);
                    for (int x=0;x<41;x++)
                    {
                        rxModeBuffer.put(buffer[x+4+typeLen]);
                    }
                }
            }
            break;

            case TYPE_DSTAR_DATA:
            {
                if (hostClient[conn_id].active && currentMode == hostClient[conn_id].mode)
                {
                    txOn = true;
                    rxModeBuffer.put(0xE0);
                    rxModeBuffer.put(0x0F);
                    rxModeBuffer.put(type);
                    for (int x=0;x<12;x++)
                    {
                        rxModeBuffer.put(buffer[x+4+typeLen]);
                    }
                }
            }
            break;

            case TYPE_DSTAR_EOT:
            {
                if (hostClient[conn_id].active && currentMode == hostClient[conn_id].mode)
                {
                    txOn = false;
                    rxModeBuffer.put(0xE0);
                    rxModeBuffer.put(0x03);
                    rxModeBuffer.put(type);
                }
            }
            break;
        }
        delay(50);
    }
    if (connections > 0)
        connections--;
    hostClient[conn_id].active = false;
    setProtocol(mode[conn_id].name, false);

    if (mode[conn_id].use_lp_filter)
    {
        free(mode[conn_id].tx_lp_filter_pCoeffs);
        free(mode[conn_id].tx_lp_filter_pState);
    }
    free(mode[conn_id].rx_filter_pCoeffs);
    free(mode[conn_id].rx_filter_pState);
    free(mode[conn_id].tx_filter_pCoeffs);
    free(mode[conn_id].tx_filter_pState);

    sem_wait(&shutDownSem);
    if (currentMode != "idle")
    {
        currentMode = "idle";
        buffer[0] = 0xE0;
        buffer[1] = 0x04;
        buffer[2] = MODEM_MODE;
        buffer[3] = 0x00; // IDLE_MODE
        for (uint8_t i=0;i<4;i++)
            modemCommandBuffer.put(buffer[i]);
        //           if (debugM)
        fprintf(stderr, "Current mode set to IDLE.\n");
    }
    delMode("main", mode[conn_id].name);
    if (modem == "mmdvmhs")
        set_ConfigHS();
    else
        set_Config();

    delay(50000);
    close(sockFd);
    fprintf(stderr, "Client thread exited. Connection Id: %d\n", conn_id);
    int iRet = 200;
    sem_post(&shutDownSem);
    pthread_exit(&iRet);
    return NULL;
}

// This thread listens for incoming TCP connections from protocol services.
void *startTCPServer(void *arg)
{
    struct hostent *hostp; /* client host info */
    int childfd; /* child socket */
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
    setsockopt(sockFd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));

    /* this is an Internet address */
    serveraddr.sin_family = AF_INET;

    /* let the system figure out our IP address */
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* this is the port we will listen on */
    serveraddr.sin_port = htons((unsigned short)host_port);

    if (bind(sockFd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0)
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
        childfd = accept(sockFd, (struct sockaddr *) &clientaddr, &clientlen);
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
        hostp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr, sizeof(clientaddr.sin_addr.s_addr), AF_INET);
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
        for (uint8_t i=0;i<MAX_CLIENT_CONNECTIONS;i++)
        {  // look for free connection
            if (!hostClient[i].active)
            {
                bzero(mode[i].name, 11);
                bzero(mode[i].modem_type, 6);
                newConn = i;
                break;
            }
        }

        if (newConn == 255) // check if max connections
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
    sem_wait(&shutDownSem);
    fprintf(stderr, "TCP server exited.\n");
    int iRet = 100;
    delay(500000);
    sem_post(&shutDownSem);
    pthread_exit(&iRet);
    running = false;
    return NULL;
}

// Mmdvm modem specific function
// Process incoming modem bytes.
int processSerial(void)
{
    unsigned char buffer[BUFFER_LENGTH];
    unsigned int  respLen;
    unsigned int  offset;
    unsigned int  type;
    ssize_t       len;

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
    type = buffer[2];

    offset += 1;
    while (offset < respLen)
    {
        len = read(serialModemFd, buffer + offset, respLen - offset);

        if (len == 0)
            delay(5);
        else
            offset += len;
    }

    if (txModeBuffer.getSpace() == 0)
    {
        fprintf(stderr, "txModeBuffer out of space.\n");
        txModeBuffer.reset();
    }

    if (debugM)
    {
        dump((char*)"SERIAL:", buffer, respLen);
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
    else if (type == MODEM_DEBUG)
    {
        if (debugM)
            dump((char*)"DEBUG1:", buffer, respLen);
    }
    else if (type == MODEM_VERSION)
    {
        fprintf(stderr, "Version\n");
    }
    else if (type == MODEM_STATUS)
    {
        if (debugM)
            dump((char*)"STATUS:", buffer, respLen);
        setM17Space(buffer[12]);
        setDSTARSpace(buffer[6]);
    }
    else if (type == TYPE_M17_LSF && respLen == 54)
    {
        if (connections > 0 && (currentMode == "idle" || currentMode == "M17"))
        {
            txModeBuffer.put(0x61);
            txModeBuffer.put(0x00);
            txModeBuffer.put(0x38);
            txModeBuffer.put(0x04);
            txModeBuffer.put(TYPE_LSF_M17[0]);
            txModeBuffer.put(TYPE_LSF_M17[1]);
            txModeBuffer.put(TYPE_LSF_M17[2]);
            txModeBuffer.put(TYPE_LSF_M17[3]);
            for (int x=0;x<48;x++)
            {
                txModeBuffer.put(buffer[x+4]);
            }
        }
    }
    else if (type == TYPE_M17_STREAM && respLen == 54)
    {
        if (connections > 0 && (currentMode == "idle" || currentMode == "M17"))
        {
            txModeBuffer.put(0x61);
            txModeBuffer.put(0x00);
            txModeBuffer.put(0x38);
            txModeBuffer.put(0x04);
            txModeBuffer.put(TYPE_STREAM_M17[0]);
            txModeBuffer.put(TYPE_STREAM_M17[1]);
            txModeBuffer.put(TYPE_STREAM_M17[2]);
            txModeBuffer.put(TYPE_STREAM_M17[3]);
            for (int x=0;x<48;x++)
            {
                txModeBuffer.put(buffer[x+4]);
            }
        }
    }
    else if (type == TYPE_M17_PACKET && respLen == 54)
    {
        if (connections > 0 && (currentMode == "idle" || currentMode == "M17"))
        {
            txModeBuffer.put(0x61);
            txModeBuffer.put(0x00);
            txModeBuffer.put(0x38);
            txModeBuffer.put(0x04);
            txModeBuffer.put(TYPE_PACKET_M17[0]);
            txModeBuffer.put(TYPE_PACKET_M17[1]);
            txModeBuffer.put(TYPE_PACKET_M17[2]);
            txModeBuffer.put(TYPE_PACKET_M17[3]);
            for (int x=0;x<48;x++)
            {
                txModeBuffer.put(buffer[x+4]);
            }
        }
    }
    else if ((type == TYPE_M17_EOT || type == TYPE_M17_LOST) && respLen == 3)
    {
        if (connections > 0 && (currentMode == "idle" || currentMode == "M17"))
        {
            txModeBuffer.put(0x61);
            txModeBuffer.put(0x00);
            txModeBuffer.put(0x08);
            txModeBuffer.put(0x04);
            txModeBuffer.put(TYPE_EOT_M17[0]);
            txModeBuffer.put(TYPE_EOT_M17[1]);
            txModeBuffer.put(TYPE_EOT_M17[2]);
            txModeBuffer.put(TYPE_EOT_M17[3]);
        }
    }
    else if (type == TYPE_DSTAR_HEADER && respLen == 46)
    {
        if (connections > 0 && (currentMode == "idle" || currentMode == "DSTAR"))
        {
            txModeBuffer.put(0x61);
            txModeBuffer.put(0x00);
            txModeBuffer.put(0x31);
            txModeBuffer.put(0x04);
            txModeBuffer.put(TYPE_HEADER_DSTAR[0]);
            txModeBuffer.put(TYPE_HEADER_DSTAR[1]);
            txModeBuffer.put(TYPE_HEADER_DSTAR[2]);
            txModeBuffer.put(TYPE_HEADER_DSTAR[3]);
            for (int x=0;x<41;x++)
            {
                txModeBuffer.put(buffer[x+3]);
            }
        }
    }
    else if (type == TYPE_DSTAR_DATA && (respLen == 15 || respLen == 17))
    {
        if (connections > 0 && (currentMode == "idle" || currentMode == "DSTAR"))
        {
            txModeBuffer.put(0x61);
            txModeBuffer.put(0x00);
            txModeBuffer.put(0x14);
            txModeBuffer.put(0x04);
            txModeBuffer.put(TYPE_DATA_DSTAR[0]);
            txModeBuffer.put(TYPE_DATA_DSTAR[1]);
            txModeBuffer.put(TYPE_DATA_DSTAR[2]);
            txModeBuffer.put(TYPE_DATA_DSTAR[3]);
            for (int x=0;x<12;x++)
            {
                txModeBuffer.put(buffer[x+3]);
            }
        }
    }
    else if ((type == TYPE_DSTAR_EOT || type == TYPE_DSTAR_LOST) && respLen == 3 )
    {
        if (connections > 0 && (currentMode == "idle" || currentMode == "DSTAR"))
        {
            txModeBuffer.put(0x61);
            txModeBuffer.put(0x00);
            txModeBuffer.put(0x08);
            txModeBuffer.put(0x04);
            txModeBuffer.put(TYPE_EOT_DSTAR[0]);
            txModeBuffer.put(TYPE_EOT_DSTAR[1]);
            txModeBuffer.put(TYPE_EOT_DSTAR[2]);
            txModeBuffer.put(TYPE_EOT_DSTAR[3]);
        }
    }
    else
    {
        if (debugM)
            dump((char*)"Modem rec data:", buffer, respLen);
    }

    return 1;
}

int main(int argc, char **argv)
{
    bool daemon = false;
    int  ret;
    int  c;

    while ((c = getopt(argc, argv, "d:vx")) != -1)
    {
        switch (c)
        {
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
                fprintf(stderr, "Usage: Modem_Host [-d] [-v] [-x]\n");
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

    sem_init(&txBufSem, 0, 1);
    sem_init(&shutDownSem, 0, 1);

    for (uint8_t i=0;i<MAX_CLIENT_CONNECTIONS;i++)
    {
        hostClient[i].active = false;
        hostClient[i].isTx = false;
        strcpy(hostClient[i].mode, "");
        hostClient[i].command.reset();
    }

    modem = readModemConfig("modem1", "modem");
    commPort = readModemConfig("modem1", "port");
    modemBaud = readModemConfig("modem1", "baud");

    callsign = readHostConfig("main", "callsign");

    strcpy(modem_rxFrequency, readModemConfig("modem1", "rxFrequency").c_str());
    strcpy(modem_txFrequency, readModemConfig("modem1", "txFrequency").c_str());

    sprintf(modemtty, "/dev/%s", commPort.c_str());
    serialModemFd = openSerial(modemtty);
    if (serialModemFd < 0)
        return 1;

    int err = pthread_create(&(modemTxid), NULL, &modemTxThread, NULL);
    if (err != 0)
    {
        fprintf(stderr, "Can't create modem tx thread :[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM)
            fprintf(stderr, "Modem tx thread created successfully\n");
    }
    err = pthread_create(&(timerid), NULL, &timerThread, NULL);
    if (err != 0)
    {
        fprintf(stderr, "Can't create timer thread :[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM)
            fprintf(stderr, "Timer thread created successfully\n");
    }
    err = pthread_create(&(modetxid), NULL, &modeTxThread, NULL);
    if (err != 0)
    {
        fprintf(stderr, "Can't create mode tx thread :[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM)
            fprintf(stderr, "Mode tx thread created successfully\n");
    }
    err = pthread_create(&(tcpid), NULL, &startTCPServer, NULL);
    if (err != 0)
    {
        fprintf(stderr, "Can't create TCP server thread :[%s]", strerror(err));
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
        fprintf(stderr, "Can't create command thread :[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM)
            fprintf(stderr, "Command thread created successfully\n");
    }

    if (modem == "mmdvmhs")
        set_ConfigHS();
    else
        set_Config();

    setHostConfig("main", "gateways", "none", "none");
    setHostConfig("main", "activeModes", "none", "none");
    delay(10000);
    setFrequency(modem_rxFrequency, modem_txFrequency, modem_txFrequency, 255);

    while (running)
    {
        ret = processSerial();
        if (!ret)
            break;
    }
    sem_wait(&shutDownSem);
    setHostConfig("main", "gateways", "none", "none");
    setHostConfig("main", "activeModes", "none", "none");
    fprintf(stderr, "Modem host terminated.\n");
    logError("main", "Modem host terminated.");
    sem_post(&shutDownSem);
    return 0;
}
