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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <cstdint>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <cassert>

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/param.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <CRingBuffer.h>
#include <tools.h>
#include <p25_func.h>

using namespace std;

#define VERSION     "2026-01-01"
#define BUFFER_SIZE 1024

pthread_t p25Reflid;
pthread_t clientid;

int      clientPort        = 18300;
uint8_t  modemId           = 1;          //< Modem Id used to create modem name.
char     modemName[10]     = "modem1";   //< Modem name that this program is associated with.
char     p25Host[80]       = "127.0.0.1";
char     P25Name[8]        = "";
bool     p25ReflDisconnect = false;
bool     p25ReflConnected  = false;
//bool     reflPacketRdy     = false;
bool     connected         = true;
bool     debugM            = false;
bool     txOn              = false;
uint8_t  duration          = 0;
time_t   start_time;
uint8_t  lastIMBE[11]      = {0};
int      nac               = 0x293;

std::string currentP25Refl("");
std::string rptrCallsign("N0CALL");

RingBuffer<uint8_t> reflTxBuffer(800);
RingBuffer<uint8_t> txBuffer(800);

const char* TYPE_HEADER      = "P25H";
const char* TYPE_LDU         = "P25L";
const char* TYPE_EOT         = "P25E";
const char *TYPE_NACK        = "NACK";
const char *TYPE_DISCONNECT  = "DISC";
const char *TYPE_CONNECT     = "CONN";
const char *TYPE_STATUS      = "STAT";
const char *TYPE_COMMAND     = "COMM";

const uint8_t COMM_UPDATE_CONF  = 0x04;

uint8_t ldu1[P25_LDU_FRAME_LENGTH_BYTES];
uint8_t ldu2[P25_LDU_FRAME_LENGTH_BYTES];

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

// Print debug data.
// From MMDVM project by Jonathan Naylor G4KLX.
void dump(char* text, uint8_t* data, unsigned int length)
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

    fprintf(stdout, "%s: %llu (ms)\n", text, milliseconds);

    while (length > 0U)
    {
        unsigned int bytes = (length > 16U) ? 16U : length;

        fprintf(stdout, "%04X:  ", offset);

        for (i = 0U; i < bytes; i++) fprintf(stdout, "%02X ", data[offset + i]);

        for (i = bytes; i < 16U; i++) fputs("   ", stdout);

        fputs("   *", stdout);

        for (i = 0U; i < bytes; i++)
        {
            uint8_t c = data[offset + i];

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

// Send out going bytes to P25 service.
void* txThread(void *arg)
{
    int  sockfd = (intptr_t)arg;
    uint16_t loop = 0;

    while (connected)
    {
        delay(100);
        loop++;

        if (loop > 1000)
        {
            if (txBuffer.dataSize() >= 5)
            {
                uint8_t buf[1];
                txBuffer.peek(buf, 1);
                if (buf[0] != 0x61)
                {
                    fprintf(stderr, "TX invalid header.\n");
                    continue;
                }
                uint8_t  byte[3];
                uint16_t len = 0;
                txBuffer.peek(byte, 3);
                len = (byte[1] << 8) + byte[2];;
                if (txBuffer.dataSize() >= len)
                {
                    uint8_t buf[len];
                    txBuffer.getData(buf, len);
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

void insertMissingAudio(uint8_t* data)
{
    if (data[0U] == 0x00U)
    {
        memcpy(data + 10U, lastIMBE, 11U);
    }
    else
    {
        memcpy(lastIMBE, data + 10U, 11U);
    }

    if (data[25U] == 0x00U)
    {
        memcpy(data + 26U, lastIMBE, 11U);
    }
    else
    {
        memcpy(lastIMBE, data + 26U, 11U);
    }

    if (data[50U] == 0x00U)
    {
        memcpy(data + 55U, lastIMBE, 11U);
    }
    else
    {
        memcpy(lastIMBE, data + 55U, 11U);
    }

    if (data[75U] == 0x00U)
    {
        memcpy(data + 80U, lastIMBE, 11U);
    }
    else
    {
        memcpy(lastIMBE, data + 80U, 11U);
    }

    if (data[100U] == 0x00U)
    {
        memcpy(data + 105U, lastIMBE, 11U);
    }
    else
    {
        memcpy(lastIMBE, data + 105U, 11U);
    }

    if (data[125U] == 0x00U)
    {
        memcpy(data + 130U, lastIMBE, 11U);
    }
    else
    {
        memcpy(lastIMBE, data + 130U, 11U);
    }

    if (data[150U] == 0x00U)
    {
        memcpy(data + 155U, lastIMBE, 11U);
    }
    else
    {
        memcpy(lastIMBE, data + 155U, 11U);
    }

    if (data[175U] == 0x00U)
    {
        memcpy(data + 180U, lastIMBE, 11U);
    }
    else
    {
        memcpy(lastIMBE, data + 180U, 11U);
    }

    if (data[200U] == 0x00U)
    {
        memcpy(data + 204U, lastIMBE, 11U);
    }
    else
    {
        memcpy(lastIMBE, data + 204U, 11U);
    }
}

bool writeLDU1(const uint8_t* ldu1, bool end)
{
    assert(ldu1 != NULL);

    decodeLDU1(ldu1);

    uint8_t buffer[23U];

    // The '62' record
    buffer[0] = 23U;
    memcpy(buffer + 1, REC62, 22U);
    audip25decode(ldu1, buffer + 11U, 0U);

    reflTxBuffer.addData(buffer, 23U);

    // The '63' record
    buffer[0] = 15U;
    memcpy(buffer + 1, REC63, 14U);
    audip25decode(ldu1, buffer + 2U, 1U);

    reflTxBuffer.addData(buffer, 15U);

    // The '64' record
    buffer[0] = 18U;
    memcpy(buffer + 1, REC64, 17U);
    buffer[2U] = getLCF();
    buffer[3U] = getMFId();
    audip25decode(ldu1, buffer + 6U, 2U);

    reflTxBuffer.addData(buffer, 18U);

    // The '65' record
    buffer[0] = 18U;
    memcpy(buffer + 1, REC65, 17U);
    unsigned int id = getDstId();
    buffer[2U]      = (id >> 16) & 0xFFU;
    buffer[3U]      = (id >> 8) & 0xFFU;
    buffer[4U]      = (id >> 0) & 0xFFU;
    audip25decode(ldu1, buffer + 6U, 3U);

    reflTxBuffer.addData(buffer, 18U);

    // The '66' record
    buffer[0] = 18U;
    memcpy(buffer + 1, REC66, 17U);
    id         = getSrcId();
    buffer[2U] = (id >> 16) & 0xFFU;
    buffer[3U] = (id >> 8) & 0xFFU;
    buffer[4U] = (id >> 0) & 0xFFU;
    audip25decode(ldu1, buffer + 6U, 4U);

    reflTxBuffer.addData(buffer, 18U);

    // The '67' record
    buffer[0] = 18U;
    memcpy(buffer + 1, REC67, 17U);
    audip25decode(ldu1, buffer + 6U, 5U);

    reflTxBuffer.addData(buffer, 18U);

    // The '68' record
    buffer[0] = 18U;
    memcpy(buffer + 1, REC68, 17U);
    audip25decode(ldu1, buffer + 6U, 6U);

    reflTxBuffer.addData(buffer, 18U);

    // The '69' record
    buffer[0] = 18U;
    memcpy(buffer + 1, REC69, 17U);
    audip25decode(ldu1, buffer + 6U, 7U);

    reflTxBuffer.addData(buffer, 18U);

    // The '6A' record
    buffer[0] = 17U;
    memcpy(buffer + 1, REC6A, 16U);
    buffer[2U] = lowSpeedData_getLSD1();
    buffer[3U] = lowSpeedData_getLSD2();
    audip25decode(ldu1, buffer + 5U, 8U);

    reflTxBuffer.addData(buffer, 17U);

    if (end)
    {
        buffer[0] = 18U;
        memcpy(buffer + 1, REC80, 17U);
        reflTxBuffer.addData(buffer, 18U);
    }

    return true;
}

bool writeLDU2(const uint8_t* ldu2, bool end)
{
    assert(ldu2 != NULL);

    decodeLDU2(ldu2);

    uint8_t buffer[23U];

    // The '6B' record
    buffer[0] = 23U;
    memcpy(buffer + 1, REC6B, 22U);
    audip25decode(ldu2, buffer + 11U, 0U);

    reflTxBuffer.addData(buffer, 23U);

    // The '6C' record
    buffer[0] = 15U;
    memcpy(buffer + 1, REC6C, 14U);
    audip25decode(ldu2, buffer + 2U, 1U);

    reflTxBuffer.addData(buffer, 15U);

    uint8_t mi[P25_MI_LENGTH_BYTES];
    getMI(mi);

    // The '6D' record
    buffer[0] = 18U;
    memcpy(buffer + 1, REC6D, 17U);
    buffer[2U] = mi[0U];
    buffer[3U] = mi[1U];
    buffer[4U] = mi[2U];
    audip25decode(ldu2, buffer + 6U, 2U);

    reflTxBuffer.addData(buffer, 18U);

    // The '6E' record
    buffer[0] = 18U;
    memcpy(buffer + 1, REC6E, 17U);
    buffer[2U] = mi[3U];
    buffer[3U] = mi[4U];
    buffer[4U] = mi[5U];
    audip25decode(ldu2, buffer + 6U, 3U);

    reflTxBuffer.addData(buffer, 18U);

    // The '6F' record
    buffer[0] = 18U;
    memcpy(buffer + 1, REC6F, 17U);
    buffer[2U] = mi[6U];
    buffer[3U] = mi[7U];
    buffer[4U] = mi[8U];
    audip25decode(ldu2, buffer + 6U, 4U);

    reflTxBuffer.addData(buffer, 18U);

    // The '70' record
    buffer[0] = 18U;
    memcpy(buffer + 1, REC70, 17U);
    buffer[2U]      = getAlgId();
    unsigned int id = getKId();
    buffer[3U]      = (id >> 8) & 0xFFU;
    buffer[4U]      = (id >> 0) & 0xFFU;
    audip25decode(ldu2, buffer + 6U, 5U);

    reflTxBuffer.addData(buffer, 18U);

    // The '71' record
    buffer[0] = 18U;
    memcpy(buffer + 1, REC71, 17U);
    audip25decode(ldu2, buffer + 6U, 6U);

    reflTxBuffer.addData(buffer, 18U);

    // The '72' record
    buffer[0] = 18U;
    memcpy(buffer + 1, REC72, 17U);
    audip25decode(ldu2, buffer + 6U, 7U);

    reflTxBuffer.addData(buffer, 18U);

    // The '73' record
    buffer[0] = 17U;
    memcpy(buffer + 1, REC73, 16U);
    buffer[2U] = lowSpeedData_getLSD1();
    buffer[3U] = lowSpeedData_getLSD2();
    audip25decode(ldu2, buffer + 5U, 8U);

    reflTxBuffer.addData(buffer, 17U);

    if (end)
    {
        buffer[0] = 18U;
        memcpy(buffer + 1, REC80, 17U);
        reflTxBuffer.addData(buffer, 18U);
    }

    return true;
}

void createHeader(const uint8_t* data)
{
    txOn = true;

    uint8_t lcf        = data[51U];
    uint8_t mfId       = data[52U];
    unsigned int dstId = (data[76U] << 16) + (data[77U] << 8) + data[78U];
    unsigned int srcId = (data[101U] << 16) + (data[102U] << 8) + data[103U];

    fprintf(stderr, "Src: %d  Dst: %d  NAC: %X  *********************************\n", srcId, dstId, getNAC());

    p25Reset();
    setLCF(lcf);
    setMFId(mfId);
    setSrcId(srcId);
    setDstId(dstId);

    //	std::string source = m_lookup->find(srcId);

    uint8_t header[P25_HDR_FRAME_LENGTH_BYTES + 8U];
    memset(header, 0x00U, P25_HDR_FRAME_LENGTH_BYTES + 8U);

    header[0U] = 0x61U;
    header[1U] = 0x00U;
    header[2U] = P25_HDR_FRAME_LENGTH_BYTES + 8U;
    header[3U] = 0x04U;
    memcpy(header + 4, TYPE_HEADER, 4);

    // Add the sync
    addSync(header + 8U);

    // Add the NID
    encodeNID(header + 8U, P25_DUID_HEADER);

    // Add the header
    encodeHeader(header + 8U);

    // Add busy bits, inbound/outbound
    addBusyBits(header + 8U, P25_HDR_FRAME_LENGTH_BITS, false, true);

    txBuffer.addData(header, P25_HDR_FRAME_LENGTH_BYTES + 8U);
}

void createNetLDU1(uint8_t* data)
{
    uint8_t ldu[P25_LDU_FRAME_LENGTH_BYTES + 8];
	memset(ldu, 0x00U, P25_LDU_FRAME_LENGTH_BYTES + 8U);

    insertMissingAudio(data);

    ldu[0U] = 0x61U;
    ldu[1U] = 0x00U;
    ldu[2U] = P25_LDU_FRAME_LENGTH_BYTES + 8U;
    ldu[3U] = 0x04U;
    memcpy(ldu + 4, TYPE_LDU, 4);

    // Add the sync
    addSync(ldu + 8U);

    // Add the NID
    encodeNID(ldu + 8U, P25_DUID_LDU1);

    // Add the LDU1 data
    encodeLDU1(ldu + 8U);

    // Add the Audio
    audip25encode(ldu + 8U, data + 10U, 0U);
    audip25encode(ldu + 8U, data + 26U, 1U);
    audip25encode(ldu + 8U, data + 55U, 2U);
    audip25encode(ldu + 8U, data + 80U, 3U);
    audip25encode(ldu + 8U, data + 105U, 4U);
    audip25encode(ldu + 8U, data + 130U, 5U);
    audip25encode(ldu + 8U, data + 155U, 6U);
    audip25encode(ldu + 8U, data + 180U, 7U);
    audip25encode(ldu + 8U, data + 204U, 8U);

    // Add the Low Speed Data
    lowSpeedData_setLSD1(data[201U]);
    lowSpeedData_setLSD2(data[202U]);
    lowSpeedData_encode1(ldu + 8U);

    // Add busy bits, inbound/outbound
    addBusyBits(ldu + 8U, P25_LDU_FRAME_LENGTH_BITS, false, true);

    // m_netFrames += 9U;
    txBuffer.addData(ldu, P25_LDU_FRAME_LENGTH_BYTES + 8U);
    memset(ldu1, 0x00U, P25_LDU_FRAME_LENGTH_BYTES);
}

void createNetLDU2(uint8_t* data)
{
    uint8_t ldu[P25_LDU_FRAME_LENGTH_BYTES + 8];
	memset(ldu, 0x00U, P25_LDU_FRAME_LENGTH_BYTES + 8U);

    insertMissingAudio(data);

    ldu[0U] = 0x61U;
    ldu[1U] = 0x00U;
    ldu[2U] = P25_LDU_FRAME_LENGTH_BYTES + 8U;
    ldu[3U] = 0x04U;
    memcpy(ldu + 4, TYPE_LDU, 4);

    // Add the sync
    addSync(ldu + 8U);

    // Add the NID
    encodeNID(ldu + 8U, P25_DUID_LDU2);

    // Add the LDU2 data
    encodeLDU2(ldu + 8U);

     // Add the Audio
    audip25encode(ldu + 8U, data + 10U, 0U);
    audip25encode(ldu + 8U, data + 26U, 1U);
    audip25encode(ldu + 8U, data + 55U, 2U);
    audip25encode(ldu + 8U, data + 80U, 3U);
    audip25encode(ldu + 8U, data + 105U, 4U);
    audip25encode(ldu + 8U, data + 130U, 5U);
    audip25encode(ldu + 8U, data + 155U, 6U);
    audip25encode(ldu + 8U, data + 180U, 7U);
    audip25encode(ldu + 8U, data + 204U, 8U);

    // Add the Low Speed Data
    lowSpeedData_setLSD1(data[201U]);
    lowSpeedData_setLSD2(data[202U]);
    lowSpeedData_encode1(ldu + 8U);

    // Add busy bits, inbound/outbound
    addBusyBits(ldu + 8U, P25_LDU_FRAME_LENGTH_BITS, false, true);

    // m_netFrames += 9U;
    txBuffer.addData(ldu, P25_LDU_FRAME_LENGTH_BYTES + 8U);
    memset(ldu2, 0x00U, P25_LDU_FRAME_LENGTH_BYTES);
}

void checkNetLDU1()
{
    if (!txOn)
    	return;

    // Check for an unflushed LDU1
    if (ldu1[0U] != 0x00U || ldu1[25U] != 0x00U ||
        ldu1[50U] != 0x00U || ldu1[75U] != 0x00U ||
        ldu1[100U] != 0x00U || ldu1[125U] != 0x00U ||
        ldu1[150U] != 0x00U || ldu1[175U] != 0x00U ||
        ldu1[200U] != 0x00U)
        createNetLDU1(ldu1);
}

void checkNetLDU2()
{
    if (!txOn)
    	return;

    // Check for an unflushed LDU2
    if (ldu2[0U] != 0x00U || ldu2[25U] != 0x00U ||
        ldu2[50U] != 0x00U || ldu2[75U] != 0x00U ||
        ldu2[100U] != 0x00U || ldu2[125U] != 0x00U ||
        ldu2[150U] != 0x00U || ldu2[175U] != 0x00U ||
        ldu2[200U] != 0x00U)
        createNetLDU2(ldu2);
}

void createNetTerminator()
{
    uint8_t data[P25_TERM_FRAME_LENGTH_BYTES + 8U];
    memset(data, 0x00U, P25_TERM_FRAME_LENGTH_BYTES + 8U);

    data[0U] = 0x61U;
    data[1U] = 0x00U;
    data[2U] = P25_TERM_FRAME_LENGTH_BYTES + 8U;
    data[3U] = 0x04U;
    memcpy(data + 4, TYPE_EOT, 4);

    // Add the sync
    addSync(data + 8U);

    // Add the NID
    encodeNID(data + 8U, P25_DUID_TERM);

    // Add busy bits, inbound/outbound
    addBusyBits(data + 8U, P25_TERM_FRAME_LENGTH_BITS, true, false);

    txBuffer.addData(data, P25_TERM_FRAME_LENGTH_BYTES + 8U);
    txOn = false;
}

void writeNetwork(uint8_t* dataIn)
{
    switch (dataIn[0U])
    {
        case 0x62U:
            memcpy(ldu1 + 0U, dataIn, 22U);
            checkNetLDU2();
            break;
        case 0x63U:
            memcpy(ldu1 + 25U, dataIn, 14U);
            checkNetLDU2();
            break;
        case 0x64U:
            memcpy(ldu1 + 50U, dataIn, 17U);
            checkNetLDU2();
            break;
        case 0x65U:
            memcpy(ldu1 + 75U, dataIn, 17U);
            checkNetLDU2();
            break;
        case 0x66U:
            memcpy(ldu1 + 100U, dataIn, 17U);
            checkNetLDU2();
            break;
        case 0x67U:
            memcpy(ldu1 + 125U, dataIn, 17U);
            checkNetLDU2();
            break;
        case 0x68U:
            memcpy(ldu1 + 150U, dataIn, 17U);
            checkNetLDU2();
            break;
        case 0x69U:
            memcpy(ldu1 + 175U, dataIn, 17U);
            checkNetLDU2();
            break;
        case 0x6AU:
        {
            memcpy(ldu1 + 200U, dataIn, 16U);
            checkNetLDU2();
            if (txOn)
            {
               // createHeader(ldu1);
                createNetLDU1(ldu1);
            }
        }
        break;
        case 0x6BU:
            memcpy(ldu2 + 0U, dataIn, 22U);
            checkNetLDU1();
            break;
        case 0x6CU:
            memcpy(ldu2 + 25U, dataIn, 14U);
            checkNetLDU1();
            break;
        case 0x6DU:
            memcpy(ldu2 + 50U, dataIn, 17U);
            checkNetLDU1();
            break;
        case 0x6EU:
            memcpy(ldu2 + 75U, dataIn, 17U);
            checkNetLDU1();
            break;
        case 0x6FU:
            memcpy(ldu2 + 100U, dataIn, 17U);
            checkNetLDU1();
            break;
        case 0x70U:
            memcpy(ldu2 + 125U, dataIn, 17U);
            checkNetLDU1();
            break;
        case 0x71U:
            memcpy(ldu2 + 150U, dataIn, 17U);
            checkNetLDU1();
            break;
        case 0x72U:
            memcpy(ldu2 + 175U, dataIn, 17U);
            checkNetLDU1();
            break;
        case 0x73U:
        {
            memcpy(ldu2 + 200U, dataIn, 16U);
            if (!txOn)
            {
                createHeader(ldu1);
                createNetLDU1(ldu1);
            }
            else
                checkNetLDU1();

            createNetLDU2(ldu2);
        }
        break;
        case 0x80U:
            // Stream terminator;
            createNetTerminator();
            break;
        default:
            break;
    }
}

// Connect and process bytes from P25 reflector.
void *connectP25Refl(void *argv)
{
    struct sockaddr_in serveraddr;
    struct hostent *server;
    int sockfd, portno, n;
    int hostfd;
    int serverlen;
    int timeout = 0;
    char hostname[80];
    char source[11];
    uint8_t buf[1024];

    sscanf((char*)argv, "%d %s %s %d", &hostfd, source, hostname, &portno);
    printf("%s  %s  %d\n", source, hostname, portno);
    rptrCallsign = source;

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error((char*)"ERROR opening socket");

    /* gethostbyname: get the server's DNS entry */
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host as %s\n", hostname);
        exit(0);
    }

    struct timeval read_timeout;
    read_timeout.tv_sec = 0;
    read_timeout.tv_usec = 10;
    int on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof read_timeout);
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    /* build the server's Internet address */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);
    serverlen = sizeof(serveraddr);

    bzero(buf, 1024);

    /* send connect message to the server */
    buf[0] = 0xf0;
    memset(buf + 1, 32, 10);
    memcpy(buf + 1, source, strlen(source));

    n = sendto(sockfd, buf, 11, 0, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
    if (n < 0)
      error((char*)"ERROR in sendto");

    if (debugM)
        fprintf(stderr, "Entering P25 reflector comm loop.\n");

    sleep(1);
    p25ReflConnected = false;
    buf[0] = 0x61;
    buf[1] = 0x00;
    buf[2] = 0x0E;
    buf[3] = 0x04;
    txBuffer.addData(buf, 4);

    bzero(buf, 1024);
    n = recvfrom(sockfd, buf, 11, 0, (struct sockaddr*)&serveraddr, (socklen_t*)&serverlen);
    if (n == 11)
    {
        p25ReflConnected = true;
        uint8_t tmp[4];
        memcpy(tmp, TYPE_CONNECT, 4);
        txBuffer.addData(tmp, 4);
        txBuffer.addData((uint8_t*)P25Name, 6);
    }
    else
    {
        uint8_t tmp[4];
        memcpy(tmp, TYPE_NACK, 4);
        txBuffer.addData(tmp, 4);
        txBuffer.addData((uint8_t*)P25Name, 6);
        sleep(1);
        close(sockfd);
        fprintf(stderr, "Failed to connect to P25 reflector.\n");
        int iRet = 100;
        pthread_exit(&iRet);
        return NULL;
    }

    if (debugM)
        dump((char*)"P25 Refl recv data", (uint8_t*)buf, n);

    p25ReflDisconnect = false;

    while (p25ReflConnected)
    {
        bzero(buf, 100);

        /* print the server's reply */
        n = recvfrom(sockfd, buf, 100, 0, (struct sockaddr*)&serveraddr, (socklen_t*)&serverlen);
        if (n > 11)
        {
            if (debugM)
                dump((char*)"P25 Refl recv data", (uint8_t*)buf, n);

            writeNetwork(buf);
        }
        else if (n == 11)
        {
            if (debugM)
                dump((char*)"P25 Refl recv data", (uint8_t*)buf, n);
        }

        if (timeout >= 5000)
        {
            // Send ping every 5 seconds.
            buf[0] = 0xf0;
            memset(buf + 1, 32, 10);
            memcpy(buf + 1, source, strlen(source));
            n = sendto(sockfd, buf, 11, 0, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
            if (n < 0)
                error((char*)"ERROR in sendto");
            timeout = 0;
        }

        if (timeout > 6000) // 6 seconds
            break;
        else
            timeout++;

        if (reflTxBuffer.dataSize() >= 14)
        {
            reflTxBuffer.peek(buf, 1);
            if (reflTxBuffer.dataSize() >= buf[0])
            {
                reflTxBuffer.getData(buf, buf[0]);
                n = sendto(sockfd, buf + 1, buf[0] - 1, 0, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
                if (n < 0)
                    error((char*)"ERROR in sendto");
            }
        }

        if (p25ReflDisconnect)
        {
            txOn = false;
            p25ReflConnected = false;
            /* send disconnect message to the server */
            buf[0] = 0xf1;
            memset(buf + 1, 32, 10);
            memcpy(buf + 1, source, strlen(source));

            n = sendto(sockfd, buf, 11, 0, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
            if (n < 0)
                error((char*)"ERROR in sendto");
        }

        delay(500);
    }
    txOn = false;
    uint8_t tmp[4];
    tmp[0] = 0x61;
    tmp[1] = 0x00;
    tmp[2] = 0x0E;
    tmp[3] = 0x04;
    txBuffer.addData(tmp, 4);
    txBuffer.addData((uint8_t*)TYPE_DISCONNECT, 4);
    txBuffer.addData((uint8_t*)P25Name, 6);
    p25ReflConnected = false;
    sleep(1);
    close(sockfd);
    fprintf(stderr, "Disconnected from P25 reflector.\n");
    int iRet = 100;
    pthread_exit(&iRet);
    return NULL;
}

// Start up reflector threads.
void connectToRefl(int sockfd, char *source, char *reflector, uint16_t port)
{
    static char server[80] = "";

    p25ReflDisconnect = true;
    sleep(1);

    sprintf(server, "%d %s %s %d", sockfd, source, reflector, port);

    pthread_t txid;
    int err = pthread_create(&(txid), NULL, &txThread, (void*)(intptr_t)sockfd);
    if (err != 0)
       fprintf(stderr, "Can't create tx thread :[%s]", strerror(err));
    else
    {
        if (debugM)
            fprintf(stderr, "TX thread created successfully\n");
    }
    err = pthread_create(&(p25Reflid), NULL, &connectP25Refl, server);
    if (err != 0)
        fprintf(stderr, "Can't create P25 reflector thread :[%s]", strerror(err));
    else
    {
        if (debugM)
            fprintf(stderr, "P25 reflector thread created successfully\n");
    }
}

// Start up connection to P25 service.
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
    printf("Connected to P25 host.\n");

    ssize_t  len = 0;
    uint8_t  offset = 0;
    uint16_t respLen = 0;
    uint8_t  typeLen = 0;
    bool     lsfOk = false;

    while (connected)
    {
        int len = read(sockfd, buffer, 1);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            error((char*)"ERROR: P25 host connection closed remotely.");
            break;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            delay(5);
        }

        if (len != 1)
        {
            fprintf(stderr, "P25_Gateway: error when reading from P25 host, errno=%d\n", errno);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "P25_Gateway: unknown byte from P25 host, 0x%02X\n", buffer[0]);
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
            dump((char*)"P25 Service data:", (uint8_t*)buffer, respLen);

        if (memcmp(type, TYPE_COMMAND, typeLen) == 0)
        {
            if (buffer[8] == COMM_UPDATE_CONF)
            {
                uint8_t buf[4];
                buf[0] = 0x61;
                buf[1] = 0x00;
                buf[2] = 0x08;
                buf[3] = 0x04;
                txBuffer.addData(buf, 4);
                txBuffer.addData((uint8_t*)TYPE_COMMAND, 4);
            }
        }
        else if (memcmp(type, TYPE_CONNECT, typeLen) == 0)
        {
            if (!p25ReflConnected)
            {
                char source[8];
                bzero(source, 8);
                bzero(P25Name, 8);
                memcpy(source, buffer + 8, 6);
                memcpy(P25Name, buffer + 14, 6);
                fprintf(stderr, "Name: %s\n", P25Name);
                char url[80];
                char ipaddr[15];
                uint16_t port;
                if (findReflector("P25", P25Name, ipaddr, &port))
                {
                    connectToRefl(sockfd, source, ipaddr, port);
                }
            }
        }
        else if (memcmp(type, TYPE_DISCONNECT, typeLen) == 0)
        {
            p25ReflDisconnect = true;
        }
        else if (memcmp(type, TYPE_STATUS, typeLen) == 0)
        {
        }
        else if (memcmp(type, "P25L", 4) == 0)
        {
            bool valid = decodeNID((uint8_t*)(buffer + 8));
            if (valid)
            {
                uint8_t duid = getDUID();
                if (duid == P25_DUID_LDU1)
                    writeLDU1((uint8_t*)(buffer + 8), false);
                else if (duid == P25_DUID_LDU2)
                    writeLDU2((uint8_t*)(buffer + 8), false);
                else if (duid == P25_DUID_TSDU)
                    writeLDU2((uint8_t*)(buffer + 8), true);
            }
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
                clientPort = 18300 + modemId - 1;
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
                fprintf(stdout, "P25_Gateway: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: P25_Gateway [-d] [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            fprintf(stderr, "P25_Gateway: error in fork(), exiting\n");
            return 1;
        }

        // If this is the parent, exit
        if (pid > 0)
            return 0;

        // We are the child from here onwards
        setsid();

        umask(0);
    }

    int err = pthread_create(&(clientid), NULL, &startClient, p25Host);
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

    memcpy(lastIMBE, P25_NULL_IMBE, 11U);
    memset(ldu1, 0x00U, P25_LDU_FRAME_LENGTH_BYTES);
    memset(ldu2, 0x00U, P25_LDU_FRAME_LENGTH_BYTES);

    char tmp[10];
    readHostConfig(modemName, "P25", "NAC", tmp);
    nac = strtol(tmp, NULL, 16);
    resetNID(nac);

    while (1)
    {
         delay(50000);
         if (!connected)
             break;
    }
    fprintf(stderr, "P25 Gateway terminated.\n");
    logError(modemName, "main", "P25 Gateway terminated.");
}
