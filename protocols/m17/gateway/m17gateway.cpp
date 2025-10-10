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

#include "../../tools/CRingBuffer.h"
#include "../../tools/tools.h"

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

using namespace std;
using namespace M17;

#define VERSION     "2025-08-15"
#define BUFFER_SIZE 1024

pthread_t m17Reflid;
pthread_t clientid;

int      clientPort        = 18100;
char     M17Name[8]        = "";
char     M17Module[2]      = "T";
char     metaText[53]      = "";
char     tx_state[4]       = "off";
char     m17Host[80]       = "127.0.0.1";
char     srcCallsign[10]   = "";
char     dstCallsign[10]   = "";
bool     m17ReflDisconnect = false;
bool     m17ReflConnected  = false;
bool     reflPacketRdy     = false;
bool     connected         = true;
bool     debugM            = false;
bool     txOn              = false;
bool     smsStarted        = false;
uint8_t  duration          = 0;
uint8_t  smsLastFrame      = 0;
uint8_t  totalSMSMessages  = 0;
uint8_t  packetData[823]   = {};
uint16_t totalSMSLength    = 0;
uint16_t streamId          = 0;
time_t   start_time;

std::string currentM17Refl("");
std::string rptrCallsign("N0CALL");

M17::M17LinkSetupFrame rx_lsf;          ///< M17 link setup frame
M17::M17LinkSetupFrame pkt_lsf;      ///< M17 packet link setup frame
M17::M17FrameDecoder   decoder;      ///< M17 frame decoder
M17::M17FrameEncoder   encoder;      ///< M17 frame encoder

RingBuffer<uint8_t> reflTxBuffer(3600);
RingBuffer<uint8_t> txBuffer(3300);

const int   magicLen             = 4;
const char *magicACKN           = "ACKN";
const char *magicCONN           = "CONN";
const char *magicDISC           = "DISC";
const char *magicLSTN           = "LSTN";
const char *magicNACK           = "NACK";
const char *magicPING           = "PING";
const char *magicPONG           = "PONG";
const char *magicM17Voice       = "M17 ";
const char *magicM17VoiceHeader = "M17H";
const char *magicM17VoiceData   = "M17D";
const char *magicM17Packet      = "M17P";

const char *TYPE_LSF            = "M17L";
const char *TYPE_STREAM         = "M17S";
const char *TYPE_PACKET         = "M17P";
const char *TYPE_EOT            = "M17E";
const char *TYPE_NACK           = "NACK";
const char *TYPE_DISCONNECT     = "DISC";
const char *TYPE_CONNECT        = "CONN";
const char *TYPE_STATUS         = "STAT";


// M17 Packets
//all structures must be big endian on the wire, so you'll want htonl (man byteorder 3) and such.
using SM17Lich = struct __attribute__((__packed__)) lich_tag {
    uint8_t  addr_dst[6];
    uint8_t  addr_src[6];
    uint16_t frametype;      //frametype flag field per the M17 spec
    uint8_t  meta_data[14];  //bytes for the meta data, as described in the M17 spec
}; // 6 + 6 + 2 + 14 = 28 bytes

//without SYNC or other parts
using SM17Frame = struct __attribute__((__packed__)) m17_tag {
    uint8_t  magic[4];
    uint16_t streamid;
    SM17Lich lich;
    uint16_t framenumber;
    uint8_t  payload[16];
    uint16_t crc;     //16 bit CRC
}; // 4 + 2 + 28 + 2 + 16 + 2 = 54 bytes

using SM17Packet = struct __attribute__((__packed__)) pkt_tag {
    uint8_t magic[4];
    SM17Lich lich;
    uint16_t crc;     //16 bit CRC
};


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

// Encode to M17 callsign bytes.
bool encode_refl_callsign(const std::string& callsign, call_t& encodedCall)
{
    encodedCall.fill(0x00);
    if(callsign.size() > 9) return false;

    // Encode the characters to base-40 digits.
    uint64_t address = 0;

    const char *p = callsign.c_str();
    // find the last char , but don 't select more than 9 characters
    while (*p++ && (p - callsign.c_str() < 9));
    // process each char from the end to the beginning
    for (p--;p >= callsign.c_str();p--)
    {
        unsigned val = 0; // the default value of the character
        if ('A' <= *p && *p <= 'Z')
            val = *p - 'A' + 1;
        else if ('0' <= *p && *p <= '9')
            val = *p - '0' + 27;
        else if ('-' == *p)
            val = 37;
        else if ('/' == *p)
            val = 38;
        else if ('.' == *p)
            val = 39;
        else if ('a' <= *p && *p <= 'z')
            val = *p - 'a' + 1;
        address = 40 * address + val ; // increment and add
    }

    for (int i=5;i>=0;i--) // put it in network byte order
    {
        encodedCall[i] = address & 0xff;
        address /= 0x100;
    }
    return true;
}

// Send out going bytes to M17 service.
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

// Connect and process bytes from M17 reflector.
void *connectM17Refl(void *argv)
{
    int sockfd, portno, n;
    int hostfd;
    int serverlen;
    int timeout = 0;
    uint16_t fn = 0;
    struct sockaddr_in serveraddr;
    struct hostent *server;
    char hostname[80];
    char source[9];
    char module[2];
    uint8_t buf[1024];
    timer_t t_id = NULL;

    sscanf((char*)argv, "%d %s %s %s %d", &hostfd, source, hostname, module, &portno);
    printf("%s  %s  %s  %d\n", source, hostname, module, portno);
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

    bzero(buf, 1024);

    /* send connect message to the server */
    serverlen = sizeof(serveraddr);
    call_t srcCall;
    encode_refl_callsign(rptrCallsign, srcCall);
    strcpy((char*)buf, magicCONN);
    memcpy(buf+4, srcCall.data(), 6);
    buf[10] = module[0];
    if (debugM)
       dump((char*)"M17 Refl data:", (uint8_t*)buf, 11);

    n = sendto(sockfd, buf, 11, 0, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
    if (n < 0)
      error((char*)"ERROR in sendto");

    if (debugM)
        fprintf(stderr, "Entering M17 reflector comm loop.\n");

    sleep(1);
    m17ReflConnected = false;
    buf[0] = 0x61;
    buf[1] = 0x00;
    buf[2] = 0x10;
    buf[3] = 0x04;
    txBuffer.addData(buf, 4);

    bzero(buf, 1024);
    n = recvfrom(sockfd, buf, 1023, 0, (struct sockaddr*)&serveraddr, (socklen_t*)&serverlen);
    if (n > 0)
    {
        if (debugM)
            dump((char*)"M17 Refl recv data:", (uint8_t*)buf, n);

        char magic[4];
        memcpy(magic, buf, 4);

        if (memcmp(magic, magicACKN, 4) == 0)
        {
            m17ReflConnected = true;
            uint8_t tmp[4];
            tmp[0] = TYPE_CONNECT[0];
            tmp[1] = TYPE_CONNECT[1];
            tmp[2] = TYPE_CONNECT[2];
            tmp[3] = TYPE_CONNECT[3];
            txBuffer.addData(tmp, 4);
        }
        else
        {
            uint8_t tmp[4];
            tmp[0] = TYPE_NACK[0];
            tmp[1] = TYPE_NACK[1];
            tmp[2] = TYPE_NACK[2];
            tmp[3] = TYPE_NACK[3];
            txBuffer.addData(tmp, 4);
        }
    }
    else
    {
        uint8_t tmp[4];
        tmp[0] = TYPE_NACK[0];
        tmp[1] = TYPE_NACK[1];
        tmp[2] = TYPE_NACK[2];
        tmp[3] = TYPE_NACK[3];
        txBuffer.addData(tmp, 4);
    }

    txBuffer.addData((uint8_t*)M17Name, 7);
    txBuffer.addData((uint8_t*)M17Module, 1);

    M17::M17LinkSetupFrame lsf;
    M17::M17FrameEncoder   encoder;      ///< M17 frame encoder
    lsf.clear();
    m17ReflDisconnect = false;

    while (m17ReflConnected)
    {
        bzero(buf, 1024);

        /* print the server's reply */
        n = recvfrom(sockfd, buf, 1023, 0, (struct sockaddr*)&serveraddr, (socklen_t*)&serverlen);
        if (n > 0)
        {
            if (debugM)
                dump((char*)"M17 Refl recv data:", (uint8_t*)buf, n);

            char magic[4];
            memcpy(magic, buf, 4);

            if (memcmp(magic, magicACKN, 4) == 0)
                m17ReflConnected = true;

            if (memcmp(magic, magicNACK, 4) == 0)
                m17ReflConnected = false;

            if (memcmp(magic, magicDISC, 4) == 0)
                m17ReflConnected = false;

            if (memcmp(magic, magicPING, 4) == 0)
            {
                timeout = 0;
                encode_callsign(rptrCallsign, srcCall);
                bzero(buf, 1024);
                strcpy((char*)buf, magicPONG);
                memcpy(buf+4, srcCall.data(), 6);
                n = sendto(sockfd, buf, 10, 0, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
                if (n < 0)
                    error((char*)"ERROR in sendto");
                if (debugM)
                    dump((char*)"M17 Refl send data:", (uint8_t*)buf, n);
            }

            if (memcmp(magic, magicM17Voice, 4) == 0 && !txOn)
            {
                if (txBuffer.freeSpace() < 52)
                {
                    uint8_t tmp[1];
                    txBuffer.peek(tmp, 1);
                    fprintf(stderr, "**********************Buffer full [%02X]\n", tmp[0]);
                    continue;
                }
                if (debugM)
                    dump((char*)"M17 Refl recv data:", (uint8_t*)buf, n);

                SM17Frame sf;
                memcpy((uint8_t*)&sf, buf, sizeof(sf));
                memcpy((uint8_t*)lsf.getData(), sf.lich.addr_dst, 6);
                memcpy((uint8_t*)&lsf.getData()[6], sf.lich.addr_src, 6);
                strcpy(dstCallsign, lsf.getDestination().c_str());
                strcpy(srcCallsign, lsf.getSource().c_str());
                lsf.setMetaText(sf.lich.meta_data);
                streamType_t st;
                st.fields.dataMode = 1;
                st.fields.dataType = 2;
                st.fields.encType = 0;
                st.fields.encSubType = 0;
        //    fprintf(stderr, "CAN: %d\n", st.fields.CAN);
                st.fields.CAN = 0;
                lsf.setType(st);
                lsf.updateCrc();

                frame_t m17Frame;
                uint8_t len = 0;
                if (strcmp(tx_state, "on") != 0)
                {
                    encoder.reset();
                    start_time = time(NULL);
                    encoder.encodeLsf(lsf, m17Frame);
                    uint8_t tmp[4];
                    tmp[0] = 0x61;
                    len = 48 + 4 + 4;
                    tmp[1] = 0x00;
                    tmp[2] = len;
                    tmp[3] = 0x04;
                    txBuffer.addData(tmp, 4);
                    txBuffer.addData((uint8_t*)TYPE_LSF, 4);
                    txBuffer.addData(m17Frame.data(), 48);
                    strcpy(tx_state, "on");
                }
                payload_t dataFrame;
                memcpy(dataFrame.data(), sf.payload, 16);
                encoder.encodeStreamFrame(dataFrame, m17Frame, ((sf.framenumber & 0x0080) == 0x0080));

                uint8_t tmp[4];
                tmp[0] = 0x61;
                len = 48 + 4 + 4;
                tmp[1] = 0x00;
                tmp[2] = len;
                tmp[3] = 0x04;
                txBuffer.addData(tmp, 4);
                txBuffer.addData((uint8_t*)TYPE_STREAM, 4);
                txBuffer.addData(m17Frame.data(), 48);
                if (debugM)
                    printf("FN: %04X\n", sf.framenumber);
                if ((sf.framenumber & 0x0080) == 0x0080)
                {
                    tmp[0] = 0x61;
                    len = 48 + 4 + 4;
                    tmp[1] = 0x00;
                    tmp[2] = len;
                    tmp[3] = 0x04;
                    txBuffer.addData(tmp, 4);
                    txBuffer.addData((uint8_t*)TYPE_STREAM, 4);
                    txBuffer.addData(m17Frame.data(), 48);
                    duration = difftime(time(NULL), start_time);
                    strcpy(tx_state, "off");

                    lsf.clear();
                    encoder.encodeEotFrame(m17Frame);
                    tmp[0] = 0x61;
                    len = 48 + 4 + 4;
                    tmp[1] = 0x00;
                    tmp[2] = len;
                    tmp[3] = 0x04;
                    txBuffer.addData(tmp, 4);
                    txBuffer.addData((uint8_t*)TYPE_EOT, 4);
                    txBuffer.addData(m17Frame.data(), 48);
                }
                timeout = 0;
            }

            if (memcmp(magic, magicM17Packet, 4) == 0 && !txOn)
            {
                uint8_t len = 0;
                strcpy(tx_state, "on");
                SM17Packet sp;
                memcpy((uint8_t*)&sp, buf, sizeof(sp));
                M17::M17LinkSetupFrame lsf;
                memcpy((uint8_t*)lsf.getData(), sp.lich.addr_dst, 6);
                memcpy((uint8_t*)&lsf.getData()[6], sp.lich.addr_src, 6);
                std::string dst = lsf.getDestination();
                std::string src = lsf.getSource();
                strcpy(dstCallsign, lsf.getDestination().c_str());
                strcpy(srcCallsign, lsf.getSource().c_str());
                strcpy(metaText, (char*)lsf.metadata().raw_data);

                streamType_t type;
                type.fields.dataMode   = M17_DATAMODE_PACKET;     // Packet
                type.fields.dataType   = 0;
                type.fields.CAN        = 0;  // Channel access number

                lsf.setType(type);
                lsf.updateCrc();

                encoder.reset();
                frame_t m17Frame;
                encoder.encodeLsf(lsf, m17Frame);
                uint8_t tmp[4];
                tmp[0] = 0x61;
                len = 48 + 4 + 4;
                tmp[1] = 0x00;
                tmp[2] = len;
                tmp[3] = 0x04;
                txBuffer.addData(tmp, 4);
                txBuffer.addData((uint8_t*)TYPE_LSF, 4);
                txBuffer.addData(m17Frame.data(), 48);
                timeout = 0;

                memset(packetData, 0, 823);
                int numPacketbytes = 0;
                for (int i=34;i<n-2;i++)
                {
                    if ((i-34) < 823) // limit to 823 chars
                    {
                        packetData[i-34] = buf[i];
                        numPacketbytes++;
                    }
                }
                uint16_t packet_crc          = lsf.m17Crc(packetData, numPacketbytes);
                packetData[numPacketbytes]   = packet_crc & 0xFF;
                packetData[numPacketbytes+1] = packet_crc >> 8;
                numPacketbytes += 2; //count 2-byte CRC too

                pktPayload_t packetFrame;
                uint8_t cnt = 0;
                while(numPacketbytes > 25)
                {
                    memcpy(packetFrame.data(), &packetData[cnt*25], 25);
                    packetFrame[25] = cnt << 2;
                    encoder.encodePacketFrame(packetFrame, m17Frame);
                    tmp[0] = 0x61;
                    len = 48 + 4 + 4;
                    tmp[1] = 0x00;
                    tmp[2] = len;
                    tmp[3] = 0x04;
                    txBuffer.addData(tmp, 4);
                    txBuffer.addData((uint8_t*)TYPE_PACKET, 4);
                    txBuffer.addData(m17Frame.data(), 48);
                    cnt++;
                    numPacketbytes -= 25;
                }

                memset(packetFrame.data(), 0, 26);
                memcpy(packetFrame.data(), &packetData[cnt*25], numPacketbytes);
                packetFrame[25] = 0x80 | (numPacketbytes << 2);
                encoder.encodePacketFrame(packetFrame, m17Frame);

                tmp[0] = 0x61;
                len = 48 + 4 + 4;
                tmp[1] = 0x00;
                tmp[2] = len;
                tmp[3] = 0x04;
                txBuffer.addData(tmp, 4);
                txBuffer.addData((uint8_t*)TYPE_PACKET, 4);
                txBuffer.addData(m17Frame.data(), 48);

                encoder.encodeEotFrame(m17Frame);

                tmp[0] = 0x61;
                len = 48 + 4 + 4;
                tmp[1] = 0x00;
                tmp[2] = len;
                tmp[3] = 0x04;
                txBuffer.addData(tmp, 4);
                txBuffer.addData((uint8_t*)TYPE_EOT, 4);
                txBuffer.addData(m17Frame.data(), 48);

                strcpy(tx_state, "off");
            }
        }

        if (m17ReflDisconnect)
        {
            strcpy((char*)buf, magicDISC);
            memcpy(buf+4, srcCall.data(), 6);
            n = sendto(sockfd, buf, 10, 0, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
            if (n < 0)
                error((char*)"ERROR in sendto");
            m17ReflConnected = false;
        }

        if (reflTxBuffer.dataSize() >= 18)
        {
            SM17Frame sf;
            strcpy((char*)sf.magic, magicM17Voice);
            sf.streamid = (streamId >> 8) | ((streamId & 0xff) << 8);
            memcpy((uint8_t*)&sf.lich, rx_lsf.getData(), 28);
            uint8_t tmp[2];
            reflTxBuffer.getData(tmp, 2);
            sf.framenumber = (tmp[1] << 8) | tmp[0];
            reflTxBuffer.getData(sf.payload, 16);
            if (debugM)
                fprintf(stderr, "FN: %d\n", (tmp[0] << 8) | tmp[1]);
            sf.crc = rx_lsf.m17Crc((uint8_t*)&sf, sizeof(sf) - 2);
            n = sendto(sockfd, (uint8_t*)&sf, sizeof(sf), 0, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
            if (n < 0)
                error((char*)"ERROR in sendto");
            if (debugM)
               dump((char*)"M17 Refl send data:", (uint8_t*)&sf, n);
        }

        if (reflPacketRdy)
        {
            strcpy((char*)buf, magicM17Packet);
            memcpy(buf+4, rx_lsf.getData(), 30);
            memset(buf+34, 0x05, 1);
            memcpy(buf+35, (uint8_t*)packetData, strlen((char*)packetData)+1);
            uint16_t crc = rx_lsf.m17Crc(buf+34, strlen((char*)packetData)+2);
            memcpy(buf+35+strlen((char*)packetData)+1, (uint8_t*)&crc, 2);
            n = sendto(sockfd, buf, 35+strlen((char*)packetData)+3, 0, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
            if (n < 0)
                error((char*)"ERROR in sendto");
            if (debugM)
               dump((char*)"M17 Refl send data:", (uint8_t*)buf, n);
            reflPacketRdy = false;
        }

        delay(1000);

        if (strcmp(tx_state, "on") == 0 && timeout >= 300000) // 5 minutes
        {
            strcpy(tx_state, "off");
            lsf.clear();
            frame_t m17Frame;
            encoder.encodeEotFrame(m17Frame);
            uint8_t len = 48 + 4 + 4;
            uint8_t tmp[len];
            tmp[0] = 0x61;
            tmp[1] = 0x00;
            tmp[2] = len;
            tmp[3] = 0x04;
            txBuffer.addData(tmp, 4);
            txBuffer.addData((uint8_t*)TYPE_EOT, 4);
            txBuffer.addData(m17Frame.data(), 48);
        }

        if (timeout > 5000) // 5 seconds
            break;
        else
            timeout++;
    }
    uint8_t tmp[4];
    tmp[0] = 0x61;
    tmp[1] = 0x00;
    tmp[2] = 0x0F;
    tmp[3] = 0x04;
    txBuffer.addData(tmp, 4);
    txBuffer.addData((uint8_t*)TYPE_DISCONNECT, 4);
    txBuffer.addData((uint8_t*)M17Name, 7);
    close(sockfd);
    sleep(1);
    fprintf(stderr, "Disconnected from M17 reflector.\n");
     int iRet = 100;
    pthread_exit(&iRet);
    return 0;
}

// Start up reflector threads.
void connectToRefl(int sockfd, char *source, char *reflector, char *module, uint16_t port)
{
    static char server[80] = "";

    m17ReflDisconnect = true;
    sleep(1);

    sprintf(server, "%d %s %s %s %d", sockfd, source, reflector, module, port);

    pthread_t txid;
    int err = pthread_create(&(txid), NULL, &txThread, (void*)(intptr_t)sockfd);
    if (err != 0)
       fprintf(stderr, "Can't create tx thread :[%s]", strerror(err));
    else
    {
        if (debugM)
            fprintf(stderr, "TX thread created successfully\n");
    }
    err = pthread_create(&(m17Reflid), NULL, &connectM17Refl, server);
    if (err != 0)
        fprintf(stderr, "Can't create M17 reflector thread :[%s]", strerror(err));
    else
    {
        if (debugM)
            fprintf(stderr, "M17 reflector thread created successfully\n");
    }
}

// Start up connection to M17 service.
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
    printf("Connected to M17 host.\n");

    ssize_t  len = 0;
    uint8_t  offset = 0;
    uint16_t respLen = 0;
    uint8_t  typeLen = 0;
    bool     lsfOk = false;
    M17FrameType ftype;

    while (connected)
    {
        int len = read(sockfd, buffer, 1);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            error((char*)"ERROR: M17 host connection closed remotely.");
            break;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            delay(5);
        }

        if (len != 1)
        {
            fprintf(stderr, "M17_Gateway: error when reading from M17 host, errno=%d\n", errno);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "M17_Gateway: unknown byte from M17 host, 0x%02X\n", buffer[0]);
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
            dump((char*)"M17 Service data:", (unsigned char*)buffer, respLen);

        if (memcmp(type, TYPE_CONNECT, typeLen) == 0)
        {
            if (!m17ReflConnected)
            {
                char source[7];
                bzero(source, 7);
                bzero(M17Name, 8);
                bzero(M17Module, 2);
                memcpy(source, buffer+8, 6);
                memcpy(M17Name, buffer+14, 7);
                memcpy(M17Module, buffer+21, 1);
                fprintf(stderr, "Name: %s  Mod: %s\n", M17Name, M17Module);
                char url[80];
                char ipaddr[15];
                uint16_t  port;
                if (findReflector("M17", M17Name, ipaddr, &port))
                {
                    connectToRefl(sockfd, source, ipaddr, M17Module, port);
                }
            }
        }
        else if (memcmp(type, TYPE_DISCONNECT, typeLen) == 0)
        {
            m17ReflDisconnect = true;
        }
        else if (memcmp(type, TYPE_STATUS, typeLen) == 0)
        {
        }
        else if (memcmp(type, TYPE_LSF, typeLen) == 0)
        {
            if (!txOn)
            {
                decoder.reset();
                rx_lsf.clear();
                streamId = (rand() & 0x7ff);
            }
            frame_t frame;
            for (uint8_t x=0;x<48;x++)
                frame.data()[x] = buffer[x+4+typeLen];
            ftype   = decoder.decodeFrame(frame);
            rx_lsf  = decoder.getLsf();
            lsfOk   = rx_lsf.valid();
        }
        else if (memcmp(type, TYPE_STREAM, typeLen) == 0)
        {
            frame_t frame;
            for (uint8_t x=0;x<48;x++)
                frame.data()[x] = buffer[x+4+typeLen];
            ftype   = decoder.decodeFrame(frame);
            rx_lsf  = decoder.getLsf();
            lsfOk   = rx_lsf.valid();
            if (lsfOk)
            {
                txOn = true;
                M17StreamFrame sf = decoder.getStreamFrame();
                if (m17ReflConnected)
                {
                    reflTxBuffer.addData(sf.getData(), 18);
                }
            }
        }
        else if (memcmp(type, TYPE_PACKET, typeLen) == 0)
        {
            frame_t frame;
            for (uint8_t x=0;x<48;x++)
                frame.data()[x] = buffer[x+4+typeLen];
            ftype   = decoder.decodeFrame(frame);
            rx_lsf  = decoder.getLsf();
            lsfOk   = rx_lsf.valid();
            if (lsfOk)
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
                    txOn = true;
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
                            reflPacketRdy = true;
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
            }
        }
        else if (memcmp(type, TYPE_EOT, typeLen) == 0)
        {
            txOn = false;
            lsfOk = false;
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
                fprintf(stdout, "M17_Gateway: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: M17_Gateway [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            fprintf(stderr, "M17_Gateway: error in fork(), exiting\n");
            return 1;
        }

        // If this is the parent, exit
        if (pid > 0)
            return 0;

        // We are the child from here onwards
        setsid();

        umask(0);
    }

    int err = pthread_create(&(clientid), NULL, &startClient, m17Host);
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

    while (1)
    {
         delay(50000);
         if (!connected)
             break;
    }
    fprintf(stderr, "M17 Gateway terminated.\n");
    logError("main", "M17 Gateway terminated.");
}
