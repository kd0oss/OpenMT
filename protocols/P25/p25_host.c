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
 *   Much of this code is from, based on or inspired by MMDVM created by   *
 *   Jonathan Naylor G4KLX                                                 *
 ***************************************************************************/

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <RingBuffer.h>
#include <tools.h>
#include <dsp_tools.h>
#include <ADF7021.h>

#include <arm_math.h>

#include "p25_func.h"

#define VERSION "2025-12-28"
#define BUFFER_SIZE 500

q31_t          DC_FILTER[]      = {3367972, 0, 3367972, 0, 2140747704, 0}; // {b0, 0, b1, b2, -a1, -a2}
const uint32_t DC_FILTER_STAGES = 1U; // One Biquad stage
bool           USE_DC_FILTER    = true;

char MODE_NAME[11]   = "P25";
char MODEM_TYPE[6]   = "4FSK";
bool USE_LP_FILTER   = false;

const char* TYPE_HEADER     = "P25H";
const char* TYPE_LDU        = "P25L";
const char* TYPE_EOT        = "P25E";
const char* TYPE_ACK        = "ACK ";
const char* TYPE_NACK       = "NACK";
const char* TYPE_DISCONNECT = "DISC";
const char* TYPE_CONNECT    = "CONN";
const char* TYPE_STATUS     = "STAT";
const char* TYPE_MODE       = "MODE";
const char* TYPE_COMMAND    = "COMM";
const char* TYPE_SAMPLE     = "SAMP";
const char* TYPE_BITS       = "BITS";

const uint8_t PACKET_TYPE_BIT   = 0;
const uint8_t PACKET_TYPE_SAMP  = 1;
const uint8_t PACKET_TYPE_FRAME = 2;

const uint8_t COMM_SET_DUPLEX  = 0x00;
const uint8_t COMM_SET_SIMPLEX = 0x01;
const uint8_t COMM_SET_MODE    = 0x02;
const uint8_t COMM_SET_IDLE    = 0x03;
const uint8_t COMM_UPDATE_CONF = 0x04;

const uint8_t SYNC_BIT_START_ERRS = 2U;
const uint8_t SYNC_BIT_RUN_ERRS   = 4U;

enum P25RX_STATE
{
    P25RXS_NONE,
    P25RXS_HDR,
    P25RXS_LDU
};

enum RF_STATE
{
    RS_RF_LISTENING,
    RS_RF_LATE_ENTRY,
    RS_RF_AUDIO,
    RS_RF_DATA_AUDIO,
    RS_RF_DATA,
    RS_RF_REJECTED,
    RS_RF_INVALID
};

uint8_t  packetType          = 0;
uint8_t  modemId             = 1;          //< Modem Id used to create modem name.
char     modemName[10]       = "modem1";   //< Modem name that this program is associated with.
char     station_call[9]     = "";
char     src_callsign[7]     = "";
char     dst_callsign[7]     = "";
bool     debugM              = false;
bool     connected           = true;
bool     modem_duplex        = false;
bool     p25ReflConnected    = false;
bool     p25GWConnected      = false;
bool     reflBusy            = false;
bool     rx_started          = false;
uint8_t  duration            = 0;
uint8_t  p25_space           = 0;
bool     txOn                = false;
uint16_t modeHang            = 30000;
uint8_t  txLevel             = 50;
uint8_t  rfPower             = 128;
time_t   start_time;
char     modem_rxFrequency[11]  = "435000000";
char     modem_txFrequency[11]  = "435000000";
int      nac                 = 0x293;
enum RF_STATE rf_state       = RS_RF_LISTENING;

char               modemHost[80]  = "127.0.0.1";
int                sockfd         = 0;
uint16_t           serverPort     = 18300;
uint16_t           clientPort     = 18000;
unsigned int       clientlen;   //< byte size of client's address
char*              hostaddrp;   //< dotted decimal host addr string
int                optval;      //< flag value for setsockopt
struct sockaddr_in serveraddr;  //< server's addr
struct sockaddr_in clientaddr;  //< client addr

RingBuffer txBuffer;
RingBuffer rxBuffer;
RingBuffer gwTxBuffer;
RingBuffer gwCommand;

/* Mutexes protecting shared RingBuffers and state variables */
pthread_mutex_t timerMutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t rxBufMutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gwTxBufMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gwCmdMutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stateMutex   = PTHREAD_MUTEX_INITIALIZER;

pthread_t modemHostid;
pthread_t gwHostid;
pthread_t timerid;

const uint8_t SETMODE[] = {0x61, 0x00, 0x05, 0x01, COMM_SET_MODE};
const uint8_t SETIDLE[] = {0x61, 0x00, 0x05, 0x01, COMM_SET_IDLE};

enum P25RX_STATE p25state;
uint32_t    p25bitBuffer[P25_RADIO_SYMBOL_LENGTH];
q15_t       p25buffer[P25_LDU_FRAME_LENGTH_SAMPLES];
uint16_t    p25bitPtr;
uint16_t    p25dataPtr;
uint16_t    p25hdrStartPtr;
uint16_t    p25lduStartPtr;
uint16_t    p25lduEndPtr;
uint16_t    p25minSyncPtr;
uint16_t    p25maxSyncPtr;
uint16_t    p25hdrSyncPtr;
uint16_t    p25lduSyncPtr;
q31_t       p25maxCorr;
uint16_t    p25lostCount;
uint8_t     p25countdown;
q15_t       p25center[16U];
q15_t       p25centerVal;
q15_t       p25threshold[16U];
q15_t       p25thresholdVal;
uint8_t     p25averagePtr;
uint8_t     p25poBuffer[250U];
uint8_t     p25txDelay;
uint16_t    p25poLen;
uint16_t    p25poPtr;

uint64_t    p25_64BitBuffer;
uint64_t    m_bitBuffer;
uint8_t     p25outBuffer[P25_LDU_FRAME_LENGTH_BYTES + 3U];
uint8_t*    m_buffer;
uint16_t    p25bufferPtr;
uint16_t    p25endPtr;

arm_biquad_casd_df1_inst_q31 dcFilter;
q31_t                        dcState[4];

arm_fir_interpolate_instance_q15 modFilter;
arm_fir_instance_q15             lpFilter;
q15_t                            modState[16U];    // blockSize + phaseLength - 1, 4 + 9 - 1 plus some spare
q15_t                            lpState[60U];     // NoTaps + BlockSize - 1, 32 + 20 - 1 plus some spare

arm_fir_instance_q15 p25boxcar5Filter;
q15_t                p25boxcar5State[30U];        // NoTaps + BlockSize - 1,  6 + 20 - 1 plus some spare

typedef struct TIMERS
{
    char name[20];
    bool valid;
    bool enabled;
    uint32_t duration;
    uint32_t count;
    bool triggered;
} TIMERS;

TIMERS timer[10];

void processNone(q15_t sample);
void processHdr(q15_t sample);
void processLdu(q15_t sample);
bool correlateSync();
void decodeFrame(uint8_t* buffer, uint8_t length, bool isNet);
void processTx(const uint8_t* data, uint8_t length, const char* type, bool isTx);
void* startTCPServer(void* arg);

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

bool isActiveMode()
{
    char active_mode[10] = "IDLE";

    readStatus(modemName, "main", "active_mode", active_mode);

    if (strcasecmp(active_mode, "P25") == 0 || strcasecmp(active_mode, "IDLE") == 0)
        return true;
    else
        return false;
}

void reset()
{
    p25state        = P25RXS_NONE;
    p25dataPtr      = 0U;
    p25bitPtr       = 0U;
    p25maxCorr      = 0;
    p25averagePtr   = NOAVEPTR;
    p25hdrStartPtr  = NOENDPTR;
    p25lduStartPtr  = NOENDPTR;
    p25lduEndPtr    = NOENDPTR;
    p25hdrSyncPtr   = NOENDPTR;
    p25lduSyncPtr   = NOENDPTR;
    p25minSyncPtr   = NOENDPTR;
    p25maxSyncPtr   = NOENDPTR;
    p25centerVal    = 0;
    p25thresholdVal = 0;
    p25lostCount    = 0U;
    p25countdown    = 0U;
    p25duid         = 0U;
    p25mfId         = 0U;
    p25algId        = 0x80U;
    p25kId          = 0U;
    p25lcf          = 0x00U;
    p25emergency    = false;
    p25srcId        = 0U;
    p25dstId        = 0U;
    p25lastDUID     = 0U;
    p25nac          = 0U;
    p25poLen        = 0U;
    p25poPtr        = 0U;
    p25txDelay      = 240U;
    p25_64BitBuffer = 0U;
    memset(p25mi, 0x00U, P25_MI_LENGTH_BYTES);
}

void decodeP25(uint8_t* data, bool isNet)
{
    // Decode the NID
    bool valid = decodeNID(data + 8);

    uint8_t duid = getDUID();
    fprintf(stderr, "Val: %d  DUID: %d  TX: %d  NAC: %X  ST: %d\n", valid, duid, txOn, getNAC(), rf_state);

    if (rf_state == RS_RF_LISTENING && !valid)
        return;

    if (!valid)
    {
        switch (p25lastDUID)
        {
            case P25_DUID_HEADER:
            case P25_DUID_LDU2:
                duid = P25_DUID_LDU1;
                break;
            case P25_DUID_LDU1:
                duid = P25_DUID_LDU2;
                break;
            case P25_DUID_PDU:
                duid = P25_DUID_PDU;
                break;
            case P25_DUID_TSDU:
                duid = P25_DUID_TSDU;
                break;
            default:
                break;
        }
    }

    char cType[4] = "RF";
    if (isNet)
        strcpy(cType, "NET");

    if (duid == P25_DUID_HEADER)
    {
        if (rf_state == RS_RF_LISTENING)
        {
            write(sockfd, SETMODE, 5);
            start_time = time(NULL);
            p25lastDUID = duid;

            pthread_mutex_lock(&stateMutex);
            txOn = true;
            pthread_mutex_unlock(&stateMutex);
        }
        return;
    }
    else if (duid == P25_DUID_LDU1)
    {
        if (rf_state == RS_RF_LISTENING)
        {
            if (p25ReflConnected && !isNet)
            {
                pthread_mutex_lock(&gwTxBufMutex);
                RingBuffer_addData(&gwTxBuffer, data, P25_LDU_FRAME_LENGTH_BYTES + 8);
                pthread_mutex_unlock(&gwTxBufMutex);
            }

            bool ret = decodeLDU1(data + 8);
            // Add busy bits, inbound busy
            addBusyBits(data + 8, P25_LDU_FRAME_LENGTH_BITS, false, true);
            if (getSrcId() > 0 && !rx_started && valid && ret)
            {
                pthread_mutex_lock(&stateMutex);
                rx_started = true;
                if (!txOn)
                {
                    write(sockfd, SETMODE, 5);
                    start_time = time(NULL);
                    txOn = true;
                }
                pthread_mutex_unlock(&stateMutex);

                p25srcId = getSrcId();
                p25dstId = getDstId();
                sprintf(src_callsign, "%d", p25srcId);
                sprintf(dst_callsign, "%d", p25dstId);
                char tmp[100];
                bzero(tmp, 100);
                if (findDMRId(p25srcId, src_callsign, tmp))
                {
                    sprintf(tmp, "%u", p25srcId);
                    fprintf(stderr, "Src: %d  Call: %s  Dst: %d\n", p25srcId, src_callsign, p25dstId);
                    saveLastCall(1, modemName, "P25", cType, src_callsign, tmp, dst_callsign, "", NULL, "", true);
                }
                else
                {
                    fprintf(stderr, "Src: %d  Dst: %d\n", p25srcId, p25dstId);
                    sprintf(src_callsign, "%d", p25srcId);
                    saveLastCall(1, modemName, "P25", cType, src_callsign, "", dst_callsign, "", NULL, "", true);
                }
            }

            pthread_mutex_lock(&stateMutex);
            rf_state = RS_RF_AUDIO;
            pthread_mutex_unlock(&stateMutex);

            p25lastDUID = duid;
        }
        else if (rf_state == RS_RF_AUDIO)
        {
            if (p25ReflConnected && !isNet)
            {
                pthread_mutex_lock(&gwTxBufMutex);
                RingBuffer_addData(&gwTxBuffer, data, P25_LDU_FRAME_LENGTH_BYTES + 8);
                pthread_mutex_unlock(&gwTxBufMutex);
            }
        }
        return;
    }
    else if (duid == P25_DUID_LDU2)
    {
        if (rf_state == RS_RF_AUDIO)
        {
            if (p25ReflConnected && !isNet)
            {
                pthread_mutex_lock(&gwTxBufMutex);
                RingBuffer_addData(&gwTxBuffer, data, P25_LDU_FRAME_LENGTH_BYTES + 8);
                pthread_mutex_unlock(&gwTxBufMutex);
            }

            addBusyBits(data + 8, P25_LDU_FRAME_LENGTH_BITS, false, true);
            //      decodeLDU2(data + 8);
            p25lastDUID = duid;
        }
        return;
    }
    else if (duid == P25_DUID_TSDU)
    {
        addBusyBits(data + 8, P25_TSDU_FRAME_LENGTH_BITS, false, true);
        p25lastDUID = duid;
        return;
    }
    else if (duid == P25_DUID_TERM || duid == P25_DUID_TERM_LC)
    {
        if (rf_state == RS_RF_AUDIO)
        {
            addBusyBits(data + 8, P25_TERM_FRAME_LENGTH_BITS, false, true);

            pthread_mutex_lock(&stateMutex);
            if (txOn)
            {
                duration   = difftime(time(NULL), start_time);
                txOn       = false;
                rx_started = false;
            }
            pthread_mutex_unlock(&stateMutex);

            saveLastCall(1, modemName, "P25", cType, src_callsign, "", dst_callsign, "", NULL, "", false);
            saveHistory(modemName, "P25", cType, src_callsign, "", dst_callsign, 0, "", duration);

            pthread_mutex_lock(&stateMutex);
            rf_state = RS_RF_LISTENING;
            pthread_mutex_unlock(&stateMutex);

            p25lastDUID = duid;
        }
        return;
    }
    else if (duid == P25_DUID_PDU)
    {
        p25lastDUID = duid;
        return;
    }
}

void samples(const q15_t* samples, uint8_t length)
{
    for (uint8_t i = 0U; i < length; i++)
    {
        q15_t sample = samples[i];

        p25bitBuffer[p25bitPtr] <<= 1;
        if (sample < 0)
            p25bitBuffer[p25bitPtr] |= 0x01U;

        p25buffer[p25dataPtr] = sample;

        switch (p25state)
        {
            case P25RXS_HDR:
                processHdr(sample);
                break;
            case P25RXS_LDU:
                processLdu(sample);
                break;
            default:
                processNone(sample);
                break;
        }

        p25dataPtr++;
        if (p25dataPtr >= P25_LDU_FRAME_LENGTH_SAMPLES)
        {
            p25dataPtr = 0U;
            p25duid    = 0U;
        }

        p25bitPtr++;
        if (p25bitPtr >= P25_RADIO_SYMBOL_LENGTH)
            p25bitPtr = 0U;
    }
}

void processNone(q15_t sample)
{
    bool ret = correlateSync();
    if (ret)
    {
        // On the first sync, start the countdown to the state change
        if (p25countdown == 0U)
        {
            p25averagePtr = NOAVEPTR;
            p25countdown  = CORRELATION_COUNTDOWN;
        }
    }

    if (p25countdown > 0U) p25countdown--;

    if (p25countdown == 1U)
    {
        // These are the sync positions for the following LDU after a HDR
        p25minSyncPtr = p25hdrSyncPtr + P25_HDR_FRAME_LENGTH_SAMPLES - 1U;
        if (p25minSyncPtr >= P25_LDU_FRAME_LENGTH_SAMPLES)
            p25minSyncPtr -= P25_LDU_FRAME_LENGTH_SAMPLES;

        p25maxSyncPtr = p25hdrSyncPtr + P25_HDR_FRAME_LENGTH_SAMPLES + 1U;
        if (p25maxSyncPtr >= P25_LDU_FRAME_LENGTH_SAMPLES)
            p25maxSyncPtr -= P25_LDU_FRAME_LENGTH_SAMPLES;

        p25state     = P25RXS_HDR;
        p25countdown = 0U;
    }
}

void samplesToBits(uint16_t start, uint16_t count, uint8_t* buffer, uint16_t offset, q15_t center, q15_t threshold)
{
    for (uint16_t i = 0U; i < count; i++)
    {
        q15_t sample = p25buffer[start] - center;

        if (sample < -threshold)
        {
            WRITE_BIT(buffer, offset, false);
            offset++;
            WRITE_BIT(buffer, offset, true);
            offset++;
        }
        else if (sample < 0)
        {
            WRITE_BIT(buffer, offset, false);
            offset++;
            WRITE_BIT(buffer, offset, false);
            offset++;
        }
        else if (sample < threshold)
        {
            WRITE_BIT(buffer, offset, true);
            offset++;
            WRITE_BIT(buffer, offset, false);
            offset++;
        }
        else
        {
            WRITE_BIT(buffer, offset, true);
            offset++;
            WRITE_BIT(buffer, offset, true);
            offset++;
        }

        start += P25_RADIO_SYMBOL_LENGTH;
        if (start >= P25_LDU_FRAME_LENGTH_SAMPLES)
            start -= P25_LDU_FRAME_LENGTH_SAMPLES;
    }
}

bool correlateSync()
{
    if (countBits32((p25bitBuffer[p25bitPtr] & P25_SYNC_SYMBOLS_MASK) ^ P25_SYNC_SYMBOLS) <= MAX_SYNC_SYMBOLS_ERRS)
    {
        uint16_t ptr = p25dataPtr + P25_LDU_FRAME_LENGTH_SAMPLES - P25_SYNC_LENGTH_SAMPLES + P25_RADIO_SYMBOL_LENGTH;
        if (ptr >= P25_LDU_FRAME_LENGTH_SAMPLES)
            ptr -= P25_LDU_FRAME_LENGTH_SAMPLES;

        q31_t corr = 0;
        q15_t min  = 16000;
        q15_t max  = -16000;

        for (uint8_t i = 0U; i < P25_SYNC_LENGTH_SYMBOLS; i++)
        {
            q15_t val = p25buffer[ptr];

            if (val > max) max = val;
            if (val < min) min = val;

            switch (P25_SYNC_SYMBOLS_VALUES[i])
            {
                case +3:
                    corr -= (val + val + val);
                    break;
                case +1:
                    corr -= val;
                    break;
                case -1:
                    corr += val;
                    break;
                default:  // -3
                    corr += (val + val + val);
                    break;
            }

            ptr += P25_RADIO_SYMBOL_LENGTH;
            if (ptr >= P25_LDU_FRAME_LENGTH_SAMPLES)
                ptr -= P25_LDU_FRAME_LENGTH_SAMPLES;
        }

        if (corr > p25maxCorr)
        {
            if (p25averagePtr == NOAVEPTR)
            {
                p25centerVal = (max + min) >> 1;

                q31_t v1        = (max - p25centerVal) * SCALING_FACTOR;
                p25thresholdVal = (q15_t)(v1 >> 15);
            }

            uint16_t startPtr = p25dataPtr + P25_LDU_FRAME_LENGTH_SAMPLES - P25_SYNC_LENGTH_SAMPLES + P25_RADIO_SYMBOL_LENGTH;
            if (startPtr >= P25_LDU_FRAME_LENGTH_SAMPLES)
                startPtr -= P25_LDU_FRAME_LENGTH_SAMPLES;

            uint8_t sync[P25_SYNC_BYTES_LENGTH];
            samplesToBits(startPtr, P25_SYNC_LENGTH_SYMBOLS, sync, 0U, p25centerVal, p25thresholdVal);

            uint8_t maxErrs;
            if (p25state == P25RXS_NONE)
                maxErrs = MAX_SYNC_BIT_START_ERRS;
            else
                maxErrs = MAX_SYNC_BIT_RUN_ERRS;

            uint8_t errs = 0U;
            for (uint8_t i = 0U; i < P25_SYNC_BYTES_LENGTH; i++)
                errs += countBits8(sync[i] ^ P25_SYNC_BYTES[i]);

            if (errs <= maxErrs)
            {
                p25maxCorr   = corr;
                p25lostCount = MAX_SYNC_FRAMES;

                p25lduSyncPtr = p25dataPtr;

                // These are the positions of the start and end of an LDU
                p25lduStartPtr = startPtr;

                p25lduEndPtr = p25dataPtr + P25_LDU_FRAME_LENGTH_SAMPLES - P25_SYNC_LENGTH_SAMPLES - 1U;
                if (p25lduEndPtr >= P25_LDU_FRAME_LENGTH_SAMPLES)
                    p25lduEndPtr -= P25_LDU_FRAME_LENGTH_SAMPLES;

                if (p25state == P25RXS_NONE)
                {
                    p25hdrSyncPtr = p25dataPtr;

                    // This is the position of the start of a HDR
                    p25hdrStartPtr = startPtr;

                    // These are the range of positions for a sync for an LDU
                    // following a HDR
                    p25minSyncPtr =
                        p25dataPtr + P25_HDR_FRAME_LENGTH_SAMPLES - 1U;
                    if (p25minSyncPtr >= P25_LDU_FRAME_LENGTH_SAMPLES)
                        p25minSyncPtr -= P25_LDU_FRAME_LENGTH_SAMPLES;

                    p25maxSyncPtr =
                        p25dataPtr + P25_HDR_FRAME_LENGTH_SAMPLES + 1U;
                    if (p25maxSyncPtr >= P25_LDU_FRAME_LENGTH_SAMPLES)
                        p25maxSyncPtr -= P25_LDU_FRAME_LENGTH_SAMPLES;
                }

                return true;
            }
        }
    }

    return false;
}

void calculateLevels(uint16_t start, uint16_t count)
{
    q15_t maxPos = -16000;
    q15_t minPos = 16000;
    q15_t maxNeg = 16000;
    q15_t minNeg = -16000;

    for (uint16_t i = 0U; i < count; i++)
    {
        q15_t sample = p25buffer[start];

        if (sample > 0)
        {
            if (sample > maxPos) maxPos = sample;
            if (sample < minPos) minPos = sample;
        }
        else
        {
            if (sample < maxNeg) maxNeg = sample;
            if (sample > minNeg) minNeg = sample;
        }

        start += P25_RADIO_SYMBOL_LENGTH;
        if (start >= P25_LDU_FRAME_LENGTH_SAMPLES)
            start -= P25_LDU_FRAME_LENGTH_SAMPLES;
    }

    q15_t posThresh = (maxPos + minPos) >> 1;
    q15_t negThresh = (maxNeg + minNeg) >> 1;

    q15_t center = (posThresh + negThresh) >> 1;

    q15_t threshold = posThresh - center;

    if (p25averagePtr == NOAVEPTR)
    {
        for (uint8_t i = 0U; i < 16U; i++)
        {
            p25center[i]    = center;
            p25threshold[i] = threshold;
        }

        p25averagePtr = 0U;
    }
    else
    {
        p25center[p25averagePtr]    = center;
        p25threshold[p25averagePtr] = threshold;

        p25averagePtr++;
        if (p25averagePtr >= 16U) p25averagePtr = 0U;
    }

    p25centerVal    = 0;
    p25thresholdVal = 0;

    for (uint8_t i = 0U; i < 16U; i++)
    {
        p25centerVal    += p25center[i];
        p25thresholdVal += p25threshold[i];
    }

    p25centerVal    >>= 4;
    p25thresholdVal >>= 4;
}

void processHdr(q15_t sample)
{
    if (p25minSyncPtr < p25maxSyncPtr)
    {
        if (p25dataPtr >= p25minSyncPtr && p25dataPtr <= p25maxSyncPtr)
            correlateSync();
    }
    else
    {
        if (p25dataPtr >= p25minSyncPtr || p25dataPtr <= p25maxSyncPtr)
            correlateSync();
    }

    if (p25dataPtr == p25maxSyncPtr)
    {
        uint16_t nidStartPtr = p25hdrStartPtr + P25_SYNC_LENGTH_SAMPLES;
        if (nidStartPtr >= P25_LDU_FRAME_LENGTH_SAMPLES)
            nidStartPtr -= P25_LDU_FRAME_LENGTH_SAMPLES;

        uint8_t nid[2U];
        samplesToBits(nidStartPtr, (2U * 4U), nid, 0U, p25centerVal, p25thresholdVal);

        p25duid = nid[1U] & 0x0F;

        switch (p25duid)
        {
            case P25_DUID_HDU:
            {
                calculateLevels(p25hdrStartPtr, P25_HDR_FRAME_LENGTH_SYMBOLS);

                uint8_t frame[P25_HDR_FRAME_LENGTH_BYTES + 1U];
                samplesToBits(p25hdrStartPtr, P25_HDR_FRAME_LENGTH_SYMBOLS, frame, 8U, p25centerVal, p25thresholdVal);

                frame[0U] = 0x01U;
            }
            break;

            case P25_DUID_PDU:
            {
                calculateLevels(p25hdrSyncPtr, P25_PDU_HDR_FRAME_LENGTH_SYMBOLS);

                uint8_t frame[P25_PDU_HDR_FRAME_LENGTH_BYTES + 1U];
                samplesToBits(p25hdrSyncPtr, P25_PDU_HDR_FRAME_LENGTH_SYMBOLS, frame, 8U, p25centerVal, p25thresholdVal);

                frame[0U] = 0x01U;
            }
            break;

            case P25_DUID_TSDU:
            {
                calculateLevels(p25hdrStartPtr, P25_TSDU_FRAME_LENGTH_SYMBOLS);

                uint8_t frame[P25_TSDU_FRAME_LENGTH_BYTES + 1U];
                samplesToBits(p25hdrStartPtr, P25_TSDU_FRAME_LENGTH_SYMBOLS, frame, 8U, p25centerVal, p25thresholdVal);

                frame[0U] = 0x01U;
            }
            break;

            case P25_DUID_TDU:
            {
                calculateLevels(p25hdrStartPtr, P25_TERM_FRAME_LENGTH_SYMBOLS);

                uint8_t frame[P25_TERM_FRAME_LENGTH_BYTES + 1U];
                samplesToBits(p25hdrStartPtr, P25_TERM_FRAME_LENGTH_SYMBOLS, frame, 8U, p25centerVal, p25thresholdVal);

                frame[0U] = 0x01U;
            }
            break;

            case P25_DUID_TDULC:
            {
                calculateLevels(p25hdrStartPtr, P25_TERMLC_FRAME_LENGTH_SYMBOLS);

                uint8_t frame[P25_TERMLC_FRAME_LENGTH_BYTES + 1U];
                samplesToBits(p25hdrStartPtr, P25_TERMLC_FRAME_LENGTH_SYMBOLS, frame, 8U, p25centerVal, p25thresholdVal);

                frame[0U] = 0x01U;
            }
            break;

            default:
                break;
        }

        p25minSyncPtr = p25lduSyncPtr + P25_LDU_FRAME_LENGTH_SAMPLES - 1U;
        if (p25minSyncPtr >= P25_LDU_FRAME_LENGTH_SAMPLES)
            p25minSyncPtr -= P25_LDU_FRAME_LENGTH_SAMPLES;

        p25maxSyncPtr = p25lduSyncPtr + 1U;
        if (p25maxSyncPtr >= P25_LDU_FRAME_LENGTH_SAMPLES)
            p25maxSyncPtr -= P25_LDU_FRAME_LENGTH_SAMPLES;

        p25state   = P25RXS_LDU;
        p25maxCorr = 0;
    }
}

void processLdu(q15_t sample)
{
    if (p25minSyncPtr < p25maxSyncPtr)
    {
        if (p25dataPtr >= p25minSyncPtr && p25dataPtr <= p25maxSyncPtr)
            correlateSync();
    }
    else
    {
        if (p25dataPtr >= p25minSyncPtr || p25dataPtr <= p25maxSyncPtr)
            correlateSync();
    }

    if (p25dataPtr == p25lduEndPtr)
    {
        // Only update the center and threshold if they are from a good sync
        if (p25lostCount == MAX_SYNC_FRAMES)
        {
            p25minSyncPtr = p25lduSyncPtr + P25_LDU_FRAME_LENGTH_SAMPLES - 1U;
            if (p25minSyncPtr >= P25_LDU_FRAME_LENGTH_SAMPLES)
                p25minSyncPtr -= P25_LDU_FRAME_LENGTH_SAMPLES;

            p25maxSyncPtr = p25lduSyncPtr + 1U;
            if (p25maxSyncPtr >= P25_LDU_FRAME_LENGTH_SAMPLES)
                p25maxSyncPtr -= P25_LDU_FRAME_LENGTH_SAMPLES;
        }

        calculateLevels(p25lduStartPtr, P25_LDU_FRAME_LENGTH_SYMBOLS);

        uint8_t frame[P25_LDU_FRAME_LENGTH_BYTES + 3U];
        samplesToBits(p25lduStartPtr, P25_LDU_FRAME_LENGTH_SYMBOLS, frame, 8U, p25centerVal, p25thresholdVal);

        // We've not seen a data sync for too long, signal RXLOST and change to RX_NONE
        p25lostCount--;
        if (p25lostCount == 0U)
        {
            p25state      = P25RXS_NONE;
            p25lduEndPtr  = NOENDPTR;
            p25averagePtr = NOAVEPTR;
            p25countdown  = 0U;
            p25maxCorr    = 0;
            p25duid       = 0U;
        }
        else
        {
            frame[0U] = p25lostCount == (MAX_SYNC_FRAMES - 1U) ? 0x01U : 0x00U;
            p25maxCorr = 0;
        }
    }
}

void setEndPtr(const uint8_t duid)
{
    switch (duid)
    {
        case P25_DUID_HDU:
            p25endPtr = P25_HDR_FRAME_LENGTH_BITS;
            break;
        case P25_DUID_TDU:
            p25endPtr = P25_TERM_FRAME_LENGTH_BITS;
            break;
        case P25_DUID_TSDU:
            p25endPtr = P25_TSDU_FRAME_LENGTH_BITS;
            break;
        case P25_DUID_PDU:
            p25endPtr = P25_PDU_HDR_FRAME_LENGTH_BITS;
            break;
        case P25_DUID_TDULC:
            p25endPtr = P25_TERMLC_FRAME_LENGTH_BITS;
            break;
        default:
            p25endPtr = P25_LDU_FRAME_LENGTH_BITS;
            break;
    }
}

void processBitNone(bool bit)
{
    p25_64BitBuffer <<= 1;
    if (bit)
        p25_64BitBuffer |= 0x01U;

    // Fuzzy matching of the data sync bit sequence
    if (countBits64((p25_64BitBuffer & P25_SYNC_BITS_MASK) ^ P25_SYNC_BITS) <= SYNC_BIT_START_ERRS)
    {
        for (uint8_t i = 0U; i < P25_SYNC_LENGTH_BYTES; i++)
            p25buffer[i] = P25_SYNC_BYTES[i];

        p25lostCount = MAX_SYNC_FRAMES;
        p25bufferPtr = P25_SYNC_LENGTH_BITS;
        p25state     = P25RXS_HDR;
    }
}

void processBitHdr(bool bit)
{
    p25_64BitBuffer <<= 1;
    if (bit)
        p25_64BitBuffer |= 0x01U;

    WRITE_BIT(p25buffer, p25bufferPtr, bit);

    p25bufferPtr++;
    if (p25bufferPtr > P25_LDU_FRAME_LENGTH_BITS)
        reset();

    if (p25bufferPtr == P25_SYNC_LENGTH_BITS + 16U)
    {
        p25duid = p25buffer[7U] & 0x0F;

        if (p25duid != P25_DUID_HDU && p25duid != P25_DUID_TSDU && p25duid != P25_DUID_TDU && p25duid != P25_DUID_TDULC)
        {
            p25lostCount = MAX_SYNC_FRAMES;
            p25state     = P25RXS_LDU;
            return;
        }

        setEndPtr(p25duid);
    }

    // Search for end of header frame
    if (p25bufferPtr == p25endPtr)
    {
        uint8_t buf[(p25endPtr / 8U) + 8];
        buf[0] = 0x61; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x04;
        buf[4] = 'P'; buf[5] = '2'; buf[6] = '5'; buf[7] = 'H';
        buf[2] = (p25endPtr / 8U) + 8;
        memcpy(buf + 8, p25buffer, (p25endPtr / 8U));

        decodeFrame(buf, (p25endPtr / 8U), false);

        p25lostCount = MAX_SYNC_FRAMES;
        p25bufferPtr = 0U;
        p25state     = P25RXS_LDU;
    }
}

void processBitLdu(bool bit)
{
    p25_64BitBuffer <<= 1;
    if (bit)
        p25_64BitBuffer |= 0x01U;

    WRITE_BIT(p25buffer, p25bufferPtr, bit);

    p25bufferPtr++;
    if (p25bufferPtr > P25_LDU_FRAME_LENGTH_BITS) reset();

    // Only search for a sync in the right place +-2 bits
    if (p25bufferPtr >= (P25_SYNC_LENGTH_BITS - 2U) && p25bufferPtr <= (P25_SYNC_LENGTH_BITS + 2U))
    {
        // Fuzzy matching of the data sync bit sequence
        if (countBits64((p25_64BitBuffer & P25_SYNC_BITS_MASK) ^ P25_SYNC_BITS) <= SYNC_BIT_RUN_ERRS)
        {
            p25lostCount = MAX_SYNC_FRAMES;
            p25bufferPtr = P25_SYNC_LENGTH_BITS;
        }
    }

    if (p25bufferPtr == P25_SYNC_LENGTH_BITS + 16U)
    {
        p25duid = p25buffer[7U] & 0x0F;
        setEndPtr(p25duid);
    }

    // Send a data frame to the host if the required number of bits have been received
    if (p25bufferPtr == P25_LDU_FRAME_LENGTH_BITS)
    {
        p25lostCount--;
        // We've not seen a data sync for too long, signal RXLOST and change to RX_NONE
        if (p25lostCount == 0U)
        {
            uint8_t buf[(p25endPtr / 8U) + 8];
            buf[0] = 0x61; buf[1] = 0x00; buf[2] = 0x08; buf[3] = 0x04;
            buf[4] = 'P'; buf[5] = '2'; buf[6] = '5'; buf[7] = 'E';
            decodeFrame(buf, 8U, false);
            reset();
        }
        else
        {
            uint8_t buf[(p25endPtr / 8U) + 8];
            buf[0] = 0x61; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x04;
            buf[4] = 'P'; buf[5] = '2'; buf[6] = '5'; buf[7] = 'L';
            buf[2] = (p25endPtr / 8U) + 8;
            memcpy(buf + 8, p25buffer, (p25endPtr / 8U));
            decodeFrame(buf, (p25endPtr / 8U), false);

            p25bufferPtr = 0U;
        }

        // Check if we found a TDU to avoid a false "lost lock"
        if (p25duid == P25_DUID_TDU || p25duid == P25_DUID_TDULC)
        {
            reset();
        }
    }
}

void processBits(bool bit)
{
    switch (p25state)
    {
        case P25RXS_HDR:
            processHdr(bit);
            break;
        case P25RXS_LDU:
            processLdu(bit);
            break;
        default:
            processNone(bit);
            break;
    }
}

void writeBits(uint8_t c, bool isEOT)
{
    static uint8_t bytes[200] = {0x61, 0x00, 0xc8, 0x04, 'B', 'I', 'T', 'S'}; // setup 8 byte header
    static uint8_t bytePos = 8; // set first position after header

    bytes[bytePos++] = c;
    if (bytePos == 200)
    {
        write(sockfd, bytes, 200);
        bytePos = 8;
    }

    if (bytePos > 8 && isEOT) // If EOT and number of new bytes less than 200 pad out with 0.
    {
        memset(bytes + bytePos, 0, 200 - bytePos);
        write(sockfd, bytes, 200);
        bytePos = 8;
    }
}

void processTx(const uint8_t* data, uint8_t length, const char* type, bool isTx)
{
    if (strcasecmp(type, TYPE_EOT) == 0)
    {
        uint8_t tmp[1] = {0x00};
        writeBits(tmp[0], true);
        return;
    }

    if (!isTx)
    {
        for (uint8_t i = 0U; i < p25txDelay; i++)
        {
            writeBits(P25_START_SYNC, false);
        }
        delay(10000);
    }

    for (uint8_t i = 0U; i < length; i++)
    {
        writeBits(data[i], false);
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

void resetTimer(const char* name)
{
    for (uint8_t i = 0; i < 10; i++)
    {
        pthread_mutex_lock(&timerMutex);
        if (timer[i].valid && strcasecmp(timer[i].name, name) == 0)
        {
            timer[i].count     = 0;
            timer[i].triggered = false;
            pthread_mutex_unlock(&timerMutex);
            return;
        }
        pthread_mutex_unlock(&timerMutex);
    }
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
void* timerThread(void *arg)
{
    bool     idle = true;

    if (getTimer("modeHang", modeHang) < 0)
    {
        fprintf(stderr, "Timer thread exited.\n");
        int iRet = 600;
        pthread_exit(&iRet);
        return NULL;
    }

    while (connected)
    {
        delay(1000);

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
            // fprintf(stderr, "T: %d  N: %s D: %u C: %u\n", i, timer[i].name, timer[i].duration, timer[i].count);
            if (txOn && strcasecmp(timer[i].name, "modeHang") == 0)
            {
                idle = false;
                resetTimer("modeHang");
            }
            else if (!txOn && strcasecmp(timer[i].name, "modeHang") == 0)
            {
                if (!idle)
                {
                    if (isTimerTriggered("modeHang"))
                    {
                        pthread_mutex_lock(&rxBufMutex);
                        RingBuffer_addData(&rxBuffer, SETIDLE, 5);
                        pthread_mutex_unlock(&rxBufMutex);
                        //    write(sockfd, SETIDLE, 5);
                        idle = true;
                        resetTimer("modeHang");
                    }
                }
            }
        }
    }

    fprintf(stderr, "Timer thread exited.\n");
    int iRet = 600;
    pthread_exit(&iRet);
    return NULL;
}

void* rxThread(void* arg)
{
    uint8_t loop = 0;

    while (connected)
    {
        delay(100);
        loop++;

        if (loop > 100)
        {
            pthread_mutex_lock(&rxBufMutex);
            uint32_t avail = RingBuffer_dataSize(&rxBuffer);
            pthread_mutex_unlock(&rxBufMutex);

            if (avail >= 5)
            {
                uint8_t buf[1];

                pthread_mutex_lock(&rxBufMutex);
                RingBuffer_peek(&rxBuffer, buf, 1);
                pthread_mutex_unlock(&rxBufMutex);

                if (buf[0] != 0x61)
                {
                    pthread_mutex_lock(&rxBufMutex);
                    RingBuffer_getData(&rxBuffer, buf, 1);
                    pthread_mutex_unlock(&rxBufMutex);
                    fprintf(stderr, "RX invalid header.\n");
                    continue;
                }

                uint8_t  byte[3];
                uint16_t len = 0;

                pthread_mutex_lock(&rxBufMutex);
                RingBuffer_peek(&rxBuffer, byte, 3);
                pthread_mutex_unlock(&rxBufMutex);

                len = (byte[1] << 8) + byte[2];

                pthread_mutex_lock(&rxBufMutex);
                avail = RingBuffer_dataSize(&rxBuffer);
                pthread_mutex_unlock(&rxBufMutex);

                if (avail >= len)
                {
                    uint8_t rbuf[len];

                    pthread_mutex_lock(&rxBufMutex);
                    RingBuffer_getData(&rxBuffer, rbuf, len);
                    pthread_mutex_unlock(&rxBufMutex);

                    if (len == 5)
                    {
                        if (write(sockfd, rbuf, len) < 0)
                        {
                            fprintf(stderr, "ERROR: remote disconnect\n");
                            break;
                        }
                        continue;
                    }

                    uint8_t type[4];
                    memcpy(type, rbuf + 4, 4);

                    if (memcmp(type, TYPE_HEADER, 4) == 0)
                    {
                        processTx(rbuf + 8, P25_HDR_FRAME_LENGTH_BYTES, TYPE_HEADER, false);
                    }
                    else if (memcmp(type, TYPE_LDU, 4) == 0)
                    {
                        processTx(rbuf + 8, P25_LDU_FRAME_LENGTH_BYTES, TYPE_LDU, true);
                    }
                    else if (memcmp(type, TYPE_EOT, 4) == 0)
                    {
                        processTx(rbuf + 8, 8, TYPE_EOT, true);
                    }
                }
            }
            loop = 0;
        }
    }

    fprintf(stderr, "RX thread exited.\n");
    int iRet       = 500;
    pthread_mutex_lock(&stateMutex);
    p25GWConnected = false;
    pthread_mutex_unlock(&stateMutex);
    pthread_exit(&iRet);
    return NULL;
}

// Send bytes to gateway.
void* txThread(void *arg)
{
    int      sockfd = (intptr_t)arg;
    uint16_t loop   = 0;

    while (1)
    {
        delay(100);
        loop++;

        if (loop > 100)
        {
            pthread_mutex_lock(&gwTxBufMutex);
            uint32_t avail = RingBuffer_dataSize(&gwTxBuffer);
            pthread_mutex_unlock(&gwTxBufMutex);

            if (avail >= 5)
            {
                uint8_t buf[1];

                pthread_mutex_lock(&gwTxBufMutex);
                RingBuffer_peek(&gwTxBuffer, buf, 1);
                pthread_mutex_unlock(&gwTxBufMutex);

                if (buf[0] != 0x61)
                {
                    pthread_mutex_lock(&gwTxBufMutex);
                    RingBuffer_getData(&gwTxBuffer, buf, 1);
                    pthread_mutex_unlock(&gwTxBufMutex);
                    fprintf(stderr, "TX invalid header. [%02X]\n", buf[0]);
                    continue;
                }

                uint8_t  byte[3];
                uint16_t len = 0;

                pthread_mutex_lock(&gwTxBufMutex);
                RingBuffer_peek(&gwTxBuffer, byte, 3);
                len = (byte[1] << 8) + byte[2];
                avail = RingBuffer_dataSize(&gwTxBuffer);
                pthread_mutex_unlock(&gwTxBufMutex);

                if (avail >= len)
                {
                    uint8_t tbuf[len];

                    pthread_mutex_lock(&gwTxBufMutex);
                    RingBuffer_getData(&gwTxBuffer, tbuf, len);
                    pthread_mutex_unlock(&gwTxBufMutex);

                    if (write(sockfd, tbuf, len) < 0)
                    {
                        fprintf(stderr, "ERROR: remote reflector gateway disconnected.\n");
                    }
                }
            }

            loop = 0;
        }

        pthread_mutex_lock(&gwCmdMutex);
        uint32_t cmdAvail = RingBuffer_dataSize(&gwCommand);
        pthread_mutex_unlock(&gwCmdMutex);

        if (cmdAvail >= 5)
        {
            uint8_t buf[1];

            pthread_mutex_lock(&gwCmdMutex);
            RingBuffer_peek(&gwCommand, buf, 1);
            pthread_mutex_unlock(&gwCmdMutex);

            if (buf[0] != 0x61)
            {
                pthread_mutex_lock(&gwCmdMutex);
                RingBuffer_getData(&gwCommand, buf, 1);
                pthread_mutex_unlock(&gwCmdMutex);
                fprintf(stderr, "TX invalid header.\n");
            }
            else
            {
                uint8_t  byte[3];
                uint16_t len = 0;

                pthread_mutex_lock(&gwCmdMutex);
                RingBuffer_peek(&gwCommand, byte, 3);
                len = (byte[1] << 8) + byte[2];
                cmdAvail = RingBuffer_dataSize(&gwCommand);
                pthread_mutex_unlock(&gwCmdMutex);

                if (cmdAvail >= len)
                {
                    uint8_t cbuf[len];

                    pthread_mutex_lock(&gwCmdMutex);
                    RingBuffer_getData(&gwCommand, cbuf, len);
                    pthread_mutex_unlock(&gwCmdMutex);

                    if (write(sockfd, cbuf, len) < 0)
                    {
                        fprintf(stderr, "ERROR: remote reflector gateway disconnected.\n");
                    }
                }
            }
        }
    }

    pthread_mutex_lock(&stateMutex);
    p25GWConnected = false;
    pthread_mutex_unlock(&stateMutex);
    fprintf(stderr, "TX GW thread exited.\n");
    int iRet = 500;
    pthread_exit(&iRet);
    return NULL;
}

void decodeFrame(uint8_t* buffer, uint8_t length, bool isNet)
{
    uint8_t typeLen = 4;

    char cType[4] = "RF";
    if (isNet)
        strcpy(cType, "NET");

    if (memcmp(buffer + 4, "P25", 3) == 0 && memcmp(buffer + 4, TYPE_EOT, typeLen) != 0)
    {
        decodeP25(buffer, isNet);
        if ((modem_duplex || isNet) && txOn)
        {
            if (write(sockfd, buffer, length) < 0)
            {
                fprintf(stderr, "ERROR: host disconnect\n");
            }
        }
    }
    else if (memcmp(buffer + 4, TYPE_EOT, 4) == 0)
    {
        pthread_mutex_lock(&stateMutex);
        bool wasOn = txOn && (rf_state == RS_RF_AUDIO);
        if (wasOn)
        {
            rf_state   = RS_RF_LISTENING;
            txOn       = false;
            rx_started = false;
        }
        pthread_mutex_unlock(&stateMutex);

        if (wasOn)
        {
            duration = difftime(time(NULL), start_time);
            char tmp[100];
            bzero(tmp, 100);
            sprintf(tmp, "%u", p25srcId);
            if (strcasecmp(src_callsign, tmp) != 0)
            {
                saveLastCall(1, modemName, "P25", cType, src_callsign, tmp, dst_callsign, "", NULL, "", false);
                saveHistory(modemName, "P25", cType, src_callsign, tmp, dst_callsign, 0, "", duration);
            }
            else
            {
                saveLastCall(1, modemName, "P25", cType, tmp, "", dst_callsign, "", NULL, "", false);
                saveHistory(modemName, "P25", cType, tmp, "", dst_callsign, 0, "", duration);
            }
        }
    }
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

    pthread_mutex_lock(&stateMutex);
    p25GWConnected = true;
    pthread_mutex_unlock(&stateMutex);

    addGateway(modemName, "main", "P25");
    txOn = false;

    fprintf(stderr, "Gateway connected.\n");

    while (1)
    {
        int len = read(childfd, buffer, 1);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            error((char*)"ERROR: P25 gateway connection closed remotely.");
            break;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            delay(5);
        }

        if (len != 1)
        {
            fprintf(stderr, "P25_Service: error when reading from P25 gateway, errno=%d\n", errno);
            close(childfd);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "P25_Service: unknown byte from P25 gateway, 0x%02X\n", buffer[0]);
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
        memcpy(type, buffer + 4, typeLen);

        if (debugM)
            dump((char*)"P25 Gateway data:", (uint8_t*)buffer, respLen);

        if (memcmp(type, TYPE_NACK, typeLen) == 0)
        {
            pthread_mutex_lock(&stateMutex);
            p25ReflConnected = false;
            pthread_mutex_unlock(&stateMutex);

            char tmp[8];
            bzero(tmp, 8);
            memcpy(tmp, buffer + 4 + typeLen, 6);
            ackDashbCommand(modemName, "reflLinkP25", "failed");
            setReflectorStatus(modemName, "P25", (const char*)tmp, false);
        }
        else if (memcmp(type, TYPE_CONNECT, typeLen) == 0)
        {
            pthread_mutex_lock(&stateMutex);
            p25ReflConnected = true;
            pthread_mutex_unlock(&stateMutex);

            ackDashbCommand(modemName, "reflLinkP25", "success");
            char tmp[8];
            bzero(tmp, 8);
            memcpy(tmp, buffer + 4 + typeLen, 6);
            setReflectorStatus(modemName, "P25", (const char*)tmp, true);
        }
        else if (memcmp(type, TYPE_DISCONNECT, typeLen) == 0)
        {
            pthread_mutex_lock(&stateMutex);
            p25ReflConnected = false;
            pthread_mutex_unlock(&stateMutex);

            char tmp[8];
            bzero(tmp, 8);
            memcpy(tmp, buffer + 4 + typeLen, 6);
            ackDashbCommand(modemName, "reflLinkP25", "success");
            setReflectorStatus(modemName, "P25", (const char*)tmp, false);
        }
        else if (memcmp(type, TYPE_COMMAND, typeLen) == 0)
        {
            ackDashbCommand(modemName, "updateConfP25", "success");
        }
        else if (memcmp(type, TYPE_STATUS, typeLen) == 0)
        {
        }
        else if (memcmp(type, "P25", 3) == 0 && isActiveMode())
        {
            pthread_mutex_lock(&stateMutex);
            reflBusy = true;
            pthread_mutex_unlock(&stateMutex);

            decodeFrame(buffer, respLen, true);
        }
        delay(5);
    }

    fprintf(stderr, "Gateway disconnected.\n");

    pthread_mutex_lock(&stateMutex);
    p25GWConnected   = false;
    p25ReflConnected = false;
    pthread_mutex_unlock(&stateMutex);

    delGateway(modemName, "main", "P25");
    clearReflLinkStatus(modemName, "P25");
    int iRet = 100;
    pthread_exit(&iRet);
    return 0;
}

// Start up communication with modem host.
void* startClient(void *arg)
{
    struct sockaddr_in serv_addr;
    uint8_t buffer[BUFFER_SIZE] = {0};
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
    {
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

    int err = pthread_create(&(gwHostid), NULL, &startTCPServer, NULL);
    if (err != 0)
    {
        fprintf(stderr, "Can't create gateway host thread :[%s]", strerror(err));
        return 0;
    }
    else
    {
        if (debugM)
            fprintf(stderr, "Gateway host thread created successfully\n");
    }

    pthread_t rxid;
    err = pthread_create(&(rxid), NULL, &rxThread, NULL);
    if (err != 0)
    {
        fprintf(stderr, "Can't create rx thread :[%s]", strerror(err));
        return 0;
    }
    else
    {
        if (debugM) fprintf(stderr, "RX thread created successfully\n");
    }

    err = pthread_create(&(timerid), NULL, &timerThread, NULL);
    if (err != 0)
    {
        fprintf(stderr, "Can't create timer thread :[%s]", strerror(err));
        return 0;
    }
    else
    {
        if (debugM) fprintf(stderr, "Timer thread created successfully\n");
    }

    sleep(1);
    fprintf(stderr, "Connected to host.\n");

    ssize_t  len       = 0;
    uint8_t  offset    = 0;
    uint16_t respLen   = 0;
    uint8_t  typeLen   = 0;
    uint8_t  configLen = 4 + 4 + 11 + 6 + 1 + 40 + 16 + 1 + 1;

    buffer[0] = 0x61;
    buffer[1] = 0x00;
    buffer[2] = configLen;
    buffer[3] = 0x04;
    memcpy(buffer + 4,  TYPE_MODE,  4);
    memcpy(buffer + 8,  MODE_NAME,  11);
    memcpy(buffer + 19, MODEM_TYPE, 6);
    buffer[25] = txLevel;

    uint8_t bytes[40];

    uint32_t reg3  = 0x2a4c80d3;
    uint32_t reg10 = 0x49e472a;

    uint32_t reg4  = (uint32_t) 0b0100           << 0;
    reg4 |= (uint32_t) 0b011                     << 4;
    reg4 |= (uint32_t) 0b0                       << 7;
    reg4 |= (uint32_t) 0b11                      << 8;
    reg4 |= (uint32_t) 394U                      << 10;
    reg4 |= (uint32_t) 6U                        << 20;
    reg4 |= (uint32_t) 0b00                      << 30;

    uint32_t reg2  = (uint32_t) 0b10             << 28;
    reg2 |= (uint32_t) 0b111                     << 4;

    uint32_t reg13 = (uint32_t) 0b1101           << 0;
    reg13 |= (uint32_t) ADF7021_SLICER_TH_P25    << 4;

    ifConf(bytes, reg2, reg3, reg4, reg10, reg13, atol(modem_rxFrequency), atol(modem_txFrequency),
           txLevel, rfPower, 0, true);

    memcpy(buffer + 26, bytes, 40);
    uint64_t tmp[2] = {P25_SYNC_BITS, P25_SYNC_BITS_MASK};
    memcpy(buffer + 66, (uint8_t*)tmp, 16);
    buffer[82] = 0x20;
    buffer[83] = 0x11;
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
            fprintf(stderr, "p25_Service: error when reading from server, errno=%d\n", errno);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "P25_Service: unknown byte from server, 0x%02X\n", buffer[0]);
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
        memcpy(type, buffer + 4, typeLen);

        if (debugM)
            dump((char*)"P25 data", (uint8_t*)buffer, respLen);

        if (memcmp(type, TYPE_COMMAND, typeLen) == 0)
        {
            if (buffer[8] == COMM_SET_DUPLEX)
                modem_duplex = true;
            else if (buffer[8] == COMM_SET_SIMPLEX)
                modem_duplex = false;
        }
        else if (memcmp(type, TYPE_SAMPLE, typeLen) == 0 && packetType == PACKET_TYPE_SAMP)
        {
            q15_t smp[2];
            q15_t in[2];
            for (uint16_t i = 8; i < respLen; i = i + 4)
            {
                memcpy(in, buffer + i, 4);
                if (USE_DC_FILTER)
                {
                    q31_t q31Samples[2];
                    arm_q15_to_q31(in, q31Samples, 2);

                    q31_t dcValues[2];
                    arm_biquad_cascade_df1_q31(&dcFilter, q31Samples, dcValues, 2);

                    q31_t dcLevel = 0;
                    for (uint8_t i = 0U; i < 2; i++)
                        dcLevel += dcValues[i];

                    dcLevel /= 2;

                    q15_t offset = (q15_t)(__SSAT((dcLevel >> 16), 16));

                    q15_t dcSamples[2];
                    for (uint8_t i = 0U; i < 2; i++)
                        dcSamples[i] = in[i] - offset;

                    arm_fir_fast_q15(&p25boxcar5Filter, dcSamples, smp, 2);
                }
                else
                    arm_fir_fast_q15(&p25boxcar5Filter, in, smp, 2);

                samples(smp, 2);
            }
        }
        else if (memcmp(type, TYPE_BITS, typeLen) == 0 && packetType == PACKET_TYPE_BIT)
        {
//            processBits(buffer + 8, respLen - 8);
        }
        else if (memcmp(type, TYPE_HEADER, 3) == 0 && packetType == PACKET_TYPE_FRAME)
        {
            decodeFrame(buffer, respLen, false);
        }
    }

    pthread_mutex_lock(&stateMutex);
    txOn = false;
    pthread_mutex_unlock(&stateMutex);

    fprintf(stderr, "Disconnected from host.\n");
    close(sockfd);
    connected = false;
    int iRet = 100;
    pthread_exit(&iRet);
    return 0;
}

// Listen for incoming gateway connection.
void* startTCPServer(void* arg)
{
    struct hostent* hostp;
    int childfd;
    int sockFd;

    sockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd < 0)
    {
        fprintf(stderr, "P25_Service: error when creating the socket: %s\n", strerror(errno));
        exit(1);
    }

    optval = 1;
    setsockopt(sockFd, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));

    bzero((char*)&serveraddr, sizeof(serveraddr));

    serveraddr.sin_family      = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port        = htons((unsigned short)serverPort);

    if (bind(sockFd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
    {
        fprintf(stderr, "P25_Service: error when binding the socket to port %u: %s\n", serverPort, strerror(errno));
        exit(1);
    }

    if (debugM)
        fprintf(stdout, "Opened the TCP socket on port %u\n", serverPort);

    if (listen(sockFd, 10) < 0)
        error((char*)"ERROR on listen");

    clientlen = sizeof(clientaddr);

    if (sockFd < 0)
    {
        error((char*)"Gateway connect failed.");
        exit(1);
    }

    while (connected)
    {
        childfd = accept(sockFd, (struct sockaddr*)&clientaddr, &clientlen);
        if (childfd < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            perror("accept failed");
            break;
        }

        if (childfd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;

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

        pthread_t txid;
        int err = pthread_create(&(txid), NULL, &txThread, (void*)(intptr_t)childfd);
        if (err != 0)
            fprintf(stderr, "Can't create tx thread :[%s]", strerror(err));
        else
        {
            if (debugM) fprintf(stderr, "TX thread created successfully\n");
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

int main(int argc, char** argv)
{
    bool daemon = 0;
    int  ret;
    int  c;

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
                clientPort = 18000 + modemId - 1;
                serverPort = 18300 + modemId - 1;
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
                fprintf(stdout, "P25_Service: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: P25_Service [-m modem_number (1-10)] [-d] [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            fprintf(stderr, "P25_Service: error in fork(), exiting\n");
            return 1;
        }

        // If this is the parent, exit
        if (pid > 0) return 0;

        // We are the child from here onwards
        setsid();

        umask(0);
    }

    /* Initialize RingBuffers */
    RingBuffer_Init(&txBuffer,    800);
    RingBuffer_Init(&rxBuffer,   1600);
    RingBuffer_Init(&gwTxBuffer, 1600);
    RingBuffer_Init(&gwCommand,   200);

    initTimers();
    if (getTimer("status", 2000) < 0) return 0;
    if (getTimer("dbComm", 2000) < 0) return 0;

    memset(dcState, 0x00U, 4U * sizeof(q31_t));
    dcFilter.numStages = DC_FILTER_STAGES;
    dcFilter.pState    = dcState;
    dcFilter.pCoeffs   = DC_FILTER;
    dcFilter.postShift = 0;

    memset(p25boxcar5State, 0x00U, 30U * sizeof(q15_t));
    p25boxcar5Filter.numTaps = BOXCAR5_FILTER_LEN;
    p25boxcar5Filter.pState  = p25boxcar5State;
    p25boxcar5Filter.pCoeffs = BOXCAR5_FILTER;

    memset(modState, 0x00U, 16U * sizeof(q15_t));
    memset(lpState,  0x00U, 60U * sizeof(q15_t));

    modFilter.L           = P25_RADIO_SYMBOL_LENGTH;
    modFilter.phaseLength = RC_0_2_FILTER_PHASE_LEN;
    modFilter.pCoeffs     = RC_0_2_FILTER;
    modFilter.pState      = modState;

    lpFilter.numTaps = LOWPASS_FILTER_LEN;
    lpFilter.pState  = lpState;
    lpFilter.pCoeffs = LOWPASS_FILTER;

    reset();

    char tmp[15];
    readHostConfig(modemName, "config", "modem", tmp);
    if (strcasecmp(tmp, "openmt") == 0)
        packetType = PACKET_TYPE_SAMP;
    else if (strcasecmp(tmp, "openmths") == 0)
        packetType = PACKET_TYPE_BIT;
    else
        packetType = PACKET_TYPE_FRAME;

    readHostConfig(modemName, "P25", "txLevel", tmp);
    if (strlen(tmp) == 0)
    {
        setHostConfig(modemName, "P25", "NAC",     "input", "0x293");
        setHostConfig(modemName, "P25", "txLevel", "input", "50");
        setHostConfig(modemName, "P25", "rfPower", "input", "128");
    }

    readHostConfig(modemName, "P25", "txLevel", tmp);
    txLevel = atoi(tmp);
    readHostConfig(modemName, "P25", "rfPower", tmp);
    rfPower = atoi(tmp);
    readHostConfig(modemName, "P25", "NAC", tmp);
    nac = strtol(tmp, NULL, 16);
    resetNID(nac);

    readHostConfig(modemName, "config", "rxFrequency", modem_rxFrequency);
    readHostConfig(modemName, "config", "txFrequency", modem_txFrequency);

    readHostConfig(modemName, "main", "callsign", station_call);

    clearReflLinkStatus(modemName, "P25");

    int err = pthread_create(&(modemHostid), NULL, &startClient, modemHost);
    if (err != 0)
    {
        fprintf(stderr, "Can't create modem host thread :[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM) fprintf(stderr, "Modem host thread created successfully\n");
    }

    while (connected)
    {
        if (isTimerTriggered("status"))
        {
            char parameter[31];

            if (p25GWConnected)
            {
                readDashbCommand(modemName, "updateConfP25", parameter);
                if (strlen(parameter) > 0)
                {
                    if (strcasecmp(parameter, "readConfig") == 0)
                    {
                        uint8_t buf[9];
                        buf[0] = 0x61;
                        buf[1] = 0x00;
                        buf[2] = 0x09;
                        buf[3] = 0x04;
                        memcpy(buf + 4, TYPE_COMMAND, 4);
                        buf[8] = COMM_UPDATE_CONF;

                        pthread_mutex_lock(&gwCmdMutex);
                        RingBuffer_addData(&gwCommand, buf, 9);
                        pthread_mutex_unlock(&gwCmdMutex);
                    }
                }

                readDashbCommand(modemName, "reflLinkP25", parameter);
                if (strlen(parameter) == 0)
                {
                    resetTimer("status");
                    continue;
                }
                if (strcasecmp(parameter, "unlink") == 0)
                {
                    uint8_t buf[8];
                    buf[0] = 0x61;
                    buf[1] = 0x00;
                    buf[2] = 0x08;
                    buf[3] = 0x04;
                    memcpy(buf + 4, TYPE_DISCONNECT, 4);

                    pthread_mutex_lock(&gwCmdMutex);
                    RingBuffer_addData(&gwCommand, buf, 8);
                    pthread_mutex_unlock(&gwCmdMutex);

                    sleep(3);
                    resetTimer("status");
                    continue;
                }
                else if (!p25ReflConnected)
                {
                    char tmp[31];
                    strcpy(tmp, parameter);
                    char* token = NULL;
                    token       = strtok((char*)tmp, ",");
                    if (token != NULL)
                    {
                        if (strcasecmp(token, "link") == 0)
                        {
                            token = strtok(NULL, ",");
                            char name[8];
                            bzero(name, 8);
                            strcpy(name, token);
                            uint8_t buf[8];
                            buf[0] = 0x61;
                            buf[1] = 0x00;
                            buf[2] = 0x14;
                            buf[3] = 0x04;
                            memcpy(buf + 4, TYPE_CONNECT, 4);
                            char callsign[9];
                            readHostConfig(modemName, "main", "callsign", callsign);

                            pthread_mutex_lock(&gwCmdMutex);
                            RingBuffer_addData(&gwCommand, buf, 8);
                            RingBuffer_addData(&gwCommand, (uint8_t*)callsign, 6);
                            RingBuffer_addData(&gwCommand, (uint8_t*)name, 6);
                            pthread_mutex_unlock(&gwCmdMutex);

                            sleep(3);
                        }
                        else
                            ackDashbCommand(modemName, "reflLinkP25", "failed");
                    }
                    else
                        ackDashbCommand(modemName, "reflLinkP25", "failed");
                }
                else
                    ackDashbCommand(modemName, "reflLinkP25", "failed");
            }
            else
            {
                char parameter[31];
                readDashbCommand(modemName, "reflLinkP25", parameter);
                if (strlen(parameter) > 0)
                {
                    ackDashbCommand(modemName, "reflLinkP25", "No gateway");
                }
            }
            resetTimer("status");
        }
        delay(500000); // 5 secs
    }

    /* Cleanup RingBuffers */
    RingBuffer_Destroy(&txBuffer);
    RingBuffer_Destroy(&rxBuffer);
    RingBuffer_Destroy(&gwTxBuffer);
    RingBuffer_Destroy(&gwCommand);

    clearReflLinkStatus(modemName, "P25");
    fprintf(stderr, "P25 service terminated.\n");
    logError(modemName, "main", "P25 host terminated.");
    return 0;
}
