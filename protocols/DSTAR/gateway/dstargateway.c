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
 *   Some functions based on or inspired by MMDVM Jonathan Naylor G4KLX    *
 ***************************************************************************/

#include <CCITTChecksumReverse.h>
#include <RingBuffer.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
#include <time.h>
#include <tools.h>
#include <unistd.h>

#include "../dstar_gps.h"

#define VERSION "2025-09-25"
#define BUFFER_SIZE 1024

pthread_t dstarReflid;
pthread_t clientid;
pthread_t txid;

int sockoutfd            = 0;
int clientPort           = 18100;
uint8_t modemId          = 1;
char modemName[8]        = "modem1";
char DSTARName[8]        = "";
char DSTARModule[2]      = "C";
char myCall[9]           = "";
char urCall[9]           = "CQCQCQ";
char rpt1Call[9]         = "DIRECT";
char rpt2Call[9]         = "DIRECT";
char lastCall[9]         = "";
char suffix[5]           = "";
char metaText[23]        = "";
char gwAddress[16]       = "127.0.0.1";
char tx_state[4]         = "off";
char DSTARHost[80]       = "127.0.0.1";
bool DSTARReflDisconnect = false;
bool DSTARReflConnected  = false;
bool reflPacketRdy       = false;
bool connected           = true;
bool debugM              = false;
bool slowSpeedUpdate     = false;
uint8_t duration         = 0;
uint16_t streamId        = 0;
uint16_t gwPortIn        = 20011;
uint16_t gwPortOut       = 20010;
time_t start_time;
uint8_t iBER = 0;

struct sockaddr_in servaddrin, servaddrout, cliaddr;

char currentDSTARRefl[256] = "";
char rptrCallsign[20]      = "N0CALL";

/* Gateway State Machine - Half Duplex */
typedef enum
{
    GATEWAY_IDLE = 0,         /* No transmission in either direction */
    GATEWAY_TX_TO_REFLECTOR,  /* Repeater → Reflector (outbound) */
    GATEWAY_RX_FROM_REFLECTOR /* Reflector → Repeater (inbound) */
} GatewayState;

typedef struct
{
    GatewayState state;
    uint16_t reflector_stream_id; /* Only for RX direction */
    uint64_t last_packet_time;
    pthread_mutex_t mutex;
} GatewayCommState;

/* Global gateway state */
GatewayCommState gwState = {
    .state               = GATEWAY_IDLE,
    .reflector_stream_id = 0,
    .last_packet_time    = 0,
    .mutex               = PTHREAD_MUTEX_INITIALIZER};

// RingBuffer reflTxBuffer;
RingBuffer txBuffer;

/* Mutex to protect txBuffer RingBuffer */
pthread_mutex_t txBufMutex = PTHREAD_MUTEX_INITIALIZER;

const char* TYPE_HEADER     = "DSTH";
const char* TYPE_DATA       = "DSTD";
const char* TYPE_EOT        = "DSTE";
const char* TYPE_ACK        = "ACK ";
const char* TYPE_NACK       = "NACK";
const char* TYPE_DISCONNECT = "DISC";
const char* TYPE_CONNECT    = "CONN";
const char* TYPE_STATUS     = "STAT";
const char* TYPE_COMMAND    = "COMM";

const uint8_t COMM_UPDATE_CONF = 0x04;

const uint8_t AMBE_HEADER[]   = {0x61, 0x00, 0x0B, 0x01, 0x01, 0x48};
const uint8_t AMBE_HEADER_LEN = 6;
const uint8_t AMBE_SILENCE[]  = {0x9e, 0x8d, 0x32, 0x32, 0x26, 0x1a, 0x3f, 0x61, 0xe8};

char sGps[256]     = "";
char sGPSCall[256] = "";
float fLat         = 0.0f;
float fLong        = 0.0f;
uint16_t altitude  = 0;

typedef union crc
{
    unsigned int m_crc16;
    unsigned char m_crc8[2];
} m_crc;
m_crc mcrc;

// error - wrapper for perror
void error(char* msg)
{
    perror(msg);
    exit(1);
}

// Wait for 'delay' miroseconds
void delay(uint32_t delay)
{
    struct timespec req, rem;
    req.tv_sec  = 0;
    req.tv_nsec = delay * 1000;
    nanosleep(&req, &rem);
};

/* Get current time in milliseconds */
static uint64_t getTimeMillis(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
        return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static bool gwStateStartTx(void)
{
    pthread_mutex_lock(&gwState.mutex);

    if (gwState.state != GATEWAY_IDLE)
    {
        GatewayState current = gwState.state;
        pthread_mutex_unlock(&gwState.mutex);
        fprintf(stderr, "REJECT TX: Gateway busy (%s)\n", current == GATEWAY_TX_TO_REFLECTOR ? "TX active" : "RX active");
        return false;
    }

    gwState.state            = GATEWAY_TX_TO_REFLECTOR;
    gwState.last_packet_time = getTimeMillis();
    pthread_mutex_unlock(&gwState.mutex);

    fprintf(stderr, "START TX: Host→Reflector\n");
    return true;
}

/* Try to start reflector→host reception */
static bool gwStateStartRx(uint16_t stream_id)
{
    pthread_mutex_lock(&gwState.mutex);

    if (gwState.state != GATEWAY_IDLE)
    {
        GatewayState current = gwState.state;
        pthread_mutex_unlock(&gwState.mutex);
        if (gwState.reflector_stream_id != stream_id)
            fprintf(stderr, "REJECT RX: Gateway busy (%s)\n", current == GATEWAY_TX_TO_REFLECTOR ? "TX active" : "RX active");
        return false;
    }

    gwState.state               = GATEWAY_RX_FROM_REFLECTOR;
    gwState.reflector_stream_id = stream_id;
    gwState.last_packet_time    = getTimeMillis();
    pthread_mutex_unlock(&gwState.mutex);

    fprintf(stderr, "START RX: Reflector→Host (stream %04X)\n", stream_id);
    return true;
}

/* Update packet timestamp */
static void gwStateUpdateTime(void)
{
    pthread_mutex_lock(&gwState.mutex);
    gwState.last_packet_time = getTimeMillis();
    pthread_mutex_unlock(&gwState.mutex);
}

/* End transmission */
static void gwStateEnd(const char* reason)
{
    pthread_mutex_lock(&gwState.mutex);

    if (gwState.state != GATEWAY_IDLE)
    {
        fprintf(stderr, "END: %s (reason: %s)\n", gwState.state == GATEWAY_TX_TO_REFLECTOR ? "TX" : "RX", reason);
        gwState.state               = GATEWAY_IDLE;
        gwState.reflector_stream_id = 0;
        gwState.last_packet_time    = 0;
    }

    pthread_mutex_unlock(&gwState.mutex);
}

/* Check for timeout and reset if needed */
static bool gwStateCheckTimeout(uint32_t timeout_ms)
{
    pthread_mutex_lock(&gwState.mutex);

    if (gwState.state == GATEWAY_IDLE || gwState.last_packet_time == 0)
    {
        pthread_mutex_unlock(&gwState.mutex);
        return false;
    }

    uint64_t now     = getTimeMillis();
    uint64_t elapsed = now - gwState.last_packet_time;

    if (elapsed > timeout_ms)
    {
        fprintf(stderr, "TIMEOUT: %s stream %04X (%llu ms)\n", gwState.state == GATEWAY_TX_TO_REFLECTOR ? "TX" : "RX",
                gwState.reflector_stream_id, elapsed);
        gwState.state               = GATEWAY_IDLE;
        gwState.reflector_stream_id = 0;
        gwState.last_packet_time    = 0;
        pthread_mutex_unlock(&gwState.mutex);
        return true;
    }

    pthread_mutex_unlock(&gwState.mutex);
    return false;
}

/* Check if TX active */
static bool gwStateIsTxActive(void)
{
    pthread_mutex_lock(&gwState.mutex);
    bool active = (gwState.state == GATEWAY_TX_TO_REFLECTOR);
    pthread_mutex_unlock(&gwState.mutex);
    return active;
}

/* Check if RX active */
static bool gwStateIsRxActive(void)
{
    pthread_mutex_lock(&gwState.mutex);
    bool active = (gwState.state == GATEWAY_RX_FROM_REFLECTOR);
    pthread_mutex_unlock(&gwState.mutex);
    return active;
}

/* Verify stream ID */
static bool gwStateVerifyStreamId(uint16_t stream_id)
{
    pthread_mutex_lock(&gwState.mutex);
    bool matches      = (gwState.reflector_stream_id == stream_id);
    uint16_t expected = gwState.reflector_stream_id;
    pthread_mutex_unlock(&gwState.mutex);

    if (!matches)
        fprintf(stderr, "STREAM MISMATCH: Expected %04X, got %04X\n", expected, stream_id);

    return matches;
}

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

        for (i = 0U; i < bytes; i++) fprintf(stdout, "%02X ", data[offset + i]);

        for (i = bytes; i < 16U; i++) fputs("   ", stdout);

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

// Search DSTAR reflector files for given reflect name. Currently not used.
int searchDSTARHostFile(const char* refl, char* ipaddr)
{
    FILE* file     = NULL;
    char name[10]  = "";
    char line[101] = "";
    uint8_t ret    = 0;

    file = fopen("./DPlus_Hosts.txt", "r");
    if (file != NULL)
    {
        clearReflectorList("DPLUS");
        while (!feof(file))
        {
            fgets(line, 100, file);
            if (line[0] == '#')
                continue;

            fscanf(file, "%s  %s", name, ipaddr);

            if (strcasecmp(name, refl) == 0)
            {
                addReflector("DPLUS", name, ipaddr, "none", 0, "none", "none", "na");
                ret = 1;
                break;
            }
        }
        fclose(file);
    }

    file = fopen("./DExtra_Hosts.txt", "r");
    if (file != NULL)
    {
        clearReflectorList("DEXTRA");
        while (!feof(file))
        {
            fgets(line, 100, file);
            if (line[0] == '#')
                continue;

            fscanf(file, "%s  %s", name, ipaddr);

            if (strcasecmp(name, refl) == 0)
            {
                addReflector("DEXTRA", name, ipaddr, "none", 0, "none", "none", "na");
                ret = 1;
                break;
            }
        }
        fclose(file);
    }

    file = fopen("./DCS_Hosts.txt", "r");
    if (file != NULL)
    {
        clearReflectorList("DCS");
        while (!feof(file))
        {
            fgets(line, 100, file);
            if (line[0] == '#')
                continue;

            fscanf(file, "%s  %s", name, ipaddr);

            if (strcasecmp(name, refl) == 0)
            {
                addReflector("DCS", name, ipaddr, "none", 0, "none", "none", "na");
                ret = 1;
                break;
            }
        }
        fclose(file);
    }

    file = fopen("./CCS_Hosts.txt", "r");
    if (file != NULL)
    {
        clearReflectorList("CCS");
        while (!feof(file))
        {
            fgets(line, 100, file);
            if (line[0] == '#')
                continue;

            fscanf(file, "%s  %s", name, ipaddr);

            if (strcasecmp(name, refl) == 0)
            {
                addReflector("CCS", name, ipaddr, "none", 0, "none", "none", "na");
                ret = 1;
                break;
            }
        }
        fclose(file);
    }

    return ret;
}

// Thread used to send data to DSTAR host.
void* txThread(void* arg)
{
    int sockfd    = (intptr_t)arg;
    uint16_t loop = 0;

    while (connected)
    {
        delay(100);
        loop++;

        if (loop > 50)
        {
            pthread_mutex_lock(&txBufMutex);
            if (RingBuffer_dataSize(&txBuffer) >= 5)
            {
                uint8_t buf[1];
                RingBuffer_peek(&txBuffer, buf, 1);
                if (buf[0] != 0x61)
                {
                    pthread_mutex_unlock(&txBufMutex);
                    fprintf(stderr, "TX EOT invalid header.\n");
                    continue;
                }
                uint8_t byte[3];
                uint16_t len = 0;
                RingBuffer_peek(&txBuffer, byte, 3);
                len = (byte[1] << 8) + byte[2];

                if (RingBuffer_dataSize(&txBuffer) >= len)
                {
                    uint8_t buf[len];
                    RingBuffer_getData(&txBuffer, buf, len);
                    pthread_mutex_unlock(&txBufMutex);

                    if (write(sockfd, buf, len) < 0)
                    {
                        fprintf(stderr, "ERROR: remote disconnect\n");
                        break;
                    }
                }
                else
                {
                    pthread_mutex_unlock(&txBufMutex);
                }
            }
            else
            {
                pthread_mutex_unlock(&txBufMutex);
            }

            loop = 0;
        }
    }
    fprintf(stderr, "TX thread exited.\n");
    int iRet  = 500;
    connected = false;
    pthread_exit(&iRet);
    return NULL;
}

/* Decode GPS data in slow-speed bytes */
bool decodeGPS(unsigned char c)
{
    static char tgps[201];
    static int gpsidx = 0;

    return dstar_parse_gps(c, tgps, &gpsidx, sGPSCall, &fLat, &fLong, &altitude);
}

/* Decode slow-speed data bytes */
int slowSpeedDataDecode(unsigned char a, unsigned char b, unsigned char c)
{
    static bool bSyncFound;
    static bool bFirstSection;
    static bool bHeaderActive;
    static int iSection;
    static unsigned char ucType;
    int iRet = 0;
    static char cText[30];

    // Unscramble
    a ^= 0x70;
    b ^= 0x4f;
    c ^= 0x93;

    if (a == 0x25 && b == 0x62 && c == 0x85)
    {
        bSyncFound    = true;
        bFirstSection = true;
        memset(cText, 0x20, 29);
    }

    if (bFirstSection)
    {
        ucType = (unsigned char)(a & 0xf0);

        // DV Header start
        if (bSyncFound == true && ucType == 0x50)
        {
            bSyncFound    = false;
            bHeaderActive = true;
        }

        bFirstSection = false;

        switch (ucType)
        {
            case 0x50:  // Header
                break;

            case 0x30:  // GPS
                if (decodeGPS(b))
                    iRet = 2;
                else if (decodeGPS(c))
                    iRet = 2;
                break;

            case 0x40:  // Text
                iSection = a & 0x0f;
                if (iSection >= 4)
                {
                    bFirstSection = true;
                    break;
                }
                cText[iSection * 5 + 0] = b;
                cText[iSection * 5 + 1] = c;
                break;

            case 0xc0:  // Code Squelch
                break;

            default:
                bFirstSection = true;
                break;
        }
    }
    else
    {
        switch (ucType)
        {
            case 0x50:  // Header
                break;

            case 0x30:  // GPS
                if (decodeGPS(a))
                    iRet = 2;
                else if (decodeGPS(b))
                    iRet = 2;
                else if (decodeGPS(c))
                    iRet = 2;
                break;

            case 0x40:  // Text
                cText[iSection * 5 + 2] = a;
                cText[iSection * 5 + 3] = b;
                cText[iSection * 5 + 4] = c;
                if (iSection == 3)
                {
                    if (memcmp(cText, metaText, 20) != 0)
                    {
                        slowSpeedUpdate = true;
                        memcpy(metaText, cText, 20);
                        metaText[20]    = 0;
                        slowSpeedUpdate = false;
                    }
                    iRet = 3;
                    memset(cText, 0x20, 30);
                }

                break;

            case 0xc0:  // Code Squelch
                break;
        }
        bFirstSection = true;
    }
    return iRet;
}

// Encode message to slow-speed bytes.
void slowSpeedDataEncode(char* cMessage, unsigned char* ucBytes, unsigned char ucMode)
{
    static int iIndex;
    static unsigned char ucFrameCount;

    if (ucMode == 10)
    {
        ucFrameCount = 0;
        return;
    }

    int iSyncfrm = ucFrameCount % 21;

    if (ucFrameCount > 251)
        ucFrameCount = 0;
    else
        ucFrameCount++;

    if (iSyncfrm == 0)
    {
        ucBytes[0] = 0x25 ^ 0x70;
        ucBytes[1] = 0x62 ^ 0x4f;
        ucBytes[2] = 0x85 ^ 0x93;
        iIndex     = 0;
        return;
    }

    if (ucMode == 0)
    {
        char slowMessage[40];
        memset(slowMessage, 0x20, 39);

        if (iIndex <= 36)
        {
            int iSection = iIndex % 5;
            if (iSection == 0)
            {
                if (iIndex == 36)
                    ucBytes[0] = 0x51;
                else
                    ucBytes[0] = 0x55;
                ucBytes[1] = (unsigned char)slowMessage[iIndex++];
                ucBytes[2] = (unsigned char)slowMessage[iIndex++];
            }
            else
            {
                ucBytes[0] = (unsigned char)slowMessage[iIndex++];
                ucBytes[1] = (unsigned char)slowMessage[iIndex++];
                ucBytes[2] = (unsigned char)slowMessage[iIndex++];
            }
        }
        else
        {
            ucBytes[0] = 0x66;
            ucBytes[1] = 0x66;
            ucBytes[2] = 0x66;
        }
    }

    if (ucMode == 1)
    {
        if (iIndex <= 17)
        {
            int iSection = iIndex % 5;
            if (iSection == 0)
            {
                ucBytes[0] = (unsigned char)((0x40 + iIndex / 5) ^ 0x70);
                ucBytes[1] = (unsigned char)cMessage[iIndex++] ^ 0x4f;
                ucBytes[2] = (unsigned char)cMessage[iIndex++] ^ 0x93;
            }
            else
            {
                ucBytes[0] = (unsigned char)cMessage[iIndex++] ^ 0x70;
                ucBytes[1] = (unsigned char)cMessage[iIndex++] ^ 0x4f;
                ucBytes[2] = (unsigned char)cMessage[iIndex++] ^ 0x93;
            }
        }
        else
        {
            ucBytes[0] = 0x66 ^ 0x70;
            ucBytes[1] = 0x66 ^ 0x4f;
            ucBytes[2] = 0x66 ^ 0x93;
        }
    }
}

/* Queue up bytes to send to IRCDDB gateway */
void sendToGw(char* cHeader, uint8_t* ucData, bool bSsEncode, bool bEnd)
{
    CCITTChecksumReverse ccr;
    uint8_t ucBuffer[50];
    static int iPktCount;
    static uint16_t iStreamId;
    static bool bSentHeader = false;
    static bool bBusy       = false;
    int8_t n                = 0;

    ccitt_checksum_init(&ccr);
    mcrc.m_crc16 = 0xFFFF;

    ucBuffer[0] = 'D';
    ucBuffer[1] = 'S';
    ucBuffer[2] = 'R';
    ucBuffer[3] = 'P';

    if (iPktCount == 0 && !bSentHeader)
    {
        printf("Network transmission started.\n");

        ucBuffer[4]  = bBusy ? 0x22U : 0x20U;
        iStreamId    = (rand() % 0xffff) + 1;
        ucBuffer[5]  = (iStreamId & 0xff00) >> 8;
        ucBuffer[6]  = iStreamId & 0x00ff;
        ucBuffer[7]  = 0;
        ucBuffer[8]  = 0;
        ucBuffer[9]  = 0;
        ucBuffer[10] = 0;
        memcpy(ucBuffer + 11, cHeader, 36);
        ccitt_checksum_update_bytes(&ccr, ucBuffer + 8, 39); /* 4 * 8 + 4 + 3 */
        ccitt_checksum_result_bytes(&ccr, ucBuffer + 47);
        n = sendto(sockoutfd, ucBuffer, 49, 0, (struct sockaddr*)&servaddrout, sizeof(servaddrout));
        if (n < 0)
            error((char*)"ERROR in sendto");

        if (!bEnd)
            bSentHeader = true;

        return;
    }

    ucBuffer[4] = bBusy ? 0x23U : 0x21U;

    ucBuffer[5] = (iStreamId & 0xff00) >> 8;
    ucBuffer[6] = iStreamId & 0x00ff;

    if (ucData[9] == 0x55 && ucData[10] == 0x2d && ucData[11] == 0x16)
        iPktCount = 0;

    ucBuffer[7] = iPktCount;

    if (bSsEncode)
        slowSpeedDataEncode(metaText, ucData + 9, 1);

    ucBuffer[8] = iBER;
    if (bEnd)
    {
        memcpy(ucBuffer + 9, AMBE_SILENCE, 9);
        ucBuffer[18] = 0x55;
        ucBuffer[19] = 0x55;
        ucBuffer[20] = 0x55;
        n            = sendto(sockoutfd, ucBuffer, 21, 0, (struct sockaddr*)&servaddrout, sizeof(servaddrout));
        if (n < 0)
            error((char*)"ERROR in sendto");

        iPktCount++;
        ucBuffer[7] = iPktCount;
        ucBuffer[7] |= 0x40;  // End of data marker
        ucBuffer[21] = 0x55;
        ucBuffer[22] = 0xc8;
        ucBuffer[23] = 0x7a;
        n            = sendto(sockoutfd, ucBuffer, 24, 0, (struct sockaddr*)&servaddrout, sizeof(servaddrout));
        if (n < 0)
            error((char*)"ERROR in sendto");

        iPktCount   = -1;
        bSentHeader = false;
        fprintf(stderr, "End network transmission.\n");
    }
    else
    {
        memcpy(ucBuffer + 9, ucData, 12);
        //      dump((char*)"UCHAR", ucBuffer, 21);
        sendto(sockoutfd, ucBuffer, 21, 0, (struct sockaddr*)&servaddrout, sizeof(servaddrout));
    }

    iPktCount++;
    if (iPktCount > 0x14)
        iPktCount = 0;
}

// Connect to and process incoming IRCDDB gateway bytes.
void* connectIRCDDBGateway(void* argv)
{
    //   uint16_t iStreamId;
    int sockinfd;
    int portout, portin, n;
    int hostfd;
    char local_address[20];
    char gw_address[80];
    uint8_t cRecvline[50];
    uint8_t cRxBuffer[40];
    uint8_t cTxHeader[37];
    uint8_t buffer[100];
    uint32_t timeout = 0;
    char myCall[9]   = "";
    char urCall[9]   = "";
    char rpt1Call[9] = "";
    char rpt2Call[9] = "";
    char lastCall[9] = "";
    char suffix[5]   = "";

    sscanf((char*)argv, "%d %s %s %d %d", &hostfd, local_address, gw_address, &portout, &portin);

    sockinfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockinfd < 0)
        error((char*)"ERROR opening gw input socket");

    bzero(&servaddrin, sizeof(servaddrin));
    servaddrin.sin_family      = AF_INET;
    servaddrin.sin_addr.s_addr = inet_addr(local_address);
    servaddrin.sin_port        = htons(portin);
    n                          = bind(sockinfd, (struct sockaddr*)&servaddrin, sizeof(servaddrin));

    fcntl(sockinfd, F_SETFL, fcntl(sockinfd, F_GETFL, 0) | O_NONBLOCK);

    sockoutfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockoutfd < 0)
        error((char*)"ERROR opening gw output socket");

    bzero((char*)&servaddrout, sizeof(servaddrout));
    servaddrout.sin_family      = AF_INET;
    servaddrout.sin_addr.s_addr = inet_addr(gw_address);
    servaddrout.sin_port        = htons(portout);

    fprintf(stderr, "Connected to IRCDDB gateway.\n");
    DSTARReflDisconnect = false;
    DSTARReflConnected  = false;

    memset(metaText, 0, 23);
    cRxBuffer[0] = 'D';
    cRxBuffer[1] = 'S';
    cRxBuffer[2] = 'R';
    cRxBuffer[3] = 'P';
    cRxBuffer[4] = 0x0AU;  // Poll with text

    // Include the nul at the end also
    memcpy(cRxBuffer + 5U, "linux_pirptr-20250926", 21);
    cRxBuffer[26] = 0x00;
    sendto(sockoutfd, cRxBuffer, 27, 0, (struct sockaddr*)&servaddrout, sizeof(servaddrout));

    while (connected)
    {
        bzero(cRecvline, 50);

        socklen_t len = sizeof(cliaddr);
        n             = recvfrom(sockinfd, cRecvline, 50, 0, (struct sockaddr*)&cliaddr, &len);
        if (n > 0)
        {
            if (debugM)
                dump((char*)"DSTAR gateway recv data:", (uint8_t*)cRecvline, n);
        }
        if (n > 0 && (n != 21 && n != 49 && n != 34))
            fprintf(stderr, "n = %d\n", n);

        /* Old code removed - now using state machine below */

        /* Check for timeout */
        gwStateCheckTimeout(5000); /* 5 second timeout */

        /* Handle incoming header from reflector (49 bytes, type 0x20) */
        if (n == 49 && cRecvline[4] == 0x20)
        {
            uint16_t stream_id = (cRecvline[5] << 8) | cRecvline[6];

            if (gwStateStartRx(stream_id))
            {
                //       iStreamId = stream_id;
                bzero(myCall, 9);
                bzero(suffix, 5);
                bzero(urCall, 9);
                bzero(rpt1Call, 9);
                bzero(rpt2Call, 9);
                memcpy(myCall, cRecvline + 35, 8);
                memcpy(suffix, cRecvline + 43, 4);
                memcpy(rpt1Call, cRecvline + 11, 8);
                memcpy(rpt2Call, cRecvline + 19, 8);
                memcpy(urCall, cRecvline + 27, 8);
                fprintf(stderr, "UR: %8s  MY: %8s%4s  Rpt1: %8s  Rpt2: %8s\n", urCall, myCall, suffix, rpt1Call, rpt2Call);
                if (strcasecmp(myCall, lastCall) != 0)
                {
                    sprintf((char*)cTxHeader, "%8s%8s%8s%8s%4s", rpt2Call, rpt1Call, urCall, myCall, suffix);
                    strcpy(lastCall, myCall);
                }
                buffer[0] = 0x61;
                buffer[1] = 0x00;
                buffer[2] = 0x31;
                buffer[3] = 0x04;
                memcpy(buffer + 4, TYPE_HEADER, 4);
                buffer[8]  = 0x40;
                buffer[9]  = 0x00;
                buffer[10] = 0x00;
                memcpy(buffer + 11, cTxHeader, 36);
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_addData(&txBuffer, buffer, 49);
                pthread_mutex_unlock(&txBufMutex);
            }
        }
        /* Handle incoming voice/data from reflector (21 bytes, type 0x21) */
        else if (n == 21 && cRecvline[4] == 0x21)
        {
            if (gwStateIsRxActive())
            {
                uint16_t pkt_stream_id = (cRecvline[5] << 8) | cRecvline[6];

                if (gwStateVerifyStreamId(pkt_stream_id))
                {
                    gwStateUpdateTime();
                    if ((cRecvline[7] & 0x40) == 0x40)
                    {
                        buffer[0] = 0x61;
                        buffer[1] = 0x00;
                        buffer[2] = 0x08;
                        buffer[3] = 0x04;
                        memcpy(buffer + 4, TYPE_EOT, 4);
                        pthread_mutex_lock(&txBufMutex);
                        RingBuffer_addData(&txBuffer, buffer, 8);
                        pthread_mutex_unlock(&txBufMutex);
                        fprintf(stderr, "EOT\n");
                        gwStateEnd("Reflector EOT");
                    }
                    else
                    {
                        buffer[0] = 0x61;
                        buffer[1] = 0x00;
                        buffer[2] = 0x14;
                        buffer[3] = 0x04;
                        memcpy(buffer + 4, TYPE_DATA, 4);
                        memcpy(buffer + 8, cRecvline + 9, 12);
                        pthread_mutex_lock(&txBufMutex);
                        RingBuffer_addData(&txBuffer, buffer, 20);
                        pthread_mutex_unlock(&txBufMutex);
                    }
                }
            }
            else
            {
                fprintf(stderr, "DROP: Data packet but gateway not in RX mode\n");
            }
        }
        /* Handle status messages (34 bytes) */
        else if (n == 34)
        {
            bzero(buffer, 30);
            memcpy(buffer, cRecvline + 5, 29);
            fprintf(stderr, "Status: %s\n", buffer);
            gwStateEnd("Status msg");

            if (strncasecmp((const char*)cRecvline + 5, "Not", 3) == 0)
            {
                uint8_t buf[4];
                buf[0] = 0x61;
                buf[1] = 0x00;
                buf[2] = 0x10;
                buf[3] = 0x04;
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_addData(&txBuffer, buf, 4);
                RingBuffer_addData(&txBuffer, (uint8_t*)TYPE_DISCONNECT, 4);
                replace_char(DSTARName, 7, 0, ' ');
                RingBuffer_addData(&txBuffer, (uint8_t*)DSTARName, 7);
                RingBuffer_addData(&txBuffer, (uint8_t*)DSTARModule, 1);
                pthread_mutex_unlock(&txBufMutex);
                DSTARReflConnected = false;
            }
            else if (strncasecmp((const char*)cRecvline + 5, "Linked", 6) == 0)
            {
                memcpy(DSTARName, cRecvline + 15, 7);
                memcpy(DSTARModule, cRecvline + 22, 1);
                uint8_t buf[4];
                buf[0] = 0x61;
                buf[1] = 0x00;
                buf[2] = 0x10;
                buf[3] = 0x04;
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_addData(&txBuffer, buf, 4);
                RingBuffer_addData(&txBuffer, (uint8_t*)TYPE_CONNECT, 4);
                replace_char(DSTARName, 7, 0, ' ');
                RingBuffer_addData(&txBuffer, (uint8_t*)DSTARName, 7);
                RingBuffer_addData(&txBuffer, (uint8_t*)DSTARModule, 1);
                pthread_mutex_unlock(&txBufMutex);
                DSTARReflConnected = true;
            }
        }

        if (DSTARReflDisconnect)
        {
            gwStateEnd("Reflector disconnected.");
            DSTARReflConnected = false;
        }

        if (timeout >= 1000000)
        {
            cRxBuffer[0] = 'D';
            cRxBuffer[1] = 'S';
            cRxBuffer[2] = 'R';
            cRxBuffer[3] = 'P';
            cRxBuffer[4] = 0x0AU;  // Poll with text

            // Include the nul at the end also
            memcpy(cRxBuffer + 5U, "linux_pirptr-20250926", 21);
            cRxBuffer[26] = 0x00;
            sendto(sockoutfd, cRxBuffer, 27, 0, (struct sockaddr*)&servaddrout, sizeof(servaddrout));
            timeout = 0;
        }
        else
            timeout++;

        delay(1000);
    }

    uint8_t tmp[4];
    tmp[0] = 0x61;
    tmp[1] = 0x00;
    tmp[2] = 0x10;
    tmp[3] = 0x04;
    RingBuffer_addData(&txBuffer, tmp, 4);
    RingBuffer_addData(&txBuffer, (uint8_t*)TYPE_DISCONNECT, 4);
    replace_char(DSTARName, 7, 0, ' ');
    RingBuffer_addData(&txBuffer, (uint8_t*)DSTARName, 7);
    RingBuffer_addData(&txBuffer, (uint8_t*)DSTARModule, 1);
    DSTARReflConnected = false;
    close(sockoutfd);
    close(sockinfd);
    sleep(1);
    fprintf(stderr, "Disconnected from DSTAR gateway.\n");
    int iRet = 100;
    pthread_exit(&iRet);
    return 0;
}

static int openHostSocket(const char* hostAddress)
{
    struct sockaddr_in serv_addr;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("Socket creation error");
        return -1;
    }

    int on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    /* SO_KEEPALIVE: detect silent host drop in ~11 seconds */
    int keepalive = 1, keepidle = 5, keepintvl = 2, keepcnt = 3;
    setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(clientPort);

    if (inet_pton(AF_INET, hostAddress, &serv_addr.sin_addr) <= 0)
    {
        perror("Invalid address");
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        if (errno != EINPROGRESS)
        {
            perror("Connection failed");
            close(sockfd);
            return -1;
        }
    }

    return sockfd;
}

// Create threads for gateway communication.
void connectToIRCDDB(int sockfd, const char* local_address, const char* gw_address, uint16_t portout, uint16_t portin)
{
    static char server[120] = "";

    DSTARReflDisconnect = true;
    sleep(1);

    sprintf(server, "%d %s %s %d %d", sockfd, local_address, gw_address, portout, portin);

    int err = pthread_create(&txid, NULL, &txThread, (void*)(intptr_t)sockfd);
    if (err != 0)
        fprintf(stderr, "Can't create tx thread: [%s]", strerror(err));

    err = pthread_create(&dstarReflid, NULL, &connectIRCDDBGateway, server);
    if (err != 0)
    {
        fprintf(stderr, "Can't create IRCDDB gateway thread: [%s]", strerror(err));
        return;
    }

    if (debugM)
        fprintf(stderr, "IRCDDB gateway thread created successfully\n");

    sleep(4);
    char cBuffer[40];
    uint8_t ucBytes[12];
    sprintf(cBuffer, "%8s%8s%8s%8s%4s", rpt2Call, rpt1Call, "       U", myCall, "TEST");
    memset(ucBytes, 0, 9);
    ucBytes[9]  = 0x55;
    ucBytes[10] = 0x2d;
    ucBytes[11] = 0x16;
    sendToGw(cBuffer, ucBytes, true, true);
}
/*
void connectToIRCDDB(int sockfd, const char* local_address, const char* gw_address, uint16_t portout, uint16_t portin)
{
    static char server[80] = "";

    DSTARReflDisconnect = true;
    sleep(1);

    sprintf(server, "%d %s %s %d %d", sockfd, local_address, gw_address, portout, portin);

    pthread_t txid;
    int err = pthread_create(&(txid), NULL, &txThread, (void*)(intptr_t)sockfd);
    if (err != 0)
        fprintf(stderr, "Can't create tx thread :[%s]", strerror(err));
    else
    {
        if (debugM)
            fprintf(stderr, "TX thread created successfully\n");
    }
    err = pthread_create(&(dstarReflid), NULL, &connectIRCDDBGateway, server);
    if (err != 0)
        fprintf(stderr, "Can't create IRCDDB gateway thread :[%s]", strerror(err));
    else
    {
        if (debugM)
            fprintf(stderr, "IRCDDB gateway thread created successfully\n");

        sleep(4);
        // Maker sure we are not linked to a reflector.
        char cBuffer[40];
        uint8_t ucBytes[12];
        sprintf(cBuffer, "%8s%8s%8s%8s%4s", rpt2Call, rpt1Call, "       U", myCall, "TEST");
        memset(ucBytes, 0, 9);
        ucBytes[9]  = 0x55;
        ucBytes[10] = 0x2d;
        ucBytes[11] = 0x16;
        sendToGw(cBuffer, ucBytes, true, true);
    }
}
*/

void hostReadLoop(int sockfd)
{
    char buffer[BUFFER_SIZE] = {0};
    ssize_t len              = 0;
    uint8_t offset           = 0;
    uint16_t respLen         = 0;
    uint8_t typeLen          = 0;
    char source[7];

    while (connected)
    {
        int len = read(sockfd, buffer, 1);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            error((char*)"ERROR: DSTAR host connection closed remotely.");
            break;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            delay(5);
        }

        if (len != 1)
        {
            fprintf(stderr, "DSTAR_Gateway: error when reading from DSTAR host, errno=%d\n", errno);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "DSTAR_Gateway: unknown byte from DSTAR host, 0x%02X\n", buffer[0]);
            continue;
        }

        offset = 0;
        while (offset < 3)
        {
            len = read(sockfd, buffer + 1 + offset, 3 - offset);
            if (len == 0) break;
            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                delay(5);
            else
                offset += len;
        }

        if (len == 0)
            break;

        respLen = (buffer[1] << 8) + buffer[2];

        offset += 1;
        while (offset < respLen)
        {
            len = read(sockfd, buffer + offset, respLen - offset);
            if (len == 0) break;
            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                delay(5);
            else
                offset += len;
        }

        if (len == 0)
            break;

        typeLen = buffer[3];
        uint8_t type[typeLen];
        memcpy(type, buffer + 4, typeLen);

        if (debugM)
            dump((char*)"DSTAR Service data:", (unsigned char*)buffer, respLen);

        if (memcmp(type, TYPE_COMMAND, typeLen) == 0)
        {
            if (buffer[8] == COMM_UPDATE_CONF)
            {
                readHostConfig(modemName, "DSTAR", "rpt1Call", rpt1Call);
                readHostConfig(modemName, "DSTAR", "rpt2Call", rpt2Call);
                readHostConfig(modemName, "main", "callsign", myCall);
                uint8_t buf[4];
                buf[0] = 0x61;
                buf[1] = 0x00;
                buf[2] = 0x08;
                buf[3] = 0x04;
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_addData(&txBuffer, buf, 4);
                RingBuffer_addData(&txBuffer, (uint8_t*)TYPE_COMMAND, 4);
                pthread_mutex_unlock(&txBufMutex);
            }
        }
        else if (memcmp(type, TYPE_CONNECT, typeLen) == 0)
        {
            if (!DSTARReflConnected)
            {
                bzero(source, 7);
                bzero(DSTARName, 8);
                bzero(DSTARModule, 2);
                memcpy(source, buffer + 8, 6);
                memcpy(DSTARName, buffer + 14, 7);
                memcpy(DSTARModule, buffer + 21, 1);
                fprintf(stderr, "Name: %s  Mod: %s\n", DSTARName, DSTARModule);

                char cBuffer[40];
                uint8_t ucBytes[12];
                sprintf(cBuffer, "%8s%8s%8s%8s%4s", rpt2Call, rpt1Call, "        ", source, "TEST");
                memcpy(cBuffer + 16, DSTARName, 6);
                cBuffer[22] = DSTARModule[0];
                cBuffer[23] = 'L';
                memset(ucBytes, 0, 9);
                ucBytes[9]  = 0x55;
                ucBytes[10] = 0x2d;
                ucBytes[11] = 0x16;
                sendToGw(cBuffer, ucBytes, true, true);
                /*
                 *                                uint8_t buf[4];
                 *                                buf[0] = 0x61;
                 *                                buf[1] = 0x00;
                 *                                buf[2] = 0x10;
                 *                                buf[3] = 0x04;
                 *                                RingBuffer_addData(&txBuffer, buf, 4);
                 *                                RingBuffer_addData(&txBuffer, (uint8_t*)TYPE_CONNECT, 4);
                 *                                replace_char(DSTARName, 7, 0, ' ');
                 *                                RingBuffer_addData(&txBuffer, (uint8_t*)DSTARName, 7);
                 *                                RingBuffer_addData(&txBuffer, (uint8_t*)DSTARModule, 1);
                 *                                DSTARReflConnected = true; */
            }
        }
        else if (memcmp(type, TYPE_DISCONNECT, typeLen) == 0)
        {
            if (DSTARReflConnected)
            {
                char cBuffer[40];
                uint8_t ucBytes[12];
                sprintf(cBuffer, "%8s%8s%8s%8s%4s", rpt2Call, rpt1Call, "       U", source, "TEST");
                memset(ucBytes, 0, 9);
                ucBytes[9]  = 0x55;
                ucBytes[10] = 0x2d;
                ucBytes[11] = 0x16;
                sendToGw(cBuffer, ucBytes, true, true);
                /*
                 *                                uint8_t buf[4];
                 *                                buf[0] = 0x61;
                 *                                buf[1] = 0x00;
                 *                                buf[2] = 0x10;
                 *                                buf[3] = 0x04;
                 *                                RingBuffer_addData(&txBuffer, buf, 4);
                 *                                RingBuffer_addData(&txBuffer, (uint8_t*)TYPE_DISCONNECT, 4);
                 *                                replace_char(DSTARName, 7, 0, ' ');
                 *                                RingBuffer_addData(&txBuffer, (uint8_t*)DSTARName, 7);
                 *                                RingBuffer_addData(&txBuffer, (uint8_t*)DSTARModule, 1); */
            }
            //  DSTARReflDisconnect = true;
        }
        else if (memcmp(type, TYPE_STATUS, typeLen) == 0)
        {
        }
        else if (memcmp(type, TYPE_HEADER, typeLen) == 0)
        {
            if (gwStateStartTx())
            {
                char cBuffer[37];
                uint8_t ucBytes[12];
                memcpy(rpt2Call, buffer + 11, 8);
                memcpy(rpt1Call, buffer + 19, 8);
                memcpy(urCall, buffer + 27, 8);
                memcpy(myCall, buffer + 35, 8);
                memcpy(suffix, buffer + 43, 4);
                sprintf(cBuffer, "%8s%8s%8s%8s%4s", rpt2Call, rpt1Call, urCall, myCall, suffix);
                fprintf(stderr, "Header: %s\n", cBuffer);
                memcpy(ucBytes, AMBE_SILENCE, 9);
                ucBytes[9]  = 0x55;
                ucBytes[10] = 0x2d;
                ucBytes[11] = 0x16;
                sendToGw(cBuffer, ucBytes, false, false);
            }
        }
        else if (memcmp(type, TYPE_DATA, typeLen) == 0)
        {
            if (gwStateIsTxActive())
            {
                gwStateUpdateTime();
                char cBuffer[37];
                sprintf(cBuffer, "%8s%8s%8s%8s%4s", rpt2Call, rpt1Call, urCall, myCall, suffix);
                //     dump((char*)"ucData", (uint8_t*)(buffer + 8), 12);
                if (buffer[17] == 0x55 && buffer[18] == 0x55 && buffer[19] == 0x55)
                    sendToGw(cBuffer, (uint8_t*)(buffer + 8), false, true);
                else
                    sendToGw(cBuffer, (uint8_t*)(buffer + 8), false, false);
            }
        }
        else if (memcmp(type, TYPE_EOT, typeLen) == 0)
        {
            if (gwStateIsTxActive())
                gwStateEnd("Host EOT");
        }

        delay(500);
    }
}

// Start up connection to DSTAR service.
void* startClient(void* arg)
{
    char hostAddress[80];
    strcpy(hostAddress, (char*)arg);

    while (1)
    {
        fprintf(stderr, "Connecting to DSTAR host...\n");

        int sockfd = openHostSocket(hostAddress);
        if (sockfd < 0)
        {
            fprintf(stderr, "Host connect failed, retrying in 10s\n");
            sleep(10);
            continue;
        }

        sleep(1);
        fprintf(stderr, "Connected to DSTAR host.\n");

        connected = true;
        connectToIRCDDB(sockfd, "0.0.0.0", gwAddress, gwPortOut, gwPortIn);

        /* Run the read loop — exits when host drops or EOF */
        hostReadLoop(sockfd); /* existing while(connected){...} body */

        /* Host dropped — wait for child threads to finish cleanly */
        fprintf(stderr, "Host disconnected. Waiting for threads to exit...\n");
        connected = false;
        close(sockfd);

        pthread_join(txid, NULL);
        pthread_join(dstarReflid, NULL);

        /* Flush stale buffer data before reconnecting */
        pthread_mutex_lock(&txBufMutex);
        RingBuffer_clear(&txBuffer);
        pthread_mutex_unlock(&txBufMutex);

        gwStateEnd("Host reconnect");

        fprintf(stderr, "Reconnecting in 5 seconds...\n");
        sleep(5);
    }

    int iRet = 100;
    pthread_exit(&iRet);
    return 0;
}
#ifdef OLD
void* startClient(void* arg)
{
    int sockfd = 0;
    struct sockaddr_in serv_addr;
    char buffer[BUFFER_SIZE] = {0};
    char hostAddress[80];
    strcpy(hostAddress, (char*)arg);
    // Create socket file descriptor
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("Socket creation error");
        exit(EXIT_FAILURE);
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1)
    {
        perror("fcntl F_GETFL");
        exit(EXIT_FAILURE);
    }

    int on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    // Prepare the sockaddr_in structure
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(clientPort);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, hostAddress, &serv_addr.sin_addr) <= 0)
    {  // Connect to localhost
        perror("Invalid address/ Address not supported");
        exit(EXIT_FAILURE);
    }

    // Connect to the server
    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        if (errno != EINPROGRESS)
        {
            perror("connection failed");
            exit(EXIT_FAILURE);
        }
    }

    sleep(1);
    printf("Connected to DSTAR host.\n");

    //    connectToIRCDDB(sockfd, "127.0.0.1", "127.0.0.1", 20010, 20011);
    //    connectToIRCDDB(sockfd, "0.0.0.0", "192.168.0.235", 20010, 20012);
    connectToIRCDDB(sockfd, "0.0.0.0", gwAddress, gwPortOut, gwPortIn);

    ssize_t len      = 0;
    uint8_t offset   = 0;
    uint16_t respLen = 0;
    uint8_t typeLen  = 0;
    char source[7];

    while (connected)
    {
        int len = read(sockfd, buffer, 1);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            error((char*)"ERROR: DSTAR host connection closed remotely.");
            break;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            delay(5);
        }

        if (len != 1)
        {
            fprintf(stderr, "DSTAR_Gateway: error when reading from DSTAR host, errno=%d\n", errno);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "DSTAR_Gateway: unknown byte from DSTAR host, 0x%02X\n", buffer[0]);
            continue;
        }

        offset = 0;
        while (offset < 3)
        {
            len = read(sockfd, buffer + 1 + offset, 3 - offset);
            if (len == 0) break;
            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                delay(5);
            else
                offset += len;
        }

        if (len == 0)
            break;

        respLen = (buffer[1] << 8) + buffer[2];

        offset += 1;
        while (offset < respLen)
        {
            len = read(sockfd, buffer + offset, respLen - offset);
            if (len == 0) break;
            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                delay(5);
            else
                offset += len;
        }

        if (len == 0)
            break;

        typeLen = buffer[3];
        uint8_t type[typeLen];
        memcpy(type, buffer + 4, typeLen);

        if (debugM)
            dump((char*)"DSTAR Service data:", (unsigned char*)buffer, respLen);

        if (memcmp(type, TYPE_COMMAND, typeLen) == 0)
        {
            if (buffer[8] == COMM_UPDATE_CONF)
            {
                readHostConfig(modemName, "DSTAR", "rpt1Call", rpt1Call);
                readHostConfig(modemName, "DSTAR", "rpt2Call", rpt2Call);
                readHostConfig(modemName, "main", "callsign", myCall);
                uint8_t buf[4];
                buf[0] = 0x61;
                buf[1] = 0x00;
                buf[2] = 0x08;
                buf[3] = 0x04;
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_addData(&txBuffer, buf, 4);
                RingBuffer_addData(&txBuffer, (uint8_t*)TYPE_COMMAND, 4);
                pthread_mutex_unlock(&txBufMutex);
            }
        }
        else if (memcmp(type, TYPE_CONNECT, typeLen) == 0)
        {
            if (!DSTARReflConnected)
            {
                bzero(source, 7);
                bzero(DSTARName, 8);
                bzero(DSTARModule, 2);
                memcpy(source, buffer + 8, 6);
                memcpy(DSTARName, buffer + 14, 7);
                memcpy(DSTARModule, buffer + 21, 1);
                fprintf(stderr, "Name: %s  Mod: %s\n", DSTARName, DSTARModule);

                char cBuffer[40];
                uint8_t ucBytes[12];
                sprintf(cBuffer, "%8s%8s%8s%8s%4s", rpt2Call, rpt1Call, "        ", source, "TEST");
                memcpy(cBuffer + 16, DSTARName, 6);
                cBuffer[22] = DSTARModule[0];
                cBuffer[23] = 'L';
                memset(ucBytes, 0, 9);
                ucBytes[9]  = 0x55;
                ucBytes[10] = 0x2d;
                ucBytes[11] = 0x16;
                sendToGw(cBuffer, ucBytes, true, true);
                /*
                                uint8_t buf[4];
                                buf[0] = 0x61;
                                buf[1] = 0x00;
                                buf[2] = 0x10;
                                buf[3] = 0x04;
                                RingBuffer_addData(&txBuffer, buf, 4);
                                RingBuffer_addData(&txBuffer, (uint8_t*)TYPE_CONNECT, 4);
                                replace_char(DSTARName, 7, 0, ' ');
                                RingBuffer_addData(&txBuffer, (uint8_t*)DSTARName, 7);
                                RingBuffer_addData(&txBuffer, (uint8_t*)DSTARModule, 1);
                                DSTARReflConnected = true; */
            }
        }
        else if (memcmp(type, TYPE_DISCONNECT, typeLen) == 0)
        {
            if (DSTARReflConnected)
            {
                char cBuffer[40];
                uint8_t ucBytes[12];
                sprintf(cBuffer, "%8s%8s%8s%8s%4s", rpt2Call, rpt1Call, "       U", source, "TEST");
                memset(ucBytes, 0, 9);
                ucBytes[9]  = 0x55;
                ucBytes[10] = 0x2d;
                ucBytes[11] = 0x16;
                sendToGw(cBuffer, ucBytes, true, true);
                /*
                                uint8_t buf[4];
                                buf[0] = 0x61;
                                buf[1] = 0x00;
                                buf[2] = 0x10;
                                buf[3] = 0x04;
                                RingBuffer_addData(&txBuffer, buf, 4);
                                RingBuffer_addData(&txBuffer, (uint8_t*)TYPE_DISCONNECT, 4);
                                replace_char(DSTARName, 7, 0, ' ');
                                RingBuffer_addData(&txBuffer, (uint8_t*)DSTARName, 7);
                                RingBuffer_addData(&txBuffer, (uint8_t*)DSTARModule, 1); */
            }
            //  DSTARReflDisconnect = true;
        }
        else if (memcmp(type, TYPE_STATUS, typeLen) == 0)
        {
        }
        else if (memcmp(type, TYPE_HEADER, typeLen) == 0)
        {
            if (gwStateStartTx())
            {
                char cBuffer[37];
                uint8_t ucBytes[12];
                memcpy(rpt2Call, buffer + 11, 8);
                memcpy(rpt1Call, buffer + 19, 8);
                memcpy(urCall, buffer + 27, 8);
                memcpy(myCall, buffer + 35, 8);
                memcpy(suffix, buffer + 43, 4);
                sprintf(cBuffer, "%8s%8s%8s%8s%4s", rpt2Call, rpt1Call, urCall, myCall, suffix);
                fprintf(stderr, "Header: %s\n", cBuffer);
                memcpy(ucBytes, AMBE_SILENCE, 9);
                ucBytes[9]  = 0x55;
                ucBytes[10] = 0x2d;
                ucBytes[11] = 0x16;
                sendToGw(cBuffer, ucBytes, false, false);
            }
        }
        else if (memcmp(type, TYPE_DATA, typeLen) == 0)
        {
            if (gwStateIsTxActive())
            {
                gwStateUpdateTime();
                char cBuffer[37];
                sprintf(cBuffer, "%8s%8s%8s%8s%4s", rpt2Call, rpt1Call, urCall, myCall, suffix);
                //     dump((char*)"ucData", (uint8_t*)(buffer + 8), 12);
                if (buffer[17] == 0x55 && buffer[18] == 0x55 && buffer[19] == 0x55)
                    sendToGw(cBuffer, (uint8_t*)(buffer + 8), false, true);
                else
                    sendToGw(cBuffer, (uint8_t*)(buffer + 8), false, false);
            }
        }
        else if (memcmp(type, TYPE_EOT, typeLen) == 0)
        {
            if (gwStateIsTxActive())
                gwStateEnd("Host EOT");
        }

        delay(500);
    }
    fprintf(stderr, "Client thread exited.\n");
    close(sockfd);
    int iRet  = 100;
    connected = false;
    pthread_exit(&iRet);
    return 0;
}
#endif
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
                clientPort = 18100 + modemId - 1;
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
                fprintf(stdout, "DSTAR_Gateway: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: DSTAR_Gateway [-m modem_number (1-10)] [-d] [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            fprintf(stderr, "DSTAR_Gateway: error in fork(), exiting\n");
            return 1;
        }

        // If this is the parent, exit
        if (pid > 0)
            return 0;

        // We are the child from here onwards
        setsid();

        umask(0);
    }

    /* Initialize RingBuffers */
    //   RingBuffer_Init(&reflTxBuffer, 800);
    RingBuffer_Init(&txBuffer, 800);

    readHostConfig(modemName, "DSTAR", "rpt1Call", rpt1Call);
    readHostConfig(modemName, "DSTAR", "rpt2Call", rpt2Call);
    readHostConfig(modemName, "DSTAR", "callsign", myCall);
    char tmp[15];
    readHostConfig(modemName, "DSTAR", "gwPortOut", tmp);
    gwPortOut = atol(tmp);
    readHostConfig(modemName, "DSTAR", "gwPortIn", tmp);
    gwPortIn = atol(tmp);
    readHostConfig(modemName, "DSTAR", "gwAddress", gwAddress);

    int err = pthread_create(&(clientid), NULL, &startClient, DSTARHost);
    if (err != 0)
    {
        fprintf(stderr, "Can't create DSTAR host thread :[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM)
            fprintf(stderr, "DSTAR host thread created successfully\n");
    }

    while (1)
    {
        delay(50000);
        if (!connected)
            break;
    }

    /* Cleanup RingBuffers */
    //    RingBuffer_Destroy(&reflTxBuffer);
    RingBuffer_Destroy(&txBuffer);

    fprintf(stderr, "DSTAR Gateway terminated.\n");
    logError(modemName, "main", "DSTAR Gateway terminated.");
    return 0;
}
