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
 *   This software makes use of M17 functions from OpenRTX (C).            *
 *   http://openrtx.org                                                    *
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

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/param.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <M17/M17Callsign.hpp>
#include <M17/M17LinkSetupFrame.hpp>
#include <M17/M17Datatypes.hpp>
#include <M17/M17Callsign.hpp>
#include <M17/M17PacketFrame.hpp>
#include <M17/M17StreamFrame.hpp>
#include <M17/M17FrameDecoder.hpp>
#include <M17/M17FrameEncoder.hpp>
#include <M17/M17Constants.hpp>
#include <M17/Correlator.hpp>
#include <M17/M17ConvolutionalEncoder.hpp>
#include <M17/M17DSP.hpp>
#include <M17/M17Golay.hpp>
#include <M17/M17Interleaver.hpp>
#include <M17/M17Prbs.hpp>
#include <M17/M17Viterbi.hpp>
#include <M17/Synchronizer.hpp>

#include "../../tools/tools.h"
#include "../../tools/RingBuffer.h"

using namespace std;
using namespace M17;

#define VERSION     "2025-08-15"
#define BUFFER_SIZE 1024

// Mode specific data sent to configure modem. Data from MMDVM project by Jonathan Naylor G4KLX
//================================================================================================================================
static int16_t TX_RRC_0_5_FILTER[] = {0, 0, 0, 0, -290, -174, 142, 432, 438, 90, -387, -561, -155, 658, 1225, 767,
				  -980, -3326, -4648, -3062, 2527, 11552, 21705, 29724, 32767, 29724, 21705,
				  11552, 2527, -3062, -4648, -3326, -980, 767, 1225, 658, -155, -561, -387, 90,
				  438, 432, 142, -174, -290}; // numTaps = 45, L = 5
static int16_t RX_RRC_0_5_FILTER[] = {-147, -88, 72, 220, 223, 46, -197, -285, -79, 334, 623, 390, -498, -1691, -2363, -1556, 1284, 5872, 11033,
				  15109, 16656, 15109, 11033, 5872, 1284, -1556, -2363, -1691, -498, 390, 623, 334, -79, -285, -197, 46, 223,
				  220, 72, -88, -147, 0};
const uint8_t  RX_RRC_0_5_FILTER_LEN       = 42;
const uint8_t  TX_RRC_0_5_FILTER_LEN       = 45;
uint8_t        TX_RRC_0_5_FILTER_PHASE_LEN = 9;
uint8_t        RX_RRC_FILTER_STATE_LEN     = 70;
uint8_t        TX_RRC_FILTER_STATE_LEN     = 16;
uint8_t        TX_SYMBOL_LENGTH            = 5;
char           MODE_NAME[11]               = "M17";
char           MODEM_TYPE[6]               = "4FSK";
bool           USE_DC_FILTER               = true;
bool           USE_LP_FILTER               = false;
//================================================================================================================================

const char *TYPE_LSF            = "M17L";
const char *TYPE_STREAM         = "M17S";
const char *TYPE_PACKET         = "M17P";
const char *TYPE_EOT            = "M17E";
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
char     tx_state[4]      = "off";
char     srcCallsign[10]  = "";
char     dstCallsign[10]  = "";
char     exCall1[10]      = "";
char     exCall2[10]      = "";
char     metaText[53]     = "";
char     textBuffer[53]   = "";
bool     txOn             = false;
bool     validFrame       = false;
bool     debugM           = false;
bool     connected        = true;
bool     textStarted      = true;
bool     smsStarted       = false;
bool     modem_duplex     = true;
bool     m17ReflConnected = false;
bool     m17GWConnected   = false;
bool     statusTimeout    = false;
bool     reflBusy         = false;
bool     gpsFound         = false;
uint8_t  duration         = 0;
uint8_t  m17_space        = 0;
uint8_t  textOffset       = 0;
uint8_t  blk_id_tot       = 0;
uint8_t  frameCnt         = 0;
uint8_t  packetData[823]  = {};
uint8_t  smsLastFrame     = 0;
uint8_t  totalSMSMessages = 0;
uint16_t totalSMSLength   = 0;
uint16_t voiceFrameCnt    = 0;
uint16_t lastFrameNum     = 0;
uint16_t serverPort       = 18100;
uint16_t clientPort       = 18000;
uint16_t streamId         = 0;
uint16_t modeHang         = 30000;
time_t   start_time;

M17::M17LinkSetupFrame rx_lsf;   //< M17 link setup frame
M17::M17LinkSetupFrame pkt_lsf;  //< M17 packet link setup frame
M17::M17FrameDecoder   decoder;  //< M17 frame decoder
M17::M17FrameEncoder   encoder;  //< M17 frame encoder

unsigned int       clientlen;    //< byte size of client's address
char              *hostaddrp;    //< dotted decimal host addr string
int                optval;       //< flag value for setsockopt
struct sockaddr_in serveraddr;   //< server's addr
struct sockaddr_in clientaddr;   //< client addr

CRingBuffer<uint8_t> txBuffer(3600);
CRingBuffer<uint8_t> rxBuffer(3600);
CRingBuffer<uint8_t> gwTxBuffer(3600);
CRingBuffer<uint8_t> gwCommand(200);

pthread_t modemHostid;
pthread_t gwHostid;
pthread_t timerid;

uint8_t setMode[] = {0x61, 0x00, 0x05, 0x01, COMM_SET_MODE};
uint8_t setIdle[] = {0x61, 0x00, 0x05, 0x01, COMM_SET_IDLE};

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

// Simple timer thread.
// Each loop through the while statement takes 1 millisecond.
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

// Start up communication with modem host.
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

    char gps[50] = "";
    ssize_t len = 0;
    uint8_t offset = 0;
    uint16_t respLen = 0;
    uint8_t typeLen = 0;
    uint8_t configLen = 4 + 4 + 11 + 6 + 1 + 1 + 1 + 1 + 42 + 1 + 1 + 1 + 1 + 45;

    buffer[0] = 0x61;
    buffer[1] = 0x00;
    buffer[2] = configLen;
    buffer[3] = 0x04;
    memcpy(buffer+4, TYPE_MODE, 4);
    memcpy(buffer+8, MODE_NAME, 11);
    memcpy(buffer+19, MODEM_TYPE, 6);
    buffer[25] = USE_DC_FILTER;
    buffer[26] = USE_LP_FILTER;
    buffer[27] = RX_RRC_FILTER_STATE_LEN;
    buffer[28] = RX_RRC_0_5_FILTER_LEN;
    memcpy(buffer+29, RX_RRC_0_5_FILTER, RX_RRC_0_5_FILTER_LEN);
    buffer[71] = TX_SYMBOL_LENGTH;
    buffer[72] = TX_RRC_FILTER_STATE_LEN;
    buffer[73] = TX_RRC_0_5_FILTER_PHASE_LEN;
    buffer[74] = TX_RRC_0_5_FILTER_LEN;
    memcpy(buffer+75, TX_RRC_0_5_FILTER, TX_RRC_0_5_FILTER_LEN);
    write(sockfd, buffer, configLen);
    sleep(1);

    decoder.reset();
    rx_lsf.clear();

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
            fprintf(stderr, "M17_Service: error when reading from server, errno=%d\n", errno);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "M17_Service: unknown byte from server, 0x%02X\n", buffer[0]);
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
            dump((char*)"M17 Frame data", (unsigned char*)buffer, respLen);

        if (memcmp(type, TYPE_COMMAND, typeLen) == 0)
        {
            if (buffer[8] == COMM_SET_DUPLEX)
                modem_duplex = true;
            else if (buffer[8] == COMM_SET_SIMPLEX)
                modem_duplex = false;
        }
        else if (memcmp(type, TYPE_LSF, typeLen) == 0 || memcmp(type, TYPE_STREAM, typeLen) == 0 || memcmp(type, TYPE_PACKET, typeLen) == 0)
        {
            frame_t frame;
            for (int x=0;x<48;x++)
            {
                frame.data()[x] = buffer[x+4+typeLen];
            }
            auto  ftype  = decoder.decodeFrame(frame);
            rx_lsf      = decoder.getLsf();
            bool  lsfOk = rx_lsf.valid();
            if (!txOn)
            {
                streamId = (rand() & 0x7ff);
                voiceFrameCnt = 0;
                frameCnt = 0;
                validFrame = false;
                lastFrameNum = 0;
                start_time = time(NULL);
            }
            if (lsfOk)
            {
                if (m17ReflConnected && gwTxBuffer.getSpace() >= respLen)
                {
                    for (uint8_t x=0;x<respLen;x++)
                        gwTxBuffer.put(buffer[x]);
                }
                if (!txOn)
                {
                    if (write(sockfd, setMode, 5) < 0)
                    {
                        fprintf(stderr, "ERROR: host disconnect\n");
                        break;
                    }
                }
                txOn = true;
                validFrame = true;
                pkt_lsf = decoder.getLsf();
                strcpy(srcCallsign, rx_lsf.getSource().c_str());
                strcpy(dstCallsign, rx_lsf.getDestination().c_str());
                // Retrieve extended callsign data
                if (!smsStarted)
                    packetData[0] = 0x0;
                streamType_t streamType = rx_lsf.getType();

                if ((streamType.fields.encType   == M17_ENCRYPTION_NONE) &&
                    (streamType.fields.encSubType == M17_META_EXTD_CALLSIGN))
                {
                    meta_t& meta = rx_lsf.metadata();
                    strcpy(exCall1, decode_callsign(meta.extended_call_sign.call1).c_str());
                    strcpy(exCall2, decode_callsign(meta.extended_call_sign.call2).c_str());

                    if (frameCnt == 6)
                        frameCnt = 0;
                }
                // Check if metatext is present
                else if ((streamType.fields.encType   == M17_ENCRYPTION_NONE) &&
                         (streamType.fields.encSubType == M17_META_TEXT) &&
                          rx_lsf.valid() && frameCnt == 6)
                {
                    frameCnt = 0;
                    meta_t& meta = rx_lsf.metadata();
                    uint8_t blk_len = (meta.raw_data[0] & 0xf0) >> 4;
                    uint8_t blk_id = (meta.raw_data[0] & 0x0f);
                    if (blk_id == 1)
                    {  // On first block reset everything
                        memset(metaText, 0, 53);
                        memset(textBuffer, 0, 53);
                        textOffset = 0;
                        blk_id_tot = 0;
                        textStarted = true;
                    }
                    // check if first valid metatext block is found
                    if (textStarted)
                    {
                        // Check for valid block id
                        if (blk_id <= 0x0f)
                        {
                            blk_id_tot += blk_id;
                            memcpy(textBuffer+textOffset, meta.raw_data+1, 13);
                            textOffset += 13;
                            // Check for completed text message
                            if ((blk_len == blk_id_tot) || textOffset == 52)
                            {
                                memcpy(metaText, textBuffer, textOffset);
                                textOffset = 0;
                                blk_id_tot = 0;
                                textStarted = false;
                                if (debugM)
                                    fprintf(stderr, "Text: %s\n", metaText);
                            }
                        }
                    }
                }
                else if ((streamType.fields.encType   == M17_ENCRYPTION_NONE) &&
                         (streamType.fields.encSubType == M17_META_GNSS) &&
                          rx_lsf.valid())
                {
                    gpsFound = true;
                }

                if (modem_duplex && ftype == M17FrameType::LINK_SETUP && validFrame)
                {
                    if (write(sockfd, buffer, respLen) < 0)
                    {
                        fprintf(stderr, "ERROR: host disconnect\n");
                        break;
                    }
                }
                else if (ftype == M17FrameType::STREAM && validFrame)
                {
                    if (!txOn)
                    {
                        if (write(sockfd, setMode, 5) < 0)
                        {
                            fprintf(stderr, "ERROR: host disconnect\n");
                            break;
                        }
                    }
                    M17StreamFrame sf = decoder.getStreamFrame();
                    uint16_t diff = (sf.getFrameNumber() & 0x7fff) - (lastFrameNum & 0x7fff);
                    if ((sf.getFrameNumber() && 0x8000) != 0x8000)
                    {
                        if (diff > 2 || sf.getFrameNumber() == 0)
                        {
                            fprintf(stderr, "Frame number invalid.\n");
                            lastFrameNum = sf.getFrameNumber() & 0x7fff;
                            continue;;
                        }
                    }
                    else
                    {
                        fprintf(stderr, "Last frame detected.\n");
                    }
                    lastFrameNum = sf.getFrameNumber();
                    frameCnt++;
                    if (!txOn)
                        voiceFrameCnt = 0;
                    txOn = true;
                    meta_t& meta = rx_lsf.metadata();
                    if (!gpsFound && streamType.fields.encSubType == M17_META_TEXT)
                    {
                        uint8_t blk_len = (meta.raw_data[0] & 0xf0) >> 4;
                        uint8_t blk_id = (meta.raw_data[0] & 0x0f);
                        if (blk_id == 1)
                        {  // On first block reset everything
                            memset(metaText, 0, 53);
                            memset(textBuffer, 0, 53);
                            textOffset = 0;
                            blk_id_tot = 0;
                            textStarted = true;
                        }
                        // check if first valid metatext block is found
                        if (textStarted)
                        {
                            // Check for valid block id
                            if (blk_id <= 0x0f)
                            {
                                blk_id_tot += blk_id;
                                memcpy(textBuffer+textOffset, meta.raw_data+1, 13);
                                textOffset += 13;
                                // Check for completed text message
                                if ((blk_len == blk_id_tot) || textOffset == 52)
                                {
                                    memcpy(metaText, textBuffer, textOffset);
                                    textOffset = 0;
                                    blk_id_tot = 0;
                                    textStarted = false;
                                    if (debugM)
                                        fprintf(stderr, "Text 2: %s\n", metaText);
                                }
                            }
                        }
                    }
                    else
                    {
                        float    ltm = 90.0f / 8388607.0f;
                        float    lgm = 180.0f / 8388607.0f;
                        uint8_t  data_source = (meta.raw_data[0] & 0xf0) >> 4;
                        uint8_t  station_type = meta.raw_data[0] & 0x0f;
                        uint8_t  validity = (meta.raw_data[1] & 0xf0) >> 4;
                        uint8_t  radius = (meta.raw_data[1] & 0x0e) >> 1;
                        uint16_t bearing = ((meta.raw_data[1] & 01) << 8) + meta.raw_data[2];
                        int32_t  lat = (meta.raw_data[3] << 16) + (meta.raw_data[4] << 8) + meta.raw_data[5];
                        int32_t  lon = (meta.raw_data[6] << 16) + (meta.raw_data[7] << 8) + meta.raw_data[8];
                        uint16_t altitude = (((meta.raw_data[9] << 8) + meta.raw_data[10]) * 0.5f) - 500;
                        uint16_t speed = ((meta.raw_data[11] << 4) + ((meta.raw_data[12] & 0xf0) >> 4)) * 0.5f;
                        float    latitude = lat * ltm;
                        float    longitude = (lon * lgm) - 360.0f;
                        if (debugM)
                            fprintf(stderr, "Lat: %f  Lon: %f  Alt: %d\n", latitude, longitude, altitude);
                        sprintf(gps, "%f %f %d %d %d", latitude, longitude, altitude, bearing, speed);
                        gpsFound = false;
                    }
                    if (voiceFrameCnt == 0)
                        saveLastCall("M17", "RF", srcCallsign, dstCallsign, metaText, NULL, "", true);
                    voiceFrameCnt++;

                    if (modem_duplex)
                    {
                        if (write(sockfd, buffer, respLen) < 0)
                        {
                            fprintf(stderr, "ERROR: host disconnect\n");
                            break;
                        }
                    }
                }
                else if (ftype == M17FrameType::PACKET && validFrame)
                {
                    M17PacketFrame pf = decoder.getPacketFrame();
                    if (!smsStarted && pf.payload()[0] == 0x05)
                    {  // check for valid SMS packet message
                        smsLastFrame = 0;
                        smsStarted = true;
                        totalSMSLength = 0;
                        totalSMSMessages = 0;
                        memset(packetData, 0, 821);
                        fprintf(stderr, "Packet data detected.\n");
                    }

                    // store next frame of message
                    if (smsStarted)
                    {
                        uint8_t rx_fn   = (pf.payload()[25] >> 2) & 0x1F;
                        uint8_t rx_last =  pf.payload()[25] >> 7;
                        if (rx_fn <= 31 && rx_fn == smsLastFrame && !rx_last)
                        {
                            memcpy(&packetData[totalSMSLength], pf.payload().data(), 25);
                            smsLastFrame++;
                            totalSMSLength += 25;
                        }
                        else if (rx_last)
                        {
                            memcpy(&packetData[totalSMSLength], pf.payload().data(), rx_fn < 25 ? rx_fn : 25);
                            totalSMSLength += rx_fn < 25 ? rx_fn : 25;
                            // check crc matches
                            uint16_t packet_crc = rx_lsf.m17Crc(packetData, totalSMSLength - 2);
                            uint16_t crc;
                            memcpy((uint8_t*)&crc, &packetData[totalSMSLength - 2], 2);
                            // store completed message into message queue
                            char *tmp = (char*)malloc(totalSMSLength-3);
                            if (tmp != NULL && crc == packet_crc)
                            {
                                memset(tmp, 0, totalSMSLength-3);
                                memcpy(tmp, &packetData[1], totalSMSLength-3);
                                strcpy((char*)packetData, tmp);
                                totalSMSMessages++;
                            }
                            else
                            {   // if message memory allocation fails, crc does not match
                                // or duplicate message delete sender call
                                if (tmp != NULL)
                                    free(tmp);
                            }
                            smsStarted    = false;
                        }
                    }

                    if (totalSMSMessages > 0)
                        saveLastCall("M17", "RF", srcCallsign, dstCallsign, "Packet", (const char*)packetData, "", true);

                    if (modem_duplex)
                    {
                        if (write(sockfd, buffer, respLen) < 0)
                        {
                            fprintf(stderr, "ERROR: host disconnect\n");
                            break;
                        }
                    }
                }
            }
        }
        else if (memcmp(type, TYPE_EOT, typeLen) == 0 && validFrame)
        {
            if (m17ReflConnected && gwTxBuffer.getSpace() >= respLen)
            {
                for (uint8_t x=0;x<respLen;x++)
                    gwTxBuffer.put(buffer[x]);
            }

            frameCnt = 0;
            lastFrameNum = 0;
            rx_lsf.clear();
            decoder.reset();
            if (debugM)
                fprintf(stderr, "Found M17 EOT\n");
            float loss_BER = (float)decoder.bitErr / 3.68F;
            duration = difftime(time(NULL), start_time);
            if (voiceFrameCnt > 0)
            {
                saveLastCall("M17", "RF", srcCallsign, dstCallsign, metaText, NULL, gps, false);
                saveHistory("M17", "RF", srcCallsign , dstCallsign, loss_BER, metaText, duration);
            }
            else if (totalSMSMessages > 0)
            {
                saveLastCall("M17", "RF", srcCallsign, dstCallsign, "Packet", (const char*)packetData, gps, false);
                saveHistory("M17", "RF", srcCallsign , dstCallsign, loss_BER, "Packet", duration);
            }
            voiceFrameCnt = 0;
            gps[0] = 0;
            if (modem_duplex)
            {
                if (write(sockfd, buffer, respLen) < 0)
                {
                    fprintf(stderr, "ERROR: host disconnect\n");
                    break;
                }
            }
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

// Send bytes to gateway.
void* txThread(void *arg)
{
    int  sockfd = (intptr_t)arg;
    uint16_t loop = 0;

    while (1)
    {
        delay(100);
        loop++;

        if (loop > 100)
        {
            if (gwTxBuffer.getData() >= 5)
            {
                if (gwTxBuffer.peek() != 0x61)
                {
                    fprintf(stderr, "TX invalid header.\n");
                    continue;
                }
                uint8_t  byte[2];
                uint16_t len = 0;
                gwTxBuffer.npeek(byte[0], 1);
                gwTxBuffer.npeek(byte[1], 2);
                len = (byte[0] << 8) + byte[1];;
                if (gwTxBuffer.getData() >= len)
                {
                    uint8_t buf[len];
                    for (int i=0;i<len;i++)
                    {
                        gwTxBuffer.get(buf[i]);
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

        if (gwCommand.getData() >= 5)
        {
            if (gwCommand.peek() != 0x61)
            {
                fprintf(stderr, "TX invalid header.\n");
            }
            else
            {
                uint8_t  byte[2];
                uint16_t len = 0;
                gwCommand.npeek(byte[0], 1);
                gwCommand.npeek(byte[1], 2);
                len = (byte[0] << 8) + byte[1];;
                if (gwCommand.getData() >= len)
                {
                    uint8_t buf[len];
                    for (int i=0;i<len;i++)
                    {
                        gwCommand.get(buf[i]);
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

// Process bytes from gateway.
void *processGatewaySocket(void *arg)
{
    int      childfd = (intptr_t)arg;
    ssize_t  len = 0;
    uint8_t  offset = 0;
    uint16_t respLen = 0;
    uint8_t  typeLen = 0;
    uint8_t  buffer[BUFFER_SIZE];
    char     gps[50] = "";

    m17GWConnected = true;
    addGateway("main", "M17");
    txOn = false;

    while (1)
    {
        int len = read(childfd, buffer, 1);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            error((char*)"ERROR: M17 gateway connection closed remotely.");
            break;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            delay(5);
        }

        if (len != 1)
        {
            fprintf(stderr, "M17_Service: error when reading from M17 gateway, errno=%d\n", errno);
            close(childfd);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "M17_Service: unknown byte from M17 gateway, 0x%02X\n", buffer[0]);
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
            dump((char*)"M17 Gateway data:", (unsigned char*)buffer, respLen);

        if (memcmp(type, TYPE_NACK, typeLen) == 0)
        {
            m17ReflConnected = false;
            char tmp[8];
            bzero(tmp, 8);
            memcpy(tmp, buffer+4+typeLen, 7);
            ackDashbCommand("reflLinkM17", "failed");
            setReflectorStatus("M17", (const char*)tmp, "Unlinked");
        }
        else if (memcmp(type, TYPE_CONNECT, typeLen) == 0)
        {
            m17ReflConnected = true;
            ackDashbCommand("reflLinkM17", "success");
            char tmp[8];
            bzero(tmp, 8);
            memcpy(tmp, buffer+4+typeLen, 7);
            char module[2];
            bzero(module, 2);
            module[0] = buffer[15];
            setReflectorStatus("M17", (const char*)tmp, module);
        }
        else if (memcmp(type, TYPE_DISCONNECT, typeLen) == 0)
        {
            m17ReflConnected = false;
            char tmp[8];
            bzero(tmp, 8);
            memcpy(tmp, buffer+4+typeLen, 7);
            ackDashbCommand("reflLinkM17", "success");
            setReflectorStatus("M17", (const char*)tmp, "Unlinked");
        }
        else if (memcmp(type, TYPE_STATUS, typeLen) == 0)
        {
        }
        else if (memcmp(type, TYPE_LSF, typeLen) == 0)
        {
            reflBusy = true;
            start_time = time(NULL);
            decoder.reset();
            rx_lsf.clear();
            smsStarted = false;
            duration = 0;
            if (!txOn)
                write(sockfd, setMode, 5);
            txOn = true;
            bzero(metaText, 53);
            strcpy(srcCallsign, "N0CALL");
            strcpy(dstCallsign, "N0CALL");
            frame_t frame;
            for (int x=0;x<48;x++)
            {
                frame.data()[x] = buffer[x+4+typeLen];
            }
            auto  ftype = decoder.decodeFrame(frame);
            rx_lsf      = decoder.getLsf();
            strcpy(srcCallsign, rx_lsf.getSource().c_str());
            strcpy(dstCallsign, rx_lsf.getDestination().c_str());
            streamType_t streamType = rx_lsf.getType();
            if ((streamType.fields.encType   == M17_ENCRYPTION_NONE) &&
                (streamType.fields.encSubType == M17_META_TEXT))
            {
                meta_t& meta = rx_lsf.metadata();
                memcpy(metaText, meta.raw_data+1, 13);
            }
            else if ((streamType.fields.encType   == M17_ENCRYPTION_NONE) &&
                     (streamType.fields.encSubType == M17_META_GNSS) &&
                      rx_lsf.valid())
            {
                meta_t&  meta = rx_lsf.metadata();
                float    ltm = 90.0f / 8388607.0f;
                float    lgm = 180.0f / 8388607.0f;
                uint8_t  data_source = (meta.raw_data[0] & 0xf0) >> 4;
                uint8_t  station_type = meta.raw_data[0] & 0x0f;
                uint8_t  validity = (meta.raw_data[1] & 0xf0) >> 4;
                uint8_t  radius = (meta.raw_data[1] & 0x0e) >> 1;
                uint16_t bearing = ((meta.raw_data[1] & 01) << 8) + meta.raw_data[2];
                int32_t  lat = (meta.raw_data[3] << 16) + (meta.raw_data[4] << 8) + meta.raw_data[5];
                int32_t  lon = (meta.raw_data[6] << 16) + (meta.raw_data[7] << 8) + meta.raw_data[8];
                uint16_t altitude = (((meta.raw_data[9] << 8) + meta.raw_data[10]) * 0.5f) - 500;
                uint16_t speed = ((meta.raw_data[11] << 4) + ((meta.raw_data[12] & 0xf0) >> 4)) * 0.5f;
                float    latitude = lat * ltm;
                float    longitude = (lon * lgm) - 360.0f;
                fprintf(stderr, "Lat: %f  Lon: %f  Alt: %d\n", latitude, longitude, altitude);
                sprintf(gps, "%f %f %d %d %d", latitude, longitude, altitude, bearing, speed);
            }
            write(sockfd, buffer, respLen);
            saveLastCall("M17", "NET", srcCallsign, dstCallsign, metaText, NULL, "", true);
        }
        else if (memcmp(type, TYPE_STREAM, typeLen) == 0)
        {
            txOn = true;
            write(sockfd, buffer, respLen);
        }
        else if (memcmp(type, TYPE_PACKET, typeLen) == 0)
        {
            txOn = true;
            frame_t frame;
            for (int x=0;x<48;x++)
            {
                frame.data()[x] = buffer[x+4+typeLen];
            }
            auto  ftype = decoder.decodeFrame(frame);
            M17PacketFrame pf = decoder.getPacketFrame();
            if (!smsStarted && pf.payload()[0] == 0x05)
            {  // check for valid SMS packet message
                smsLastFrame = 0;
                smsStarted = true;
                totalSMSLength = 0;
                totalSMSMessages = 0;
                memset(packetData, 0, 821);
            }

            // store next frame of message
            if (smsStarted)
            {
                uint8_t rx_fn   = (pf.payload()[25] >> 2) & 0x1F;
                uint8_t rx_last =  pf.payload()[25] >> 7;
                if (rx_fn <= 31 && rx_fn == smsLastFrame && !rx_last)
                {
                    memcpy(&packetData[totalSMSLength], pf.payload().data(), 25);
                    smsLastFrame++;
                    totalSMSLength += 25;
                }
                else if (rx_last)
                {
                    memcpy(&packetData[totalSMSLength], pf.payload().data(), rx_fn < 25 ? rx_fn : 25);
                    totalSMSLength += rx_fn < 25 ? rx_fn : 25;
                    // check crc matches
                    uint16_t packet_crc = rx_lsf.m17Crc(packetData, totalSMSLength - 2);
                    uint16_t crc;
                    memcpy((uint8_t*)&crc, &packetData[totalSMSLength - 2], 2);
                    // store completed message into message queue
                    char *tmp = (char*)malloc(totalSMSLength-3);
                    if (tmp != NULL && crc == packet_crc)
                    {
                        memset(tmp, 0, totalSMSLength-3);
                        memcpy(tmp, &packetData[1], totalSMSLength-3);
                        strcpy((char*)packetData, tmp);
                        totalSMSMessages++;
                    }
                    else
                    {   // if message memory allocation fails, crc does not match
                        // or duplicate message delete sender call
                        if (tmp != NULL)
                            free(tmp);
                    }
                    smsStarted    = false;
                }
            }

            if (totalSMSMessages > 0)
            {
                saveLastCall("M17", "NET", srcCallsign, dstCallsign, "Packet", (const char*)packetData, "", false);
                saveHistory("M17", "NET", srcCallsign , dstCallsign, 0, "Packet", 0);
                smsStarted = false;
            }

            write(sockfd, buffer, respLen);
        }
        else if (memcmp(type, TYPE_EOT, typeLen) == 0)
        {
            write(sockfd, buffer, respLen);
            duration = difftime(time(NULL), start_time);
            if (totalSMSMessages == 0)
            {
                saveLastCall("M17", "NET", srcCallsign, dstCallsign, metaText, NULL, gps, false);
                saveHistory("M17", "NET", srcCallsign , dstCallsign, 0, metaText, duration);
            }
            txOn = false;
            totalSMSMessages = 0;
            gps[0] = 0;
        }
        delay(5);
    }
    fprintf(stderr, "Gateway disconnected.\n");
    m17GWConnected = false;
    m17ReflConnected = false;
    delGateway("main", "M17");
    clearReflLinkStatus("M17");
    int iRet = 100;
    pthread_exit(&iRet);
    return 0;
}

// Listem for incoming gateway connection.
void *startTCPServer(void *arg)
{
    struct hostent *hostp; /* client host info */
    int childfd; /* child socket */
    int sockFd;

    sockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd < 0)
    {
        fprintf(stderr, "M17_Service: error when creating the socket: %s\n", strerror(errno));
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
        fprintf(stderr, "M17_Service: error when binding the socket to port %u: %s\n", serverPort, strerror(errno));
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
                fprintf(stdout, "M17_Service: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: M17_Service [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            fprintf(stderr, "M17_Service: error in fork(), exiting\n");
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
    clearReflLinkStatus("M17");

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
            if (m17GWConnected)
            {
                std::string parameter = readDashbCommand("reflLinkM17");
                if (parameter.empty())
                {
                    statusTimeout = false;
                    continue;
                }
                if (parameter == "unlink")
                {
                    gwCommand.put(0x61);
                    gwCommand.put(0x00);
                    gwCommand.put(0x08);
                    gwCommand.put(0x04);
                    gwCommand.put(TYPE_DISCONNECT[0]);
                    gwCommand.put(TYPE_DISCONNECT[1]);
                    gwCommand.put(TYPE_DISCONNECT[2]);
                    gwCommand.put(TYPE_DISCONNECT[3]);
                    sleep(3);
                    statusTimeout = false;
                    continue;
                }
                else if (!m17ReflConnected)
                {
                    char tmp[41];
                    strcpy(tmp, parameter.c_str());
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
                            gwCommand.put(0x61);
                            gwCommand.put(0x00);
                            gwCommand.put(0x16);
                            gwCommand.put(0x04);
                            gwCommand.put(TYPE_CONNECT[0]);
                            gwCommand.put(TYPE_CONNECT[1]);
                            gwCommand.put(TYPE_CONNECT[2]);
                            gwCommand.put(TYPE_CONNECT[3]);
                            std::string callsign = readHostConfig("main", "callsign");
                            for (uint8_t x=0;x<6;x++)
                                gwCommand.put(callsign.c_str()[x]);
                            for (uint8_t x=0;x<7;x++)
                                gwCommand.put(name[x]);
                            gwCommand.put(module[0]);
                            sleep(3);
                        }
                        else
                            ackDashbCommand("reflLinkM17", "failed");
                    }
                    else
                        ackDashbCommand("reflLinkM17", "failed");
                }
                else
                   ackDashbCommand("reflLinkM17", "failed");
            }
            else
            {
                std::string parameter = readDashbCommand("reflLinkM17");
                if (!parameter.empty())
                {
                    ackDashbCommand("reflLinkM17", "No gateway");
                }
            }
            statusTimeout = false;
        }
        delay(500000);
    }
    fprintf(stderr, "M17 service terminated.\n");
    logError("main", "M17 host terminated.");
}
