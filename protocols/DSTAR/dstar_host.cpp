/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS             *
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <string.h>
#include <cstdint>
#include <type_traits>
#include <vector>
#include <fcntl.h>
#include <pthread.h>
#include <iostream>
#include <sstream>
#include <signal.h>
#include <time.h>
#include <math.h>

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/param.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "../../tools/tools.h"
#include "../../tools/RingBuffer.h"
#include "../../tools/CCITTChecksumReverse.h"

using namespace std;

#define VERSION     "2025-09-13"
#define BUFFER_SIZE 1024

static int16_t TX_GAUSSIAN_0_35_FILTER[] = {0, 0, 0, 0, 1001, 3514, 9333, 18751, 28499, 32767, 28499, 18751, 9333, 3514, 1001}; // numTaps = 15, L = 5
uint8_t        TX_GAUSSIAN_0_35_FILTER_PHASE_LEN = 3; // phaseLength = numTaps/L
uint8_t        TX_FILTER_STATE_LEN = 20;
uint8_t        RX_FILTER_STATE_LEN = 40;
static int16_t RX_GAUSSIAN_0_5_FILTER[] = {8, 104, 760, 3158, 7421, 9866, 7421, 3158, 760, 104, 8, 0};
const uint8_t  RX_GAUSSIAN_0_5_FILTER_LEN  = 12;
const uint8_t  TX_GAUSSIAN_0_35_FILTER_LEN = 15;
const uint8_t  TX_SYMBOL_LENGTH = 5;
char           MODE_NAME[11]        = "DSTAR";
char           MODEM_TYPE[6]        = "GMSK";
bool           USE_DC_FILTER        = true;
bool           USE_LP_FILTER        = false;

const char *TYPE_HEADER         = "DSTH";
const char *TYPE_DATA           = "DSTD";
const char *TYPE_EOT            = "DSTE";
const char *TYPE_ACK            = "ACK ";
const char *TYPE_NACK           = "NACK";
const char *TYPE_DISCONNECT     = "DISC";
const char *TYPE_CONNECT        = "CONN";
const char *TYPE_STATUS         = "STAT";
const char *TYPE_MODE           = "MODE";
const char *TYPE_COMMAND        = "COMM";

const uint8_t COMM_SET_DUPLEX   = 0x00;
const uint8_t COMM_SET_SIMPLEX  = 0x01;
const uint8_t COMM_SET_MODE     = 0x02;
const uint8_t COMM_SET_IDLE     = 0x03;

int      sockfd           = 0;
char     modemHost[80]    = "127.0.0.1";
char     myCall[9]        = "";
char     urCall[9]        = "";
char     rpt1Call[9]      = "";
char     rpt2Call[9]      = "";
char     suffix[5]        = "";
char     metaText[23]     = "";
bool     txOn             = false;
bool     slowSpeedUpdate  = false;
bool     validFrame       = false;
bool     debugM           = false;
bool     connected        = true;
bool     modem_duplex     = true;
bool     dstarReflConnected = false;
bool     dstarGWConnected   = false;
bool     statusTimeout    = false;
bool     reflBusy         = false;
uint8_t  duration         = 0;
uint8_t  dstar_space      = 0;
uint16_t serverPort       = 18101;
uint16_t clientPort       = 18000;
uint16_t streamId         = 0;
uint16_t modeHang         = 30000;
time_t   start_time;

std::string sGps     = "";
std::string sGPSCall = "";
float       fLat     = 0.0f;
float       fLong    = 0.0f;
uint16_t    altitude = 0;

unsigned int       clientlen;  //< byte size of client's address
char              *hostaddrp;  //< dotted decimal host addr string
int                optval;     //< flag value for setsockopt
struct sockaddr_in serveraddr; //< server's addr
struct sockaddr_in clientaddr; //< client addr

CRingBuffer<uint8_t> txBuffer(3600);
CRingBuffer<uint8_t> rxBuffer(3600);
CRingBuffer<uint8_t> reflTxBuffer(3600);
CRingBuffer<uint8_t> reflCommand(200);
CRingBuffer<uint8_t> echoBuffer(20000);
uint8_t header[49]; // store header for echo function

pthread_t modemHostid;
pthread_t gwHostid;
pthread_t timerid;

uint8_t setMode[] = {0x61, 0x00, 0x05, 0x01, COMM_SET_MODE};
uint8_t setIdle[] = {0x61, 0x00, 0x05, 0x01, COMM_SET_IDLE};

typedef union crc {
    uint16_t   m_crc16;
    uint8_t    m_crc8[2];
} m_crc;
m_crc mcrc;

// error - wrapper for perror
void error(char *msg)
{
  perror(msg);
  exit(1);
}

// Wait for 'delay' miroseconds
void delay(uint32_t delay)
{
    struct timespec req, rem;
    req.tv_sec = 0;
    req.tv_nsec = delay * 1000;
    nanosleep(&req, &rem);
};

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
//    fputs(text, stdout);
//    fputc('\n', stdout);

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

void* timerThread(void *arg)
{
    uint32_t loop[3] = {0, 0, 0};
    bool     idle = true;

    while (connected)
    {
        delay(1000);

        if (loop[0] >= 2000)
        {
            statusTimeout = true;
            loop[0] = 0;
        }

        if (loop[1] >= 30000000)
        {
            reflBusy = false;
            loop[1] = 0;
        }

        if (!statusTimeout)
            loop[0]++;

        if (reflBusy)
            loop[1]++;

        if (txOn)
        {
            idle = false;
            loop[2] = 0;
        }
        else
        {
            if (!idle)
            {
                if (loop[2] >= modeHang)
                {
                    write(sockfd, setIdle, 5);
                    idle = true;
                }
                else
                    loop[2]++;
            }
        }
    }
    if (debugM)
        fprintf(stderr, "Timer thread exited.\n");
    int iRet = 600;
    pthread_exit(&iRet);
    return NULL;
}

void recordEcho(const uint8_t* data)
{
    for (uint8_t x=0;x<12;x++)
        echoBuffer.put(data[x]);
}

void echoPlayback(int sockfd)
{
    uint8_t buf[20] = {0x61, 0x00, 0x14, 0x04, 'D', 'S', 'T', 'D'};

    if (write(sockfd, header, 49) < 0)
    {
        fprintf(stderr, "ERROR: host disconnect\n");
    }
    delay(20000);
    while (echoBuffer.getData() >= 12)
    {
        for (uint8_t i=0;i<12;i++)
            echoBuffer.get(buf[8+i]);
        if (write(sockfd, buf, 20) < 0)
        {
            fprintf(stderr, "ERROR: host disconnect\n");
        }
        delay(19000);
    }
    buf[2] = 0x08;
    memcpy(buf+4, TYPE_EOT, 4);
    if (write(sockfd, buf, 8) < 0)
    {
        fprintf(stderr, "ERROR: host disconnect\n");
    }
}

bool decodeGPS(unsigned char c)
{
    static char      tgps[200];
    static short int gpsidx;

    if (gpsidx == 0)
        memset(tgps, 0, 200);

    if (c != 0x66)
    {
        tgps[gpsidx] = c;
        if (gpsidx > 0 || c == '$')
            gpsidx++;

        if ((c == '\n' || c == '\r' || gpsidx >= 200) && (gpsidx > 14))
        {
            if (tgps[0] == '$' && tgps[1] == '$' && tgps[2] == 'C')
            {
                CCCITTChecksumReverse *cc = new CCCITTChecksumReverse();
                cc->update((const unsigned char*)tgps+10, gpsidx-10);
                unsigned char calccrc[2];
                cc->result(calccrc);
                delete cc;
                char sum[5];
                sprintf(sum, "%02X%02X", calccrc[1], calccrc[0]);
                if (memcmp(sum, tgps+5, 4) == 0)
                {
                    sGps.clear();
                    for (int i = 0; i < gpsidx; i++)
                    {
                        if (tgps[i] != '\n' && tgps[i] != '\r')
                        {
                            sGps += tgps[i];
                        }
                    }

                    if (debugM)
                        fprintf(stderr, "GPS:%s\n", sGps.c_str());

                    std::vector<std::string> fields = splitString(sGps, ',');
                    sGPSCall = splitString(fields[1], '>').at(0);
                    if (sGps.find("DSTAR*:/") != std::string::npos)
                    {
                        std::string sTmp;
                        if (splitString(sGps, '/')[1].find("z") != std::string::npos)
                            sTmp = "z";
                        else
                            sTmp = "h";
                        float deg=0.0;
                        fields = splitString(sGps, sTmp.c_str()[0]);
                        std::vector<std::string> lat = splitString(fields[1], '.');
                        float fract = modff(atoi(lat[0].c_str())/100.0, &deg)*100.0;
                        sTmp = "  ";
                        sTmp[0] = lat[1].c_str()[0];
                        sTmp[1] = lat[1].c_str()[1];
                        float min = fract + atoi(sTmp.c_str())/100.0;
                        if (lat[1].c_str()[2] == 'N')
                            fLat = (deg*1.0) + (min/60.0);
                        else
                            fLat = ((deg*1.0) + (min/60.0))*-1.0;

                        fields = splitString(sGps, '/');
                        std::vector<std::string> lon = splitString(fields[2], '.');
                        fract = modff(atoi(lon[0].c_str())/100.0, &deg)*100.0;
                        sTmp[0] = lon[1].c_str()[0];
                        sTmp[1] = lon[1].c_str()[1];
                        min = fract + atoi(sTmp.c_str())/100.0;
                        if (lon[1].c_str()[2] == 'W')
                            fLong = ((deg*1.0) + (min/60.0))*-1.0;
                        else
                            fLong = ((deg*1.0) + (min/60.0));
                        fields = splitString(lon[1], 'A');
                        altitude = atoi(fields[1].c_str());
                    }
                    else
                        if (sGps.find("DSTAR*:!") != std::string::npos)
                        {
                            std::string sTmp;
                            float deg=0.0;
                            std::vector<std::string> fields = splitString(sGps, '!');
                            std::vector<std::string> lat = splitString(fields[1], '.');
                            float fract = modff(atoi(lat[0].c_str())/100.0, &deg)*100.0;
                            sTmp = "  ";
                            sTmp[0] = lat[1].c_str()[0];
                            sTmp[1] = lat[1].c_str()[1];
                            float min = fract + atoi(sTmp.c_str())/100.0;
                            if (lat[1].c_str()[2] == 'N')
                                fLat = (deg*1.0) + (min/60.0);
                            else
                                fLat = ((deg*1.0) + (min/60.0))*-1.0;

                            fields = splitString(sGps, '/');
                            std::vector<std::string> lon = splitString(fields[2], '.');
                            fract = modff(atoi(lon[0].c_str())/100.0, &deg)*100.0;
                            sTmp[0] = lon[1].c_str()[0];
                            sTmp[1] = lon[1].c_str()[1];
                            min = fract + atoi(sTmp.c_str())/100.0;
                            if (lon[1].c_str()[2] == 'W')
                                fLong = ((deg*1.0) + (min/60.0))*-1.0;
                            else
                                fLong = ((deg*1.0) + (min/60.0));
                            fields = splitString(lon[1], 'A');
                            altitude = atoi(fields[1].c_str());
                        }
                        else
                            if (sGps.find("DSTAR*:;") != std::string::npos && splitString(sGps, 'z')[1].find("I") != std::string::npos)
                            {
                                std::string sTmp;
                                float deg=0.0;
                                std::vector<std::string> fields = splitString(sGps, 'z');
                                std::vector<std::string> lat = splitString(fields[1], '.');
                                float fract = modff(atoi(lat[0].c_str())/100.0, &deg)*100.0;
                                sTmp = "  ";
                                sTmp[0] = lat[1].c_str()[0];
                                sTmp[1] = lat[1].c_str()[1];
                                float min = fract + atoi(sTmp.c_str())/100.0;
                                if (lat[1].c_str()[2] == 'N')
                                    fLat = (deg*1.0) + (min/60.0);
                                else
                                    fLat = ((deg*1.0) + (min/60.0))*-1.0;

                                fields = splitString(sGps, 'I');
                                std::vector<std::string> lon = splitString(fields[2], '.');
                                fract = modff(atoi(lon[0].c_str())/100.0, &deg)*100.0;
                                sTmp[0] = lon[1].c_str()[0];
                                sTmp[1] = lon[1].c_str()[1];
                                min = fract + atoi(sTmp.c_str())/100.0;
                                if (lon[1].c_str()[2] == 'W')
                                    fLong = ((deg*1.0) + (min/60.0))*-1.0;
                                else
                                    fLong = ((deg*1.0) + (min/60.0));
                                fields = splitString(lon[1], 'A');
                                altitude = atoi(fields[1].c_str());
                            }
                    gpsidx = 0;
                    return true;
                }
                sGps.clear();
                gpsidx = 0;
                return false;
            }
        }
        if (gpsidx >= 200)
            gpsidx = 0;
        return false;
    }
    return false;
} // end decodeGPS


int slowSpeedDataDecode(unsigned char a, unsigned char b, unsigned char c)
{
  static bool          bSyncFound;
  static bool          bFirstSection;
  static bool          bHeaderActive;
  static int           iSection;
  static unsigned char ucType;
  int                  iRet = 0;
  static char          cText[30];

  // Unscramble
  a ^= 0x70;
  b ^= 0x4f;
  c ^= 0x93;

  if (a == 0x25 && b == 0x62 && c == 0x85)
  {
    bSyncFound = true;
    bFirstSection = true;
    memset(cText, 0x20, 29);
  }

  if (bFirstSection)
  {
    ucType = (unsigned char)(a & 0xf0);

    // DV Header start
    if (bSyncFound == true && ucType == 0x50)
    {
      bSyncFound = false;
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
            metaText[20] = 0;
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

void* startClient(void *arg)
{
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
    serv_addr.sin_port = htons(clientPort);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, hostAddress, &serv_addr.sin_addr) <= 0)
    { // Connect to localhost
        perror("Invalid address/ Address not supported");
        exit(EXIT_FAILURE);
    }

    // Connect to the server
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        if (errno != EINPROGRESS)
        {
            perror("connection failed");
            exit(EXIT_FAILURE);
        }
    }

    sleep(1);
    fprintf(stderr, "Connected to host.\n");

    char     gps[50] = "";
    ssize_t  len = 0;
    uint8_t  offset = 0;
    uint16_t respLen = 0;
    uint8_t  typeLen = 0;
    uint8_t  configLen = 4 + 4 + 11 + 6 + 1 + 1 + 1 + 1 + 12 + 1 + 1 + 1 +1 + 15;

    txOn = false;

    buffer[0] = 0x61;
    buffer[1] = 0x00;
    buffer[2] = configLen;
    buffer[3] = 0x04;
    memcpy(buffer+4, TYPE_MODE, 4);
    memcpy(buffer+8, MODE_NAME, 11);
    memcpy(buffer+19, MODEM_TYPE, 6);
    buffer[25] = USE_DC_FILTER;
    buffer[26] = USE_LP_FILTER;
    buffer[27] = RX_FILTER_STATE_LEN;
    buffer[28] = RX_GAUSSIAN_0_5_FILTER_LEN;
    memcpy(buffer+29, RX_GAUSSIAN_0_5_FILTER, RX_GAUSSIAN_0_5_FILTER_LEN);
    buffer[41] = TX_SYMBOL_LENGTH;
    buffer[42] = TX_FILTER_STATE_LEN;
    buffer[43] = TX_GAUSSIAN_0_35_FILTER_PHASE_LEN;
    buffer[44] = TX_GAUSSIAN_0_35_FILTER_LEN;
    memcpy(buffer+45, TX_GAUSSIAN_0_35_FILTER, TX_GAUSSIAN_0_35_FILTER_LEN);

    write(sockfd, buffer, configLen);
    sleep(1);

    while (connected)
    {
        // Read data from server
        len = read(sockfd, buffer, 1);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            error((char*)"ERROR: connection to host lost.");
            break;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            delay(5);
        }

        if (len != 1)
        {
            fprintf(stderr, "DSTAR_Service: error when reading from server, errno=%d\n", errno);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "DSTAR_Service: unknown byte from server, 0x%02X\n", buffer[0]);
            continue;;
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

        if (len == 0) break;

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

        if (len == 0) break;

        typeLen = buffer[3];
        uint8_t type[typeLen];
        memcpy(type, buffer+4, typeLen);

        if (debugM)
            dump((char*)"DSTAR Frame data:", (unsigned char*)buffer, respLen);

        if (memcmp(type, TYPE_COMMAND, typeLen) == 0)
        {
            if (buffer[8] == COMM_SET_DUPLEX)
                modem_duplex = true;
            else if (buffer[8] == COMM_SET_SIMPLEX)
                modem_duplex = false;
        }
        else if (memcmp(type, TYPE_HEADER, typeLen) == 0)
        {
            validFrame = true;
            if (!txOn)
                write(sockfd, setMode, 5);
            txOn = true;
            if (validFrame && modem_duplex)
            {
                if (write(sockfd, buffer, respLen) < 0)
                {
                    fprintf(stderr, "ERROR: host disconnect\n");
                    break;
                }
            }

            bzero(myCall, 9);
            bzero(urCall, 9);
            bzero(suffix, 5);
            bzero(rpt1Call, 9);
            bzero(rpt2Call, 9);
            memcpy(rpt1Call, buffer+11, 8);
            memcpy(rpt2Call, buffer+19, 8);
            memcpy(urCall, buffer+27, 8);
            memcpy(myCall, buffer+35, 8);
            memcpy(suffix, buffer+33, 4);

            if (urCall[7] == 'E')
                memcpy(header, buffer, respLen);
            start_time = time(NULL);
            saveLastCall("DSTAR", "RF", myCall, urCall, metaText, NULL, gps, true);
            fprintf(stderr, "DSTAR Header: %s  %s  %s  %s-%s\n", rpt1Call, rpt2Call, urCall, myCall, suffix);
        }
        else if (memcmp(type, TYPE_DATA, typeLen) == 0)
        {
            txOn = true;
            if (validFrame && modem_duplex)
            {
                if (write(sockfd, buffer, respLen) < 0)
                {
                    fprintf(stderr, "ERROR: host disconnect\n");
                    break;
                }
            }
            slowSpeedDataDecode(buffer[17], buffer[18], buffer[19]);
            if (urCall[7] == 'E')
                recordEcho((uint8_t*)&buffer[8]);
        }
        else if (memcmp(type, TYPE_EOT, typeLen) == 0)
        {
            if (dstarReflConnected && reflTxBuffer.getSpace() >= respLen)
            {
                for (uint8_t x=0;x<respLen;x++)
                    reflTxBuffer.put(buffer[x]);
            }

            if (debugM)
                fprintf(stderr, "Found DSTAR EOT\n");
            if (validFrame)
            {
                gps[0] = 0;
                if (fLat != 0.0f && fLong != 0.0)
                {
                    fprintf(stderr, "Lat: %f  Long: %f  Alt: %d\n", fLat, fLong, altitude);
                    sprintf(gps, "%f %f %d 0 0", fLat, fLong, altitude);
                }
                float loss_BER = 0.0f; // (float)decoder.bitErr / 3.68F;
                duration = difftime(time(NULL), start_time);
                saveLastCall("DSTAR", "RF", myCall, urCall, metaText, NULL, gps, false);
                saveHistory("DSTAR", "RF", myCall , urCall, loss_BER, metaText, duration);
            }
            if (validFrame && modem_duplex)
            {
                if (write(sockfd, buffer, respLen) < 0)
                {
                    fprintf(stderr, "ERROR: host disconnect\n");
                    break;
                }
            }
            if (urCall[7] == 'E')
            {
                sleep(2);
                if (echoBuffer.getData() >= 12)
                    echoPlayback(sockfd);
            }
            bzero(header, 49);
            bzero(urCall, 8);
            bzero(metaText, 23);
            validFrame = false;
            txOn = false;
        }
    }
    fprintf(stderr, "Disconnected from host.\n");
    // Close socket
    close(sockfd);
    connected = false;
    int iRet = 100;
    pthread_exit(&iRet);
    return 0;
}

void* txThread(void *arg)
{
    int  sockfd = (intptr_t)arg;
    uint16_t loop = 0;

    while (connected)
    {
        delay(100);
        loop++;

        if (loop > 100)
        {
            if (reflTxBuffer.getData() >= 5)
            {
                if (reflTxBuffer.peek() != 0x61)
                {
                    fprintf(stderr, "TX invalid header.\n");
                    continue;
                }
                uint8_t  byte[2];
                uint16_t len = 0;
                reflTxBuffer.npeek(byte[0], 1);
                reflTxBuffer.npeek(byte[1], 2);
                len = (byte[0] << 8) + byte[1];;
                if (reflTxBuffer.getData() >= len)
                {
                    uint8_t buf[len];
                    for (int i=0;i<len;i++)
                    {
                        reflTxBuffer.get(buf[i]);
                    }
                    if (write(sockfd, buf, len) < 0)
                    {
                        fprintf(stderr, "ERROR: remote disconnect\n");
                        break;
                    }
                }
            }

            loop = 0;
        }

        if (reflCommand.getData() >= 5)
        {
            if (reflCommand.peek() != 0x61)
            {
                fprintf(stderr, "TX invalid header.\n");
            }
            else
            {
                uint8_t  byte[2];
                uint16_t len = 0;
                reflCommand.npeek(byte[0], 1);
                reflCommand.npeek(byte[1], 2);
                len = (byte[0] << 8) + byte[1];;
                if (reflCommand.getData() >= len)
                {
                    uint8_t buf[len];
                    for (int i=0;i<len;i++)
                    {
                        reflCommand.get(buf[i]);
                    }
                    if (write(sockfd, buf, len) < 0)
                    {
                        fprintf(stderr, "ERROR: remote disconnect\n");
                        break;
                    }
                }
            }
        }
    }
    fprintf(stderr, "TX thread exited.\n");
    int iRet = 500;
    pthread_exit(&iRet);
    return NULL;
}

void *processGatewaySocket(void *arg)
{
    int      childfd = (intptr_t)arg;
    ssize_t  len = 0;
    uint8_t  offset = 0;
    uint16_t respLen = 0;
    uint8_t  typeLen = 0;
    uint8_t  buffer[BUFFER_SIZE];

    dstarGWConnected = true;
    addGateway("main", "DSTAR");

    while (connected)
    {
        int len = read(childfd, buffer, 1);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            error((char*)"ERROR: DSTAR gateway connection closed remotely.");
            break;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            delay(5);
            continue;
        }

        if (len != 1)
        {
            fprintf(stderr, "DSTAR_Service: error when reading from DSTAR gateway, errno=%d\n", errno);
            close(childfd);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "DSTAR_Service: unknown byte from DTAR gateway, 0x%02X\n", buffer[0]);
            continue;
        }

        offset = 0;
        while (offset < 3)
        {
            len = read(childfd, buffer + 1 + offset, 3 - offset);
            if (len == 0) break;
            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                delay(5);
            else
                offset += len;
        }

        if (len == 0)
        {
            close(childfd);
            break;
        }

        respLen = (buffer[1] << 8) + buffer[2];

        offset += 1;
        while (offset < respLen)
        {
            len = read(childfd, buffer + offset, respLen - offset);
            if (len == 0) break;
            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                delay(5);
            else
                offset += len;
        }

        if (len == 0)
        {
            close(childfd);
            break;
        }

        typeLen = buffer[3];
        uint8_t type[typeLen];
        memcpy(type, buffer+4, typeLen);

        if (debugM)
            dump((char*)"DSTAR Gateway data:", (unsigned char*)buffer, respLen);

        if (memcmp(type, TYPE_NACK, typeLen) == 0)
        {
            dstarReflConnected = false;
            char tmp[8];
            bzero(tmp, 8);
            memcpy(tmp, buffer+4+typeLen, 7);
            ackDashbCommand("reflLinkDSTAR", "failed");
            setReflectorStatus("DSTAR", (const char*)tmp, "Unlinked");
        }
        else if (memcmp(type, TYPE_CONNECT, typeLen) == 0)
        {
            dstarReflConnected = true;
            ackDashbCommand("reflLinkDSTAR", "success");
            char tmp[8];
            bzero(tmp, 8);
            memcpy(tmp, buffer+4+typeLen, 7);
            char module[2];
            bzero(module, 2);
            module[0] = buffer[15];
            setReflectorStatus("DSTAR", (const char*)tmp, module);
        }
        else if (memcmp(type, TYPE_DISCONNECT, typeLen) == 0)
        {
            dstarReflConnected = false;
            char tmp[8];
            bzero(tmp, 8);
            memcpy(tmp, buffer+4+typeLen, 7);
            ackDashbCommand("reflLinkDSTAR", "success");
            setReflectorStatus("DSTAR", (const char*)tmp, "Unlinked");
        }
        else if (memcmp(type, TYPE_STATUS, typeLen) == 0)
        {
        }
        else if (memcmp(type, TYPE_EOT, typeLen) == 0)
        {
            write(sockfd, buffer, respLen);
            duration = difftime(time(NULL), start_time);
        }
        delay(5);
    }
    fprintf(stderr, "Gateway disconnected.\n");
    dstarGWConnected = false;
    dstarReflConnected = false;
    delGateway("main", "DSTAR");
    clearReflLinkStatus("DSTAR");
    int iRet = 100;
    pthread_exit(&iRet);
    return 0;
}

void *startTCPServer(void *arg)
{
    struct hostent *hostp; /* client host info */
    int childfd; /* child socket */
    int sockFd;

    sockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd < 0)
    {
        fprintf(stderr, "DSTAR_Service: error when creating the socket: %s\n", strerror(errno));
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
    serveraddr.sin_port = htons((unsigned short)serverPort);

    if (bind(sockFd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0)
    {
        fprintf(stderr, "DSTAR_Service: error when binding the socket to port %u: %s\n", serverPort, strerror(errno));
        exit(1);
    }

    if (debugM)
        fprintf(stdout, "Opened the TCP socket on port %u\n", serverPort);

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
        error((char*)"Gateway connect failed.");
        exit(1);
    }

    while (connected)
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

        pthread_t txid;
        int err = pthread_create(&(txid), NULL, &txThread, (void*)(intptr_t)childfd);
        if (err != 0)
            fprintf(stderr, "Can't create tx thread :[%s]", strerror(err));
        else
        {
            if (debugM)
                fprintf(stderr, "TX thread created successfully\n");
        }

        pthread_t procid;
        err = pthread_create(&(procid), NULL, &processGatewaySocket, (void*)(intptr_t)childfd);
        if (err != 0)
        {
            fprintf(stderr, "Can't create gateway process thread :[%s]", strerror(err));
            continue;
        }
        else
        {
            dstarGWConnected = true;
            if (debugM)
                fprintf(stderr, "Client process thread created successfully\n");
        }
        delay(1000);
    }
    int iRet = 100;
    pthread_exit(&iRet);
    return NULL;
}

int main(int argc, char **argv)
{
    bool daemon = 0;
    int  ret;
    int  c;

    while ((c = getopt(argc, argv, "vx")) != -1)
    {
        switch (c)
        {
            case 'd':
                daemon = true;
                break;
            case 'v':
                fprintf(stdout, "DSTAR_Service: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: DSTAR_Service [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            fprintf(stderr, "DSTAR_Service: error in fork(), exiting\n");
            return 1;
        }

        // If this is the parent, exit
        if (pid > 0)
            return 0;

        // We are the child from here onwards
        setsid();

        umask(0);
    }

    clearDashbCommands();
    clearReflLinkStatus("DSTAR");

    int err = pthread_create(&(modemHostid), NULL, &startClient, modemHost);
    if (err != 0)
    {
        fprintf(stderr, "Can't create modem host thread :[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM)
            fprintf(stderr, "Modem host thread created successfully\n");
    }

    err = pthread_create(&(gwHostid), NULL, &startTCPServer, NULL);
    if (err != 0)
    {
        fprintf(stderr, "Can't create gateway host thread :[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM)
            fprintf(stderr, "Gateway host thread created successfully\n");
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

    while (connected)
    {
        if (statusTimeout)
        {
            if (dstarGWConnected)
            {
                std::string parameter = readDashbCommand("reflLinkDSTAR");
                if (parameter.empty())
                {
                    statusTimeout = false;
                    continue;
                }
                if (parameter == "unlink")
                {
                    reflCommand.put(0x61);
                    reflCommand.put(0x00);
                    reflCommand.put(0x08);
                    reflCommand.put(0x04);
                    reflCommand.put(TYPE_DISCONNECT[0]);
                    reflCommand.put(TYPE_DISCONNECT[1]);
                    reflCommand.put(TYPE_DISCONNECT[2]);
                    reflCommand.put(TYPE_DISCONNECT[3]);
                    sleep(3);
                    statusTimeout = false;
                    continue;
                }
                else if (!dstarReflConnected)
                {
                    char tmp[41];
                    strcpy(tmp, parameter.c_str());
               //     fprintf(stderr, "%s\n", tmp);
                    char *token = NULL;
                    token = strtok((char*)tmp, ",");
                    if (token != NULL)
                    {
                        if (strcasecmp(token, "link") == 0)
                        {
                            token = strtok(NULL, ",");
                            char name[8];
                            bzero(name, 8);
                            strcpy(name, token);
                            token = strtok(NULL, ",");
                            char module[2];
                            strcpy(module, token);
                            reflCommand.put(0x61);
                            reflCommand.put(0x00);
                            reflCommand.put(0x16);
                            reflCommand.put(0x04);
                            reflCommand.put(TYPE_CONNECT[0]);
                            reflCommand.put(TYPE_CONNECT[1]);
                            reflCommand.put(TYPE_CONNECT[2]);
                            reflCommand.put(TYPE_CONNECT[3]);
                            std::string callsign = readHostConfig("main", "callsign");
                            for (uint8_t x=0;x<6;x++)
                                reflCommand.put(callsign.c_str()[x]);
                            for (uint8_t x=0;x<7;x++)
                                reflCommand.put(name[x]);
                            reflCommand.put(module[0]);
                            sleep(3);
                        }
                        else
                            ackDashbCommand("reflLinkDSTAR", "failed");
                    }
                    else
                        ackDashbCommand("reflLinkDSTAR", "failed");
                }
                else
                   ackDashbCommand("reflLinkDSTAR", "failed");
            }
            else
            {
                std::string parameter = readDashbCommand("reflLinkDSTAR");
                if (!parameter.empty())
                {
                    ackDashbCommand("reflLinkDSTAR", "No gateway");
                }
            }
            statusTimeout = false;
        }
        delay(500000);
    }
    fprintf(stderr, "DSTAR service terminated.\n");
    logError("main", "DSTAR host terminated.");
}
