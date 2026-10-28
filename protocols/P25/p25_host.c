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
#include <poll.h>
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
#include <fec_tools.h>
#include <ADF7021.h>

#include <dsp_compat.h> //<arm_math.h>

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
char     src_callsign[16]    = "";   //< Callsign or id of the source (a DMR id lookup gives up to 10 characters; an id up to 8 digits)
char     dst_callsign[16]    = "";   //< Callsign or id of the destination
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
uint8_t  rxLevel             = 50;
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
uint8_t*    m_buffer = p25outBuffer + 1U;
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


void processNone(q15_t sample);
void processHdr(q15_t sample);
void processLdu(q15_t sample);
bool correlateSync();
void decodeFrame(uint8_t* buffer, uint8_t length, bool isNet);
void processTx(const uint8_t* data, uint8_t length, const char* type, bool isTx);
void* startTCPServer(void* arg);

/*
 * error()
 *
 * Reports a message with reportError() and ends the program with exit status 1.
 *
 * Parameters:
 *   msg - Text printed in front of the system error message.
 *
 * Returns: nothing.
 */
void error(char* msg)
{
    reportError(modemName, "P25", "%s: %s", msg, strerror(errno));
    exit(1);
}

;

/*
 * isActiveMode()
 *
 * Tells whether this host may process frames: the modem host's active mode
 * is P25 or idle.
 *
 * Parameters: none.
 *
 * Returns: true if the active mode is P25 or IDLE, false if another protocol
 *   owns the modem.
 */
bool isActiveMode()
{
    char active_mode[10] = "IDLE";

    readStatus(modemName, "main", "active_mode", active_mode);

    if (strcasecmp(active_mode, "P25") == 0 || strcasecmp(active_mode, "IDLE") == 0)
        return true;
    else
        return false;
}

/*
 * reset()
 *
 * Resets the SAMPLE and BIT mode receive state machine: the state, buffer
 * pointers, sync pointers, correlation values and counters go back to their
 * start-up values.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
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
  //  p25nac          = 0U;
    p25poLen        = 0U;
    p25poPtr        = 0U;
    p25txDelay      = 240U;
    p25_64BitBuffer = 0U;
    memset(p25mi, 0x00U, P25_MI_LENGTH_BYTES);
}

/* 1 = rewrite the voice frames of RF LDUs with their FEC-corrected version
 * (what MMDVMHost does) before they are relayed.  0 = only count errors.
 * Frames that needed more than P25_FEC_MAX_ERRORS corrections (p25_fec.h)
 * are relayed unchanged. */
#ifndef P25_FEC_REGENERATE
#define P25_FEC_REGENERATE 1
#endif

static unsigned int p25RfErrs = 0U;     // bit errors found in this RF call
static unsigned int p25RfBits = 0U;     // protected bits checked in this RF call

/*
 * p25RfVoice()
 *
 * Checks the voice frames of a received RF LDU.  It counts the FEC errors
 * and, if regeneration is enabled, corrects the frames and rebuilds the NID.
 *
 * Parameters:
 *   data     - The LDU frame, starting with the OpenMT header; the LDU
 *              itself is at offset 8.
 *   nidValid - true if the NID of the frame was decoded without errors.
 *   duid     - Data unit id of the frame.
 *
 * Returns: nothing.
 */
static void p25RfVoice(uint8_t* data, bool nidValid, uint8_t duid)
{
    p25RfErrs += p25RegenerateLDU(data + 8, P25_FEC_REGENERATE != 0);
    p25RfBits += P25_FEC_BITS_PER_LDU;

    /* The NID was matched against the expected values, so put them back */
    if (P25_FEC_REGENERATE != 0 && nidValid)
    {
        encodeNID(data + 8, duid);
    }
}

/*
 * p25RfBer()
 *
 * Prints the BER of the RF call that has just ended and clears the error
 * counts.  A network call has no BER and leaves the counts alone.
 *
 * Parameters:
 *   isNet - true if the call came from the network.
 *
 * Returns: The BER as a fraction of bits in error, or 0 if there was none.
 */
static float p25RfBer(bool isNet)
{
    float ber = 0.0f;

    /* Network calls were never on the air here, so they have no BER and
     * must not disturb the counters of an RF call. */
    if (isNet)
    {
        return ber;
    }

    if (p25RfBits > 0U)
    {
        ber = (float)p25RfErrs / (float)p25RfBits;
        fprintf(stderr, "BER: %.6f (%u errors / %u bits)\n", ber, p25RfErrs, p25RfBits);
    }

    p25RfErrs = 0U;
    p25RfBits = 0U;

    return ber;
}

/* True while the link control of the call being relayed is known: LDU1 gives
 * the first, LDU2 the second.  A frame is only rebuilt from link control that
 * was decoded in this call, never from an earlier call's. */
static bool p25RebuildLcValid   = false;
static bool p25RebuildLdu2Valid = false;

/*
 * p25RebuildFrame()
 *
 * Rebuilds a frame from what is decoded from it, as MMDVMHost does: nothing
 * the radio sent is repeated as it came, except the voice itself (whose FEC
 * is regenerated separately, see p25RfVoice()).
 *
 *   - Every frame gets its sync and its NID (with this host's NAC) again, and
 *     its busy bits.
 *   - The header is decoded (Golay and Reed-Solomon) and encoded again, so
 *     the message indicator, algorithm, key and talkgroup are repeated clean.
 *   - LDU1 and LDU2 get their link control encoded again from the decoded
 *     link control (format, ids, message indicator, algorithm, key), and
 *     their low speed data corrected and written again.
 *   - The terminator with link control has its link control (Golay and
 *     Reed-Solomon) decoded and encoded again, whatever its format.  One that
 *     does not decode keeps what it came with.
 *   - Trunking signalling (TSDU) and the packet data header (PDU) have their
 *     rate 1/2 trellis block decoded, checked with its CRC and encoded
 *     again.  The data blocks that follow a PDU header are not received by
 *     this host.
 * A frame whose link control does not decode keeps the link control it came
 * with, unless this call already gave a good one.
 *
 * The busy bits of a terminator follow the sender: a frame from the network
 * has them as the gateway sets them, one from the radio as this host sets
 * them for its own terminator.
 *
 * Parameters:
 *   frame - The P25 frame (without the 8 byte packet prefix), changed in
 *           place.
 *   duid  - Data unit id of the frame.
 *   isNet - true if the frame came from the network.
 *
 * Returns: nothing.
 */
static void p25RebuildFrame(uint8_t* frame, uint8_t duid, bool isNet)
{
    switch (duid)
    {
        case P25_DUID_HEADER:
            p25RebuildLcValid   = false;
            p25RebuildLdu2Valid = false;
            if (decodeHeader(frame))
            {
                addSync(frame);
                encodeNID(frame, duid);
                encodeHeader(frame);
                addBusyBits(frame, P25_HDR_FRAME_LENGTH_BITS, false, true);
            }
            break;

        case P25_DUID_LDU1:
            if (decodeLDU1(frame))
                p25RebuildLcValid = true;
            addSync(frame);
            encodeNID(frame, duid);
            if (p25RebuildLcValid)
                encodeLDU1(frame);
            lowSpeedData_process(frame);
            addBusyBits(frame, P25_LDU_FRAME_LENGTH_BITS, false, true);
            break;

        case P25_DUID_LDU2:
            if (decodeLDU2(frame))
                p25RebuildLdu2Valid = true;
            addSync(frame);
            encodeNID(frame, duid);
            if (p25RebuildLdu2Valid)
                encodeLDU2(frame);
            lowSpeedData_process(frame);
            addBusyBits(frame, P25_LDU_FRAME_LENGTH_BITS, false, true);
            break;

        case P25_DUID_TERM:
            p25RebuildLcValid   = false;
            p25RebuildLdu2Valid = false;
            addSync(frame);
            encodeNID(frame, duid);
            addBusyBits(frame, P25_TERM_FRAME_LENGTH_BITS, isNet, !isNet);
            break;

        case P25_DUID_TERM_LC:
        {
            uint8_t lc[9];

            p25RebuildLcValid   = false;
            p25RebuildLdu2Valid = false;
            if (decodeTDULC(frame, lc))
                encodeTDULC(frame, lc);
            addSync(frame);
            encodeNID(frame, duid);
            addBusyBits(frame, P25_TERMLC_FRAME_LENGTH_BITS, isNet, !isNet);
            break;
        }

        case P25_DUID_TSDU:
            /* Trunking signalling: the TSBK is decoded (trellis 1/2 and
             * CRC) and encoded again; one that does not decode is repeated
             * as it came */
            if (rebuildTrellis12(frame))
            {
                addSync(frame);
                encodeNID(frame, duid);
            }
            addBusyBits(frame, P25_TSDU_FRAME_LENGTH_BITS, false, true);
            break;

        case P25_DUID_PDU:
            /* Packet data header, the one block of a PDU that this host
             * receives: decoded and encoded again the same way */
            if (rebuildTrellis12(frame))
            {
                addSync(frame);
                encodeNID(frame, duid);
                addBusyBits(frame, P25_PDU_HDR_FRAME_LENGTH_BITS, false, true);
            }
            break;

        default:
            break;
    }
}

/* Set by decodeP25() when the frame it decoded is trunking signalling or a
 * packet data header from the radio: a frame that is repeated in duplex mode
 * although no voice call is on. */
static bool p25RelayData = false;

/*
 * decodeP25()
 *
 * Handles one P25 frame from the radio or the network.  The NID is decoded
 * to find the data unit (header, LDU1, LDU2, TSDU, PDU or terminator); when
 * the NID is bad the unit is guessed from the one before.  The header starts
 * a call and keys the modem, LDU frames are forwarded to the gateway (and to
 * the dashboard record), and a terminator ends the call.  For RF voice the
 * FEC errors are counted first.  The frame is then rebuilt from what is
 * decoded from it (p25RebuildFrame()), so what is relayed or sent to the
 * gateway is clean.
 *
 * Parameters:
 *   data  - The frame, starting with the OpenMT header; the P25 data is at
 *           offset 8.
 *   isNet - true if the frame came from the network, false if from the
 *           radio.
 *
 * Returns: nothing.
 */
void decodeP25(uint8_t* data, bool isNet)
{
    // Decode the NID
    bool valid = decodeNID(data + 8);

    p25RelayData = false;

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

    /* RF voice: count the FEC errors and regenerate the frames before the
     * gateway copy and the relay below are made. */
    if (!isNet)
    {
        if (duid == P25_DUID_LDU1 && rf_state == RS_RF_LISTENING)
        {
            p25RfErrs = 0U;
            p25RfBits = 0U;
            p25RfVoice(data, valid, duid);
        }
        else if ((duid == P25_DUID_LDU1 || duid == P25_DUID_LDU2) && rf_state == RS_RF_AUDIO)
        {
            p25RfVoice(data, valid, duid);
        }
    }

    /* Rebuild the frame from what is decoded from it, before the gateway
     * copy and the relay below are made. */
    p25RebuildFrame(data + 8, duid, isNet);

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
                if (findDMRId(p25srcId, src_callsign, sizeof(src_callsign), tmp, sizeof(tmp)))
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
        p25RelayData = !isNet;
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
            saveHistory(modemName, "P25", cType, src_callsign, "", dst_callsign, p25RfBer(isNet), "", duration);

            pthread_mutex_lock(&stateMutex);
            rf_state = RS_RF_LISTENING;
            pthread_mutex_unlock(&stateMutex);

            p25lastDUID = duid;
        }
        return;
    }
    else if (duid == P25_DUID_PDU)
    {
        p25lastDUID  = duid;
        p25RelayData = !isNet;
        return;
    }
}

/*
 * samples()
 *
 * Receive entry point for SAMPLE mode.  Each sample is added to the sample
 * buffer and the bit buffer, and the state machine (idle, header or LDU) is
 * run for it.
 *
 * Parameters:
 *   samples - Demodulated samples.
 *   length  - Number of samples.
 *
 * Returns: nothing.
 */
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

/*
 * processNone()
 *
 * Idle receive state.  Looks for the P25 frame sync, and when it has been
 * found keeps looking for a short time to pick the best match before
 * starting the header or LDU.
 *
 * Parameters:
 *   sample - The newest received sample.
 *
 * Returns: nothing.
 */
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

/*
 * samplesToBits()
 *
 * Converts samples to dibits for the receive frame, using the centre and
 * threshold levels to pick the symbol (two bits for each sample).
 *
 * Parameters:
 *   start     - Position of the first sample in the sample buffer.
 *   count     - Number of symbols to convert.
 *   buffer    - Receives the bits.
 *   offset    - Bit position in buffer where the first bit is written.
 *   center    - Centre level of the signal.
 *   threshold - Level that separates the inner and outer symbols.
 *
 * Returns: nothing.
 */
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

/*
 * correlateSync()
 *
 * Correlates the received symbols against the P25 sync pattern.  A match
 * better than the stored one is remembered, together with the signal levels
 * at that sync, so that the position of the frame can be found.
 *
 * Parameters: none.
 *
 * Returns: true if a sync was found, false if not.
 */
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

/*
 * calculateLevels()
 *
 * Works out the centre and threshold levels of the signal from the maximum
 * and minimum positive and negative samples of a range, averaged with the
 * earlier values.
 *
 * Parameters:
 *   start - Position of the first sample in the sample buffer.
 *   count - Number of symbols to look at.
 *
 * Returns: nothing.
 */
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

/*
 * processHdr()
 *
 * Header receive state.  When the end of the frame is reached it works out
 * the signal levels, converts the samples to bits and passes the frame to
 * the NID and frame decoder for the unit that was found (header, PDU, TSDU
 * or terminator); LDU frames move the receiver to the LDU state.  A sync
 * that arrives in the expected window is correlated.
 *
 * Parameters:
 *   sample - The newest received sample.
 *
 * Returns: nothing.
 */
void processHdr(q15_t sample)
{
    uint8_t hdrFrame[108] = {};

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
        fprintf(stderr, "P25_DUID: %d\n", p25duid);
        switch (p25duid)
        {
            case P25_DUID_HDU:
            {
                calculateLevels(p25hdrStartPtr, P25_HDR_FRAME_LENGTH_SYMBOLS);

                uint8_t frame[P25_HDR_FRAME_LENGTH_BYTES];
                samplesToBits(p25hdrStartPtr, P25_HDR_FRAME_LENGTH_SYMBOLS, frame, 0U, p25centerVal, p25thresholdVal);

                hdrFrame[0] = 0x61;
                hdrFrame[1] = 0x00;
                hdrFrame[2] = 0x6B;
                hdrFrame[3] = 0x04;
                memcpy(hdrFrame + 4, "P25H", 4);
                memcpy(hdrFrame + 8, frame, P25_HDR_FRAME_LENGTH_BYTES);
                decodeFrame(hdrFrame, P25_HDR_FRAME_LENGTH_BYTES + 8U, false);
            }
            break;

            case P25_DUID_PDU:
            {
                calculateLevels(p25hdrSyncPtr, P25_PDU_HDR_FRAME_LENGTH_SYMBOLS);

                uint8_t frame[P25_PDU_HDR_FRAME_LENGTH_BYTES];
                samplesToBits(p25hdrSyncPtr, P25_PDU_HDR_FRAME_LENGTH_SYMBOLS, frame, 0U, p25centerVal, p25thresholdVal);

                hdrFrame[0] = 0x61;
                hdrFrame[1] = 0x00;
                hdrFrame[2] = 0x35;
                hdrFrame[3] = 0x04;
                memcpy(hdrFrame + 4, "P25H", 4);
                memcpy(hdrFrame + 8, frame, P25_PDU_HDR_FRAME_LENGTH_BYTES);
                decodeFrame(hdrFrame, P25_PDU_HDR_FRAME_LENGTH_BYTES + 8U, false);
            }
            break;

            case P25_DUID_TSDU:
            {
                calculateLevels(p25hdrStartPtr, P25_TSDU_FRAME_LENGTH_SYMBOLS);

                uint8_t frame[P25_TSDU_FRAME_LENGTH_BYTES];
                samplesToBits(p25hdrStartPtr, P25_TSDU_FRAME_LENGTH_SYMBOLS, frame, 0U, p25centerVal, p25thresholdVal);

                hdrFrame[0] = 0x61;
                hdrFrame[1] = 0x00;
                hdrFrame[2] = 0x35;
                hdrFrame[3] = 0x04;
                memcpy(hdrFrame + 4, "P25H", 4);
                memcpy(hdrFrame + 8, frame, P25_TSDU_FRAME_LENGTH_BYTES);
                decodeFrame(hdrFrame, P25_TSDU_FRAME_LENGTH_BYTES + 8U, false);
            }
            break;

            case P25_DUID_TDU:
            {
                calculateLevels(p25hdrStartPtr, P25_TERM_FRAME_LENGTH_SYMBOLS);

                uint8_t frame[P25_TERM_FRAME_LENGTH_BYTES];
                samplesToBits(p25hdrStartPtr, P25_TERM_FRAME_LENGTH_SYMBOLS, frame, 0U, p25centerVal, p25thresholdVal);

                hdrFrame[0] = 0x61;
                hdrFrame[1] = 0x00;
                hdrFrame[2] = 0x1A;
                hdrFrame[3] = 0x04;
                memcpy(hdrFrame + 4, "P25E" , 4);
                memcpy(hdrFrame + 8, frame, P25_TERM_FRAME_LENGTH_BYTES);
                decodeFrame(hdrFrame, P25_TERM_FRAME_LENGTH_BYTES + 8U, false);
            }
            break;

            case P25_DUID_TDULC:
            {
                calculateLevels(p25hdrStartPtr, P25_TERMLC_FRAME_LENGTH_SYMBOLS);

                uint8_t frame[P25_TERMLC_FRAME_LENGTH_BYTES];
                samplesToBits(p25hdrStartPtr, P25_TERMLC_FRAME_LENGTH_SYMBOLS, frame, 0U, p25centerVal, p25thresholdVal);

                hdrFrame[0] = 0x61;
                hdrFrame[1] = 0x00;
                hdrFrame[2] = 0x3E;
                hdrFrame[3] = 0x04;
                memcpy(hdrFrame + 4, "P25E", 4);
                memcpy(hdrFrame + 8, frame, P25_TERMLC_FRAME_LENGTH_BYTES);
                decodeFrame(hdrFrame, P25_TERMLC_FRAME_LENGTH_BYTES + 8U, false);
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

/*
 * processLdu()
 *
 * LDU receive state.  Follows the data sync of a voice call: at the end of
 * each LDU it works out the signal levels (only from a good sync), converts
 * the samples to bits and passes the frame on.  If no sync has been seen for
 * too long the call is signalled as lost and the receiver goes back to idle.
 *
 * Parameters:
 *   sample - The newest received sample.
 *
 * Returns: nothing.
 */
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

        uint8_t frame[P25_LDU_FRAME_LENGTH_BYTES];
        uint8_t lduFrame[P25_LDU_FRAME_LENGTH_BYTES + 8U];
        samplesToBits(p25lduStartPtr, P25_LDU_FRAME_LENGTH_SYMBOLS, frame, 0U, p25centerVal, p25thresholdVal);

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
            lduFrame[0] = 0x61;
            lduFrame[1] = 0x00;
            lduFrame[2] = 0xE0;
            lduFrame[3] = 0x04;
            memcpy(lduFrame + 4, "P25L" , 4);
            memcpy(lduFrame + 8, frame, P25_LDU_FRAME_LENGTH_BYTES);
            decodeFrame(lduFrame, P25_LDU_FRAME_LENGTH_BYTES + 8U, false);
            p25maxCorr = 0;
        }
    }
}

/*
 * setEndPtr()
 *
 * Sets the length of the frame being received (p25endPtr, in bits) from the
 * data unit id.
 *
 * Parameters:
 *   duid - Data unit id of the frame.
 *
 * Returns: nothing.
 */
void setEndPtr(const uint8_t duid)
{
    fprintf(stderr, "Set end ptr.\n");

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

/*
 * processBitNone()
 *
 * BIT mode idle state.  Shifts in one bit and, when the sync pattern
 * matches, copies the sync bytes into the frame buffer and starts to collect
 * the frame.
 *
 * Parameters:
 *   bit - The received bit.
 *
 * Returns: nothing.
 */
void processBitNone(bool bit)
{
    p25_64BitBuffer <<= 1;
    if (bit)
        p25_64BitBuffer |= 0x01U;

    // Fuzzy matching of the data sync bit sequence
    if (countBits64((p25_64BitBuffer & P25_SYNC_BITS_MASK) ^ P25_SYNC_BITS) <= SYNC_BIT_START_ERRS)
    {
        fprintf(stderr, "First sync.\n");
        for (uint8_t i = 0U; i < P25_SYNC_LENGTH_BYTES; i++)
            m_buffer[i] = P25_SYNC_BYTES[i];

        p25lostCount = MAX_SYNC_FRAMES;
        p25bufferPtr = P25_SYNC_LENGTH_BITS;
        p25state     = P25RXS_HDR;
    }
}

/*
 * processBitHdr()
 *
 * BIT mode header state.  Collects the bits of a header or other short
 * frame; when the NID has arrived the data unit is known and the frame
 * length is set.  When the frame is complete it is passed on.
 *
 * Parameters:
 *   bit - The received bit.
 *
 * Returns: nothing.
 */
void processBitHdr(bool bit)
{
    p25_64BitBuffer <<= 1;
    if (bit)
        p25_64BitBuffer |= 0x01U;

    WRITE_BIT(m_buffer, p25bufferPtr, bit);

    p25bufferPtr++;
    if (p25bufferPtr > P25_LDU_FRAME_LENGTH_BITS)
        reset();

    if (p25bufferPtr == P25_SYNC_LENGTH_BITS + 16U)
    {
        fprintf(stderr, "Detected LDU 1.\n");

        p25duid = m_buffer[7U] & 0x0F;

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
        fprintf(stderr, "Detected LDU 2.\n");

        uint8_t buf[(p25endPtr / 8U) + 8];
        buf[0] = 0x61; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x04;
        buf[4] = 'P'; buf[5] = '2'; buf[6] = '5'; buf[7] = 'H';
        buf[2] = (p25endPtr / 8U) + 8;
        memcpy(buf + 8, m_buffer, (p25endPtr / 8U));

        decodeFrame(buf, (p25endPtr / 8U), false);

        p25lostCount = MAX_SYNC_FRAMES;
        p25bufferPtr = 0U;
        p25state     = P25RXS_LDU;
    }
}

/*
 * processBitLdu()
 *
 * BIT mode LDU state.  Collects the bits of an LDU, looking for the data
 * sync only where it is expected (plus or minus two bits).  A completed
 * frame is delivered, and the receiver goes back to idle when no sync has
 * been seen for too long or a terminator is found.
 *
 * Parameters:
 *   bit - The received bit.
 *
 * Returns: nothing.
 */
void processBitLdu(bool bit)
{
    p25_64BitBuffer <<= 1;
    if (bit)
        p25_64BitBuffer |= 0x01U;

    WRITE_BIT(m_buffer, p25bufferPtr, bit);

    p25bufferPtr++;
    if (p25bufferPtr > P25_LDU_FRAME_LENGTH_BITS) reset();

    // Only search for a sync in the right place +-2 bits
    if (p25bufferPtr >= (P25_SYNC_LENGTH_BITS - 2U) && p25bufferPtr <= (P25_SYNC_LENGTH_BITS + 2U))
    {
   //     fprintf(stderr, "Process LDU 1.\n");

        // Fuzzy matching of the data sync bit sequence
        if (countBits64((p25_64BitBuffer & P25_SYNC_BITS_MASK) ^ P25_SYNC_BITS) <= SYNC_BIT_RUN_ERRS)
        {
   //         fprintf(stderr, "Process LDU 2.\n");

            p25lostCount = MAX_SYNC_FRAMES;
            p25bufferPtr = P25_SYNC_LENGTH_BITS;
        }
    }

    if (p25bufferPtr == P25_SYNC_LENGTH_BITS + 16U)
    {
        p25duid = m_buffer[7U] & 0x0F;
        setEndPtr(p25duid);
    }

    // Send a data frame to the host if the required number of bits have been received
    if (p25bufferPtr == P25_LDU_FRAME_LENGTH_BITS)
    {
        p25lostCount--;
        // We've not seen a data sync for too long, signal RXLOST and change to RX_NONE
        if (p25lostCount == 0U)
        {
            fprintf(stderr, "Data sync not seen.\n");

            uint8_t buf[(p25endPtr / 8U) + 8];
            buf[0] = 0x61; buf[1] = 0x00; buf[2] = 0x08; buf[3] = 0x04;
            buf[4] = 'P'; buf[5] = '2'; buf[6] = '5'; buf[7] = 'E';
            decodeFrame(buf, 8U, false);
            reset();
        }
        else
        {
    //        fprintf(stderr, "Process LDU 3.\n");

            uint8_t buf[(p25endPtr / 8U) + 8];
            buf[0] = 0x61; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x04;
            buf[4] = 'P'; buf[5] = '2'; buf[6] = '5'; buf[7] = 'L';
            buf[2] = (p25endPtr / 8U) + 8;
            memcpy(buf + 8, m_buffer, (p25endPtr / 8U));
            decodeFrame(buf, (p25endPtr / 8U), false);

            p25bufferPtr = 0U;
        }

        // Check if we found a TDU to avoid a false "lost lock"
        if (p25duid == P25_DUID_TDU || p25duid == P25_DUID_TDULC)
        {
            fprintf(stderr, "TDU found.\n");

            reset();
        }
    }
}

/*
 * processBits()
 *
 * Receive entry point for BIT mode.  Feeds the bytes through the bit state
 * machine, most significant bit first.
 *
 * Parameters:
 *   bytes  - Received bytes.
 *   length - Number of bytes in bytes.
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

            switch (p25state)
            {
                case P25RXS_HDR:
                    processBitHdr(bit);
                    break;
                case P25RXS_LDU:
                    processBitLdu(bit);
                    break;
                default:
                    processBitNone(bit);
                    break;
            }
        }
    }
}

/*
 * writeSamples()
 *
 * Turns one byte of transmit bits (four symbols) into shaped transmit
 * samples and queues them for the modem host.  The samples are collected in
 * 200 byte SAMP packets.
 *
 * Parameters:
 *   c     - Byte of transmit bits.
 *   isEOT - true to send the partly filled packet now (end of transmission).
 *
 * Returns: nothing.
 */
void writeSamples(uint8_t c, bool isEOT)
{
    q15_t inBuffer[4U];
    q15_t outBuffer[P25_RADIO_SYMBOL_LENGTH * 4U];

    const uint8_t MASK = 0xC0U;

    for (uint8_t i = 0U; i < 4U; i++, c <<= 2)
    {
        switch (c & MASK)
        {
            case 0xC0U:
                inBuffer[i] = P25_LEVELA;
                break;
            case 0x80U:
                inBuffer[i] = P25_LEVELB;
                break;
            case 0x00U:
                inBuffer[i] = P25_LEVELC;
                break;
            default:
                inBuffer[i] = P25_LEVELD;
                break;
        }
    }

    arm_fir_interpolate_q15(&modFilter, inBuffer, outBuffer, 4U);

    /* 200 byte SAMP packets; a short wait after each full one paces the modem host */
    modemQueueSamples(sockfd, outBuffer, P25_RADIO_SYMBOL_LENGTH * 4U, isEOT, 500U);
}

/*
 * processTx()
 *
 * Transmits a P25 item as BITS or SAMP packets.  An end of transmission
 * flushes the partly filled packet.  When a transmission is not yet running,
 * the start sync is sent first (p25txDelay bytes) as a preamble.  The data
 * bytes are then written.
 *
 * Parameters:
 *   data   - The frame bytes.
 *   length - Number of bytes in data.
 *   type   - Frame type: header, LDU or end of transmission.
 *   isTx   - true if a transmission is already running, false to send the
 *            preamble first.
 *
 * Returns: nothing.
 */
void processTx(const uint8_t* data, uint8_t length, const char* type, bool isTx)
{
    if (strcasecmp(type, TYPE_EOT) == 0)
    {
        fprintf(stderr, "EOT ==================\n");
        uint8_t tmp[1] = {0x00};
        if (packetType == PACKET_TYPE_BIT)
            modemWriteBits(sockfd, tmp[0], true);
        else if (packetType == PACKET_TYPE_SAMP)
            writeSamples(tmp[0], true);
        return;
    }

    if (!isTx)
    {
        fprintf(stderr, "PREAMBLE ==================\n");
        for (uint8_t i = 0U; i < p25txDelay; i++)
        {
            if (packetType == PACKET_TYPE_BIT)
                modemWriteBits(sockfd, P25_START_SYNC, false);
            else if (packetType == PACKET_TYPE_SAMP)
                writeSamples(P25_START_SYNC, false);
        }
        delay(10000);
    }

    for (uint8_t i = 0U; i < length; i++)
    {
        if (packetType == PACKET_TYPE_BIT)
            modemWriteBits(sockfd, data[i], false);
        else if (packetType == PACKET_TYPE_SAMP)
            writeSamples(data[i], false);
    }
}

/*
 * timerThread()
 *
 * Timer thread.  Every pass of the loop takes about ten milliseconds and
 * advances all software timers by the time that really passed.  The modeHang timer is held at zero while a
 * transmission is on; once it has been quiet for the hang time a SETIDLE
 * command is queued to hand the modem back.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: Nothing useful; the thread ends by calling pthread_exit().
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

/*
 * rxThread()
 *
 * Thread that empties the receive ring buffer (rxBuffer), which holds what
 * the gateway wants sent to the radio.  A five byte command is written to
 * the modem host; a header, LDU or end of transmission frame is passed to
 * processTx().  A byte that is not a frame start (0x61) is dropped.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: Nothing useful; the thread ends by calling pthread_exit().
 */
void* rxThread(void* arg)
{
    bool fast = false;       /* true: look again soon (more data may be waiting) */

    while (connected)
    {
        delay(fast ? 100 : HOST_POLL_US);
        fast = false;

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
                    fast = true;
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
                            reportError(modemName, "P25", "remote disconnect");
                            break;
                        }
                        fast = true;
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
        }
    }

    fprintf(stderr, "RX thread exited.\n");
    pthread_mutex_lock(&stateMutex);
    p25GWConnected = false;
    pthread_mutex_unlock(&stateMutex);
    pthread_exit(NULL);
    return NULL;
}

/*
 * txThread()
 *
 * Thread that sends data to the P25 gateway.  At regular intervals it takes
 * complete frames from gwTxBuffer and writes them to the gateway socket.
 *
 * Parameters:
 *   arg - Socket of the gateway connection, passed as a pointer sized
 *         integer.
 *
 * Returns: Nothing useful; the thread ends by calling pthread_exit().
 */
void* txThread(void *arg)
{
    int      sockfd = (intptr_t)arg;
    bool     fast   = false;     /* true: look again soon (more data may be waiting) */

    while (1)
    {
        delay(fast ? 100 : HOST_POLL_US);
        fast = false;

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
                    fast = true;
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
                        reportError(modemName, "P25", "remote reflector gateway disconnected.");
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
                        reportError(modemName, "P25", "remote reflector gateway disconnected.");
                    }
                }
            }
        }
    }

    pthread_mutex_lock(&stateMutex);
    p25GWConnected = false;
    pthread_mutex_unlock(&stateMutex);
    fprintf(stderr, "TX GW thread exited.\n");
    pthread_exit(NULL);
    return NULL;
}

/*
 * decodeFrame()
 *
 * Handles a frame from the modem host or the gateway.  A P25 frame is
 * decoded with decodeP25() and, for duplex or network traffic, sent on to
 * the modem (as it is in frame mode, or as BITS/SAMP packets through
 * processTx() otherwise).  An end of transmission ends the call: the last
 * call and history records are saved with the BER and the duration, and txOn
 * is cleared.
 *
 * Parameters:
 *   buffer - The frame, starting with the OpenMT header.
 *   length - Number of bytes in buffer.
 *   isNet  - true if the frame came from the network.
 *
 * Returns: nothing.
 */
void decodeFrame(uint8_t* buffer, uint8_t length, bool isNet)
{
    static bool started = false;
    uint8_t     typeLen = 4;

    fprintf(stderr, "Process frame. Len: %d\n", length);

    char cType[4] = "RF";
    if (isNet)
        strcpy(cType, "NET");

    if (memcmp(buffer + 4, "P25", 3) == 0 && memcmp(buffer + 4, TYPE_EOT, typeLen) != 0)
    {
    //    hexDump((char*)"Data", buffer + 8, length - 8);
        decodeP25(buffer, isNet);
        if ((modem_duplex || isNet) && packetType == PACKET_TYPE_FRAME && txOn)
        {
            if (write(sockfd, buffer, length) < 0)
            {
                reportError(modemName, "P25", "host disconnect");
            }
        }
        else if ((modem_duplex || isNet) && txOn)
        {
            processTx(buffer + 8, length - 8, "P25 ", started);
            started = true;
        }
        else if (modem_duplex && p25RelayData && packetType == PACKET_TYPE_FRAME)
        {
            /* trunking signalling or a data header from the radio, rebuilt
             * by decodeP25(): repeated in duplex as MMDVMHost does, in
             * frame mode only (sample and bit modes need the call's own
             * transmit timing) */
            if (write(sockfd, buffer, length) < 0)
                reportError(modemName, "P25", "host disconnect");
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
            started    = false;
            p25RebuildLcValid   = false;
            p25RebuildLdu2Valid = false;
        }
        pthread_mutex_unlock(&stateMutex);

        if (wasOn)
        {
            if ((modem_duplex || isNet) && packetType != PACKET_TYPE_FRAME)
                processTx(buffer + 8, length - 8, "P25E", wasOn);
            else if ((modem_duplex || isNet) && length > 8)
            {
                /* frame mode: the modem is sent the terminator itself, rebuilt
                 * like every other frame, so it stops transmitting at once */
                uint8_t duid = ((size_t)(length - 8) == P25_TERMLC_FRAME_LENGTH_BYTES) ? P25_DUID_TERM_LC : P25_DUID_TERM;

                p25RebuildFrame(buffer + 8, duid, isNet);
                if (write(sockfd, buffer, length) < 0)
                    reportError(modemName, "P25", "host disconnect");
            }

            duration = difftime(time(NULL), start_time);
            char tmp[100];
            bzero(tmp, 100);
            sprintf(tmp, "%u", p25srcId);
            if (strcasecmp(src_callsign, tmp) != 0)
            {
                saveLastCall(1, modemName, "P25", cType, src_callsign, tmp, dst_callsign, "", NULL, "", false);
                saveHistory(modemName, "P25", cType, src_callsign, tmp, dst_callsign, p25RfBer(isNet), "", duration);
            }
            else
            {
                saveLastCall(1, modemName, "P25", cType, tmp, "", dst_callsign, "", NULL, "", false);
                saveHistory(modemName, "P25", cType, tmp, "", dst_callsign, p25RfBer(isNet), "", duration);
            }
        }
    }
}

/* A call from the network is open from its first frame until its end frame.
 * The gateway can be unlinked, or stop, without sending that end frame; the
 * host would then stay keyed (and the dashboard in TX) for ever. */
static bool     p25NetOpen = false;
static uint64_t p25NetMs   = 0U;

/*
 * p25NetEnd()
 *
 * Ends a network call that has no end frame: a terminator is made and handed
 * to decodeFrame() as if the gateway had sent it, so the call is closed the
 * normal way (modem told to stop, last call and history saved, state idle).
 *
 * Parameters:
 *   force - true to end the call now, false to end it only when no frame has
 *           come from the gateway for 1.5 seconds.
 *
 * Returns: nothing.
 */
static void p25NetEnd(bool force)
{
    if (!p25NetOpen || (!force && nowMs() - p25NetMs <= 1500U))
        return;

    p25NetOpen = false;

    uint8_t frame[8U + P25_TERM_FRAME_LENGTH_BYTES];

    memset(frame, 0x00, sizeof(frame));
    frame[0] = 0x61;
    frame[1] = 0x00;
    frame[2] = (uint8_t)sizeof(frame);
    frame[3] = 0x04;
    memcpy(frame + 4, "P25E", 4);
    addSync(frame + 8);
    encodeNID(frame + 8, P25_DUID_TERM);
    addBusyBits(frame + 8, P25_TERM_FRAME_LENGTH_BITS, true, false);

    fprintf(stderr, "P25: network call ended, no end frame from the gateway\n");
    decodeFrame(frame, (uint8_t)sizeof(frame), true);
}

/*
 * processGatewaySocket()
 *
 * Thread that serves the connected P25 gateway.  It reads the gateway's
 * packets: NAK, connect, disconnect, command and status packets are handled,
 * and P25 frames (when P25 is the active mode) are decoded as network
 * traffic.
 *
 * Parameters:
 *   arg - Socket of the gateway connection, passed as a pointer sized
 *         integer.
 *
 * Returns: Nothing useful; the thread ends by calling pthread_exit().
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
    p25GWConnected = true;
    pthread_mutex_unlock(&stateMutex);

    addGateway(modemName, "main", "P25");
    txOn = false;

    fprintf(stderr, "Gateway connected.\n");

    while (1)
    {
        /* Wait for the gateway; when it is quiet, see whether a network call
         * has gone silent (the gateway was unlinked or stopped sending). */
        struct pollfd gwPoll = {childfd, POLLIN, 0};
        int           gwWait = poll(&gwPoll, 1, 200);

        if (gwWait == 0)
        {
            p25NetEnd(false);
            continue;
        }
        if (gwWait < 0 && errno == EINTR)
            continue;

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
            reportError(modemName, "P25", "P25_Service: error when reading from P25 gateway, errno=%d", errno);
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
            hexDump((char*)"P25 Gateway data:", (uint8_t*)buffer, respLen);

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

            p25NetEnd(true);

            char tmp[8];
            bzero(tmp, 8);
            memcpy(tmp, buffer + 4 + typeLen, 6);
            ackDashbCommand(modemName, "reflLinkP25", "unlinked");
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

            if (memcmp(type, "P25E", 4) == 0)
                p25NetOpen = false;
            else
            {
                p25NetOpen = true;
                p25NetMs   = nowMs();
            }

            decodeFrame(buffer, respLen, true);
        }
        delay(5);
    }

    fprintf(stderr, "Gateway disconnected.\n");

    p25NetEnd(true);

    pthread_mutex_lock(&stateMutex);
    p25GWConnected   = false;
    p25ReflConnected = false;
    pthread_mutex_unlock(&stateMutex);

    delGateway(modemName, "main", "P25");
    clearReflLinkStatus(modemName, "P25");
    pthread_exit(NULL);
    return 0;
}

/*
 * startClient()
 *
 * Thread that connects to the modem host socket.  It starts the gateway
 * server, receive and timer threads, sends the mode and radio configuration
 * packet, then processes the packets the modem host returns: commands, the
 * SAMP or BITS mode switch and P25 frames.
 *
 * Parameters:
 *   arg - Address of the modem host, as a text string.
 *
 * Returns: Nothing useful; the thread ends by calling pthread_exit().
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
        reportError(modemName, "P25", "Socket creation error: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1)
    {
        reportError(modemName, "P25", "fcntl F_GETFL: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    // Prepare the sockaddr_in structure
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(clientPort);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (!hostToAddress(hostAddress, &serv_addr.sin_addr))
    {
        reportError(modemName, "P25", "Invalid address/ Address not supported");
        exit(EXIT_FAILURE);
    }

    // Connect to the server
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        if (errno != EINPROGRESS)
        {
            reportError(modemName, "P25", "connection failed: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    int err = pthread_create(&(gwHostid), NULL, &startTCPServer, NULL);
    if (err != 0)
    {
        reportError(modemName, "P25", "Can't create gateway host thread :[%s]", strerror(err));
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
        reportError(modemName, "P25", "Can't create rx thread :[%s]", strerror(err));
        return 0;
    }
    else
    {
        if (debugM) fprintf(stderr, "RX thread created successfully\n");
    }

    err = pthread_create(&(timerid), NULL, &timerThread, NULL);
    if (err != 0)
    {
        reportError(modemName, "P25", "Can't create timer thread :[%s]", strerror(err));
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
            reportError(modemName, "P25", "p25_Service: error when reading from server, errno=%d", errno);
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
            hexDump((char*)"P25 data", (uint8_t*)buffer, respLen);

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
            processBits(buffer + 8, respLen - 8);
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
    pthread_exit(NULL);
    return 0;
}

/*
 * startTCPServer()
 *
 * Thread that listens for the P25 gateway on the server port.  For each
 * accepted connection it starts txThread() to send data to the gateway and
 * processGatewaySocket() to read from it.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: Nothing useful; the thread ends by calling pthread_exit().
 */
void* startTCPServer(void* arg)
{
    struct hostent* hostp;
    int childfd;
    int sockFd;

    sockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd < 0)
    {
        reportError(modemName, "P25", "P25_Service: error when creating the socket: %s", strerror(errno));
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
        reportError(modemName, "P25", "P25_Service: error when binding the socket to port %u: %s", serverPort, strerror(errno));
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
            reportError(modemName, "P25", "accept failed: %s", strerror(errno));
            break;
        }

        if (childfd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;

        hostaddrp = inet_ntoa(clientaddr.sin_addr);
        if (hostaddrp == NULL)
        {
            reportError(modemName, "P25", "Can't get the address of the client.");
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
            reportError(modemName, "P25", "Can't create tx thread :[%s]", strerror(err));
        else
        {
            if (debugM) fprintf(stderr, "TX thread created successfully\n");
        }

        pthread_t procid;
        err = pthread_create(&(procid), NULL, &processGatewaySocket, (void*)(intptr_t)childfd);
        if (err != 0)
        {
            reportError(modemName, "P25", "Can't create gateway process thread :[%s]", strerror(err));
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
 * Program entry.  Parses the command line (-m modem number 1 to 10, which
 * also selects the TCP ports; -d daemon; -v version; -x debug), reads the
 * modem and P25 settings from the database (levels, power, NAC, frequencies,
 * callsign), starts the modem host client and then watches for configuration
 * changes until the program is told to stop.
 *
 * Parameters:
 *   argc - Number of command line arguments.
 *   argv - Command line arguments.
 *
 * Returns: 0 on exit.
 */
int main(int argc, char** argv)
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
                    reportError(modemName, "P25", "Invalid modem number.");
                    return 1;
                }
                sprintf(modemName, "modem%d", modemId);
                clientPort = 18000 + modemId - 1;
                serverPort = 18300 + modemId - 1;
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
                fprintf(stdout, "P25_Service: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: P25_Service [-m modem_number (1-10)] [-a address] [-s dbserver] [-u dbuser] [-p dbpassword] [-c configfile] [-d] [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            reportError(modemName, "P25", "P25_Service: error in fork(), exiting");
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
    readHostConfig(modemName, "config", "streamType", tmp);   /* published by the bridge: FRAME, BIT or SAMPLE */
    if (strcasecmp(tmp, "SAMPLE") == 0)
        packetType = PACKET_TYPE_SAMP;
    else if (strcasecmp(tmp, "BIT") == 0)
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

    readHostConfig(modemName, "P25", "rxLevel", tmp);
    rxLevel = atoi(tmp);
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
        reportError(modemName, "P25", "Can't create modem host thread :[%s]", strerror(err));
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

            if (!p25GWConnected)
            {
                /* No gateway to wait for; nothing else to reload. */
                readDashbCommand(modemName, "updateConfP25", parameter);
                if (strlen(parameter) > 0)
                    ackDashbCommand(modemName, "updateConfP25", "success");
            }

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
                    resetTimer("status", 0);
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
                    resetTimer("status", 0);
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
            resetTimer("status", 0);
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
