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

#include "../../../tools/RingBuffer.h"
#include "../../../tools/tools.h"
#include "../../../tools/CCITTChecksumReverse.h"

using namespace std;

#define VERSION     "2025-09-25"
#define BUFFER_SIZE 1024

pthread_t dstarReflid;
pthread_t clientid;

int      sockoutfd           = 0;
int      clientPort          = 18101;
char     DSTARName[8]        = "";
char     DSTARModule[2]      = "C";
char     myCall[9]           = "";
char     urCall[9]           = "";
char     rpt1Call[9]         = "";
char     rpt2Call[9]         = "";
char     lastCall[9]         = "";
char     suffix[5]           = "";
char     metaText[23]        = "";
char     tx_state[4]         = "off";
char     DSTARHost[80]       = "127.0.0.1";
bool     DSTARReflDisconnect = false;
bool     DSTARReflConnected  = false;
bool     reflPacketRdy       = false;
bool     connected           = true;
bool     debugM              = false;
bool     txOn                = false;
bool     slowSpeedUpdate     = false;
uint8_t  duration            = 0;
uint16_t streamId            = 0;
time_t   start_time;
uint8_t  iBER                = 0;

struct sockaddr_in servaddrin, servaddrout, cliaddr;

std::string currentDSTARRefl("");
std::string rptrCallsign("N0CALL");

CRingBuffer<uint8_t> reflTxBuffer(3600);
CRingBuffer<uint8_t> txBuffer(3300);

const char *TYPE_HEADER         = "DSTH";
const char *TYPE_DATA           = "DSTD";
const char *TYPE_EOT            = "DSTE";
const char *TYPE_NACK           = "NACK";
const char *TYPE_DISCONNECT     = "DISC";
const char *TYPE_CONNECT        = "CONN";
const char *TYPE_STATUS         = "STAT";

const uint8_t  AMBE_HEADER[]    = {0x61, 0x00, 0x0B, 0x01, 0x01, 0x48};
const uint8_t  AMBE_HEADER_LEN  = 6U;
const uint8_t  AMBE_SILENCE[]   = {0x9e, 0x8d, 0x32, 0x32, 0x26, 0x1a, 0x3f, 0x61, 0xe8};

std::string sGps     = "";
std::string sGPSCall = "";
float       fLat     = 0.0f;
float       fLong    = 0.0f;
uint16_t    altitude = 0;

typedef union crc {
        unsigned int   m_crc16;
        unsigned char  m_crc8[2];
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
    unsigned int offset = 0U;
    unsigned int i;

    fputs(text, stdout);
    fputc('\n', stdout);

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

int searchDSTARHostFile(const char *refl, char *ipaddr)
{
    FILE *file = NULL;
    char name[10] = "";
    char line[101] = "";
    uint8_t ret = 0;

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
            if (txBuffer.getData() >= 5)
            {
                if (txBuffer.peek() != 0x61)
                {
                 //   txBuffer.reset();
                    fprintf(stderr, "TX EOT invalid header.\n");
                    continue;
                }
                uint8_t  byte[2];
                uint16_t len = 0;
                txBuffer.npeek(byte[0], 1);
                txBuffer.npeek(byte[1], 2);
                len = (byte[0] << 8) + byte[1];;
                if (txBuffer.getData() >= len)
                {
                    uint8_t buf[len];
                    for (int i=0;i<len;i++)
                    {
                        txBuffer.get(buf[i]);
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
    }
    fprintf(stderr, "TX thread exited.\n");
    int iRet = 500;
    connected = false;
    pthread_exit(&iRet);
    return NULL;
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

void slowSpeedDataEncode(char *cMessage, unsigned char *ucBytes, unsigned char ucMode)
{
    static int           iIndex;
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
        iIndex = 0;
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
                ucBytes[0] = (unsigned char)((0x40 + iIndex/5) ^ 0x70);
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

void sendToGw(char *cHeader, uint8_t *ucData, bool bSsEncode, bool bEnd)
{
    CCCITTChecksumReverse ccr;
    uint8_t          ucBuffer[50];
    static int       iPktCount;
    static int       iFrameCount;
    static uint16_t  iStreamId;
    static bool      bSentHeader;

    mcrc.m_crc16 = 0xFFFF;

    ucBuffer[0] = 'D';
    ucBuffer[1] = 'S';
    ucBuffer[2] = 'R';
    ucBuffer[3] = 'P';

    ucBuffer[4] = 0x21;

    if (iPktCount == 0 && !bSentHeader)
    {
        printf("Network transmission started.\n");
        ucBuffer[4] = 0x0a;
        memcpy(ucBuffer+5, "linux_pirptr-20250926", 21);
        ucBuffer[26] = 0x00;
        sendto(sockoutfd, ucBuffer, 27, 0, (struct sockaddr *)&servaddrout, sizeof(servaddrout));
        ucBuffer[4] = 0x20;
        iStreamId = (rand() % 0xffff) + 1;
        ucBuffer[5] = (iStreamId & 0xff00) >> 8;
        ucBuffer[6] = iStreamId & 0x00ff;
        ucBuffer[7] = 0;
        ucBuffer[8] = 0;
        ucBuffer[9] = 0;
        ucBuffer[10] = 0;
        memcpy(ucBuffer+11, cHeader, 36);
        ccr.update(ucBuffer + 8, 39); // 4 * 8 + 4 + 3
        ccr.result(ucBuffer + 47);
        sendto(sockoutfd, ucBuffer, 49, 0, (struct sockaddr *)&servaddrout, sizeof(servaddrout));
        ucBuffer[4] = 0x21;
        bSentHeader = true;
    }

    ucBuffer[5] = (iStreamId & 0xff00) >> 8;
    ucBuffer[6] = iStreamId & 0x00ff;

    if (iPktCount == 0)
    {
        ucData[9] = 0x55;
        ucData[10] = 0x2d;
        ucData[11] = 0x16;
    }

    ucBuffer[7] = iPktCount;

	if (iFrameCount > 251)
		iFrameCount = 0;

    if (bSsEncode)
	    slowSpeedDataEncode(metaText, ucData+9, 1);

    ucBuffer[8] = iBER;
    if (bEnd)
    {
        ucBuffer[9]  = 0x9e;
        ucBuffer[10] = 0x8d;
        ucBuffer[11] = 0x32;
        ucBuffer[12] = 0x32;
        ucBuffer[13] = 0x26;
        ucBuffer[14] = 0x1a;
        ucBuffer[15] = 0x3f;
        ucBuffer[16] = 0x61;
        ucBuffer[17] = 0xe8;
        ucBuffer[18] = 0x55;
        ucBuffer[19] = 0x55;
        ucBuffer[20] = 0x55;
        sendto(sockoutfd, ucBuffer, 21, 0, (struct sockaddr *)&servaddrout, sizeof(servaddrout));
        ucBuffer[7] = iPktCount;
        ucBuffer[7] |= 0x40;			// End of data marker
        ucBuffer[21] = 0x55;
        ucBuffer[22] = 0xc8;
        ucBuffer[23] = 0x7a;
        sendto(sockoutfd, ucBuffer, 24, 0, (struct sockaddr *)&servaddrout, sizeof(servaddrout));
        iPktCount = -1;
        iFrameCount = 0;
        bSentHeader = false;
        printf("End network transmission.\n");
    }
    else
    {
        memcpy(ucBuffer+9, ucData, 12);
        sendto(sockoutfd, ucBuffer, 21, 0, (struct sockaddr *)&servaddrout, sizeof(servaddrout));
    }

	iPktCount++;
    if (iPktCount > 0x14)
    {
        iPktCount = 0;
    }
}

void *connectIRCDDBGateway(void *argv)
{
    int      sockinfd;
    int      portout, portin, n;
    int      hostfd;
    char     local_address[20];
    char     gw_address[80];
    uint8_t  cRecvline[50];
    uint8_t  cRxBuffer[400];
    uint8_t  cTxHeader[37];
    uint16_t timeout = 0;

    sscanf((char*)argv, "%d %s %s %d %d", &hostfd, local_address, gw_address, &portin, &portout);

    sockinfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockinfd < 0)
        error((char*)"ERROR opening gw input socket");

    bzero(&servaddrin, sizeof(servaddrin));
    servaddrin.sin_family = AF_INET;
    servaddrin.sin_addr.s_addr = inet_addr(local_address);
    servaddrin.sin_port = htons(portin);
    n = bind(sockinfd,(struct sockaddr *)&servaddrin, sizeof(servaddrin));

    fcntl(sockinfd, F_SETFL, fcntl(sockinfd, F_GETFL, 0) | O_NONBLOCK);

    sockoutfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockoutfd < 0)
        error((char*)"ERROR opening gw output socket");

    bzero((char *) &servaddrout, sizeof(servaddrout));
    servaddrout.sin_family = AF_INET;
    servaddrout.sin_addr.s_addr = inet_addr(gw_address);
    servaddrout.sin_port = htons(portout);

    DSTARReflDisconnect = false;

    while (connected)
    {
        bzero(cRecvline, 50);

        socklen_t len = sizeof(cliaddr);
        n = recvfrom(sockinfd, cRecvline, 50, 0, (struct sockaddr *)&cliaddr, &len);
        if (n > 0)
        {
            timeout = 0;
            if (debugM)
                dump((char*)"DSTAR gateway recv data:", (uint8_t*)cRecvline, n);
        }
        if (n > 0 && (n != 21 && n != 49))
            fprintf(stderr, "n=%d\n", n);
        if (n > 24)
        {
            bzero(myCall, 9);
            bzero(suffix, 5);
            bzero(urCall, 9);
            bzero(rpt1Call, 9);
            bzero(rpt2Call, 9);
            memcpy(myCall, cRecvline+35, 8);
            memcpy(suffix, cRecvline+43, 4);
            memcpy(rpt1Call, cRecvline+11, 8);
            memcpy(rpt2Call, cRecvline+19, 8);
            memcpy(urCall, cRecvline+27, 8);
            if (strcmp(myCall, lastCall) != 0)
            {
		        printf("UR: %8s  MY: %8s%4s  Rpt1: %8s  Rpt2: %8s\n", urCall, myCall, suffix, rpt1Call, rpt2Call);
                sprintf((char*)cTxHeader, "%8s%4s%8s%8s%8s", myCall, suffix, urCall, rpt1Call, rpt2Call);
                strcpy(lastCall, myCall);
            }
        }
        else
        if (n == 21)
        {
         //   fec.regenerate((unsigned char*)cRecvline+9);
            memcpy(cRxBuffer, AMBE_HEADER, AMBE_HEADER_LEN);
            memcpy(cRxBuffer+AMBE_HEADER_LEN, cRecvline+9, 9);
            slowSpeedDataDecode(cRecvline[18], cRecvline[19], cRecvline[20]);
        }

        if (DSTARReflDisconnect)
        {
            DSTARReflConnected = false;
        }

        delay(10000);

  //      if (timeout > 1500)
  //          break;
  //      else
  //          timeout++;
    }
    txBuffer.put(0x61);
    txBuffer.put(0x00);
    txBuffer.put(0x0F);
    txBuffer.put(0x04);
    txBuffer.put(TYPE_DISCONNECT[0]);
    txBuffer.put(TYPE_DISCONNECT[1]);
    txBuffer.put(TYPE_DISCONNECT[2]);
    txBuffer.put(TYPE_DISCONNECT[3]);
    for (uint8_t x=0;x<7;x++)
        txBuffer.put(DSTARName[x]);
    close(sockoutfd);
    close(sockinfd);
    sleep(1);
    fprintf(stderr, "Disconnected from DSTAR gateway.\n");
     int iRet = 100;
    pthread_exit(&iRet);
    return 0;
}

void connectToIRCDDB(int sockfd, const char *local_address, const char *gw_address, uint16_t portout, uint16_t portin)
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
    }
}

void* startClient(void *arg)
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
    printf("Connected to DSTAR host.\n");

    connectToIRCDDB(sockfd, "127.0.0.1", "127.0.0.1", 20010, 20011);

    ssize_t  len = 0;
    uint8_t  offset = 0;
    uint16_t respLen = 0;
    uint8_t  typeLen = 0;

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
        memcpy(type, buffer+4, typeLen);

        if (debugM)
            dump((char*)"DSTAR Service data:", (unsigned char*)buffer, respLen);

        if (memcmp(type, TYPE_CONNECT, typeLen) == 0)
        {
            if (!DSTARReflConnected)
            {
                char source[7];
                bzero(source, 7);
                bzero(DSTARName, 8);
                bzero(DSTARModule, 2);
                memcpy(source, buffer+8, 6);
                memcpy(DSTARName, buffer+14, 7);
                memcpy(DSTARModule, buffer+21, 1);
                fprintf(stderr, "Name: %s  Mod: %s\n", DSTARName, DSTARModule);
                char ipaddr[15];
                uint16_t  port;
       //         if (findReflector("DPLUS", DSTARName, ipaddr, &port))
                {
                    char    cBuffer[40];
                    uint8_t ucBytes[12];
//                    sprintf(cBuffer, "%8s%8s%8s%8s%4s", cRpt2Call, cRpt1Call, "        ", source, "TEST");
                    sprintf(cBuffer, "%8s%8s%8s%8s%4s", "KD0OSS G", "KD0OSS B", "        ", source, "TEST");
                    memcpy(cBuffer+16, DSTARName, 6);
                    cBuffer[22] = DSTARModule[0];
                    cBuffer[23] = 'L';
                    memset(ucBytes, 0, 9);
                    ucBytes[9] = 0x55;
                    ucBytes[10] = 0x2d;
                    ucBytes[11] = 0x16;
                    sendToGw(cBuffer, ucBytes, true, true);
             //       connectToRefl(sockfd, source, ipaddr, DSTARModule, port);
    txBuffer.put(0x61);
    txBuffer.put(0x00);
    txBuffer.put(0x10);
    txBuffer.put(0x04);
    txBuffer.put(TYPE_CONNECT[0]);
    txBuffer.put(TYPE_CONNECT[1]);
    txBuffer.put(TYPE_CONNECT[2]);
    txBuffer.put(TYPE_CONNECT[3]);
    for (uint8_t x=0;x<7;x++)
        txBuffer.put(DSTARName[x]);
    txBuffer.put(DSTARModule[0]);

                }
            }
        }
        else if (memcmp(type, TYPE_DISCONNECT, typeLen) == 0)
        {
            DSTARReflDisconnect = true;
        }
        else if (memcmp(type, TYPE_STATUS, typeLen) == 0)
        {
        }
        else if (memcmp(type, TYPE_HEADER, typeLen) == 0)
        {
            if (!txOn)
            {
                streamId = (rand() & 0x7ff);
            }
   //         frame_t frame;
    //        for (uint8_t x=0;x<48;x++)
     //           frame.data()[x] = buffer[x+4+typeLen];
        }
        else if (memcmp(type, TYPE_DATA, typeLen) == 0)
        {
   //         frame_t frame;
    //        for (uint8_t x=0;x<48;x++)
     //           frame.data()[x] = buffer[x+4+typeLen];
     //       if (lsfOk)
            {
                txOn = true;
                if (DSTARReflConnected)
                {
               //     for (int x=0;x<18;x++)
                 //       reflTxBuffer.put(sf.getData()[x]);
                }
            }
        }
        else if (memcmp(type, TYPE_EOT, typeLen) == 0)
        {
            txOn = false;
        }

        delay(500);
    }
    fprintf(stderr, "Client thread exited.\n");
    close(sockfd);
    int iRet = 100;
    connected = false;
    pthread_exit(&iRet);
    return 0;
}

int main(int argc, char **argv)
{
    bool daemon = false;
    int ret;
    int c;

    while ((c = getopt(argc, argv, "vx")) != -1)
    {
        switch (c)
        {
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
                fprintf(stderr, "Usage: DSTAR_Gateway [-v] [-x]\n");
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
    fprintf(stderr, "DSTAR Gateway terminated.\n");
    logError("main", "DSTAR Gateway terminated.");
}
