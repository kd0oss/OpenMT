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
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
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

extern "C" {
#include <tools.h>
#include <RingBuffer.h>  /* C RingBuffer - use extern C for linkage */
#include <ADF7021.h>
}

#include <arm_math.h>

using namespace std;
using namespace M17;

#define VERSION     "2025-08-15"
#define BUFFER_SIZE 1024

// Mode specific data sent to configure modem. Data from MMDVM project by Jonathan Naylor G4KLX
//================================================================================================================================
q31_t          DC_FILTER[]      = {3367972, 0, 3367972, 0, 2140747704, 0}; // {b0, 0, b1, b2, -a1, -a2}
const uint32_t DC_FILTER_STAGES = 1U; // One Biquad stage
const int16_t  TX_RRC_0_5_FILTER[] = {0, 0, 0, 0, -290, -174, 142, 432, 438, 90, -387, -561, -155, 658, 1225, 767,
				  -980, -3326, -4648, -3062, 2527, 11552, 21705, 29724, 32767, 29724, 21705,
				  11552, 2527, -3062, -4648, -3326, -980, 767, 1225, 658, -155, -561, -387, 90,
				  438, 432, 142, -174, -290}; // numTaps = 45, L = 5
const int16_t  RX_RRC_0_5_FILTER[] = {-147, -88, 72, 220, 223, 46, -197, -285, -79, 334, 623, 390, -498, -1691, -2363, -1556, 1284, 5872, 11033,
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
const q15_t SCALING_FACTOR = 18750;      // Q15(0.55)

const uint8_t MAX_SYNC_BIT_START_ERRS = 0U;
const uint8_t MAX_SYNC_BIT_RUN_ERRS   = 2U;

const uint8_t MAX_SYNC_SYMBOL_START_ERRS = 0U;
const uint8_t MAX_SYNC_SYMBOL_RUN_ERRS   = 1U;

const uint8_t BIT_MASK_TABLE[] = {0x80U, 0x40U, 0x20U, 0x10U, 0x08U, 0x04U, 0x02U, 0x01U};

#define WRITE_BIT1(p,i,b) p[(i)>>3] = (b) ? (p[(i)>>3] | BIT_MASK_TABLE[(i)&7]) : (p[(i)>>3] & ~BIT_MASK_TABLE[(i)&7])

const uint8_t M17_SYNC     = 0x77U;
const uint8_t M17_PREAMBLE = 0x77U;
const uint8_t M17_LSF      = 0x00U;
const uint8_t M17_STREAM   = 0x01U;
const uint8_t M17_PACKET   = 0x02U;
const uint8_t M17_EOT      = 0x03U;

const uint8_t NOAVEPTR = 99U;

const uint16_t NOENDPTR = 9999U;

const unsigned int MAX_SYNC_FRAMES = 3U + 1U;

/* M17 Constants - using #define for C compatibility */
#define M17_RADIO_SYMBOL_LENGTH 5U      /* At 24 kHz sample rate */

#define M17_FRAME_LENGTH_BITS    384U
#define M17_FRAME_LENGTH_BYTES   (M17_FRAME_LENGTH_BITS / 8U)
#define M17_FRAME_LENGTH_SYMBOLS (M17_FRAME_LENGTH_BITS / 2U)
#define M17_FRAME_LENGTH_SAMPLES (M17_FRAME_LENGTH_SYMBOLS * M17_RADIO_SYMBOL_LENGTH)

#define M17_SYNC_LENGTH_BITS    16U
#define M17_SYNC_LENGTH_BYTES   (M17_SYNC_LENGTH_BITS / 8U)
#define M17_SYNC_LENGTH_SYMBOLS (M17_SYNC_LENGTH_BITS / 2U)
#define M17_SYNC_LENGTH_SAMPLES (M17_SYNC_LENGTH_SYMBOLS * M17_RADIO_SYMBOL_LENGTH)

const uint8_t M17_LINK_SETUP_SYNC_BYTES[] = {0x55U, 0xF7U};
const uint8_t M17_STREAM_SYNC_BYTES[]     = {0xFFU, 0x5DU};
const uint8_t M17_PACKET_SYNC_BYTES[]     = {0x75U, 0xFFU};
const uint8_t M17_EOF_SYNC_BYTES[]        = {0x55U, 0x5DU};

const q15_t M17_LEVELA =  1481;
const q15_t M17_LEVELB =  494;
const q15_t M17_LEVELC = -494;
const q15_t M17_LEVELD = -1481;

const uint16_t M17_LINK_SETUP_SYNC_BITS = 0x55F7U;
const uint16_t M17_STREAM_SYNC_BITS     = 0xFF5DU;
const uint16_t PACKET_SYNC_WORD         = 0x75FFU;
const uint16_t M17_EOF_SYNC_BITS        = 0x555DU;
const uint16_t M17_PACKET_SYNC_BITS     = 0x75FFU;
const uint16_t M17_EOT_SYNC_BITS        = 0x555DU;

// 5     5     F     7
// 01 01 01 01 11 11 01 11
// +3 +3 +3 +3 -3 -3 +3 -3

const int8_t M17_LINK_SETUP_SYNC_SYMBOLS_VALUES[] = {+3, +3, +3, +3, -3, -3, +3, -3};

const uint8_t M17_LINK_SETUP_SYNC_SYMBOLS = 0xF2U;

// F     F     5     D
// 11 11 11 11 01 01 11 01
// -3 -3 -3 -3 +3 +3 -3 +3

const int8_t M17_STREAM_SYNC_SYMBOLS_VALUES[] = {-3, -3, -3, -3, +3, +3, -3, +3};

const uint8_t M17_STREAM_SYNC_SYMBOLS = 0x0DU;

// 7     5     F     F
// 01 11 01 01 11 11 11 11
// +3 -3 +3 +3 -3 -3 -3 -3

const int8_t M17_PACKET_SYNC_SYMBOLS_VALUES[] = {+3, -3, +3, +3, -3, -3, -3, -3};

const uint8_t M17_PACKET_SYNC_SYMBOLS = 0xB0U;

// 5     5     5     D
// 01 01 01 01 01 01 11 01
// +3 +3 +3 +3 +3 +3 -3 +3

const int8_t M17_EOF_SYNC_SYMBOLS_VALUES[] = {+3, +3, +3, +3, +3, +3, -3, +3};

const uint8_t M17_EOF_SYNC_SYMBOLS = 0xFDU;

enum M17RX_STATE {
  M17RXS_NONE,
  M17RXS_LINK_SETUP,
  M17RXS_STREAM,
  M17RXS_PACKET
};

arm_biquad_casd_df1_inst_q31 dcFilter;
q31_t                        dcState[4];

arm_fir_interpolate_instance_q15 m17modFilter;
q15_t                            m17modState[16U];    // blockSize + phaseLength - 1, 4 + 9 - 1 plus some spare

arm_fir_instance_q15             m17rrc05Filter;
q15_t                            m17rrc05State[70U];         // NoTaps + BlockSize - 1, 42 + 20 - 1 plus some spare

M17RX_STATE m17state;
uint8_t     m17bitBuffer[M17_RADIO_SYMBOL_LENGTH];
q15_t       m17buffer[M17_FRAME_LENGTH_SAMPLES];
uint16_t    m17bitBuf;
uint16_t    m17bitPtr;
uint16_t    m17dataPtr;
uint16_t    m17startPtr;
uint16_t    m17endPtr;
uint16_t    m17syncPtr;
uint16_t    m17bufferPtr;
uint16_t    m17minSyncPtr;
uint16_t    m17maxSyncPtr;
q31_t       m17maxCorr;
uint16_t    m17lostCount;
uint8_t     m17countdown;
M17RX_STATE m17nextState;
q15_t       m17center[16U];
q15_t       m17centerVal;
q15_t       m17threshold[16U];
q15_t       m17thresholdVal;
uint8_t     m17averagePtr;
uint8_t     m17outBuffer[M17_FRAME_LENGTH_BYTES + 3U];
uint8_t*    m17outBufPtr;
uint16_t    m17txDelay;  // In bytes

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
const char *TYPE_SAMPLE         = "SAMP";
const char *TYPE_BITS           = "BITS";

const uint8_t PACKET_TYPE_BIT   = 0;
const uint8_t PACKET_TYPE_SAMP  = 1;
const uint8_t PACKET_TYPE_FRAME = 2;

const uint8_t COMM_SET_DUPLEX   = 0x00;
const uint8_t COMM_SET_SIMPLEX  = 0x01;
const uint8_t COMM_SET_MODE     = 0x02;
const uint8_t COMM_SET_IDLE     = 0x03;
const uint8_t COMM_UPDATE_CONF  = 0x04;

int      sockfd           = 0;
char     modemHost[80]    = "127.0.0.1";
uint8_t  modemId          = 1;          //< Modem Id used to create modem name.
char     modemName[10]    = "modem1";   //< Modem name that this program is associated with.
uint8_t  packetType       = 0;
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
uint16_t serverPort       = 18200;
uint16_t clientPort       = 18000;
uint16_t streamId         = 0;
uint16_t modeHang         = 30000;
time_t   start_time;
uint8_t  txLevel          = 50;
uint8_t  rfPower          = 128;
char     modem_rxFrequency[11]  = "435000000";
char     modem_txFrequency[11]  = "435000000";
uint8_t  symBuffer[600U];
uint16_t symLen;
uint16_t symPtr;

M17::M17LinkSetupFrame rx_lsf;   //< M17 link setup frame
M17::M17LinkSetupFrame pkt_lsf;  //< M17 packet link setup frame
M17::M17FrameDecoder   decoder;  //< M17 frame decoder
M17::M17FrameEncoder   encoder;  //< M17 frame encoder

unsigned int       clientlen;    //< byte size of client's address
char              *hostaddrp;    //< dotted decimal host addr string
int                optval;       //< flag value for setsockopt
struct sockaddr_in serveraddr;   //< server's addr
struct sockaddr_in clientaddr;   //< client addr

/* C RingBuffer - for future C conversion */
RingBuffer txBuffer;
RingBuffer rxBuffer;
RingBuffer gwTxBuffer;
RingBuffer gwCommand;

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

void processBitsNone(bool);
void processBitsData(bool);
void writeBits(uint8_t c, bool isEOT);
void processNone(q15_t sample);
void processData(q15_t sample);
bool correlateSync(uint8_t syncSymbols, const int8_t* syncSymbolValues, const uint8_t* syncBytes, uint8_t maxSymbolErrs, uint8_t maxBitErrs);
void calculateLevels(uint16_t start, uint16_t count);
void samplesToBits(uint16_t start, uint16_t count, uint8_t* buffer, uint16_t offset, q15_t center, q15_t threshold);
bool isTimerTriggered(const char* name);
void resetTimer(const char* name);


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

bool isActiveMode()
{
    char active_mode[10] = "IDLE";

    readStatus(modemName, "main", "active_mode", active_mode);

    if (strcasecmp(active_mode, "M17") == 0 || strcasecmp(active_mode, "IDLE") == 0)
        return true;
    else
        return false;
}

void reset()
{
    m17state        = M17RXS_NONE;
    m17bitBuf       = 0x0000U;
    m17bufferPtr    = 0U;
    m17dataPtr      = 0U;
    m17bitPtr       = 0U;
    m17maxCorr      = 0;
    m17averagePtr   = NOAVEPTR;
    m17startPtr     = NOENDPTR;
    m17endPtr       = NOENDPTR;
    m17syncPtr      = NOENDPTR;
    m17minSyncPtr   = NOENDPTR;
    m17maxSyncPtr   = NOENDPTR;
    m17centerVal    = 0;
    m17thresholdVal = 0;
    m17lostCount    = 0U;
    m17countdown    = 0U;
    m17nextState    = M17RXS_NONE;
    m17txDelay      = 240U;      // 200ms
}

void decodeFrame(const char* type, uint8_t* buffer, uint8_t length, bool isNet)
{
    static char gps[50];
    uint8_t     typeLen = 4;

    char cType[4] = "RF";
    if (isNet)
        strcpy(cType, "NET");

    if (memcmp(type, "M17", 3) == 0 && memcmp(type, TYPE_EOT, typeLen) != 0)
    {
        frame_t frame;
        for (int x = 0; x < 48; x++)
        {
            frame.data()[x] = buffer[x];
        }

        auto ftype  = decoder.decodeFrame(frame);
        rx_lsf      = decoder.getLsf();
        bool lsfOk  = rx_lsf.valid();
        if (!txOn)
        {
            streamId = (rand() & 0x7ff);
            voiceFrameCnt = 0;
            frameCnt = 0;
            validFrame = false;
            lastFrameNum = 0;
            bzero(metaText, 53);
            start_time = time(NULL);
        }
        if (lsfOk)
        {
            pthread_mutex_lock(&gwTxBufMutex);
            if (!isNet && m17ReflConnected && RingBuffer_freeSpace(&gwTxBuffer) >= (length + 8))
            {
                uint8_t buf[8];
                buf[0] = 0x61;
                buf[1] = 0x00;
                buf[2] = length + 8;
                buf[3] = 0x04;
                memcpy(buf + 4, type, 4);
                RingBuffer_addData(&gwTxBuffer, buf, 8);
                RingBuffer_addData(&gwTxBuffer, buffer, length);
            }
            pthread_mutex_unlock(&gwTxBufMutex);
            strcpy(srcCallsign, rx_lsf.getSource().c_str());
            strcpy(dstCallsign, rx_lsf.getDestination().c_str());
            if (!txOn)
            {
                if (write(sockfd, SETMODE, 5) < 0)
                {
                    fprintf(stderr, "ERROR: host disconnect\n");
                    return;
                }
                saveLastCall(1, modemName, "M17", cType, srcCallsign, "", dstCallsign, metaText, NULL, "", true);
            }
            txOn = true;
            validFrame = true;
            pkt_lsf = decoder.getLsf();
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
                (streamType.fields.encSubType == M17_META_TEXT) && rx_lsf.valid() && frameCnt == 6)
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
            else if ((streamType.fields.encType    == M17_ENCRYPTION_NONE) &&
                (streamType.fields.encSubType == M17_META_GNSS) && rx_lsf.valid())
            {
                gpsFound = true;
            }

            if (ftype == M17FrameType::LINK_SETUP && validFrame)
            {
                if (modem_duplex || isNet)
                {
                    uint8_t buf[56] = {0x61, 0x00, 0x38, 0x04, 'M', '1', '7', 'L'};
                    memcpy(buf + 8, buffer, length);
                    if (packetType == PACKET_TYPE_FRAME)
                    {
                        if (write(sockfd, buf, 56) < 0)
                        {
                            fprintf(stderr, "ERROR: host disconnect\n");
                            return;
                        }
                    }
                    else if (packetType == PACKET_TYPE_BIT || packetType == PACKET_TYPE_SAMP)
                    {
                        pthread_mutex_lock(&rxBufMutex);
                        RingBuffer_addData(&rxBuffer, buf, length + 8);
                        pthread_mutex_unlock(&rxBufMutex);
                    }
                }
            }
            else if (ftype == M17FrameType::STREAM && validFrame)
            {
                if (!txOn)
                {
                    if (write(sockfd, SETMODE, 5) < 0)
                    {
                        fprintf(stderr, "ERROR: host disconnect\n");
                        return;
                    }
                    saveLastCall(1, modemName, "M17", cType, srcCallsign, "", dstCallsign, metaText, NULL, "", true);
                }
                M17StreamFrame sf = decoder.getStreamFrame();
                uint16_t diff = (sf.getFrameNumber() & 0x7fff) - (lastFrameNum & 0x7fff);
                if ((sf.getFrameNumber() && 0x8000) != 0x8000)
                {
                    if (diff > 2 || sf.getFrameNumber() == 0)
                    {
                        fprintf(stderr, "Frame number invalid.\n");
                        lastFrameNum = sf.getFrameNumber() & 0x7fff;
                        return;
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
                    //    if (debugM)
                    fprintf(stderr, "Lat: %f  Lon: %f  Alt: %d\n", latitude, longitude, altitude);
                    sprintf(gps, "%f %f %d %d %d", latitude, longitude, altitude, bearing, speed);
                    gpsFound = false;
                }
                voiceFrameCnt++;

                if (isTimerTriggered("status"))
                {
                    saveLastCall(1, modemName, "M17", cType, srcCallsign, "", dstCallsign, metaText, NULL, "", true);
                    resetTimer("status");
                }

                if (modem_duplex || isNet)
                {
                    uint8_t buf[56] = {0x61, 0x00, 0x38, 0x04, 'M', '1', '7', 'S'};
                    memcpy(buf + 8, buffer, length);
                    if (packetType == PACKET_TYPE_FRAME)
                    {
                        if (write(sockfd, buf, 56) < 0)
                        {
                            fprintf(stderr, "ERROR: host disconnect\n");
                            return;
                        }
                    }
                    else if (packetType == PACKET_TYPE_BIT || packetType == PACKET_TYPE_SAMP)
                    {
                        pthread_mutex_lock(&rxBufMutex);
                        RingBuffer_addData(&rxBuffer, buf, length + 8);
                        pthread_mutex_unlock(&rxBufMutex);
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
                {
                    saveLastCall(1, modemName, "M17", cType, srcCallsign, "", dstCallsign, "Packet", (const char*)packetData, "", true);
                }

                if (modem_duplex || isNet)
                {
                    uint8_t buf[56] = {0x61, 0x00, 0x38, 0x04, 'M', '1', '7', 'P'};
                    memcpy(buf + 8, buffer, length);
                    if (packetType == PACKET_TYPE_FRAME)
                    {
                        if (write(sockfd, buf, 56) < 0)
                        {
                            fprintf(stderr, "ERROR: host disconnect\n");
                            return;
                        }
                    }
                    else if (packetType == PACKET_TYPE_BIT || packetType == PACKET_TYPE_SAMP)
                    {
                        pthread_mutex_lock(&rxBufMutex);
                        RingBuffer_addData(&rxBuffer, buf, length + 8);
                        pthread_mutex_unlock(&rxBufMutex);
                    }
                }
            }
        }
    }
    else if (memcmp(type, TYPE_EOT, typeLen) == 0 && validFrame)
    {
        pthread_mutex_lock(&gwTxBufMutex);
        if (!isNet && m17ReflConnected && RingBuffer_freeSpace(&gwTxBuffer) >= (length + 8))
        {
            uint8_t buf[8];
            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = length + 8;
            buf[3] = 0x04;
            memcpy(buf + 4, type, 4);
            RingBuffer_addData(&gwTxBuffer, buf, 8);
            RingBuffer_addData(&gwTxBuffer, buffer, length);
        }
        pthread_mutex_unlock(&gwTxBufMutex);

        frameCnt = 0;
        lastFrameNum = 0;
        /* rx_lsf is a C++ object, will be updated on next decode */

        if (debugM)
            fprintf(stderr, "Found M17 EOT\n");

        float loss_BER = (float)decoder.bitErr / 3.68F;
        duration = difftime(time(NULL), start_time);
        decoder.reset();

        saveLastCall(1, modemName, "M17", cType, srcCallsign, "", dstCallsign, metaText, NULL, gps, false);
        if (voiceFrameCnt > 0)
        {
            saveHistory(modemName, "M17", cType, srcCallsign, "", dstCallsign, loss_BER, metaText, duration);
        }
        else if (totalSMSMessages > 0)
        {
            saveLastCall(1, modemName, "M17", cType, srcCallsign, "", dstCallsign, "Packet", (const char*)packetData, gps, false);
            saveHistory(modemName, "M17", cType, srcCallsign, "", dstCallsign, loss_BER, "Packet", duration);
        }
        voiceFrameCnt = 0;
        gps[0] = 0;

        if (modem_duplex || isNet)
        {
            uint8_t buf[56] = {0x61, 0x00, 0x08, 0x04, 'M', '1', '7', 'E'};
            memcpy(buf + 8, buffer, length);
            if (packetType == PACKET_TYPE_FRAME)
            {
                if (write(sockfd, buf, 8) < 0)
                {
                    fprintf(stderr, "ERROR: host disconnect\n");
                    return;
                }
            }
            else if (packetType == PACKET_TYPE_BIT || packetType == PACKET_TYPE_SAMP)
            {
                buf[2] = length + 8;
                pthread_mutex_lock(&rxBufMutex);
                RingBuffer_addData(&rxBuffer, buf, length + 8);
                pthread_mutex_unlock(&rxBufMutex);
            }
        }
        bzero(metaText, 53);
        validFrame = false;
        txOn = false;
        reflBusy = false;
    }
}


void samples(const q15_t* samples, uint8_t length)
{
    for (uint8_t i = 0U; i < length; i++)
    {
        q15_t sample = samples[i];

        m17bitBuffer[m17bitPtr] <<= 1;
        if (sample < 0)
            m17bitBuffer[m17bitPtr] |= 0x01U;

        m17buffer[m17dataPtr] = sample;

        switch (m17state)
        {
            case M17RXS_LINK_SETUP:
            case M17RXS_STREAM:
            case M17RXS_PACKET:
                processData(sample);
                break;
            default:
                processNone(sample);
                break;
        }

        m17dataPtr++;
        if (m17dataPtr >= M17_FRAME_LENGTH_SAMPLES)
            m17dataPtr = 0U;

        m17bitPtr++;
        if (m17bitPtr >= M17_RADIO_SYMBOL_LENGTH)
            m17bitPtr = 0U;
    }
}

void processNone(q15_t sample)
{
    bool ret1 = correlateSync(M17_LINK_SETUP_SYNC_SYMBOLS, M17_LINK_SETUP_SYNC_SYMBOLS_VALUES, M17_LINK_SETUP_SYNC_BYTES, MAX_SYNC_SYMBOL_START_ERRS, MAX_SYNC_BIT_START_ERRS);
    bool ret2 = correlateSync(M17_STREAM_SYNC_SYMBOLS,     M17_STREAM_SYNC_SYMBOLS_VALUES,     M17_STREAM_SYNC_BYTES,     MAX_SYNC_SYMBOL_START_ERRS, MAX_SYNC_BIT_START_ERRS);
    bool ret3 = correlateSync(M17_PACKET_SYNC_SYMBOLS,     M17_PACKET_SYNC_SYMBOLS_VALUES,     M17_PACKET_SYNC_BYTES,     MAX_SYNC_SYMBOL_START_ERRS, MAX_SYNC_BIT_START_ERRS);

    if (ret1 || ret2 || ret3)
    {
        // On the first sync, start the countdown to the state change
        if (m17countdown == 0U)
        {
            m17averagePtr = NOAVEPTR;

            m17countdown = 5U;

            if (ret1) m17nextState = M17RXS_LINK_SETUP;
            if (ret2) m17nextState = M17RXS_STREAM;
            if (ret3) m17nextState = M17RXS_PACKET;
        }
    }

    if (m17countdown > 0U)
        m17countdown--;

    if (m17countdown == 1U)
    {
        m17minSyncPtr = m17syncPtr + M17_FRAME_LENGTH_SAMPLES - 1U;
        if (m17minSyncPtr >= M17_FRAME_LENGTH_SAMPLES)
            m17minSyncPtr -= M17_FRAME_LENGTH_SAMPLES;

        m17maxSyncPtr = m17syncPtr + 1U;
        if (m17maxSyncPtr >= M17_FRAME_LENGTH_SAMPLES)
            m17maxSyncPtr -= M17_FRAME_LENGTH_SAMPLES;

        m17state     = m17nextState;
        m17countdown = 0U;
        m17nextState = M17RXS_NONE;
    }
}

void processData(q15_t sample)
{
    bool eof = false;

    if (m17minSyncPtr < m17maxSyncPtr)
    {
        if (m17dataPtr >= m17minSyncPtr && m17dataPtr <= m17maxSyncPtr)
        {
            bool ret1 = correlateSync(M17_LINK_SETUP_SYNC_SYMBOLS, M17_LINK_SETUP_SYNC_SYMBOLS_VALUES, M17_LINK_SETUP_SYNC_BYTES, MAX_SYNC_SYMBOL_START_ERRS, MAX_SYNC_BIT_START_ERRS);
            bool ret2 = correlateSync(M17_STREAM_SYNC_SYMBOLS, M17_STREAM_SYNC_SYMBOLS_VALUES, M17_STREAM_SYNC_BYTES,  MAX_SYNC_SYMBOL_RUN_ERRS, MAX_SYNC_BIT_RUN_ERRS);
            bool ret3 = correlateSync(M17_PACKET_SYNC_SYMBOLS, M17_PACKET_SYNC_SYMBOLS_VALUES, M17_PACKET_SYNC_BYTES,  MAX_SYNC_SYMBOL_RUN_ERRS, MAX_SYNC_BIT_RUN_ERRS);

            eof = correlateSync(M17_EOF_SYNC_SYMBOLS, M17_EOF_SYNC_SYMBOLS_VALUES, M17_EOF_SYNC_BYTES, MAX_SYNC_SYMBOL_RUN_ERRS, MAX_SYNC_BIT_RUN_ERRS);

            if (ret1) m17state = M17RXS_LINK_SETUP;
            if (ret2) m17state = M17RXS_STREAM;
            if (ret3) m17state = M17RXS_PACKET;
        }
    } else
    {
        if (m17dataPtr >= m17minSyncPtr || m17dataPtr <= m17maxSyncPtr)
        {
            bool ret1 = correlateSync(M17_LINK_SETUP_SYNC_SYMBOLS, M17_LINK_SETUP_SYNC_SYMBOLS_VALUES, M17_LINK_SETUP_SYNC_BYTES, MAX_SYNC_SYMBOL_START_ERRS, MAX_SYNC_BIT_START_ERRS);
            bool ret2 = correlateSync(M17_STREAM_SYNC_SYMBOLS, M17_STREAM_SYNC_SYMBOLS_VALUES, M17_STREAM_SYNC_BYTES,  MAX_SYNC_SYMBOL_RUN_ERRS, MAX_SYNC_BIT_RUN_ERRS);
            bool ret3 = correlateSync(M17_PACKET_SYNC_SYMBOLS, M17_PACKET_SYNC_SYMBOLS_VALUES, M17_PACKET_SYNC_BYTES,  MAX_SYNC_SYMBOL_RUN_ERRS, MAX_SYNC_BIT_RUN_ERRS);

            eof = correlateSync(M17_EOF_SYNC_SYMBOLS, M17_EOF_SYNC_SYMBOLS_VALUES, M17_EOF_SYNC_BYTES, MAX_SYNC_SYMBOL_RUN_ERRS, MAX_SYNC_BIT_RUN_ERRS);

            if (ret1) m17state = M17RXS_LINK_SETUP;
            if (ret2) m17state = M17RXS_STREAM;
            if (ret3) m17state = M17RXS_PACKET;
        }
    }

    if (eof)
    {
        //  DEBUG4("M17RX: eof sync found pos/center/threshold", m_syncPtr, m_centerVal, m_thresholdVal);

        //    serial.writeM17EOT();
   //     fprintf(stderr, "Found EOT\n");
        decodeFrame(TYPE_EOT, NULL, 0, false);

        m17state      = M17RXS_NONE;
        m17endPtr     = NOENDPTR;
        m17averagePtr = NOAVEPTR;
        m17countdown  = 0U;
        m17nextState  = M17RXS_NONE;
        m17maxCorr    = 0;
    }

    if (m17dataPtr == m17endPtr)
    {
        // Only update the center and threshold if they are from a good sync
        if (m17lostCount == MAX_SYNC_FRAMES)
        {
            m17minSyncPtr = m17syncPtr + M17_FRAME_LENGTH_SAMPLES - 1U;
            if (m17minSyncPtr >= M17_FRAME_LENGTH_SAMPLES)
                m17minSyncPtr -= M17_FRAME_LENGTH_SAMPLES;

            m17maxSyncPtr = m17syncPtr + 1U;
            if (m17maxSyncPtr >= M17_FRAME_LENGTH_SAMPLES)
                m17maxSyncPtr -= M17_FRAME_LENGTH_SAMPLES;
        }

        calculateLevels(m17startPtr, M17_FRAME_LENGTH_SYMBOLS);

        switch (m17state)
        {
            case M17RXS_LINK_SETUP:
                //   DEBUG4("M17RX: link setup sync found pos/center/threshold", m_syncPtr, m_centerVal, m_thresholdVal);
                break;
            case M17RXS_STREAM:
                //    DEBUG4("M17RX: stream sync found pos/center/threshold", m_syncPtr, m_centerVal, m_thresholdVal);
                break;
            case M17RXS_PACKET:
                //    DEBUG4("M17RX: packet sync found pos/center/threshold", m_syncPtr, m_centerVal, m_thresholdVal);
                break;
            default:
                break;
        }

        uint8_t frame[M17_FRAME_LENGTH_BYTES + 3U];
        samplesToBits(m17startPtr, M17_FRAME_LENGTH_SYMBOLS, frame, 8U, m17centerVal, m17thresholdVal);

        // We've not seen a stream sync for too long, signal RXLOST and change to RX_NONE
        m17lostCount--;
        if (m17lostCount == 0U)
        {
            //   DEBUG1("M17RX: sync timed out, lost lock");

            //      serial.writeM17Lost();
       //     fprintf(stderr, "Found LOST\n");
            decodeFrame(TYPE_EOT, NULL, 0, false);

            m17state      = M17RXS_NONE;
            m17endPtr     = NOENDPTR;
            m17averagePtr = NOAVEPTR;
            m17countdown  = 0U;
            m17nextState  = M17RXS_NONE;
            m17maxCorr    = 0;
        }
        else
        {
            frame[0U] = m17lostCount == (MAX_SYNC_FRAMES - 1U) ? 0x01U : 0x00U;

            switch (m17state)
            {
                case M17RXS_LINK_SETUP:
             //       fprintf(stderr, "Found LSF\n");
                    decodeFrame(TYPE_LSF, frame + 1, 48, false);
                    //       writeRSSILinkSetup(frame);
                    break;
                case M17RXS_STREAM:
            //        fprintf(stderr, "Found Stream\n");
                    decodeFrame(TYPE_STREAM, frame + 1, 48, false);
                    //       writeRSSIStream(frame);
                    break;
                case M17RXS_PACKET:
            //        fprintf(stderr, "Found Packet\n");
                    decodeFrame(TYPE_PACKET, frame + 1, 48, false);
                    //       writeRSSIPacket(frame);
                    break;
                default:
                    break;
            }

            m17maxCorr   = 0;
            m17nextState = M17RXS_NONE;
        }
    }
}

bool correlateSync(uint8_t syncSymbols, const int8_t* syncSymbolValues, const uint8_t* syncBytes, uint8_t maxSymbolErrs, uint8_t maxBitErrs)
{
    if (countBits8(m17bitBuffer[m17bitPtr] ^ syncSymbols) <= maxSymbolErrs)
    {
        uint16_t ptr = m17dataPtr + M17_FRAME_LENGTH_SAMPLES - M17_SYNC_LENGTH_SAMPLES + M17_RADIO_SYMBOL_LENGTH;
        if (ptr >= M17_FRAME_LENGTH_SAMPLES)
            ptr -= M17_FRAME_LENGTH_SAMPLES;

        q31_t corr = 0;
        q15_t min =  16000;
        q15_t max = -16000;

        for (uint8_t i = 0U; i < M17_SYNC_LENGTH_SYMBOLS; i++)
        {
            q15_t val = m17buffer[ptr];

            if (val > max)
                max = val;
            if (val < min)
                min = val;

            switch (syncSymbolValues[i])
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

            ptr += M17_RADIO_SYMBOL_LENGTH;
            if (ptr >= M17_FRAME_LENGTH_SAMPLES)
                ptr -= M17_FRAME_LENGTH_SAMPLES;
        }

        if (corr > m17maxCorr)
        {
            if (m17averagePtr == NOAVEPTR)
            {
                m17centerVal = (max + min) >> 1;

                q31_t v1 = (max - m17centerVal) * SCALING_FACTOR;
                m17thresholdVal = q15_t(v1 >> 15);
            }

            uint16_t startPtr = m17dataPtr + M17_FRAME_LENGTH_SAMPLES - M17_SYNC_LENGTH_SAMPLES + M17_RADIO_SYMBOL_LENGTH;
            if (startPtr >= M17_FRAME_LENGTH_SAMPLES)
                startPtr -= M17_FRAME_LENGTH_SAMPLES;

            uint8_t sync[M17_SYNC_LENGTH_BYTES];
            samplesToBits(startPtr, M17_SYNC_LENGTH_SYMBOLS, sync, 0U, m17centerVal, m17thresholdVal);

            uint8_t errs = 0U;
            for (uint8_t i = 0U; i < M17_SYNC_LENGTH_BYTES; i++)
                errs += countBits8(sync[i] ^ syncBytes[i]);

            if (errs <= maxBitErrs)
            {
                m17maxCorr   = corr;
                m17lostCount = MAX_SYNC_FRAMES;
                m17syncPtr   = m17dataPtr;

                m17startPtr = startPtr;

                m17endPtr = m17dataPtr + M17_FRAME_LENGTH_SAMPLES - M17_SYNC_LENGTH_SAMPLES - 1U;
                if (m17endPtr >= M17_FRAME_LENGTH_SAMPLES)
                    m17endPtr -= M17_FRAME_LENGTH_SAMPLES;

                return true;
            }
        }
    }

    return false;
}

void calculateLevels(uint16_t start, uint16_t count)
{
    q15_t maxPos = -16000;
    q15_t minPos =  16000;
    q15_t maxNeg =  16000;
    q15_t minNeg = -16000;

    for (uint16_t i = 0U; i < count; i++)
    {
        q15_t sample = m17buffer[start];

        if (sample > 0)
        {
            if (sample > maxPos)
                maxPos = sample;
            if (sample < minPos)
                minPos = sample;
        } else
        {
            if (sample < maxNeg)
                maxNeg = sample;
            if (sample > minNeg)
                minNeg = sample;
        }

        start += M17_RADIO_SYMBOL_LENGTH;
        if (start >= M17_FRAME_LENGTH_SAMPLES)
            start -= M17_FRAME_LENGTH_SAMPLES;
    }

    q15_t posThresh = (maxPos + minPos) >> 1;
    q15_t negThresh = (maxNeg + minNeg) >> 1;

    q15_t center = (posThresh + negThresh) >> 1;

    q15_t threshold = posThresh - center;

    // DEBUG5("M17RX: pos/neg/center/threshold", posThresh, negThresh, center, threshold);

    if (m17averagePtr == NOAVEPTR)
    {
        for (uint8_t i = 0U; i < 16U; i++)
        {
            m17center[i] = center;
            m17threshold[i] = threshold;
        }

        m17averagePtr = 0U;
    } else
    {
        m17center[m17averagePtr] = center;
        m17threshold[m17averagePtr] = threshold;

        m17averagePtr++;
        if (m17averagePtr >= 16U)
            m17averagePtr = 0U;
    }

    m17centerVal = 0;
    m17thresholdVal = 0;

    for (uint8_t i = 0U; i < 16U; i++)
    {
        m17centerVal += m17center[i];
        m17thresholdVal += m17threshold[i];
    }

    m17centerVal >>= 4;
    m17thresholdVal >>= 4;
}

void samplesToBits(uint16_t start, uint16_t count, uint8_t* buffer, uint16_t offset, q15_t center, q15_t threshold)
{
    for (uint16_t i = 0U; i < count; i++)
    {
        q15_t sample = m17buffer[start] - center;

        if (sample < -threshold)
        {
            WRITE_BIT1(buffer, offset, false);
            offset++;
            WRITE_BIT1(buffer, offset, true);
            offset++;
        } else if (sample < 0)
        {
            WRITE_BIT1(buffer, offset, false);
            offset++;
            WRITE_BIT1(buffer, offset, false);
            offset++;
        } else if (sample < threshold)
        {
            WRITE_BIT1(buffer, offset, true);
            offset++;
            WRITE_BIT1(buffer, offset, false);
            offset++;
        } else
        {
            WRITE_BIT1(buffer, offset, true);
            offset++;
            WRITE_BIT1(buffer, offset, true);
            offset++;
        }

        start += M17_RADIO_SYMBOL_LENGTH;
        if (start >= M17_FRAME_LENGTH_SAMPLES)
            start -= M17_FRAME_LENGTH_SAMPLES;
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
        //  delay(1000);
    }

    if (bytePos > 8 && isEOT) // If EOT and number of new bytes less than 200 pad out with 0.
    {
        memset(bytes+bytePos, 0, 200 - bytePos);
        write(sockfd, bytes, 200);
        bytePos = 8;
    }
}

void writeSamples(uint8_t c, bool isEOT)
{
    static uint8_t bytes[200] = {0x61, 0x00, 0xc8, 0x04, 'S', 'A', 'M', 'P'}; // setup 8 byte header
    static uint8_t bytePos = 8; // set first position after header
    q15_t          inBuffer[4U];
    q15_t          outBuffer[M17_RADIO_SYMBOL_LENGTH * 4U];

    const uint8_t MASK = 0xC0U;

    for (uint8_t i = 0U; i < 4U; i++, c <<= 2)
    {
        switch (c & MASK)
        {
            case 0xC0U:
                inBuffer[i] = M17_LEVELA;
            break;
            case 0x80U:
                inBuffer[i] = M17_LEVELB;
            break;
            case 0x00U:
                inBuffer[i] = M17_LEVELC;
            break;
            default:
                inBuffer[i] = M17_LEVELD;
            break;
        }
    }

    arm_fir_interpolate_q15(&m17modFilter, inBuffer, outBuffer, 4U);

    for (uint8_t i = 0U; i < M17_RADIO_SYMBOL_LENGTH * 4U; i++)
    {
        bytes[bytePos++] = (outBuffer[i] & 0x00ff);
        bytes[bytePos++] = (outBuffer[i] & 0xff00) >> 8;
        if (bytePos == 200)
        {
            write(sockfd, bytes, 200);
            bytePos = 8;
            delay(500);
        }
    }

    if (bytePos > 8 && isEOT) // If EOT and number of new bytes less than 200 pad out with 0.
    {
        memset(bytes+bytePos, 0, 200 - bytePos);
        write(sockfd, bytes, 200);
        bytePos = 8;
    }
}

void processTx(uint8_t* data, const uint8_t length, const uint8_t type, bool m_tx)
{
    if (type == M17_LSF && symLen == 0U)
    {
        if (!m_tx)
        {
            for (uint16_t i = 0U; i < m17txDelay; i++)
                symBuffer[symLen++] = M17_SYNC;
        }
        else
        {
            for (uint8_t i = 0U; i < length; i++)
                symBuffer[symLen++] = data[i];
        }

        symPtr = 0U;
        fprintf(stderr, "LSF\n");
    }

    if (type == M17_STREAM && symLen == 0U)
    {
        for (uint8_t i = 0U; i < length; i++)
            symBuffer[symLen++] = data[i];

        symPtr = 0U;
        fprintf(stderr, "Stream\n");
    }

    if (type == M17_PACKET && symLen == 0U)
    {
        for (uint8_t i = 0U; i < length; i++)
            symBuffer[symLen++] = data[i];

        symPtr = 0U;
        fprintf(stderr, "Packet\n");
    }

    if (type == M17_EOT && symLen == 0U)
    {
        for (uint8_t i = 0U; i < length; i++)
            symBuffer[symLen++] = data[i];

        symPtr = 0U;
        fprintf(stderr, "EOT\n");
    }

    while (symLen > 0U)
    {
        uint8_t c = symBuffer[symPtr++];
        if (packetType == PACKET_TYPE_SAMP)
        {
            if (type == M17_EOT && symPtr >= symLen)
                writeSamples(c, true);
            else
                writeSamples(c, false);
        }
        else if (packetType == PACKET_TYPE_BIT)
        {
            if (type == M17_EOT && symPtr >= symLen)
                writeBits(c, true);
            else
                writeBits(c, false);
        }

        if (symPtr >= symLen)
        {
            symPtr = 0U;
            symLen = 0U;
            return;
        }
    }
}

void processBits(uint8_t* bytes, uint8_t length)
{
    bool bit;

    for (uint8_t i = 0; i < length; i++)
    {
        for (int8_t j = 7; j >= 0; j--)
        {
            if ((bytes[i] & (0x01 << j)) > 0)
                bit = true;
            else
                bit = false;

            switch (m17state)
            {
                case M17RXS_LINK_SETUP:
                case M17RXS_STREAM:
                case M17RXS_PACKET:
                    processBitsData(bit);
                    break;
                default:
                    processBitsNone(bit);
                    break;
            }
        }
    }
}

void processBitsNone(bool bit)
{
    m17bitBuf <<= 1;
    if (bit)
        m17bitBuf |= 0x01U;

    // Exact matching of the packet sync bit sequence
    if (countBits16(m17bitBuf ^ M17_PACKET_SYNC_BITS) <= MAX_SYNC_BIT_START_ERRS)
    {
        fprintf(stderr, "M17RX: packet sync found in None\n");
        for (uint8_t i = 0U; i < M17_SYNC_LENGTH_BYTES; i++)
            m17outBufPtr[i] = M17_PACKET_SYNC_BYTES[i];

        m17lostCount = MAX_SYNC_FRAMES;
        m17bufferPtr = M17_SYNC_LENGTH_BITS;
        m17state     = M17RXS_PACKET;

        //  io.setDecode(true);

        return;
    }

    // Exact matching of the link setup sync bit sequence
    if (countBits16(m17bitBuf ^ M17_LINK_SETUP_SYNC_BITS) <= MAX_SYNC_BIT_START_ERRS)
    {
        fprintf(stderr, "M17RX: link setup sync found in None\n");
        for (uint8_t i = 0U; i < M17_SYNC_LENGTH_BYTES; i++)
            m17outBufPtr[i] = M17_LINK_SETUP_SYNC_BYTES[i];

        m17lostCount = MAX_SYNC_FRAMES;
        m17bufferPtr = M17_SYNC_LENGTH_BITS;
        m17state     = M17RXS_LINK_SETUP;

        // io.setDecode(true);

        return;
    }

    // Exact matching of the stream sync bit sequence
    if (countBits16(m17bitBuf ^ M17_STREAM_SYNC_BITS) <= MAX_SYNC_BIT_START_ERRS)
    {
        fprintf(stderr, "M17RX: stream sync found in None\n");
        for (uint8_t i = 0U; i < M17_SYNC_LENGTH_BYTES; i++)
            m17outBufPtr[i] = M17_STREAM_SYNC_BYTES[i];

        m17lostCount = MAX_SYNC_FRAMES;
        m17bufferPtr = M17_SYNC_LENGTH_BITS;
        m17state     = M17RXS_STREAM;

        // io.setDecode(true);

        return;
    }
}

void processBitsData(bool bit)
{
    m17bitBuf <<= 1;
    if (bit)
        m17bitBuf |= 0x01U;

    WRITE_BIT1(m17outBufPtr, m17bufferPtr, bit);

    m17bufferPtr++;
    if (m17bufferPtr > M17_FRAME_LENGTH_BITS)
        reset();

    // Only search for the syncs in the right place +-1 bit
    if (m17bufferPtr >= (M17_SYNC_LENGTH_BITS - 1U) && m17bufferPtr <= (M17_SYNC_LENGTH_BITS + 1U))
    {
        // Fuzzy matching of the packet sync bit sequence
        if (countBits16(m17bitBuf ^ M17_PACKET_SYNC_BITS) <= MAX_SYNC_BIT_RUN_ERRS)
        {
            //   DEBUG2("M17RX: found packet sync, pos", m17bufferPtr - M17_SYNC_LENGTH_BITS);
            m17lostCount = MAX_SYNC_FRAMES;
            m17bufferPtr = M17_SYNC_LENGTH_BITS;
            m17state     = M17RXS_PACKET;
            return;
        }

        // Fuzzy matching of the link setup sync bit sequence
        if (countBits16(m17bitBuf ^ M17_LINK_SETUP_SYNC_BITS) <= MAX_SYNC_BIT_RUN_ERRS)
        {
            //   DEBUG2("M17RX: found link setup sync, pos", m17bufferPtr - M17_SYNC_LENGTH_BITS);
            m17lostCount = MAX_SYNC_FRAMES;
            m17bufferPtr = M17_SYNC_LENGTH_BITS;
            m17state     = M17RXS_LINK_SETUP;
            return;
        }

        // Fuzzy matching of the stream sync bit sequence
        if (countBits16(m17bitBuf ^ M17_STREAM_SYNC_BITS) <= MAX_SYNC_BIT_RUN_ERRS)
        {
            //   DEBUG2("M17RX: found stream sync, pos", m17bufferPtr - M17_SYNC_LENGTH_BITS);
            m17lostCount = MAX_SYNC_FRAMES;
            m17bufferPtr = M17_SYNC_LENGTH_BITS;
            m17state     = M17RXS_STREAM;
            return;
        }

        // Fuzzy matching of the EOT sync bit sequence
        if (countBits16(m17bitBuf ^ M17_EOT_SYNC_BITS) <= MAX_SYNC_BIT_RUN_ERRS)
        {
            //   DEBUG2("M17RX: found eot sync, pos", m17bufferPtr - M17_SYNC_LENGTH_BITS);
            //      serial.writeM17EOT();
            //        uint8_t buf[8] = {0x61, 0x00, 0x08, 0x04, 'M', '1', '7', 'E'};
            decodeFrame(TYPE_EOT, NULL, 0, false);
            reset();
            return;
        }
    }

    // Send a frame to the host if the required number of bits have been received
    if (m17bufferPtr == M17_FRAME_LENGTH_BITS)
    {
        // We've not seen a sync for too long, signal RXLOST and change to RX_NONE
        m17lostCount--;
        if (m17lostCount == 0U)
        {
            //   DEBUG1("M17RX: sync timed out, lost lock");
            //     uint8_t buf[8] = {0x61, 0x00, 0x08, 0x04, 'M', '1', '7', 'E'};
            decodeFrame(TYPE_EOT, NULL, 0, false);
            //      serial.writeM17Lost();
            reset();
        }
        else
        {
            // Write data to host
            m17outBuffer[0U] = m17lostCount == (MAX_SYNC_FRAMES - 1U) ? 0x01U : 0x00U;

            switch (m17state)
            {
                case M17RXS_LINK_SETUP:
                {
                    //    uint8_t buf[56] = {0x61, 0x00, 0x08, 0x04, 'M', '1', '7', 'L'};
                    //    memcpy(buf + 8, m17outBuffer, 48);
                    decodeFrame(TYPE_LSF, m17outBuffer+1, 48, false);
                    //   writeRSSILinkSetup(m17outBuffer);
                    break;
                }
                case M17RXS_STREAM:
                {
                    //    uint8_t buf[56] = {0x61, 0x00, 0x08, 0x04, 'M', '1', '7', 'S'};
                    //    memcpy(buf + 8, m17outBuffer, 48);
                    decodeFrame(TYPE_STREAM, m17outBuffer+1, 48, false);
                    //   writeRSSIStream(m17outBuffer);
                    break;
                }
                case M17RXS_PACKET:
                {
                    //    uint8_t buf[56] = {0x61, 0x00, 0x08, 0x04, 'M', '1', '7', 'P'};
                    //    memcpy(buf + 8, m17outBuffer, 48);
                    decodeFrame(TYPE_PACKET, m17outBuffer+1, 48, false);
                    //   writeRSSIPacket(m17outBuffer);
                    break;
                }
                default:
                    break;
            }

            // Start the next frame
            memset(m17outBuffer, 0x00U, M17_FRAME_LENGTH_BYTES + 3U);
            m17bufferPtr = 0U;
        }
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
                    fprintf(stderr, "RX invalid header.\n");
                    pthread_mutex_lock(&rxBufMutex);
                    RingBuffer_getData(&rxBuffer, buf, 1);
                    pthread_mutex_unlock(&rxBufMutex);
                    loop = 0;
                    continue;
                }
                uint8_t byte[3];
                uint16_t len = 0;

                pthread_mutex_lock(&rxBufMutex);
                RingBuffer_peek(&rxBuffer, byte, 3);
                avail = RingBuffer_dataSize(&rxBuffer);
                pthread_mutex_unlock(&rxBufMutex);

                len = (byte[1] << 8) + byte[2];
                if (avail >= len)
                {
                    uint8_t buf[len];

                    pthread_mutex_lock(&rxBufMutex);
                    RingBuffer_getData(&rxBuffer, buf, len);
                    pthread_mutex_unlock(&rxBufMutex);

                    if (len == 5)
                    {
                        if (write(sockfd, buf, len) < 0)
                        {
                            fprintf(stderr, "ERROR: remote disconnect\n");
                            break;
                        }
                        loop = 0;
                        continue;
                    }
                    uint8_t type[4];
                    memcpy(type, buf + 4, 4);
                    if (memcmp(type, TYPE_LSF, 4) == 0)
                    {
                        processTx(buf + 8, 48, M17_LSF, false); // send preamble
                        processTx(buf + 8, 48, M17_LSF, true);  // send LSF
                    }
                    else if (memcmp(type, TYPE_STREAM, 4) == 0)
                    {
                        processTx(buf + 8, 48, M17_STREAM, true);
                    }
                    else if (memcmp(type, TYPE_PACKET, 4) == 0)
                    {
                        processTx(buf + 8, 48, M17_PACKET, true);
                    }
                    else if (memcmp(type, TYPE_EOT, 4) == 0)
                    {
                        processTx(buf + 8, 48, M17_EOT, true);
                    }
                }
            }
            loop = 0;
        }
    }

    fprintf(stderr, "RX thread exited.\n");
    int iRet         = 500;
    pthread_mutex_lock(&stateMutex);
    m17GWConnected = false;
    pthread_mutex_unlock(&stateMutex);
    pthread_exit(&iRet);
    return NULL;
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

    ssize_t len = 0;
    uint8_t offset = 0;
    uint16_t respLen = 0;
    uint8_t typeLen = 0;
    uint8_t configLen = 4 + 4 + 11 + 6 + 1 + 40 + 16 + 1 + 1; // + 1 + 1 + 1 + 1 + 42 + 1 + 1 + 1 + 1 + 45;

    buffer[0] = 0x61;
    buffer[1] = 0x00;
    buffer[2] = configLen;
    buffer[3] = 0x04;
    memcpy(buffer+4, TYPE_MODE, 4);
    memcpy(buffer+8, MODE_NAME, 11);
    memcpy(buffer+19, MODEM_TYPE, 6);
    buffer[25] = txLevel;
    //    buffer[25] = USE_DC_FILTER;
    //    buffer[26] = USE_LP_FILTER;
    uint8_t bytes[40];

    // Dev: +1 symb 800 Hz, symb rate = 4800
    uint32_t reg3 = 0x2A4CC093;
    uint32_t reg10 = 0x049E472A;

    // K=32
    uint32_t reg4  = (uint32_t) 0b0100           << 0;   // register 4
    reg4 |= (uint32_t) 0b011                     << 4;   // mode, 4FSK
    reg4 |= (uint32_t) 0b0                       << 7;
    reg4 |= (uint32_t) 0b11                      << 8;
    reg4 |= (uint32_t) 590U                      << 10;  // Disc BW
    reg4 |= (uint32_t) 7U                        << 20;  // Post dem BW
    reg4 |= (uint32_t) 0b10                      << 30;  // IF filter (12.5 kHz)

    uint32_t reg2 = (uint32_t) 0b10              << 28;  // invert data (and RC alpha = 0.5)
    reg2 |= (uint32_t) 0b111                     << 4;   // modulation (RC 4FSK)

    uint32_t reg13 = (uint32_t) 0b1101           << 0;   // register 13
    reg13 |= (uint32_t) ADF7021_SLICER_TH_M17    << 4;   // slicer threshold

    ifConf(bytes, reg2, reg3, reg4, reg10, reg13, atol(modem_rxFrequency), atol(modem_txFrequency),
           txLevel, rfPower, 0, true);

    memcpy(buffer + 26, bytes, 40);
    uint64_t tmp[2] = {M17_LINK_SETUP_SYNC_BITS, 0xffff};
    memcpy(buffer + 66, (uint8_t*)tmp, 16);
    buffer[82] = 0x20; // use 16 bit counter
    buffer[83] = 0x11; // TX MSB first / scan multiplier
    write(sockfd, buffer, configLen);
    sleep(1);

    decoder.reset();
    /* rx_lsf is a C++ object, will be updated on first decode */

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
            dump((char*)"M17 data", (unsigned char*)buffer, respLen);

        if (memcmp(type, TYPE_COMMAND, typeLen) == 0)
        {
            if (buffer[8] == COMM_SET_DUPLEX)
                modem_duplex = true;
            else if (buffer[8] == COMM_SET_SIMPLEX)
                modem_duplex = false;
        }
        else if (memcmp(type, TYPE_SAMPLE, typeLen) == 0)
        {
            packetType = PACKET_TYPE_SAMP;
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

                    q15_t offset = q15_t(__SSAT((dcLevel >> 16), 16));

                    q15_t dcSamples[2];
                    for (uint8_t i = 0U; i < 2; i++)
                        dcSamples[i] = in[i] - offset;

                    arm_fir_fast_q15(&m17rrc05Filter, dcSamples, smp, 2);
                }
                else
                    arm_fir_fast_q15(&m17rrc05Filter, in, smp, 2);

                samples(smp, 2);
            }
        }
        else if (memcmp(type, TYPE_BITS, typeLen) == 0)
        {
            packetType = PACKET_TYPE_BIT;
            processBits(buffer + 8, respLen - 8);
        }
        else
        {
            packetType = PACKET_TYPE_FRAME;
            decodeFrame((const char*)type, buffer + 8, respLen - 8, false);
        }
    }
    txOn = false;
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
                    fprintf(stderr, "TX invalid header.\n");
                    pthread_mutex_lock(&gwTxBufMutex);
                    RingBuffer_getData(&gwTxBuffer, buf, 1);
                    pthread_mutex_unlock(&gwTxBufMutex);
                    loop = 0;
                    continue;
                }
                uint8_t  byte[3];
                uint16_t len = 0;

                pthread_mutex_lock(&gwTxBufMutex);
                RingBuffer_peek(&gwTxBuffer, byte, 3);
                avail = RingBuffer_dataSize(&gwTxBuffer);
                pthread_mutex_unlock(&gwTxBufMutex);

                len = (byte[1] << 8) + byte[2];
                if (avail >= len)
                {
                    uint8_t tbuf[len];

                    pthread_mutex_lock(&gwTxBufMutex);
                    RingBuffer_getData(&gwTxBuffer, tbuf, len);
                    pthread_mutex_unlock(&gwTxBufMutex);

                    if (write(sockfd, tbuf, len) < 0)
                    {
                        fprintf(stderr, "ERROR: remote disconnect\n");
                        break;
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
                fprintf(stderr, "TX invalid header.\n");
                pthread_mutex_lock(&gwCmdMutex);
                RingBuffer_getData(&gwCommand, buf, 1);
                pthread_mutex_unlock(&gwCmdMutex);
            }
            else
            {
                uint8_t  byte[3];
                uint16_t len = 0;

                pthread_mutex_lock(&gwCmdMutex);
                RingBuffer_peek(&gwCommand, byte, 3);
                cmdAvail = RingBuffer_dataSize(&gwCommand);
                pthread_mutex_unlock(&gwCmdMutex);

                len = (byte[1] << 8) + byte[2];
                if (cmdAvail >= len)
                {
                    uint8_t cbuf[len];

                    pthread_mutex_lock(&gwCmdMutex);
                    RingBuffer_getData(&gwCommand, cbuf, len);
                    pthread_mutex_unlock(&gwCmdMutex);

                    if (write(sockfd, cbuf, len) < 0)
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

    pthread_mutex_lock(&stateMutex);
    m17GWConnected = true;
    pthread_mutex_unlock(&stateMutex);
    addGateway(modemName, "main", "M17");
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
        memcpy(type, buffer + 4, typeLen);

        if (debugM)
            dump((char*)"M17 Gateway data:", (unsigned char*)buffer, respLen);

        if (memcmp(type, TYPE_NACK, typeLen) == 0)
        {
            pthread_mutex_lock(&stateMutex);
            m17ReflConnected = false;
            pthread_mutex_unlock(&stateMutex);
            char tmp[8];
            bzero(tmp, 8);
            memcpy(tmp, buffer + 4 + typeLen, 7);
            ackDashbCommand(modemName, "reflLinkM17", "failed");
            setReflectorStatus(modemName, "M17", (const char*)tmp, false);
        }
        else if (memcmp(type, TYPE_CONNECT, typeLen) == 0)
        {
            pthread_mutex_lock(&stateMutex);
            m17ReflConnected = true;
            pthread_mutex_unlock(&stateMutex);
            ackDashbCommand(modemName, "reflLinkM17", "success");
            char tmp[9];
            bzero(tmp, 9);
            memcpy(tmp, buffer + 4 + typeLen, 7);
            tmp[7] = ' ';
            tmp[8] = buffer[15];
            setReflectorStatus(modemName, "M17", (const char*)tmp, true);
        }
        else if (memcmp(type, TYPE_DISCONNECT, typeLen) == 0)
        {
            pthread_mutex_lock(&stateMutex);
            m17ReflConnected = false;
            pthread_mutex_unlock(&stateMutex);
            char tmp[10];
            bzero(tmp, 10);
            memcpy(tmp, buffer + 4 + typeLen, 7);
            tmp[7] = ' ';
            tmp[8] = buffer[15];
            ackDashbCommand(modemName, "reflLinkM17", "success");
            setReflectorStatus(modemName, "M17", (const char*)tmp, false);
        }
        else if (memcmp(type, TYPE_STATUS, typeLen) == 0)
        {
        }
        else if (memcmp(type, TYPE_LSF, typeLen) == 0 && isActiveMode())
        {
            pthread_mutex_lock(&stateMutex);
            reflBusy = true;
            pthread_mutex_unlock(&stateMutex);
            decodeFrame(TYPE_LSF, buffer + 8, 48, true);
        }
        else if (memcmp(type, TYPE_STREAM, typeLen) == 0 && isActiveMode())
        {
            decodeFrame(TYPE_STREAM, buffer + 8, 48, true);
        }
        else if (memcmp(type, TYPE_PACKET, typeLen) == 0 && isActiveMode())
        {
            decodeFrame(TYPE_PACKET, buffer + 8, 48, true);
        }
        else if (memcmp(type, TYPE_EOT, typeLen) == 0 && isActiveMode())
        {
            decodeFrame(TYPE_EOT, buffer + 8, 48, true);
        }
        delay(5);
    }
    fprintf(stderr, "Gateway disconnected.\n");
    pthread_mutex_lock(&stateMutex);
    m17GWConnected   = false;
    m17ReflConnected = false;
    pthread_mutex_unlock(&stateMutex);
    delGateway(modemName, "main", "M17");
    clearReflLinkStatus(modemName, "M17");
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

        pthread_t rxid;
        err = pthread_create(&(rxid), NULL, &rxThread, NULL);
        if (err != 0)
            fprintf(stderr, "Can't create rx thread :[%s]", strerror(err));
        else
        {
            if (debugM) fprintf(stderr, "RX thread created successfully\n");
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
                serverPort = 18200 + modemId - 1;
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
                fprintf(stdout, "M17_Service: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: M17_Service [-m modem_number (1-10)] [-d] [-v] [-x]\n");
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

    /* Initialize C RingBuffers - for future C conversion */
    RingBuffer_Init(&txBuffer, 800);
    RingBuffer_Init(&rxBuffer, 1600);
    RingBuffer_Init(&gwTxBuffer, 800);
    RingBuffer_Init(&gwCommand, 200);

    initTimers();
    if (getTimer("status", 2000) < 0) return 0;
    if (getTimer("dbComm", 2000) < 0) return 0;

    memset(dcState, 0x00U, 4U * sizeof(q31_t));
    dcFilter.numStages = DC_FILTER_STAGES;
    dcFilter.pState    = dcState;
    dcFilter.pCoeffs   = DC_FILTER;
    dcFilter.postShift = 0;

    memset(m17rrc05State, 0x00U, 70U * sizeof(q15_t));
    m17rrc05Filter.numTaps = RX_RRC_0_5_FILTER_LEN;
    m17rrc05Filter.pState  = m17rrc05State;
    m17rrc05Filter.pCoeffs = RX_RRC_0_5_FILTER;

    memset(m17modState, 0x00U, 16U * sizeof(q15_t));
    m17modFilter.L           = M17_RADIO_SYMBOL_LENGTH;
    m17modFilter.phaseLength = TX_RRC_0_5_FILTER_PHASE_LEN;
    m17modFilter.pCoeffs     = TX_RRC_0_5_FILTER;
    m17modFilter.pState      = m17modState;

    char tmp[15];
    readHostConfig(modemName, "config", "modem", tmp);
    if (strcasecmp(tmp, "openmt") == 0)
        packetType = PACKET_TYPE_SAMP;
    else if (strcasecmp(tmp, "openmths") == 0)
        packetType = PACKET_TYPE_BIT;
    else
        packetType = PACKET_TYPE_FRAME;

    readHostConfig(modemName, "M17", "txLevel", tmp);
    if (strlen(tmp) == 0)
    {
        setHostConfig(modemName, "M17", "txLevel", "input", "50");
        setHostConfig(modemName, "M17", "rfPower", "input", "128");
    }

    readHostConfig(modemName, "config", "rxFrequency", modem_rxFrequency);
    readHostConfig(modemName, "config", "txFrequency", modem_txFrequency);

    readHostConfig(modemName, "M17", "txLevel", tmp);
    txLevel = atoi(tmp);
    readHostConfig(modemName, "M17", "rfPower", tmp);
    rfPower = atoi(tmp);

    //   clearDashbCommands();
    clearReflLinkStatus(modemName, "M17");
    reset();

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
        if (isTimerTriggered("dbComm"))
        {
            char parameter[31];

            if (m17GWConnected)
            {
                readDashbCommand(modemName, "updateConfM17", parameter);
                if (strlen(parameter) > 0)
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

                readDashbCommand(modemName, "reflLinkM17", parameter);
                if (strlen(parameter) == 0)
                {
                    resetTimer("dbComm");
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
                    resetTimer("dbComm");
                    continue;
                }
                else if (!m17ReflConnected)
                {
                    char tmp[41];
                    strcpy(tmp, parameter);
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
                            uint8_t buf[8];
                            buf[0] = 0x61;
                            buf[1] = 0x00;
                            buf[2] = 0x18;
                            buf[3] = 0x04;
                            memcpy(buf + 4, TYPE_CONNECT, 4);
                            char callsign[9];
                            bzero(callsign, 9);
                            readHostConfig(modemName, "main", "callsign", callsign);
                            pthread_mutex_lock(&gwCmdMutex);
                            RingBuffer_addData(&gwCommand, buf, 8);
                            RingBuffer_addData(&gwCommand, (uint8_t*)callsign, 8);
                            RingBuffer_addData(&gwCommand, (uint8_t*)name, 7);
                            RingBuffer_addData(&gwCommand, (uint8_t*)module, 1);
                            pthread_mutex_unlock(&gwCmdMutex);
                            sleep(3);
                        }
                        else
                            ackDashbCommand(modemName, "reflLinkM17", "failed");
                    }
                    else
                        ackDashbCommand(modemName, "reflLinkM17", "failed");
                }
                else
                {
                    ackDashbCommand(modemName, "reflLinkM17", "failed");
                    if (debugM)
                        fprintf(stderr, "Previous reflector still linked.\n");
                }
            }
            else
            {
                readDashbCommand(modemName, "reflLinkM17", parameter);
                if (strlen(parameter) > 0)
                {
                    ackDashbCommand(modemName, "reflLinkM17", "No gateway");
                }
            }
            resetTimer("dbComm");
        }
        delay(500000);
    }
    
    /* Cleanup C RingBuffers */
    RingBuffer_Destroy(&txBuffer);
    RingBuffer_Destroy(&rxBuffer);
    RingBuffer_Destroy(&gwTxBuffer);
    RingBuffer_Destroy(&gwCommand);
    
    fprintf(stderr, "M17 service terminated.\n");
    logError(modemName, "main", "M17 host terminated.");
    return 0;
}
