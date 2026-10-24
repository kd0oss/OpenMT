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
#include <poll.h>
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

#include <dsp_compat.h> //<arm_math.h>
#include <netinet/tcp.h>
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
bool           USE_DC_FILTER               = false;
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
uint8_t*    m17outBufPtr = m17outBuffer + 1;
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
#define M17_MAX_FILL 4U                 /* lost frames replaced at most (160 ms) */
uint8_t  lastStreamFrame[48];           /* last stream frame passed on (raw)     */
uint8_t  lastStreamFrameLen = 0;
uint16_t serverPort       = 18200;
uint16_t clientPort       = 18000;
uint16_t streamId         = 0;
uint16_t modeHang         = 30000;
time_t   start_time;
uint8_t  rxLevel          = 50;
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

pthread_mutex_t rxBufMutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gwTxBufMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gwCmdMutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stateMutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t txSourceMutex = PTHREAD_MUTEX_INITIALIZER; /* Prevents RF and NET TX overlap */

/* Tracks which source currently owns the transmitter */
typedef enum { TX_NONE, TX_RF, TX_NET } TxSource;
TxSource currentTxSource = TX_NONE;

pthread_t modemHostid;
pthread_t gwHostid;
pthread_t timerid;

const uint8_t SETMODE[] = {0x61, 0x00, 0x05, 0x01, COMM_SET_MODE};
const uint8_t SETIDLE[] = {0x61, 0x00, 0x05, 0x01, COMM_SET_IDLE};


void processBitsNone(bool);
void processBitsData(bool);
void processNone(q15_t sample);
void processData(q15_t sample);
bool correlateSync(uint8_t syncSymbols, const int8_t* syncSymbolValues, const uint8_t* syncBytes, uint8_t maxSymbolErrs, uint8_t maxBitErrs);
void calculateLevels(uint16_t start, uint16_t count);
void samplesToBits(uint16_t start, uint16_t count, uint8_t* buffer, uint16_t offset, q15_t center, q15_t threshold);


/*
 * error()
 *
 * Wrapper for perror that ends the program.
 *
 * Parameters:
 *   msg - Text printed before the system error message.
 *
 * Returns: nothing.
 */
void error(char *msg)
{
    reportError(modemName, "M17", "%s: %s", msg, strerror(errno));
    exit(1);
}

;

/*
 * isActiveMode()
 *
 * Tells if this host may handle frames: the active mode of the modem host is
 * M17 or idle.
 *
 * Parameters: none.
 *
 * Returns: true if the active mode is M17 or IDLE.
 */
bool isActiveMode()
{
    char active_mode[10] = "IDLE";

    readStatus(modemName, "main", "active_mode", active_mode);

    if (strcasecmp(active_mode, "M17") == 0 || strcasecmp(active_mode, "IDLE") == 0)
        return true;
    else
        return false;
}

/*
 * reset()
 *
 * Puts the sample receiver back to its start: no state, no sync, no levels,
 * and a transmit delay of 240 bytes (200 ms).
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
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

/* RF repeat: frames are re-encoded from the decoded data instead of being
 * sent on as received.  Passed on raw, every bit error of the uplink went out
 * again -- damaged sync words included, which make the receiving radio lose
 * the frame and resync (the M17 drop outs).  Re-encoded, the downlink has a
 * clean sync word and LICH, the payload as corrected by the Viterbi decoder,
 * and continuous frame numbers.  Network streams are encoded by the gateway
 * already and still go out as they come.  0 = send RF frames on raw. */
#ifndef M17_REGENERATE
#define M17_REGENERATE 1
#endif
/* RF frames with more Viterbi bit errors than this are logged (uplink
 * quality; a clean frame has 0) */
#ifndef M17_RF_ERR_LOG
#define M17_RF_ERR_LOG 10
#endif
bool    regenStarted = false;           /* LSF encoded for this RF over      */
uint8_t lastPayload[16];                /* last stream payload sent (regen)  */

/* The last-frame flag (bit 15 of the frame number) of an RF frame is not
 * trusted on its own: a damaged frame number can carry it in the middle of
 * an over (seen: 0x8079 between 0x0078 and 0x007A).  Sent on, it ended the
 * stream for the receiving radios and for the reflector, which split the
 * over in two -- the parrot echo then lost seconds of audio.  So each
 * regenerated stream frame is held back by one frame: when another stream
 * frame follows it goes out as an ordinary frame, when the over ends (EOT or
 * lost signal) it goes out as the last frame. */
bool    heldValid = false;              /* a payload waits in heldPayload     */
bool    heldLast  = false;              /* it came in flagged as last         */
uint8_t heldPayload[16];
uint16_t heldFn   = 0U;                 /* its frame number, for the log      */

/*
 * queueTxFrame()
 *
 * Queues one 48 byte M17 frame for transmission.  With an MMDVM modem it
 * goes to the modem host at once; with a sample or bit modem it is queued
 * for rxThread(), which modulates it.
 *
 * Parameters:
 *   type4   - The four character type ("M17L", "M17S" and so on).
 *   frame48 - The 48 byte frame.
 *
 * Returns: true if queued, false if the host connection was lost.
 */
static bool queueTxFrame(const char* type4, const uint8_t* frame48)
{
    uint8_t buf[56] = {0x61, 0x00, 0x38, 0x04};

    memcpy(buf + 4, type4, 4);
    memcpy(buf + 8, frame48, 48);
    if (packetType == PACKET_TYPE_FRAME)
    {
        if (write(sockfd, buf, 56) < 0)
        {
            reportError(modemName, "M17", "host disconnect");
            return false;
        }
    }
    else if (packetType == PACKET_TYPE_BIT || packetType == PACKET_TYPE_SAMP)
    {
        pthread_mutex_lock(&rxBufMutex);
        RingBuffer_addData(&rxBuffer, buf, 56);
        pthread_mutex_unlock(&rxBufMutex);
    }
    return true;
}

/*
 * queueGwFrame()
 *
 * Queues one 48 byte M17 frame for the gateway (RF traffic to the
 * reflector).  It is dropped when no reflector is linked or the queue is
 * full.
 *
 * Parameters:
 *   type4   - The four character type.
 *   frame48 - The 48 byte frame.
 *
 * Returns: nothing.
 */
static void queueGwFrame(const char* type4, const uint8_t* frame48)
{
    uint8_t hdr[8] = {0x61, 0x00, 56, 0x04};

    memcpy(hdr + 4, type4, 4);
    pthread_mutex_lock(&gwTxBufMutex);
    if (m17ReflConnected && RingBuffer_freeSpace(&gwTxBuffer) >= 56)
    {
        RingBuffer_addData(&gwTxBuffer, hdr, 8);
        RingBuffer_addData(&gwTxBuffer, frame48, 48);
    }
    pthread_mutex_unlock(&gwTxBufMutex);
}

/*
 * regenStart()
 *
 * Starts a regenerated RF over: encodes the link setup frame (as received,
 * or rebuilt from the LICH on a late entry) and queues it for the modem
 * (duplex only; the transmit side sends the preamble in front of it) and for
 * the gateway.
 *
 * Parameters: none.
 *
 * Returns: true if it worked, false if the host connection was lost.
 */
static bool regenStart(void)
{
    frame_t f;

    encoder.reset();
    encoder.encodeLsf(rx_lsf, f);
    regenStarted = true;
    heldValid    = false;
    queueGwFrame("M17L", f.data());
    if (modem_duplex)
        return queueTxFrame("M17L", f.data());
    return true;
}

/*
 * regenEmit()
 *
 * Encodes one regenerated stream frame and queues it for the modem (duplex
 * only) and the gateway.
 *
 * Parameters:
 *   payload16 - The 16 bytes of voice data.
 *   last      - true to mark it as the last frame of the over.
 *
 * Returns: true if it worked, false if the host connection was lost.
 */
static bool regenEmit(const uint8_t* payload16, bool last)
{
    frame_t   f;
    payload_t pl;

    memcpy(pl.data(), payload16, 16);
    encoder.encodeStreamFrame(pl, f, last);
    memcpy(lastPayload, payload16, 16);
    queueGwFrame("M17S", f.data());
    if (modem_duplex)
        return queueTxFrame("M17S", f.data());
    return true;
}

/*
 * regenFlush()
 *
 * Ends an RF over: the frame that was held back goes out as the last one.
 *
 * Parameters: none.
 *
 * Returns: true if it worked, false if the host connection was lost.
 */
static bool regenFlush(void)
{
    if (!regenStarted || !heldValid)
        return true;
    heldValid = false;
    return regenEmit(heldPayload, true);
}

/*
 * decodeFrame()
 *
 * Handles one M17 frame (link setup, stream or packet) or the end of a
 * transmission, from RF or from the network.  The frame is decoded and the
 * call is followed: callsigns, extended callsigns, text, GNSS position and
 * SMS packets.  Frames from RF are sent to the gateway, and are repeated on
 * RF (regenerated, so that they are clean and numbered right) or passed on
 * as they came; lost stream frames are replaced; the dashboard gets the last
 * call and the history.
 *
 * Parameters:
 *   type   - The four character type: M17L, M17S, M17P or M17E.
 *   buffer - The 48 byte frame (not used for the end of transmission from
 *            RF).
 *   length - Number of bytes in buffer.
 *   isNet  - true if the frame came from the gateway, false if from RF.
 *
 * Returns: nothing.
 */
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

        auto errBefore = decoder.bitErr;
        auto ftype  = decoder.decodeFrame(frame);
        long frameErr  = (long)(decoder.bitErr - errBefore);
        /* RF repeat and RF to reflector use regenerated frames */
        bool rfRegen = M17_REGENERATE && !isNet;
        rx_lsf      = decoder.getLsf();
        bool lsfOk  = rx_lsf.valid();
        if (!txOn)
        {
            streamId = (rand() & 0x7ff);
            voiceFrameCnt = 0;
            frameCnt = 0;
            validFrame = false;
            lastFrameNum = 0;
            lastStreamFrameLen = 0;
            regenStarted = false;
            heldValid = false;
            bzero(metaText, 53);
            start_time = time(NULL);
        }
        if (lsfOk)
        {
            pthread_mutex_lock(&gwTxBufMutex);
            if (!isNet && m17ReflConnected && RingBuffer_freeSpace(&gwTxBuffer) >= (length + 8) &&
                !(rfRegen && (ftype == M17FrameType::LINK_SETUP || ftype == M17FrameType::STREAM)))
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
                    reportError(modemName, "M17", "host disconnect");
                    return;
                }
                saveLastCall(1, modemName, "M17", cType, srcCallsign, "", dstCallsign, metaText, NULL, "", true);
            }
            txOn = true;
            currentTxSource = isNet ? TX_NET : TX_RF;
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
                if (rfRegen)
                {
                    if (!regenStarted && !regenStart())
                        return;
                }
                else if (modem_duplex || isNet)
                {
                    uint8_t buf[56] = {0x61, 0x00, 0x38, 0x04, 'M', '1', '7', 'L'};
                    memcpy(buf + 8, buffer, length);
                    if (packetType == PACKET_TYPE_FRAME)
                    {
                        if (write(sockfd, buf, 56) < 0)
                        {
                            reportError(modemName, "M17", "host disconnect");
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
                        reportError(modemName, "M17", "host disconnect");
                        return;
                    }
                    saveLastCall(1, modemName, "M17", cType, srcCallsign, "", dstCallsign, metaText, NULL, "", true);
                }
                M17StreamFrame sf = decoder.getStreamFrame();
                /* Frame numbers count 0..0x7FFF and wrap; bit 15 marks the last
                 * frame.  Every stream frame is passed on: dropping one left a
                 * 40 ms hole in the TX samples, the modem ran dry and the
                 * receiving radio lost sync for about half a second.
                 *
                 * The decoded number is not always right: with too many bit
                 * errors the decoder keeps the previous frame's data (same
                 * number again), and a damaged number can come out as anything
                 * (0x6158 for 0x0158).  Such a frame is taken as the expected
                 * one.  A number 1..M17_MAX_FILL past the expected one means
                 * frames were lost on the way in: those are replaced (see
                 * below) so the transmission keeps its timing. */
                uint16_t fn       = sf.getFrameNumber() & 0x7fff;
                uint16_t expected = (lastFrameNum + 1U) & 0x7fff;
                uint16_t ahead    = (fn - expected) & 0x7fff;
                uint16_t missing  = 0;

                if (voiceFrameCnt > 0)
                {
                    if (ahead == 0U)
                    {
                        /* in sequence */
                    }
                    else if (ahead <= M17_MAX_FILL)
                    {
                        missing = ahead;
                        fprintf(stderr, "M17: %u frame(s) lost before %04X, filling\n", (unsigned)missing, (unsigned)fn);
                    }
                    else
                    {
                        fn = expected;          /* damaged number or repeat */
                    }
                }
                if ((sf.getFrameNumber() & 0x8000) == 0x8000)
                    fprintf(stderr, "Last frame detected.\n");
                lastFrameNum = fn;
                frameCnt++;
                if (!txOn)
                    voiceFrameCnt = 0;
                txOn = true;
                currentTxSource = isNet ? TX_NET : TX_RF;
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
                    resetTimer("status", 0);
                }

                if (rfRegen)
                {
                    uint16_t rawFn = sf.getFrameNumber();

                    if (frameErr > M17_RF_ERR_LOG)
                        fprintf(stderr, "M17: RF frame %04X decoded with %ld bit errors\n", (unsigned)rawFn, frameErr);
                    if (!regenStarted && !regenStart())
                        return;
                    /* a frame follows: the held one was not the last */
                    if (heldValid)
                    {
                        if (heldLast)
                            fprintf(stderr, "M17: last-frame flag on %04X in mid over, ignored\n", (unsigned)heldFn);
                        heldValid = false;
                        if (!regenEmit(heldPayload, false))
                            return;
                    }
                    /* lost frames: the previous payload again (40 ms of
                     * repeated audio), with their own frame numbers */
                    for (uint16_t i = 0; i < missing; i++)
                    {
                        if (!regenEmit(lastPayload, false))
                            return;
                    }
                    memcpy(heldPayload, sf.getData() + 2, 16);   /* skip frame number */
                    heldLast  = (rawFn & 0x8000) == 0x8000;
                    heldFn    = rawFn;
                    heldValid = true;
                }
                else if (modem_duplex || isNet)
                {
                    uint8_t buf[56] = {0x61, 0x00, 0x38, 0x04, 'M', '1', '7', 'S'};

                    /* Lost frames: send the previous frame again in their
                     * place.  The receiver sees a repeated frame number and
                     * repeats 40 ms of audio, but the stream keeps its timing
                     * and the receiver stays in sync. */
                    if (missing > 0U && lastStreamFrameLen == length)
                    {
                        memcpy(buf + 8, lastStreamFrame, length);
                        for (uint16_t i = 0; i < missing; i++)
                        {
                            if (packetType == PACKET_TYPE_FRAME)
                            {
                                if (write(sockfd, buf, 56) < 0)
                                {
                                    reportError(modemName, "M17", "host disconnect");
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

                    memcpy(buf + 8, buffer, length);
                    if (length <= sizeof(lastStreamFrame))
                    {
                        memcpy(lastStreamFrame, buffer, length);
                        lastStreamFrameLen = length;
                    }
                    if (packetType == PACKET_TYPE_FRAME)
                    {
                        if (write(sockfd, buf, 56) < 0)
                        {
                            reportError(modemName, "M17", "host disconnect");
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
                if (modem_duplex || isNet)
                {
                    uint8_t buf[56] = {0x61, 0x00, 0x38, 0x04, 'M', '1', '7', 'P'};
                    memcpy(buf + 8, buffer, length);
                    if (packetType == PACKET_TYPE_FRAME)
                    {
                        if (write(sockfd, buf, 56) < 0)
                        {
                            reportError(modemName, "M17", "host disconnect");
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
            }
        }
    }
    else if (memcmp(type, TYPE_EOT, typeLen) == 0 && validFrame)
    {
        if (!isNet && !regenFlush())
            return;
        heldValid = false;
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
        lastStreamFrameLen = 0;
        regenStarted = false;
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
                    reportError(modemName, "M17", "host disconnect");
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
        currentTxSource = TX_NONE;
        reflBusy = false;
    }
}


/*
 * samples()
 *
 * Feeds filtered samples to the receiver: each sample is added to the bit
 * history and the frame buffer, then handled by the state machine (hunting
 * for a sync word, or following a frame).
 *
 * Parameters:
 *   samples - The samples.
 *   length  - Number of samples.
 *
 * Returns: nothing.
 */
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

/*
 * processNone()
 *
 * Looks for a link setup, stream or packet sync word while no signal is
 * being followed.  When one is found a short countdown starts, then the
 * receiver changes to the state of that frame and sets the window where the
 * next sync word is expected.
 *
 * Parameters:
 *   sample - The current sample.
 *
 * Returns: nothing.
 */
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

/*
 * processData()
 *
 * Follows a signal: looks for the sync words again in the expected window,
 * handles the end of the transmission, and at the end of each frame finds
 * the levels, slices the frame into bits and passes it to decodeFrame().
 * Too many frames without a sync word end the transmission as lost.
 *
 * Parameters:
 *   sample - The current sample.
 *
 * Returns: nothing.
 */
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
        pthread_mutex_lock(&txSourceMutex);
        if (currentTxSource != TX_NET)
            decodeFrame(TYPE_EOT, NULL, 0, false);
        pthread_mutex_unlock(&txSourceMutex);

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
            pthread_mutex_lock(&txSourceMutex);
            if (currentTxSource != TX_NET)
                decodeFrame(TYPE_EOT, NULL, 0, false);
            pthread_mutex_unlock(&txSourceMutex);

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

            pthread_mutex_lock(&txSourceMutex);
            if (currentTxSource != TX_NET)
            {
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
            }
            pthread_mutex_unlock(&txSourceMutex);

            m17maxCorr   = 0;
            m17nextState = M17RXS_NONE;
        }
    }
}

/*
 * correlateSync()
 *
 * Checks whether a sync word ends at the current sample: first by its
 * symbols, then by correlating the samples against the sync levels, and then
 * by slicing it and counting the bit errors.  The best correlation so far is
 * kept and sets the position of the frame.
 *
 * Parameters:
 *   syncSymbols      - The sign of each of the eight sync symbols, as a byte,
 *                      for the quick check.
 *   syncSymbolValues - The eight symbol values (+3 or -3) of the sync word.
 *   syncBytes        - The sync word as bytes.
 *   maxSymbolErrs    - Number of wrong bits allowed in the quick check.
 *   maxBitErrs       - Number of wrong bits allowed in the sliced sync word.
 *
 * Returns: true if a sync word with a better correlation than before was
 *   found.
 */
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

/*
 * calculateLevels()
 *
 * Works out the centre and the threshold of the symbols of a frame, and
 * averages them with those of the last 16 frames.
 *
 * Parameters:
 *   start - Position of the first symbol in the frame buffer.
 *   count - Number of symbols.
 *
 * Returns: nothing.
 */
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

/*
 * samplesToBits()
 *
 * Slices symbols into dibits (a 4FSK symbol is two bits) and writes them as
 * bits.
 *
 * Parameters:
 *   start     - Position of the first symbol in the frame buffer.
 *   count     - Number of symbols.
 *   buffer    - Receives the bits, most significant bit first.
 *   offset    - Position of the first bit to write in buffer.
 *   center    - The centre level.
 *   threshold - The threshold between the outer and the inner symbols.
 *
 * Returns: nothing.
 */
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

/*
 * writeSamples()
 *
 * Modulates one byte (four symbols): the symbols become levels and the pulse
 * shaping filter makes 5 samples for each.  The samples are collected into
 * SAMP packets of 200 bytes for the modem host; at the end of the
 * transmission the last one is padded with zeros and sent as an EOT packet.
 *
 * Parameters:
 *   c     - The byte to send, four symbols of two bits.
 *   isEOT - true if it is the last byte of the transmission.
 *
 * Returns: nothing.
 */
void writeSamples(uint8_t c, bool isEOT)
{
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

    /* 200 byte SAMP packets, no wait between them */
    modemQueueSamples(sockfd, outBuffer, M17_RADIO_SYMBOL_LENGTH * 4U, isEOT, 0U);
}

/*
 * processTx()
 *
 * Makes the symbols of a link setup frame, stream frame, packet frame or end
 * of transmission and sends them through writeSamples() or modemWriteBits(sockfd, ).  For
 * a link setup frame with m_tx false the preamble is made instead of the
 * frame.
 *
 * Parameters:
 *   data   - The 48 byte frame.
 *   length - Number of bytes in data.
 *   type   - M17_LSF, M17_STREAM, M17_PACKET or M17_EOT.
 *   m_tx   - true to make the frame, false to make only the preamble (link
 *            setup frame only).
 *
 * Returns: nothing.
 */
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
//        fprintf(stderr, "Stream\n");
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
                modemWriteBits(sockfd, c, true);
            else
                modemWriteBits(sockfd, c, false);
        }

        if (symPtr >= symLen)
        {
            symPtr = 0U;
            symLen = 0U;
            return;
        }
    }
}

/*
 * processBits()
 *
 * Feeds bytes of the bit stream of an OpenMT_HS modem to the bit receiver,
 * most significant bit first.
 *
 * Parameters:
 *   bytes  - The bytes.
 *   length - Number of bytes.
 *
 * Returns: nothing.
 */
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

/*
 * processBitsNone()
 *
 * Looks for a sync word in the bit stream while no frame is being followed
 * (an exact match is needed).
 *
 * Parameters:
 *   bit - The next bit.
 *
 * Returns: nothing.
 */
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

/*
 * processBitsData()
 *
 * Collects the bits of a frame.  The next sync word is looked for in the
 * right place (+-1 bit, up to 2 bit errors); a frame is passed to
 * decodeFrame() when all its bits are in, and the end of the transmission or
 * too many frames without a sync word end it.
 *
 * Parameters:
 *   bit - The next bit.
 *
 * Returns: nothing.
 */
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
            pthread_mutex_lock(&txSourceMutex);
            if (currentTxSource != TX_NET)
                decodeFrame(TYPE_EOT, NULL, 0, false);
            pthread_mutex_unlock(&txSourceMutex);
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
            pthread_mutex_lock(&txSourceMutex);
            if (currentTxSource != TX_NET)
                decodeFrame(TYPE_EOT, NULL, 0, false);
            pthread_mutex_unlock(&txSourceMutex);
            //      serial.writeM17Lost();
            reset();
        }
        else
        {
            // Write data to host
            m17outBuffer[0U] = m17lostCount == (MAX_SYNC_FRAMES - 1U) ? 0x01U : 0x00U;

            pthread_mutex_lock(&txSourceMutex);
            if (currentTxSource != TX_NET)
            {
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
            }
            pthread_mutex_unlock(&txSourceMutex);

            // Start the next frame
            memset(m17outBuffer, 0x00U, M17_FRAME_LENGTH_BYTES + 3U);
            m17bufferPtr = 0U;
        }
    }
}

/*
 * timerThread()
 *
 * Simple timer thread: every millisecond it ticks the timers.  When the mode
 * hang time has passed without a transmission, the modem is set back to
 * idle.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL when the thread ends.
 */
void* timerThread(void *arg)
{
    bool     idle = true;

    if (getTimer("modeHang", modeHang) < 0)
    {
        fprintf(stderr, "Timer thread exited.\n");
        pthread_exit(NULL);
        return NULL;
    }

    uint64_t lastTick = 0;      /* timers are advanced by the real time that passed, so a longer sleep loses nothing */

    while (connected)
    {
        delay(HOST_TIMER_PERIOD_US);
        uint32_t elapsed = timerElapsedMs(&lastTick);

        for (uint8_t i = 0; i < 10; i++)
        {
            timerTickN(i, elapsed);
            // fprintf(stderr, "T: %d  N: %s D: %u C: %u\n", i, timer[i].name, timer[i].duration, timer[i].count);
            if (txOn && strcasecmp(timer[i].name, "modeHang") == 0)
            {
                idle = false;
                resetTimer("modeHang", 0);
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
                        resetTimer("modeHang", 0);
                    }
                }
            }
        }
    }

    fprintf(stderr, "Timer thread exited.\n");
    pthread_exit(NULL);
    return NULL;
}

/* A stream or packet with nothing in front of it on air (no LSF caught: late
 * entry on RF, or a new over after a pause) gets the preamble first.  Without
 * it the transmission starts with no samples in hand, so any later gap in the
 * input reaches the air, and the receiver has nothing to lock on to. */
#define M17_TX_PAUSE_MS 500             /* longer gap = a new transmission */

/*
 * rxThread()
 *
 * Thread that takes the packets queued for the modem (frames to transmit and
 * mode commands) and sends them to the modem host.  Frames are modulated or
 * sent as bits; a stream or packet with no link setup frame before it, or
 * after a pause of more than 500 ms, gets the preamble first.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL when the thread ends.
 */
void* rxThread(void* arg)
{
    bool    leadInSent = false;         /* preamble sent for this transmission */
    int64_t lastTxMs   = 0;

    while (connected)
    {
        delay(HOST_POLL_US);

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
                            reportError(modemName, "M17", "remote disconnect");
                            break;
                        }
                        continue;
                    }
                    uint8_t type[4];
                    int64_t nowTxMs = (int64_t)nowMs();

                    memcpy(type, buf + 4, 4);
                    if (nowTxMs - lastTxMs > M17_TX_PAUSE_MS)
                        leadInSent = false;     /* long pause: a new over */
                    lastTxMs = nowTxMs;

                    if (memcmp(type, TYPE_LSF, 4) == 0)
                    {
                        processTx(buf + 8, 48, M17_LSF, false); // send preamble
                        processTx(buf + 8, 48, M17_LSF, true);  // send LSF
                        leadInSent = true;
                    }
                    else if (memcmp(type, TYPE_STREAM, 4) == 0 || memcmp(type, TYPE_PACKET, 4) == 0)
                    {
                        if (!leadInSent)
                        {
                            fprintf(stderr, "M17: transmission starts without LSF, sending preamble first\n");
                            processTx(buf + 8, 48, M17_LSF, false); // preamble only
                            leadInSent = true;
                        }
                        if (memcmp(type, TYPE_STREAM, 4) == 0)
                            processTx(buf + 8, 48, M17_STREAM, true);
                        else
                            processTx(buf + 8, 48, M17_PACKET, true);
                    }
                    else if (memcmp(type, TYPE_EOT, 4) == 0)
                    {
                        processTx(buf + 8, 48, M17_EOT, true);
                        leadInSent = false;
                    }
                }
            }
        }
    }

    fprintf(stderr, "RX thread exited.\n");
    pthread_mutex_lock(&stateMutex);
    m17GWConnected = false;
    pthread_mutex_unlock(&stateMutex);
    pthread_exit(NULL);
    return NULL;
}

/*
 * startClient()
 *
 * Start up communication with the modem host: connects, registers the M17
 * mode with the modem settings (the ADF7021 registers, the sync word), then
 * reads its packets: commands (duplex or simplex), samples (level, filter
 * and receiver), bits and frames (decodeFrame()).
 *
 * Parameters:
 *   arg - The address of the modem host, as a string.
 *
 * Returns: NULL when the thread ends; the connection is closed and the
 *   program is stopped.
 */
void* startClient(void *arg)
{
    struct sockaddr_in serv_addr;
    uint8_t buffer[BUFFER_SIZE] = {0};
    char hostAddress[80];
    strcpy(hostAddress, (char*)arg);
    // Create socket file descriptor
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        reportError(modemName, "M17", "Socket creation error: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1)
    {
        reportError(modemName, "M17", "fcntl F_GETFL: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }


    int on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    // Prepare the sockaddr_in structure
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(clientPort);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (!hostToAddress(hostAddress, &serv_addr.sin_addr))
    { // Connect to localhost
        reportError(modemName, "M17", "Invalid address/ Address not supported");
        exit(EXIT_FAILURE);
    }

    // Connect to the server
    setNoDelay(sockfd);
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        if (errno != EINPROGRESS)
        {
            reportError(modemName, "M17", "connection failed: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    pthread_t rxid;
    int err = pthread_create(&(rxid), NULL, &rxThread, NULL);
    if (err != 0)
        reportError(modemName, "M17", "Can't create rx thread :[%s]", strerror(err));
    else
    {
        if (debugM) fprintf(stderr, "RX thread created successfully\n");
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
    /* Clear the frame decoder before the registration goes out: once the modem host has
       the registration, frames can arrive, and a reset after that would wipe the link
       setup frame of a call that has already started. */
    decoder.reset();
    /* rx_lsf is a C++ object, will be updated on first decode */

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
            reportError(modemName, "M17", "M17_Service: error when reading from server, errno=%d", errno);
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

//        if (debugM)
  //          hexDump((char*)"M17 data", (unsigned char*)buffer, respLen);

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
                q31_t res1 = in[0] * (rxLevel * 128);
                in[0] = __SSAT((res1 >> 15), 16);
                res1 = in[1] * (rxLevel * 128);
                in[1] = __SSAT((res1 >> 15), 16);

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

                    q15_t offset = __SSAT((dcLevel >> 16), 16);

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
        else if (typeLen >= 3 && memcmp(type, "M17", 3) == 0)
        {
            /* Only an M17 frame from the modem means MMDVM frame mode; any other
               packet type must not change the mode the modem is working in. */
            packetType = PACKET_TYPE_FRAME;
            pthread_mutex_lock(&txSourceMutex);
            if (currentTxSource == TX_NET)
            {
                fprintf(stderr, "RF: frame rejected - NET TX active\n");
                pthread_mutex_unlock(&txSourceMutex);
            }
            else
            {
                decodeFrame((const char*)type, buffer + 8, respLen - 8, false);
                pthread_mutex_unlock(&txSourceMutex);
            }
        }
    }
    txOn = false;
    currentTxSource = TX_NONE;
    fprintf(stderr, "Disconnected from host.\n");
    // Close socket
    close(sockfd);
    connected = false;
    pthread_exit(NULL);
    return 0;
}

/*
 * txThread()
 *
 * Sends bytes to the gateway: the queued frames of RF traffic and the queued
 * commands.
 *
 * Parameters:
 *   arg - The socket of the gateway, as an intptr_t.
 *
 * Returns: NULL when the thread ends.
 */
void* txThread(void *arg)
{
    int  sockfd = (intptr_t)arg;
    while (1)
    {
        delay(HOST_POLL_US);

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
                        reportError(modemName, "M17", "remote disconnect");
                        break;
                    }
                }
            }

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
                        reportError(modemName, "M17", "remote disconnect");
                        break;
                    }
                }
            }
        }
    }

    fprintf(stderr, "TX thread exited.\n");
    pthread_exit(NULL);
    return NULL;
}

/* Arrival time of the last frame from the network.  The gateway can be
 * unlinked, or stop, without sending the end frame; the host would then stay
 * keyed (and the dashboard in TX) for ever. */
static uint64_t m17NetMs = 0U;

/*
 * m17NetEnd()
 *
 * Ends a network call that has no end frame, as if the gateway had sent it
 * (decodeFrame() with an end of transmission): the modem is told to stop, the
 * last call and history are saved and the state goes idle.
 *
 * Parameters:
 *   force - true to end the call now, false to end it only when no frame has
 *           come from the gateway for 1.5 seconds.
 *
 * Returns: nothing.
 */
static void m17NetEnd(bool force)
{
    pthread_mutex_lock(&txSourceMutex);
    if (currentTxSource == TX_NET && (force || nowMs() - m17NetMs > 1500U))
    {
        uint8_t eot[48];

        memset(eot, 0x00, sizeof(eot));
        fprintf(stderr, "M17: network call ended, no end frame from the gateway\n");
        decodeFrame(TYPE_EOT, eot, 48, true);
    }
    pthread_mutex_unlock(&txSourceMutex);
}

/*
 * processGatewaySocket()
 *
 * Handles the packets from the gateway: the link status (NACK, CONN and
 * DISC) goes to the dashboard, and link setup, stream, packet and end of
 * transmission frames from the network go to decodeFrame() when this mode is
 * active and RF is not transmitting.  Registers the gateway with the
 * dashboard while it is connected.
 *
 * Parameters:
 *   arg - The socket of the gateway, as an intptr_t.
 *
 * Returns: NULL when the thread ends.
 */
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
        /* Wait for the gateway; when it is quiet, see whether a network call
         * has gone silent (the gateway was unlinked or stopped sending). */
        struct pollfd gwPoll = {childfd, POLLIN, 0};
        int           gwWait = poll(&gwPoll, 1, 200);

        if (gwWait == 0)
        {
            m17NetEnd(false);
            continue;
        }
        if (gwWait < 0 && errno == EINTR)
            continue;

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
            reportError(modemName, "M17", "M17_Service: error when reading from M17 gateway, errno=%d", errno);
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

        if (typeLen == 4 && (memcmp(type, TYPE_LSF, 4) == 0 || memcmp(type, TYPE_STREAM, 4) == 0 || memcmp(type, TYPE_PACKET, 4) == 0))
            m17NetMs = nowMs();

        if (debugM)
            hexDump((char*)"M17 Gateway data:", (unsigned char*)buffer, respLen);

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
            ackDashbCommand(modemName, "reflLinkM17", "linked");
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
            m17NetEnd(true);
            char tmp[10];
            bzero(tmp, 10);
            memcpy(tmp, buffer + 4 + typeLen, 7);
            tmp[7] = ' ';
            tmp[8] = buffer[15];
            ackDashbCommand(modemName, "reflLinkM17", "unlinked");
            setReflectorStatus(modemName, "M17", (const char*)tmp, false);
        }
        else if (memcmp(type, TYPE_STATUS, typeLen) == 0)
        {
        }
        else if (memcmp(type, TYPE_LSF, typeLen) == 0 && isActiveMode())
        {
            pthread_mutex_lock(&txSourceMutex);
            if (currentTxSource == TX_RF)
            {
                fprintf(stderr, "NET: LSF rejected - RF TX active\n");
                pthread_mutex_unlock(&txSourceMutex);
            }
            else
            {
                pthread_mutex_lock(&stateMutex);
                reflBusy = true;
                pthread_mutex_unlock(&stateMutex);
                decodeFrame(TYPE_LSF, buffer + 8, 48, true);
                pthread_mutex_unlock(&txSourceMutex);
            }
        }
        else if (memcmp(type, TYPE_STREAM, typeLen) == 0 && isActiveMode())
        {
            pthread_mutex_lock(&txSourceMutex);
            if (currentTxSource == TX_RF)
            {
                pthread_mutex_unlock(&txSourceMutex);
            }
            else
            {
                decodeFrame(TYPE_STREAM, buffer + 8, 48, true);
                pthread_mutex_unlock(&txSourceMutex);
            }
        }
        else if (memcmp(type, TYPE_PACKET, typeLen) == 0 && isActiveMode())
        {
            pthread_mutex_lock(&txSourceMutex);
            if (currentTxSource == TX_RF)
            {
                pthread_mutex_unlock(&txSourceMutex);
            }
            else
            {
                decodeFrame(TYPE_PACKET, buffer + 8, 48, true);
                pthread_mutex_unlock(&txSourceMutex);
            }
        }
        else if (memcmp(type, TYPE_EOT, typeLen) == 0 && isActiveMode())
        {
            pthread_mutex_lock(&txSourceMutex);
            if (currentTxSource == TX_RF)
            {
                pthread_mutex_unlock(&txSourceMutex);
            }
            else
            {
                decodeFrame(TYPE_EOT, buffer + 8, 48, true);
                pthread_mutex_unlock(&txSourceMutex);
            }
        }
        delay(5);
    }
    fprintf(stderr, "Gateway disconnected.\n");
    m17NetEnd(true);
    pthread_mutex_lock(&stateMutex);
    m17GWConnected   = false;
    m17ReflConnected = false;
    pthread_mutex_unlock(&stateMutex);
    delGateway(modemName, "main", "M17");
    clearReflLinkStatus(modemName, "M17");
    pthread_exit(NULL);
    return 0;
}

/*
 * startTCPServer()
 *
 * Listens for the gateway on the server port.  For each connection it starts
 * txThread() and processGatewaySocket().
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL when the thread ends.
 */
void *startTCPServer(void *arg)
{
    struct hostent *hostp; /* client host info */
    int childfd; /* child socket */
    int sockFd;

    sockFd = socket(AF_INET, SOCK_STREAM, 0);

    setNoDelay(sockFd);
    if (sockFd < 0)
    {
        reportError(modemName, "M17", "M17_Service: error when creating the socket: %s", strerror(errno));
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
        reportError(modemName, "M17", "M17_Service: error when binding the socket to port %u: %s", serverPort, strerror(errno));
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
        if (childfd >= 0)
            setNoDelay(childfd);
        if (childfd < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            reportError(modemName, "M17", "accept failed: %s", strerror(errno));
            break;
        }

        if (childfd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;

        hostaddrp = inet_ntoa(clientaddr.sin_addr);
        if (hostaddrp == NULL)
        {
            reportError(modemName, "M17", "Can't get the address of the client.");
            close(childfd);
            continue;
        }

        if (debugM)
        {
            /* The name is only logged, so a client with no reverse DNS entry is still accepted */
            hostp = gethostbyaddr((const char*)&clientaddr.sin_addr.s_addr, sizeof(clientaddr.sin_addr.s_addr), AF_INET);
            fprintf(stderr, "Server established connection with %s (%s)\n",
                    hostp != NULL ? hostp->h_name : "unknown", hostaddrp);
        }

        pthread_t txid;
        int err = pthread_create(&(txid), NULL, &txThread, (void*)(intptr_t)childfd);
        if (err != 0)
            reportError(modemName, "M17", "Can't create tx thread :[%s]", strerror(err));
        else
        {
            if (debugM)
                fprintf(stderr, "TX thread created successfully\n");
        }

        pthread_t procid;
        err = pthread_create(&(procid), NULL, &processGatewaySocket, (void*)(intptr_t)childfd);
        if (err != 0)
        {
            reportError(modemName, "M17", "Can't create gateway process thread :[%s]", strerror(err));
            continue;
        }
        else
        {
            if (debugM)
                fprintf(stderr, "Client process thread created successfully\n");
        }
        delay(1000);
    }
    pthread_exit(NULL);
    return NULL;
}

/*
 * main()
 *
 * Program entry. Parses the command line (-m modem number 1-10, which also
 * selects the TCP ports, -d daemon, -v version, -x debug), reads the
 * configuration, sets the filters up, starts the threads, and then passes
 * the dashboard commands (configuration update, link and unlink of a
 * reflector) on to the gateway until the connection to the modem host is
 * lost.
 *
 * Parameters:
 *   argc - Number of command line arguments.
 *   argv - Command line arguments.
 *
 * Returns: 0 on normal exit, 1 on a command line or startup error.
 */
int main(int argc, char **argv)
{
    bool daemon = 0;
    int  ret;
    int  c;

    while ((c = getopt(argc, argv, ":m:a:s:u:p:c:dxv")) != -1)
    {
        switch (c)
        {
            case 'm':
            {
                modemId = atoi(optarg);
                if (modemId < 1 || modemId > 10)
                {
                    reportError(modemName, "M17", "Invalid modem number.");
                    return 1;
                }
                sprintf(modemName, "modem%d", modemId);
                clientPort = 18000 + modemId - 1;
                serverPort = 18200 + modemId - 1;
                fprintf(stderr, "Modem name: %s\n", modemName);
            }
                break;
            case 'a':
                snprintf(modemHost, sizeof(modemHost), "%s", optarg);   /* where the modem host runs */
                break;
            case 's':
            case 'u':
            case 'p':
            case 'c':
                dbOption(c, optarg);   /* database server, user, password, configuration file */
                break;
            case ':':
                fprintf(stderr, "Option '-%c' requires a value.\n", optopt);
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
                fprintf(stderr, "Usage: M17_Service [-m modem_number (1-10)] [-a address] [-s dbserver] [-u dbuser] [-p dbpassword] [-c configfile] [-d] [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            reportError(modemName, "M17", "M17_Service: error in fork(), exiting");
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
    if (strcasecmp(tmp, "openmt") == 0 || strcasecmp(tmp, "openmtsx") == 0)
        packetType = PACKET_TYPE_SAMP;
    else if (strcasecmp(tmp, "openmths") == 0)
        packetType = PACKET_TYPE_BIT;
    else
        packetType = PACKET_TYPE_FRAME;

    readHostConfig(modemName, "M17", "txLevel", tmp);
    if (strlen(tmp) == 0)
    {
        setHostConfig(modemName, "M17", "rxLevel", "input", "50");
        setHostConfig(modemName, "M17", "txLevel", "input", "50");
        setHostConfig(modemName, "M17", "rfPower", "input", "128");
    }

    readHostConfig(modemName, "config", "rxFrequency", modem_rxFrequency);
    readHostConfig(modemName, "config", "txFrequency", modem_txFrequency);

    readHostConfig(modemName, "M17", "rxLevel", tmp);
    rxLevel = atoi(tmp);
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
        reportError(modemName, "M17", "Can't create modem host thread :[%s]", strerror(err));
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
        reportError(modemName, "M17", "Can't create gateway host thread :[%s]", strerror(err));
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
        reportError(modemName, "M17", "Can't create timer thread :[%s]", strerror(err));
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
                    resetTimer("dbComm", 0);
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
                    resetTimer("dbComm", 0);
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
            resetTimer("dbComm", 0);
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
