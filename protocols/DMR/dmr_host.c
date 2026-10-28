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

#include <ADF7021.h>
#include <RingBuffer.h>
#include <dsp_compat.h> //<arm_math.h>
#include <arpa/inet.h>
#include <ctype.h>
//#include <dmr_emb.h>
#include <dmr_fec.h>
#include <dmr_func.h>
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
#include <tools.h>
#include <unistd.h>
#include <netinet/tcp.h>
#define DMR_SEND_MS          30U     /* frames go to the modem and to the gateway one per 30 ms (a DMR slot) */
#define VERSION "2026-01-21"
#define BUFFER_SIZE 1024

#define DMR_SYNC_BYTES_LENGTH  7U

static const uint8_t DMR_MS_DATA_SYNC_BYTES[]  = {0x0DU, 0x5DU, 0x7FU, 0x77U, 0xFDU, 0x75U, 0x70U};
static const uint8_t DMR_MS_VOICE_SYNC_BYTES[] = {0x07U, 0xF7U, 0xD5U, 0xDDU, 0x57U, 0xDFU, 0xD0U};
static const uint8_t DMR_BS_DATA_SYNC_BYTES[]  = {0x0DU, 0xFFU, 0x57U, 0xD7U, 0x5DU, 0xF5U, 0xD0U};
static const uint8_t DMR_BS_VOICE_SYNC_BYTES[] = {0x07U, 0x55U, 0xFDU, 0x7DU, 0xF7U, 0x5FU, 0x70U};
static const uint8_t DMR_S1_DATA_SYNC_BYTES[]  = {0x0FU, 0x7FU, 0xDDU, 0x5DU, 0xDFU, 0xD5U, 0x50U};
static const uint8_t DMR_S1_VOICE_SYNC_BYTES[] = {0x05U, 0xD5U, 0x77U, 0xF7U, 0x75U, 0x7FU, 0xF0U};
static const uint8_t DMR_S2_DATA_SYNC_BYTES[]  = {0x0DU, 0x75U, 0x57U, 0xF5U, 0xFFU, 0x7FU, 0x50U};
static const uint8_t DMR_S2_VOICE_SYNC_BYTES[] = {0x07U, 0xDFU, 0xFDU, 0x5FU, 0x55U, 0xD5U, 0xF0U};

static const uint64_t DMR_MS_DATA_SYNC_BITS  = 0x0000D5D7F77FD757U;
static const uint64_t DMR_MS_VOICE_SYNC_BITS = 0x00007F7D5DD57DFDU;
static const uint64_t DMR_BS_DATA_SYNC_BITS  = 0x0000DFF57D75DF5DU;
static const uint64_t DMR_BS_VOICE_SYNC_BITS = 0x0000755FD7DF75F7U;
static const uint64_t DMR_S1_DATA_SYNC_BITS  = 0x0000F7FDD5DDFD55U;
static const uint64_t DMR_S1_VOICE_SYNC_BITS = 0x00005D577F7757FFU;
static const uint64_t DMR_S2_DATA_SYNC_BITS  = 0x0000D7557F5FF7F5U;
static const uint64_t DMR_S2_VOICE_SYNC_BITS = 0x00007DFFD5F55D5FU;
static const uint64_t DMR_SYNC_BITS_MASK     = 0x0000FFFFFFFFFFFFU;

q31_t DC_FILTER[]                      = {3367972, 0, 3367972, 0, 2140747704, 0};  // {b0, 0, b1, b2, -a1, -a2}
static const uint32_t DC_FILTER_STAGES = 1U;                                       // One Biquad stage
bool USE_DC_FILTER                     = true;

static const char* TYPE_DATA1      = "DMD1";
static const char* TYPE_LOST1      = "DML1";
static const char* TYPE_DATA2      = "DMD2";
static const char* TYPE_LOST2      = "DML2";
static const char* TYPE_START      = "DMST";
static const char* TYPE_ABORT      = "DMAB";
static const char* TYPE_ACK        = "ACK ";
static const char* TYPE_NACK       = "NACK";
static const char* TYPE_DISCONNECT = "DISC";
static const char* TYPE_CONNECT    = "CONN";
static const char* TYPE_STATUS     = "STAT";
static const char* TYPE_MODE       = "MODE";
static const char* TYPE_COMMAND    = "COMM";
static const char* TYPE_SAMPLE     = "SAMP";
static const char* TYPE_BITS       = "BITS";
static const char* TYPE_TO_MODEM   = "2MOD";

static const uint8_t PACKET_TYPE_BIT   = 0;
static const uint8_t PACKET_TYPE_SAMP  = 1;
static const uint8_t PACKET_TYPE_FRAME = 2;

static const uint8_t COMM_SET_DUPLEX  = 0x00;
static const uint8_t COMM_SET_SIMPLEX = 0x01;
static const uint8_t COMM_SET_MODE    = 0x02;
static const uint8_t COMM_SET_IDLE    = 0x03;
static const uint8_t COMM_UPDATE_CONF = 0x04;

static const uint8_t TYPE_DMR_START   = 0x1D;
static const uint8_t TYPE_DMR_ABORT   = 0x1E;
static const uint8_t TYPE_DMR_DATA1   = 0x18;
static const uint8_t TYPE_DMR_DATA2   = 0x1A;
static const uint8_t TYPE_DMR_SHORTLC = 0x1c;

char MODE_NAME[11] = "DMR";
char MODEM_TYPE[6] = "4FSK";
bool USE_LP_FILTER = false;

uint8_t packetType       = 0;
char modemHost[80]       = "127.0.0.1";
uint8_t modemId          = 1;
char modemName[8]        = "modem1";
char stationCall[9]      = "N0CALL";
char slot1_call[16]      = "";
char slot2_call[16]      = "";
char slot1_name[15]      = "";
char slot2_name[15]      = "";
unsigned int slot1_srcId = 0;
unsigned int slot1_dstId = 0;
unsigned int slot2_srcId = 0;
unsigned int slot2_dstId = 0;

bool txOn             = false;
bool debugM           = false;
bool connected        = true;
bool reflBusy         = false;
bool modem_duplex     = false;
/* Simplex: a call received from the radio is in progress. Duplex marks this
 * with txOn, because the repeater relays what it hears; simplex never
 * transmits what it receives, so txOn must stay off (it keys the downlink in
 * SAMPLE and BIT mode) and this is used instead. */
static bool dmrSimplexRf = false;
bool dmrReflConnected = false;
bool dmrGWConnected   = false;
bool slotActive[2]    = {false, false};
time_t slot1_starttime     = 0;
time_t slot2_starttime     = 0;
uint8_t slot1_duration     = 0;
uint8_t slot2_duration     = 0;
uint8_t dmr_space          = 0;
uint16_t modeHang          = 30000;
uint8_t txLevel            = 50;
uint8_t rfPower            = 128;
char modem_rxFrequency[11] = "435000000";
char modem_txFrequency[11] = "435000000";
char linkedReflector[10]   = "";

bool dataSync  = false;
bool audioSync = false;

int sockfd          = 0;
int serverPort      = 18400;
uint16_t clientPort = 18000;

unsigned int clientlen;         //< byte size of client's address
char* hostaddrp;                //< dotted decimal host addr string
int optval;                     //< flag value for setsockopt
struct sockaddr_in serveraddr;  //< server's addr
struct sockaddr_in clientaddr;  //< client addr

RingBuffer txBuffer;
RingBuffer rxBuffer;
RingBuffer gwTxBuffer;
RingBuffer gwCommand;

pthread_t modemHostid;
pthread_t gwHostid;
pthread_t timerid;

pthread_mutex_t rxBufMutex     = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gwBufMutex     = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gwCommBufMutex = PTHREAD_MUTEX_INITIALIZER;

static const uint8_t SETMODE[] = {0x61, 0x00, 0x05, 0x01, COMM_SET_MODE};
static const uint8_t SETIDLE[] = {0x61, 0x00, 0x05, 0x01, COMM_SET_IDLE};

arm_biquad_casd_df1_inst_q31 dcFilter;
q31_t dcState[4];

/* --- DMR TX modulation (SAMPLE/BIT packet types) ---
 * DMR uses 4-level FSK: 2 bits/symbol, 4800 symbols/s, a root-raised-cosine
 * (sqrt, not plain raised-cosine) 0.2-rolloff pulse shape, and its own
 * deviation table -- these are DMR's actual values from MMDVM's
 * DMRDMOTX.cpp/DMRTX.cpp, not P25's (an earlier version of this file
 * incorrectly reused the P25 RC_0_2_FILTER/levels; those are a different,
 * plain-raised-cosine filter with different levels and do not apply here).
 */
static const unsigned int DMR_RADIO_SYMBOL_LENGTH = 5U;  // Samples per symbol at 24 kHz

static const q15_t DMR_LEVELA =  1362;
static const q15_t DMR_LEVELB =   454;
static const q15_t DMR_LEVELC =  -454;
static const q15_t DMR_LEVELD = -1362;

// Generated using rcosdesign(0.2, 8, 5, 'sqrt') in MATLAB
static q15_t DMR_RRC_FILTER[] = {0, 0, 0, 0, 850, 219, -720, -1548, -1795, -1172, 237, 1927, 3120, 3073, 1447, -1431, -4544, -6442,
                                 -5735, -1633, 5651, 14822, 23810, 30367, 32767, 30367, 23810, 14822, 5651, -1633, -5735, -6442,
                                 -4544, -1431, 1447, 3073, 3120, 1927, 237, -1172, -1795, -1548, -720, 219, 850}; // numTaps = 45, L = 5
static const uint16_t DMR_RRC_FILTER_PHASE_LEN = 9U;      // phaseLength = numTaps/L

arm_fir_interpolate_instance_q15 dmrModFilter;
q15_t dmrModState[16U];  // blockSize + phaseLength - 1, 4 + 9 - 1 plus spare

/* --- DMR dual-slot TX scheduler (SAMPLE/BIT packet types) ---
 * A real DMR repeater transmits a continuous TDMA carrier made up of
 * alternating slot 1 / CACH / slot 2 / CACH bursts (see MMDVM's
 * CDMRTX::process()), whether or not either slot actually has traffic --
 * a receiving subscriber radio needs that continuous structure (and the
 * CACH's short-LC/AT/TC bits) to achieve and keep frame lock. decodeDMR()
 * used to call processTx() directly and immediately whenever a frame
 * arrived for TX, with no CACH and no idle-burst filler; that produced a
 * sample stream with no TDMA structure at all, which a real radio cannot
 * decode even though bytes were reaching the modem.
 *
 * dmrTxThread() (defined further down, after the dual-slot RX it also
 * drives) is a free-running real-time scheduler that owns the actual
 * on-air cadence; decodeDMR()'s TX call sites below now just push a
 * completed 33-byte frame into the matching slot's FIFO for the scheduler
 * to pick up and frame with CACH in its proper turn. */
static RingBuffer dmrTxFifo[2U];
static pthread_mutex_t dmrTxFifoMutex[2U] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
pthread_t dmrTxThreadHandle;

// Built once in main() from DMR_IDLE_DATA + DMRSlotType_encode(), and sent
// in place of a real frame whenever a slot's FIFO is empty.
static uint8_t dmrIdleFrame[33U];  // DMR_FRAME_LENGTH_BYTES (must be a literal: global array size)

void* dmrTxThread(void* arg);


void decodeFrame(uint8_t* buffer, uint8_t length, bool isNet);
void sendDataToModem(uint8_t* data, uint8_t length, uint8_t slot, bool isNet);
void sendCommToModem(uint8_t* data, uint8_t length);
void* startTCPServer(void* arg);
void writeSamples(uint8_t c, bool isEOT);
void processTx(const uint8_t* data, uint8_t length, bool isEOT);
void decodeDMR(uint8_t* buffer, uint8_t length, bool isNet);
void dmrRxSamples(const q15_t* samples, uint16_t length);
void dmrRxBits(const uint8_t* bytes, uint8_t length);

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
    reportError(modemName, "DMR", "%s: %s", msg, strerror(errno));
    exit(1);
}

;

/*
 * isActiveMode()
 *
 * Tells whether this host may process frames: the modem host's active mode
 * is DMR or idle.
 *
 * Parameters: none.
 *
 * Returns: true if the active mode is DMR or IDLE, false if another protocol
 *   owns the modem.
 */
bool isActiveMode()
{
    char active_mode[10] = "IDLE";

    readStatus(modemName, "main", "active_mode", active_mode);

    if (strcasecmp(active_mode, "DMR") == 0 || strcasecmp(active_mode, "IDLE") == 0)
        return true;
    else
        return false;
}

/*
 * timerThread()
 *
 * Timer thread.  Every pass of the loop takes about one millisecond and
 * advances all software timers.  The modeHang timer is held at zero while a
 * transmission is on (or a simplex RF call is running); once it has been
 * quiet for the hang time a SETIDLE command is queued to hand the modem
 * back.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: Nothing useful; the thread ends by calling pthread_exit().
 */
void* timerThread(void* arg)
{
    bool idle = true;

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
            if ((txOn || dmrSimplexRf) && strcasecmp(timer[i].name, "modeHang") == 0)
            {
                idle = false;
                resetTimer("modeHang", 0);
            }
            else if (!txOn && !dmrSimplexRf && strcasecmp(timer[i].name, "modeHang") == 0)
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
 * Thread that empties the receive ring buffer (rxBuffer).  Once every
 * DMR_SEND_MS it takes one complete frame: a five byte command is written to
 * the modem host socket, any other frame is passed to decodeFrame() as
 * received data.  A frame that does not start with 0x61 is reported.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: Nothing useful; the thread ends by calling pthread_exit().
 */
void* rxThread(void* arg)
{
    uint64_t next = 0;      /* when the next frame is due; one frame is sent every DMR_SEND_MS */

    while (connected)
    {
        sleepUntilNext(&next, DMR_SEND_MS);

        {
            pthread_mutex_lock(&rxBufMutex);
            if (RingBuffer_dataSize(&rxBuffer) >= 5)
            {
                uint8_t buf[1];
                RingBuffer_peek(&rxBuffer, buf, 1);
                if (buf[0] != 0x61)
                {
                    fprintf(stderr, "RX invalid header.\n");
                    pthread_mutex_unlock(&rxBufMutex);
                    continue;
                }
                uint8_t byte[3];
                uint16_t len = 0;
                RingBuffer_peek(&rxBuffer, byte, 3);
                len = (byte[1] << 8) + byte[2];
                if (RingBuffer_dataSize(&rxBuffer) >= len)
                {
                    uint8_t buf[len];
                    RingBuffer_getData(&rxBuffer, buf, len);
                    pthread_mutex_unlock(&rxBufMutex);
                    if (len == 5)
                    {
                        if (write(sockfd, buf, len) < 0)
                        {
                            reportError(modemName, "DMR", "remote disconnect");
                            break;
                        }
                    }
                    else
                    {
                        decodeFrame(buf, len, false);
                    }
                    continue;
                }
            }
            pthread_mutex_unlock(&rxBufMutex);
        }
    }

    fprintf(stderr, "RX thread exited.\n");
    connected = false;
    pthread_exit(NULL);
    return NULL;
}

/*
 * gwTxThread()
 *
 * Thread that sends data to the DMR gateway.  Once every DMR_SEND_MS it
 * takes one complete frame from gwTxBuffer and writes it to the gateway
 * socket.  It ends when the gateway disconnects or a write fails.
 *
 * Parameters:
 *   arg - Socket of the gateway connection, passed as a pointer sized
 *         integer.
 *
 * Returns: Nothing useful; the thread ends by calling pthread_exit().
 */
void* gwTxThread(void* arg)
{
    int sockfd = (intptr_t)arg;
    uint64_t next = 0;      /* when the next frame is due; one frame is sent every DMR_SEND_MS */

    while (dmrGWConnected)
    {
        sleepUntilNext(&next, DMR_SEND_MS);

        {
            pthread_mutex_lock(&gwBufMutex);
            if (RingBuffer_dataSize(&gwTxBuffer) >= 5)
            {
                uint8_t buf[1];
                RingBuffer_peek(&gwTxBuffer, buf, 1);
                if (buf[0] != 0x61)
                {
                    fprintf(stderr, "GW TX invalid header.\n");
                    pthread_mutex_unlock(&gwBufMutex);
                    continue;
                }
                uint8_t byte[3];
                uint16_t len = 0;
                RingBuffer_peek(&gwTxBuffer, byte, 3);
                len = (byte[1] << 8) + byte[2];
                if (RingBuffer_dataSize(&gwTxBuffer) >= len)
                {
                    uint8_t buf[len];
                    RingBuffer_getData(&gwTxBuffer, buf, len);
                    pthread_mutex_unlock(&gwBufMutex);
                    if (write(sockfd, buf, len) < 0)
                    {
                        reportError(modemName, "DMR", "remote disconnect");
                        break;
                    }
                    continue;
                }
            }
            pthread_mutex_unlock(&gwBufMutex);
        }
    }

    fprintf(stderr, "GW TX thread exited.\n");
    pthread_exit(NULL);
    return NULL;
}

/*
 * sendDataToModem()
 *
 * Sends a DMR frame to the modem host as a data frame (type DMD1 or DMD2).
 * Frames from the radio are sent only for a duplex modem.  In SAMPLE and BIT
 * mode nothing is sent, because there is no modem firmware to use these
 * frames: this host keys the transmitter itself with the sample or bit
 * stream written by dmrTxThread().
 *
 * Parameters:
 *   data   - The frame data.
 *   length - Number of bytes in data.
 *   slot   - Time slot; slot 1 is sent as DMD1, anything else as DMD2.
 *   isNet  - true if the frame came from the network; network frames of a
 *            simplex modem always go as DMD1.
 *
 * Returns: nothing.
 */
void sendDataToModem(uint8_t* data, uint8_t length, uint8_t slot, bool isNet)
{
    if (!isNet && !modem_duplex) return;

    /* DMD1/DMD2 packets (TYPE_DMR_START on/off, relayed frames) are
     * FRAME-mode messages for MMDVM firmware: modem_host turns them into
     * MMDVM serial frames (0xE0, length, 0x18/0x1A, payload). In SAMPLE and
     * BIT mode there is no firmware to consume them -- modem_host logged
     * them as "RX Mode buffer invalid header [E0] [04] ... [1D] [01]" --
     * and this host keys the transmitter itself through the SAMP/BITS/EOT
     * stream (dmrTxThread()), so they're not sent at all. */
    if (packetType == PACKET_TYPE_SAMP || packetType == PACKET_TYPE_BIT)
        return;

    uint8_t buf[length + 8];

    buf[0] = 0x61;
    buf[1] = 0x00;
    buf[2] = length + 8;
    buf[3] = 0x04;
    if (slot == 1 || (isNet && !modem_duplex))
        memcpy(buf + 4, TYPE_DATA1, 4);
    else
        memcpy(buf + 4, TYPE_DATA2, 4);
    memcpy(buf + 8, data, length);
 //   pthread_mutex_lock(&rxBufMutex);
 //   RingBuffer_addData(&rxBuffer, buf, length + 8);
 //   pthread_mutex_unlock(&rxBufMutex);
    write(sockfd, buf, length + 8);
}

/*
 * sendCommToModem()
 *
 * Writes a command packet to the modem host socket.
 *
 * Parameters:
 *   data   - The command bytes.
 *   length - Number of bytes in data.
 *
 * Returns: nothing.
 */
void sendCommToModem(uint8_t* data, uint8_t length)
{
    write(sockfd, data, length);
}

/* TX samples written since the current downlink started (reset by
 * dmrTxThread() at downlink start). Lets a TX timing report from the
 * bridge be sanity-checked: it names a TX sample that has already been
 * written, so a report naming a later one belongs to some other stream. */
static volatile uint32_t dmrTxSamplesWritten = 0U;

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
    q15_t outBuffer[DMR_RADIO_SYMBOL_LENGTH * 4U];

    const uint8_t MASK = 0xC0U;

    for (uint8_t i = 0U; i < 4U; i++, c <<= 2)
    {
        switch (c & MASK)
        {
            case 0xC0U:
                inBuffer[i] = DMR_LEVELA;
                break;
            case 0x80U:
                inBuffer[i] = DMR_LEVELB;
                break;
            case 0x00U:
                inBuffer[i] = DMR_LEVELC;
                break;
            default:
                inBuffer[i] = DMR_LEVELD;
                break;
        }
    }

    /* Single raised-cosine interpolation stage straight to the output --
     * no secondary lowpass pass (see the P25 SAMPLE-mode fix this host
     * family relies on: an extra filter stage there distorted the C4FM
     * waveform enough that a real radio would not decode it). */
    arm_fir_interpolate_q15(&dmrModFilter, inBuffer, outBuffer, 4U);
    dmrTxSamplesWritten += DMR_RADIO_SYMBOL_LENGTH * 4U;

    /* 200 byte SAMP packets; a short wait after each full one paces the modem host */
    modemQueueSamples(sockfd, outBuffer, DMR_RADIO_SYMBOL_LENGTH * 4U, isEOT, 500U);
}

/*
 * processTx()
 *
 * Modulates and queues a complete DMR burst for transmission in SAMPLE or
 * BIT mode.  The caller has already added the sync, CACH and EMB fields.  A
 * DMR burst is a self-contained 33 byte (264 bit) unit sent in its own time
 * slot, so there is no preamble to add.  Callers pass isEOT as false during
 * a live transmission, so that the 200 byte packets fill up and are flushed
 * when full.  isEOT is true only for the single end-of-transmission flush
 * (see dmrTxThread()); passing it on every burst would tag about 33 packets
 * a second as end of transmission and make the modem restart its transmit
 * chain each time, overflowing its receive buffer.  Does nothing in frame
 * mode.
 *
 * Parameters:
 *   data   - The burst bytes.
 *   length - Number of bytes in data.
 *   isEOT  - true to flush the partly filled packet as end of transmission.
 *
 * Returns: nothing.
 */
void processTx(const uint8_t* data, uint8_t length, bool isEOT)
{
    if (packetType != PACKET_TYPE_SAMP && packetType != PACKET_TYPE_BIT)
        return;

    for (uint8_t i = 0U; i < length; i++)
    {
        bool last = isEOT && (i == (length - 1U));
        if (packetType == PACKET_TYPE_SAMP)
            writeSamples(data[i], last);
        else
            modemWriteBits(sockfd, data[i], last);
    }
}


/* =====================================================================
 * DMR RX (SAMPLE and BIT packet types) -- true dual-slot version,
 * single free-running correlator.
 *
 * decodeDMR() below expects an already-demodulated burst: an 8-byte wire
 * header, a control byte (DMR_SYNC_DATA/DMR_SYNC_AUDIO flags + dataType
 * in the low nibble), and the 33-byte DMR frame. In FRAME mode the modem
 * firmware builds that for us; in SAMPLE/BIT mode this host has to do
 * the demodulation itself, the same way dstar_host.c's samples()/
 * processBits() do for D-STAR.
 *
 * Two different designs were tried here before this one:
 *
 *   1. A port of MMDVM's CDMRDMORX (DMO/hotspot-style single continuous
 *      correlator over one 60ms ring buffer). It found sync reliably,
 *      but had no way to tell slot 1 and slot 2 traffic apart and always
 *      reported bursts as slot 1 -- not good enough for a true 2-slot
 *      repeater.
 *
 *   2. A port of MMDVM's CDMRSlotRX/CDMRRX: two fully independent
 *      correlators, each externally "armed" once per TDMA half-cycle by
 *      dmrTxThread()'s own nanosleep()-paced scheduling loop, mirroring
 *      how real MMDVM hardware shares one ISR between TX and RX timing.
 *      On real hardware over the air, this locked sync exactly once and
 *      then never reacquired it for the rest of a continuous voice call,
 *      even though the TX scheduler kept running correctly throughout.
 *      The reason: dmrTxThread() paces itself from CLOCK_MONOTONIC, which
 *      has no real phase relationship to the rate at which the modem
 *      actually delivers demodulated RX samples. Any drift between the
 *      two -- even a few hundred ppm of difference -- eventually walks
 *      the externally-armed scan window off the real burst position, and
 *      once that happens there is no way back, because the window is
 *      only re-armed by the (now out-of-phase) scheduler. This host is
 *      "purely reactive, no tick" by design, so there is no shared clock
 *      to lean on the way real hardware does.
 *
 * This version fixes that by giving RX no dependency on dmrTxThread() at
 * all: it times itself purely from its own incoming samples, the one
 * clock it can actually trust -- exactly the way a real subscriber radio
 * has to, since it has no access to a repeater's internal TX timing
 * either. On a TDMA DMR channel exactly one station's burst occupies the
 * air at any instant, so a single, continuously free-running correlator
 * (design #1 above) sees every burst from both slots, back-to-back, one
 * roughly every 30ms. The only thing design #1 was missing was knowing
 * which slot each burst belonged to; that is recovered here by timing
 * each newly-locked burst's absolute sample position (dataPtr never
 * resets -- it free-runs for the life of the process) against the first
 * lock of the current "sync session":
 *
 *     slot = round((syncPtr - firstSyncPtr) / samplesPerSlot) % 2
 *
 * samplesPerSlot comes from the real RX sample clock itself (24 kHz),
 * so this stays correct even though the TX scheduler never enters into
 * it. If RX sync is lost for a long stretch (silence, dropout), the
 * "first lock" reference is re-anchored on the next sync, which costs
 * nothing worse than re-deciding which physical slot number a fresh
 * burst sequence gets labelled -- both slots' content still flows
 * through correctly either way.
 *
 * Update: one correlator turned out NOT to see both slots once coasting
 * was added (voice bursts B-F have no sync, so a locked stream has to be
 * followed 60ms at a time, looking only near its own expected sync). A
 * real-hardware test with both slots keyed at once received only one
 * radio. RX now has two tracking contexts sharing one sample front end;
 * see DMR_RX_CTX below. */

/*
 * dmrCountBits8()
 *
 * Counts the bits set in a byte.
 *
 * Parameters:
 *   bits - Value to count.
 *
 * Returns: Number of one bits.
 */
static uint8_t dmrCountBits8(uint8_t bits)
{
    return (uint8_t)__builtin_popcount(bits);
}

/*
 * dmrCountBits32()
 *
 * Counts the bits set in a 32 bit value.
 *
 * Parameters:
 *   bits - Value to count.
 *
 * Returns: Number of one bits.
 */
static uint8_t dmrCountBits32(uint32_t bits)
{
    return (uint8_t)__builtin_popcount(bits);
}

/*
 * dmrCountBits64()
 *
 * Counts the bits set in a 64 bit value.
 *
 * Parameters:
 *   bits - Value to count.
 *
 * Returns: Number of one bits.
 */
static uint8_t dmrCountBits64(uint64_t bits)
{
    return (uint8_t)__builtin_popcountll(bits);
}

// A sentinel well above anything dataPtr/syncPtr/startPtr/endPtr will
// ever legitimately hold (those are free-running uint32_t sample
// counts), meaning "no burst end scheduled yet".
static const uint32_t DMR_RX_NOENDPTR = 0xFFFFFFFFU;

static const uint8_t DMR_RX_CONTROL_NONE  = 0x00U;
static const uint8_t DMR_RX_CONTROL_VOICE = 0x20U;
static const uint8_t DMR_RX_CONTROL_DATA  = 0x40U;
static const uint8_t DMR_RX_CONTROL_IDLE  = 0x80U;  // MMDVM CONTROL_IDLE: burst from the idle receiver

static const uint8_t DMR_RXS_NONE  = 0U;
static const uint8_t DMR_RXS_VOICE = 1U;
static const uint8_t DMR_RXS_DATA  = 2U;

/* p25_func.h's confirmed-working SAMPLE-mode correlator (same free-
 * running architecture as this one, same P25_RADIO_SYMBOL_LENGTH = 5)
 * uses Q15(0.57) here, not Q15(0.60) -- aligning to it since there's no
 * other basis to justify a different value for DMR specifically. */
static const q15_t DMR_RX_SCALING_FACTOR = 18750;  // Q15(0.57), was 19505/Q15(0.60)

static const uint8_t DMR_MAX_SYNC_SYMBOLS_ERRS = 2U;
static const uint8_t DMR_MAX_SYNC_BYTES_ERRS   = 4U;  // used by the separate BIT-mode RX path only
static const uint8_t DMR_MAX_SYNC_LOST_FRAMES  = 13U;

/* p25_func.h's correlator uses a DIFFERENT byte-error tolerance
 * depending on whether it's acquiring sync from cold (stricter, 2
 * errors -- avoid locking onto noise) or already tracking a burst
 * sequence (looser, 4 errors -- context already supports a weaker
 * match). This SAMPLE-mode DMR correlator used one flat value (3) for
 * both cases; splitting it the same way p25 does. */
static const uint8_t DMR_MAX_SYNC_BYTE_START_ERRS = 2U;
static const uint8_t DMR_MAX_SYNC_BYTE_RUN_ERRS   = 4U;

static const uint16_t DMR_SYNC_LENGTH_SYMBOLS_LOCAL = 24U;   // DMR_SYNC_LENGTH_BYTES * 4
static const uint16_t DMR_SYNC_LENGTH_SAMPLES_LOCAL = DMR_SYNC_LENGTH_SYMBOLS_LOCAL * DMR_RADIO_SYMBOL_LENGTH;
static const uint16_t DMR_FRAME_LENGTH_SYMBOLS_LOCAL = DMR_FRAME_LENGTH_BYTES * 4U;
static const uint16_t DMR_FRAME_LENGTH_SAMPLES_LOCAL = DMR_FRAME_LENGTH_SYMBOLS_LOCAL * DMR_RADIO_SYMBOL_LENGTH;

/* dmr_func.h's DMR_SLOT_TYPE_LENGTH_BITS/BYTES (8U/1U) describe a
 * different field than MMDVM's DMRDefines.h constant of the same name
 * (20U bits -- the slot type FEC block position within a burst), so
 * these are defined locally from the authoritative MMDVM values rather
 * than reused from dmr_func.h. */
static const uint16_t DMR_SLOT_TYPE_LENGTH_SAMPLES_LOCAL = 50U;   // 10 symbols * 5 samples/symbol
static const uint16_t DMR_INFO_LENGTH_SAMPLES_LOCAL      = 490U;  // 98 symbols * 5 samples/symbol

// One TDMA slot period (CACH + burst): 30ms @ 24kHz.
static const uint32_t DMR_SLOT_PERIOD_SAMPLES_LOCAL = 720U;

/* Same slot, next TDMA frame: 2 x 30ms = 60ms @ 24kHz. Once a voice or
 * data sequence is in progress, the next burst on this slot is expected
 * exactly this far after the current one. Voice bursts B-F carry EMB
 * instead of a sync pattern, so they can only be found by coasting this
 * far forward -- never by searching for sync. */
static const uint32_t DMR_RX_BURST_REPEAT_SAMPLES   = 2U * DMR_SLOT_PERIOD_SAMPLES_LOCAL;

/* Symbol-domain (2 bits/symbol packed) forms of the MS-sourced sync
 * patterns, used for amplitude-based SAMPLE-mode correlation. RX only
 * ever looks for MS-sourced sync: whether this host is a true duplex
 * repeater or a simplex hotspot, the thing transmitting on the other
 * end of the RF link is always a subscriber radio (see MMDVM's
 * DMRSlotRX.cpp and DMRDMORX.cpp, which both hardcode this). */
static const uint32_t DMR_MS_DATA_SYNC_SYMBOLS  = 0x0076286EU;
static const uint32_t DMR_MS_VOICE_SYNC_SYMBOLS = 0x0089D791U;
static const uint32_t DMR_SYNC_SYMBOLS_MASK     = 0x00FFFFFFU;

static const int8_t DMR_MS_DATA_SYNC_SYMBOLS_VALUES[]  = {-3, +3, +3, +3, -3, +3, +3, -3, -3, -3, +3, -3, +3, -3, -3, -3, -3, +3, +3, -3, +3, +3, +3, -3};
static const int8_t DMR_MS_VOICE_SYNC_SYMBOLS_VALUES[] = {+3, -3, -3, -3, +3, -3, -3, +3, +3, +3, -3, +3, -3, +3, +3, +3, +3, -3, -3, +3, -3, -3, -3, +3};

static const uint8_t DMR_RX_BIT_MASK_TABLE[] = {0x80U, 0x40U, 0x20U, 0x10U, 0x08U, 0x04U, 0x02U, 0x01U};
#define DMR_WRITE_BIT1(p, i, b) \
    (p)[(i) >> 3] = (b) ? ((p)[(i) >> 3] | DMR_RX_BIT_MASK_TABLE[(i) & 7]) : ((p)[(i) >> 3] & ~DMR_RX_BIT_MASK_TABLE[(i) & 7])

uint8_t dmrColorCode = 1U;  // set from config in main()

/* Shared sample front end: one ring buffer and one symbol-rate bit
 * history, fed by every incoming sample. "buffer" is a true ring
 * (indexed modulo its size) because dataPtr and every context's
 * syncPtr/startPtr/endPtr are free-running absolute sample counts that
 * never reset to 0 -- see the file comment above. */
typedef struct DMR_RX_STATE
{
    uint32_t bitBuffer[5U];   // DMR_RADIO_SYMBOL_LENGTH (must be a literal: struct member array size)
    q15_t    buffer[900U];    // ring buffer: a burst + lookback margin always fits
    uint32_t bitPtr;
    uint32_t dataPtr;
} DMR_RX_STATE;

/* One tracking context per TDMA slot. A single correlator can only
 * follow one burst stream: once locked it coasts from burst to burst
 * (60ms apart) and only looks at +/-1 sample around each expected sync,
 * so a second radio on the other slot -- 30ms away -- is never examined.
 * Real-hardware test with both slots keyed at once confirmed exactly
 * that: only one radio was ever received. With two contexts, one can be
 * locked onto each slot while the free scan keeps looking for the other
 * whenever a context is idle. */
typedef struct DMR_RX_CTX
{
    uint32_t syncPtr;
    uint32_t startPtr;
    uint32_t endPtr;
    q31_t    maxCorr;
    q15_t    centre[4U];
    q15_t    threshold[4U];
    uint8_t  averagePtr;
    uint8_t  control;
    uint8_t  syncCount;
    uint8_t  state;
    uint8_t  n;
    uint8_t  type;
    uint8_t  slot;    // TDMA slot label (0/1) this context's stream is delivered on
    uint8_t  index;   // which context this is (0/1), for debug output only
    uint32_t expectPtr;  // where this stream's next sync is expected (fixed centre of the tracking window)
    int32_t  driftQ8;    // measured burst-position drift, 1/256 sample per burst
    int32_t  driftAccQ8; // fractional drift carried between coasted bursts
    uint8_t  sinceSync;  // bursts coasted since the last sync was found
} DMR_RX_CTX;

static DMR_RX_STATE dmrRx;
static DMR_RX_CTX   dmrRxCtx[2U];

/* A cold-scan sync candidate this close (modulo one TDMA frame) to a
 * stream another context is already tracking belongs to that stream, not
 * a new one. The other slot's bursts sit 720 samples away, far outside
 * this guard. 120 samples = 5ms. */
static const int32_t DMR_RX_STREAM_GUARD_SAMPLES = 120;

/* While a voice/data sequence is being tracked, the next burst's sync is
 * searched for within +/- this many samples of where it's expected, and
 * the best correlation in that window wins. MMDVM uses +/-1 because its RX
 * and TX share one hardware clock; here samples pass through modem_host
 * and the network, and real-hardware logs showed the burst position
 * drifting ~1 sample per superframe -- enough that a +/-1 window missed a
 * call's voice syncs entirely, RX declared the call lost after 13 frames
 * while the radio was still transmitting, and the repeater de-keyed.
 * 12 samples is +/-2.4 symbols; the other slot is 720 samples away. */
static const int32_t DMR_RX_TRACK_WINDOW_SAMPLES = 12;

/* Drift tracking. A window alone only re-centres when a sync is found --
 * once per voice superframe (burst A) -- so bursts B-F in between are read
 * at a position that drifts further off with every burst. Each found sync's
 * offset from where it was expected, divided by the bursts coasted since,
 * is the residual drift per burst; half of it is folded into driftQ8 and
 * coasting steps by 1440 + drift. Clamped to +/-2 samples per burst.
 * One sync can move the estimate by at most DMR_RX_MAX_DRIFT_STEP_Q8, so a
 * one-off jump in burst timing (re-centred by the window anyway) isn't
 * mistaken for a steady drift, while a real drift still builds up over a
 * few superframes. */
static const int32_t DMR_RX_MAX_DRIFT_Q8      = 2 * 256;
static const int32_t DMR_RX_MAX_DRIFT_STEP_Q8 = 64;  // 0.25 sample/burst per sync

/* Slot identity from timing. An MS picks its timeslot from the repeater's
 * own outbound timing (CACH TC bit), and its inbound bursts carry no slot
 * number, so the only real source of slot identity is where a burst lands
 * relative to our TX schedule: its "phase", modulo the 60ms TDMA frame,
 * after dmrTxThread()'s slot-1 start time. TS1 and TS2 bursts sit 30ms
 * apart in phase. Where the TS1 group sits depends on modem/network
 * buffering, so it's a per-installation value: [DMR] SlotPhase in the
 * host config, in ms. Real-hardware measurements (with the downlink always
 * starting at a cycle boundary): stable to +/-0.5ms within a key-up, but
 * varying between key-ups -- TS1 seen at 47.1-58.0ms, TS2 at 18.3-31.7ms,
 * i.e. 47.1-61.7ms in TS1 terms. The default centre is the middle of that
 * range. A burst is TS1 if its phase is within +/-15ms of the centre, else
 * TS2, so the default leaves ~7ms of margin either side of what's been
 * seen. The key-up-to-key-up variation comes from the modem's own TX
 * start-up buffering, which this host can't observe. With debug on, every synced
 * burst's phase is printed by dmrRxLogSlotPhase() so it can be re-measured.
 *
 * Sample timestamps: RX samples arrive in packets whose delivery is
 * clumped by queueing, so "packet arrival time minus how far back the
 * sample is" jitters by tens of ms (a first test showed two bursts that
 * were exactly 6 x 60ms apart in the sample stream reading 13.2ms and
 * 48.6ms). Standard fix for that: track the MINIMUM of (arrival time -
 * nominal sample time) over a rolling window. The least-delayed packet
 * gives the best estimate of the fixed offset between the sample clock
 * and CLOCK_MONOTONIC; queueing delay only ever adds to it. */
static int64_t         dmrRxClockOffsetNs      = INT64_MAX;  // min offset, current window
static int64_t         dmrRxClockOffsetPrevNs  = INT64_MAX;  // min offset, previous window
static int64_t         dmrRxClockWindowStartNs = 0;
static const int64_t   DMR_RX_CLOCK_WINDOW_NS  = 2000000000LL;  // 2s
static int64_t         dmrTxSlot1StartNs       = 0;   // written by dmrTxThread()
static pthread_mutex_t dmrTxSlotTimeMutex      = PTHREAD_MUTEX_INITIALIZER;
static int64_t         dmrRxTs1PhaseNs         = 54500000LL;  // [DMR] SlotPhase, set in main()

/* Sample-exact slot timing (SAMPLE mode with the SX1255 bridge). The
 * bridge drives capture and playback from one I2S clock, so it can report
 * which RX sample was being captured as a given TX sample went out on air
 * (an OPENMT_TX_TIMING frame, forwarded by modem_host as "TXTM"). That is
 * the same relationship MMDVM firmware gets from sharing one ISR between
 * DAC and ADC, and it lets this host place every RX sample within its own
 * TDMA cycle exactly:
 *
 *   cycle position = (k + (rxPtr - refRxPtr)) mod 1440
 *
 * where TX sample 0 of a downlink is the start of the CACH before TX slot
 * 1 (dmrTxThread() always starts there) and a cycle is 1440 samples. Then
 * MMDVM's own windows apply: CDMRTX marks MARK_SLOT1 on the third byte of
 * that CACH (cycle sample 50), and CDMRSlotRX for TS1 accepts a sync from
 * 400 to 490 samples after the mark -- so a TS1 sync lands at cycle
 * 450-540, centre 495, and TS2 one slot (720 samples) later. A burst is
 * TS1 within +/-360 samples of 495, else TS2. [DMR] SlotDelay (samples,
 * default 0) shifts the windows later, like MMDVM's DMR delay setting.
 * Until a report arrives for the current downlink, the wall-clock phase
 * estimate above is used instead. */
static bool            dmrTxRefValid           = false;   // guarded by dmrTxSlotTimeMutex
static uint32_t        dmrTxRefRxPtr           = 0U;      // RX sample on air with TX sample dmrTxRefK
static uint32_t        dmrTxRefK               = 0U;
static int32_t         dmrSlotDelaySamples     = 0;       // [DMR] SlotDelay, set in main()
static const int32_t   DMR_CYCLE_SAMPLES       = 1440;
static const int32_t   DMR_TS1_SYNC_CYCLE_POS  = 495;     // MMDVM: mark at 50 + scan 400-490

/* TX hang (SAMPLE/BIT mode). The downlink stays up for dmrTxHangDurNs
 * after the last reason to transmit ends -- a wake-up with no call after
 * it, or the last active slot's call ending (terminator, lost signal or
 * the slot watchdog) -- and dmrTxThread() un-keys only if no new call has
 * made a slot active by then. Like a real MMDVM repeater, this lets radios
 * reply without a fresh wake-up, and lets RX pick a still-transmitting
 * radio back up after losing its sync instead of dropping straight to
 * idle (where only a wake-up is accepted). dmrTxHangNs is when the hang
 * started; 0 = no hang running. [DMR] TxHang in the host config, seconds. */
static int64_t         dmrTxHangNs             = 0;
static int64_t         dmrTxHangDurNs          = 3000000000LL;  // 3s default

/* Slot activity watchdog (SAMPLE/BIT mode). decodeDMR() marks a slot
 * active on any data-sync burst, CSBKs included, but only a terminator or
 * a LOST clears it. A slot activated by a lone CSBK -- no call behind it,
 * so no terminator and no RX coasting to produce a LOST -- stayed active
 * forever and kept the repeater keyed (seen on real hardware). Every
 * frame decodeDMR() sees refreshes its slot's timestamp; dmrSlotWatchdog()
 * delivers a LOST for any active slot quiet for DMR_SLOT_IDLE_TIMEOUT_NS,
 * which runs the normal end-of-call cleanup. A live call delivers a frame
 * every 60ms (coasted or not), so it never trips this. */
static int64_t         dmrSlotLastFrameNs[2]   = {0, 0};
static pthread_mutex_t dmrSlotActivityMutex    = PTHREAD_MUTEX_INITIALIZER;
static const int64_t   DMR_SLOT_IDLE_TIMEOUT_NS = 1500000000LL;  // 1.5s

/*
 * dmrNowNs()
 *
 * Reads the monotonic clock.
 *
 * Parameters: none.
 *
 * Returns: Time in nanoseconds.
 */
static int64_t dmrNowNs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/*
 * dmrSlotNoteFrame()
 *
 * Records that a frame has just been seen on a slot, for the slot watchdog.
 *
 * Parameters:
 *   slot - Time slot index, 0 or 1.
 *
 * Returns: nothing.
 */
static void dmrSlotNoteFrame(uint8_t slot)
{
    int64_t now = dmrNowNs();
    pthread_mutex_lock(&dmrSlotActivityMutex);
    dmrSlotLastFrameNs[slot] = now;
    pthread_mutex_unlock(&dmrSlotActivityMutex);
}

/*
 * dmrReleaseTx()
 *
 * Called when the last active slot's call ends.  In frame mode the
 * transmitter is un-keyed at once (the MMDVM firmware handles any hang
 * time).  In SAMPLE and BIT mode the TX hang is started instead, and
 * dmrTxThread() un-keys when it runs out.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void dmrReleaseTx(void)
{
    if (packetType == PACKET_TYPE_SAMP || packetType == PACKET_TYPE_BIT)
    {
        dmrTxHangNs = dmrNowNs();
        if (debugM)
            fprintf(stderr, "DMR TX: last call ended, holding downlink for %lld ms\n",
                    (long long)(dmrTxHangDurNs / 1000000LL));
    }
    else
    {
        txOn = false;
    }
}

/*
 * dmrRxNominalSampleNs()
 *
 * Converts a position in the receive sample stream to a time, assuming
 * exactly 24000 samples a second.
 *
 * Parameters:
 *   ptr - Sample position.
 *
 * Returns: Time in nanoseconds.
 */
static int64_t dmrRxNominalSampleNs(uint32_t ptr)
{
    return ((int64_t)ptr * 1000000000LL) / 24000LL;
}

/*
 * dmrRxNoteSamplePacket()
 *
 * Called once for each incoming SAMP packet, before its samples are
 * processed.  It compares the packet's arrival time with the nominal sample
 * time to track the offset between the sample clock and the system clock,
 * using the smallest offset seen in the current window.
 *
 * Parameters:
 *   arrivalNs - Arrival time of the packet, in nanoseconds on the monotonic
 *               clock.
 *   endPtr    - Value of dataPtr just past the last sample of the packet.
 *
 * Returns: nothing.
 */
static void dmrRxNoteSamplePacket(int64_t arrivalNs, uint32_t endPtr)
{
    if (arrivalNs - dmrRxClockWindowStartNs >= DMR_RX_CLOCK_WINDOW_NS)
    {
        dmrRxClockOffsetPrevNs  = dmrRxClockOffsetNs;
        dmrRxClockOffsetNs      = INT64_MAX;
        dmrRxClockWindowStartNs = arrivalNs;
    }

    int64_t offset = arrivalNs - dmrRxNominalSampleNs(endPtr);
    if (offset < dmrRxClockOffsetNs)
        dmrRxClockOffsetNs = offset;
}

/*
 * dmrRxSampleTimeNs()
 *
 * Gives the best estimate of when a sample arrived.
 *
 * Parameters:
 *   ptr - Sample position.
 *
 * Returns: Arrival time in nanoseconds on the monotonic clock, or 0 if no
 *   packet has been timed yet.
 */
static int64_t dmrRxSampleTimeNs(uint32_t ptr)
{
    int64_t best = (dmrRxClockOffsetNs < dmrRxClockOffsetPrevNs) ? dmrRxClockOffsetNs : dmrRxClockOffsetPrevNs;
    if (best == INT64_MAX)
        return 0;
    return best + dmrRxNominalSampleNs(ptr);
}

/*
 * dmrRxPhaseNs()
 *
 * Works out where a sample falls in the 60 ms TDMA frame, counted from the
 * start of TX slot 1.
 *
 * Parameters:
 *   ptr - Sample position.
 *
 * Returns: Phase in nanoseconds (0 to 59999999), or -1 if there is no timing
 *   yet.
 */
static int64_t dmrRxPhaseNs(uint32_t ptr)
{
    pthread_mutex_lock(&dmrTxSlotTimeMutex);
    int64_t t1 = dmrTxSlot1StartNs;
    pthread_mutex_unlock(&dmrTxSlotTimeMutex);

    int64_t tSample = dmrRxSampleTimeNs(ptr);
    if (t1 == 0 || tSample == 0)
        return -1;

    const int64_t cycleNs = 60000000LL;
    int64_t phase = (tSample - t1) % cycleNs;
    if (phase < 0)
        phase += cycleNs;
    return phase;
}

/*
 * dmrRxCyclePos()
 *
 * Works out the position of a receive sample within this host's TDMA cycle
 * (0 to 1439 samples), using the TX timing report from the modem.
 *
 * Parameters:
 *   ptr - Sample position.
 *
 * Returns: Position in the cycle, or -1 if there is no TX timing report for
 *   this downlink.
 */
static int32_t dmrRxCyclePos(uint32_t ptr)
{
    pthread_mutex_lock(&dmrTxSlotTimeMutex);
    bool     valid = dmrTxRefValid;
    uint32_t refRx = dmrTxRefRxPtr;
    uint32_t refK  = dmrTxRefK;
    pthread_mutex_unlock(&dmrTxSlotTimeMutex);

    if (!valid)
        return -1;

    int64_t tx  = (int64_t)refK + (int64_t)(int32_t)(ptr - refRx);
    int64_t pos = tx % DMR_CYCLE_SAMPLES;
    if (pos < 0)
        pos += DMR_CYCLE_SAMPLES;
    return (int32_t)pos;
}

/*
 * dmrRxSlotFromPhase()
 *
 * Decides which time slot a received sync belongs to, from where it falls in
 * the TDMA cycle.  It uses the TX timing report when there is one and the
 * wall clock phase otherwise.
 *
 * Parameters:
 *   ptr - Sample position of the sync.
 *
 * Returns: 0 for slot 1, 1 for slot 2, -1 if there is no timing yet.
 */
static int dmrRxSlotFromPhase(uint32_t ptr)
{
    int32_t pos = dmrRxCyclePos(ptr);
    if (pos >= 0)
    {
        // Distance from the TS1 sync position, folded into [-720, 720).
        int32_t d = (pos - DMR_TS1_SYNC_CYCLE_POS - dmrSlotDelaySamples) % DMR_CYCLE_SAMPLES;
        if (d < 0)
            d += DMR_CYCLE_SAMPLES;
        if (d >= DMR_CYCLE_SAMPLES / 2)
            d -= DMR_CYCLE_SAMPLES;
        return (d > -DMR_CYCLE_SAMPLES / 4 && d < DMR_CYCLE_SAMPLES / 4) ? 0 : 1;
    }

    // Fallback: wall-clock phase estimate.
    int64_t phase = dmrRxPhaseNs(ptr);
    if (phase < 0)
        return -1;

    // Distance from the TS1 centre, folded into [-30ms, +30ms).
    const int64_t cycleNs = 60000000LL;
    int64_t d = (phase - dmrRxTs1PhaseNs) % cycleNs;
    if (d < 0)
        d += cycleNs;
    if (d >= cycleNs / 2)
        d -= cycleNs;

    return (d > -15000000LL && d < 15000000LL) ? 0 : 1;
}

/*
 * dmrRxLogSlotPhase()
 *
 * Debug output: prints where a locked sync falls in the TDMA cycle and which
 * slot that implies, next to the slot the context was given.
 *
 * Parameters:
 *   c - Receive context that has just locked.
 *
 * Returns: nothing.
 */
static void dmrRxLogSlotPhase(const DMR_RX_CTX* c)
{
    int64_t phase = dmrRxPhaseNs(c->syncPtr);
    if (phase < 0)
        return;

    int fromPhase = dmrRxSlotFromPhase(c->syncPtr);
    int32_t pos   = dmrRxCyclePos(c->syncPtr);
    if (pos >= 0)
        fprintf(stderr, "DMR RX slot timing: sync at cycle sample %d (TS1 ~%d, TS2 ~%d) -> slot%d (ctx%u, assigned slot%u, control=0x%02X, txOn=%d)\n",
                (int)pos, (int)(DMR_TS1_SYNC_CYCLE_POS + dmrSlotDelaySamples),
                (int)(DMR_TS1_SYNC_CYCLE_POS + dmrSlotDelaySamples + DMR_CYCLE_SAMPLES / 2),
                fromPhase + 1, c->index, c->slot + 1U, c->control, (int)txOn);
    else
        fprintf(stderr, "DMR RX slot phase: %lld.%lld ms after TX slot1 start (ctx%u, assigned slot%u, phase says slot%d, control=0x%02X, txOn=%d) [no TX timing report yet]\n",
                (long long)(phase / 1000000LL), (long long)((phase / 100000LL) % 10LL),
                c->index, c->slot + 1U, fromPhase + 1, c->control, (int)txOn);
}

#define DMR_RX_BUF(s, idx) ((s)->buffer[(idx) % 900U])  // 900U = sizeof(s->buffer)/sizeof(q15_t)

/*
 * dmrRxCtxReset()
 *
 * Resets one receive context, as CDMRSlotRX::reset() does in MMDVM.  Used at
 * start-up and whenever the context's stream is lost.
 *
 * Parameters:
 *   c - Context to reset.
 *
 * Returns: nothing.
 */
static void dmrRxCtxReset(DMR_RX_CTX* c)
{
    c->syncPtr   = 0U;
    c->maxCorr   = 0;
    c->control   = DMR_RX_CONTROL_NONE;
    c->syncCount = 0U;
    c->state     = DMR_RXS_NONE;
    c->startPtr  = 0U;
    c->endPtr    = DMR_RX_NOENDPTR;
}

/*
 * dmrRxReset()
 *
 * Resets both receive contexts at start-up.  It does not touch dataPtr,
 * which runs freely for the life of the process.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void dmrRxReset(void)
{
    for (uint8_t i = 0U; i < 2U; i++)
    {
        dmrRxCtxReset(&dmrRxCtx[i]);
        dmrRxCtx[i].index = i;
        dmrRxCtx[i].slot  = i;
    }
}

/*
 * dmrRxSamplesToBits()
 *
 * Converts samples to bits for the receive burst, like
 * CDMRSlotRX::samplesToBits().  The start and count are absolute, free-
 * running sample counts, so the buffer is addressed modulo its size.  Each
 * sample is compared with the centre and threshold levels.
 *
 * Parameters:
 *   start     - Absolute position of the first sample.
 *   count     - Number of symbols to convert.
 *   buffer    - Receives the bits.
 *   offset    - Bit position in buffer where the first bit is written.
 *   centre    - Centre level of the signal.
 *   threshold - Level that separates the inner and outer symbols.
 *
 * Returns: nothing.
 */
static void dmrRxSamplesToBits(uint32_t start, uint16_t count, uint8_t* buffer, uint16_t offset, q15_t centre, q15_t threshold)
{
    DMR_RX_STATE* s = &dmrRx;

    for (uint16_t i = 0U; i < count; i++, start += DMR_RADIO_SYMBOL_LENGTH)
    {
        q15_t sample = DMR_RX_BUF(s, start) - centre;

        if (sample < -threshold)
        {
            DMR_WRITE_BIT1(buffer, offset, false);
            offset++;
            DMR_WRITE_BIT1(buffer, offset, true);
            offset++;
        }
        else if (sample < 0)
        {
            DMR_WRITE_BIT1(buffer, offset, false);
            offset++;
            DMR_WRITE_BIT1(buffer, offset, false);
            offset++;
        }
        else if (sample < threshold)
        {
            DMR_WRITE_BIT1(buffer, offset, true);
            offset++;
            DMR_WRITE_BIT1(buffer, offset, false);
            offset++;
        }
        else
        {
            DMR_WRITE_BIT1(buffer, offset, true);
            offset++;
            DMR_WRITE_BIT1(buffer, offset, true);
            offset++;
        }
    }
}

/*
 * dmrRxOtherCtx()
 *
 * Gives the other of the two receive contexts.  It is used to label a newly
 * locked stream with the opposite slot when the other context is already
 * tracking one.
 *
 * Parameters:
 *   c - One receive context.
 *
 * Returns: The other context.
 */
static DMR_RX_CTX* dmrRxOtherCtx(const DMR_RX_CTX* c)
{
    return &dmrRxCtx[c->index ^ 1U];
}

/*
 * dmrRxCtxActive()
 *
 * Tells whether a receive context is following a stream.
 *
 * Parameters:
 *   c - Receive context.
 *
 * Returns: true if the context is tracking a burst or is in a state other
 *   than idle.
 */
static bool dmrRxCtxActive(const DMR_RX_CTX* c)
{
    return c->endPtr != DMR_RX_NOENDPTR || c->state != DMR_RXS_NONE;
}

/*
 * dmrRxNearTrackedStream()
 *
 * Tells whether a sample position lines up (within the guard, modulo one
 * TDMA frame) with the sync position of a stream that an active context is
 * already tracking.
 *
 * Parameters:
 *   dataPtr - Sample position to test.
 *
 * Returns: true if it is near a tracked stream, false if not.
 */
static bool dmrRxNearTrackedStream(uint32_t dataPtr)
{
    for (uint8_t i = 0U; i < 2U; i++)
    {
        const DMR_RX_CTX* a = &dmrRxCtx[i];
        if (a->endPtr == DMR_RX_NOENDPTR)
            continue;

        int32_t diff = (int32_t)(dataPtr - a->syncPtr);
        int32_t m    = diff % (int32_t)DMR_RX_BURST_REPEAT_SAMPLES;
        if (m < 0)
            m += (int32_t)DMR_RX_BURST_REPEAT_SAMPLES;
        if (m > (int32_t)DMR_RX_BURST_REPEAT_SAMPLES / 2)
            m = (int32_t)DMR_RX_BURST_REPEAT_SAMPLES - m;

        if (m < DMR_RX_STREAM_GUARD_SAMPLES)
            return true;
    }
    return false;
}

// Diagnostic only (gated on debugM): tracks how close the symbol-level
// pre-screen (dmrCountBits32() below) is coming to a sync match even
// when it doesn't pass, so a run with no accepted locks can still show
// whether the correlator is seeing a near-miss (a tuning/threshold
// problem) or nothing resembling sync at all (a signal-path problem).
// Reported once per DMR_RX_DIAG_PERIOD_SAMPLES_LOCAL of real RX samples.
static const uint32_t DMR_RX_DIAG_PERIOD_SAMPLES_LOCAL = 24000U;  // ~1s @ 24kHz
static uint8_t  dmrRxDiagBestDist   = 255U;
static uint32_t dmrRxDiagWindowBase = 0U;

/*
 * dmrRxCorrelateSync()
 *
 * Correlates the received symbols against the DMR sync patterns for one
 * context, as CDMRSlotRX::correlateSync() does in MMDVM.  The match is
 * stricter while starting from cold, to avoid locking onto noise, and looser
 * once a burst sequence is being followed.  When a stream is freshly locked
 * ("first"), the slot label is also chosen, once: the opposite slot if the
 * other context is already tracking one, otherwise the slot given by where
 * the burst falls in the TDMA cycle.
 *
 * Parameters:
 *   c     - Receive context to work on.
 *   first - true when this is the first lock of a stream.
 *
 * Returns: nothing.
 */
static void dmrRxCorrelateSync(DMR_RX_CTX* c, bool first)
{
    DMR_RX_STATE* s = &dmrRx;

    uint8_t errs = dmrCountBits32((s->bitBuffer[s->bitPtr % DMR_RADIO_SYMBOL_LENGTH] & DMR_SYNC_SYMBOLS_MASK) ^ DMR_MS_DATA_SYNC_SYMBOLS);

    // The voice sync is the complement of the data sync
    bool isData  = (errs <= DMR_MAX_SYNC_SYMBOLS_ERRS);
    bool isVoice = (errs >= (DMR_SYNC_LENGTH_SYMBOLS_LOCAL - DMR_MAX_SYNC_SYMBOLS_ERRS));

    if (debugM)
    {
        // Distance to whichever of the two accept thresholds is closer:
        // 0 means "would have passed"; small numbers mean a near-miss.
        uint8_t distToData  = (errs > DMR_MAX_SYNC_SYMBOLS_ERRS) ? (uint8_t)(errs - DMR_MAX_SYNC_SYMBOLS_ERRS) : 0U;
        uint8_t voiceThresh = (uint8_t)(DMR_SYNC_LENGTH_SYMBOLS_LOCAL - DMR_MAX_SYNC_SYMBOLS_ERRS);
        uint8_t distToVoice = (errs < voiceThresh) ? (uint8_t)(voiceThresh - errs) : 0U;
        uint8_t dist = (distToData < distToVoice) ? distToData : distToVoice;
        if (dist < dmrRxDiagBestDist)
            dmrRxDiagBestDist = dist;

        if (s->dataPtr - dmrRxDiagWindowBase >= DMR_RX_DIAG_PERIOD_SAMPLES_LOCAL)
        {
            fprintf(stderr, "DMR RX diag: closest symbol-sync miss this window = %u (0 = would lock)\n", dmrRxDiagBestDist);
            dmrRxDiagBestDist   = 255U;
            dmrRxDiagWindowBase = s->dataPtr;
        }
    }

    if (!isData && !isVoice)
        return;

    uint32_t ptr = s->dataPtr - DMR_SYNC_LENGTH_SAMPLES_LOCAL + DMR_RADIO_SYMBOL_LENGTH;

    q31_t corr = 0;
    q15_t minV =  16000;
    q15_t maxV = -16000;

    for (uint16_t i = 0U; i < DMR_SYNC_LENGTH_SYMBOLS_LOCAL; i++)
    {
        q15_t val = DMR_RX_BUF(s, ptr);

        if (val > maxV) maxV = val;
        if (val < minV) minV = val;

        int8_t corrVal = isData ? DMR_MS_DATA_SYNC_SYMBOLS_VALUES[i] : DMR_MS_VOICE_SYNC_SYMBOLS_VALUES[i];

        switch (corrVal)
        {
            case 3:
                corr -= (val + val + val);
                break;
            case 1:
                corr -= val;
                break;
            case -1:
                corr += val;
                break;
            default:  // -3
                corr += (val + val + val);
                break;
        }

        ptr += DMR_RADIO_SYMBOL_LENGTH;
    }

    if (corr <= c->maxCorr)
        return;

    q15_t centre = (maxV + minV) >> 1;
    q31_t v1 = (maxV - centre) * DMR_RX_SCALING_FACTOR;
    q15_t threshold = (q15_t)(v1 >> 15);

    uint8_t sync[DMR_SYNC_BYTES_LENGTH];
    uint32_t syncPtr = s->dataPtr - DMR_SYNC_LENGTH_SAMPLES_LOCAL + DMR_RADIO_SYMBOL_LENGTH;
    dmrRxSamplesToBits(syncPtr, DMR_SYNC_LENGTH_SYMBOLS_LOCAL, sync, 4U, centre, threshold);

    const uint8_t* syncBytes = isData ? DMR_MS_DATA_SYNC_BYTES : DMR_MS_VOICE_SYNC_BYTES;
    uint8_t errs2 = 0U;
    for (uint8_t i = 0U; i < DMR_SYNC_BYTES_LENGTH; i++)
        errs2 += dmrCountBits8((sync[i] & SYNC_MASK[i]) ^ syncBytes[i]);

    // Stricter while acquiring sync from cold (avoid locking onto noise);
    // looser once already tracking a burst sequence (context already
    // supports a slightly weaker match) -- see p25_func.h's
    // MAX_SYNC_BIT_START_ERRS/MAX_SYNC_BIT_RUN_ERRS, which this mirrors.
    uint8_t maxByteErrs = (c->state == DMR_RXS_NONE) ? DMR_MAX_SYNC_BYTE_START_ERRS : DMR_MAX_SYNC_BYTE_RUN_ERRS;
    if (errs2 > maxByteErrs)
        return;

    if (first)
    {
        c->threshold[0U] = c->threshold[1U] = c->threshold[2U] = c->threshold[3U] = threshold;
        c->centre[0U]    = c->centre[1U]    = c->centre[2U]    = c->centre[3U]    = centre;
        c->averagePtr    = 0U;

        // Slot label for a newly locked stream. If the other context is
        // already tracking a stream, this one is by definition on the
        // other slot. Otherwise the slot comes from where the burst lands
        // relative to our own TX slot-1 timing (see dmrRxSlotFromPhase()).
        // While the repeater is un-keyed the label doesn't matter: the MS
        // isn't timed to us yet, and dmrRxCtxBurstEnd() only passes a
        // wake-up CSBK in that state.
        DMR_RX_CTX* other = dmrRxOtherCtx(c);
        if (dmrRxCtxActive(other))
        {
            c->slot = other->slot ^ 1U;
        }
        else
        {
            int fromPhase = txOn ? dmrRxSlotFromPhase(s->dataPtr) : -1;
            c->slot = (fromPhase >= 0) ? (uint8_t)fromPhase : 0U;
        }

        if (debugM)
            fprintf(stderr, "DMR RX ctx%u: new sync lock, dataPtr=%u assigned slot%u\n", c->index, s->dataPtr, c->slot + 1U);
    }
    else
    {
        c->threshold[c->averagePtr] = threshold;
        c->centre[c->averagePtr]    = centre;

        c->averagePtr++;
        if (c->averagePtr >= 4U)
            c->averagePtr = 0U;
    }

    c->maxCorr  = corr;
    c->control  = isData ? DMR_RX_CONTROL_DATA : DMR_RX_CONTROL_VOICE;
    c->syncPtr  = s->dataPtr;
    if (first)
    {
        c->expectPtr  = s->dataPtr;
        c->driftQ8    = 0;
        c->driftAccQ8 = 0;
        c->sinceSync  = 0U;
    }
    c->startPtr = s->dataPtr - DMR_SLOT_TYPE_LENGTH_SAMPLES_LOCAL / 2U - DMR_INFO_LENGTH_SAMPLES_LOCAL / 2U - DMR_SYNC_LENGTH_SAMPLES_LOCAL;
    c->endPtr   = s->dataPtr + DMR_SLOT_TYPE_LENGTH_SAMPLES_LOCAL / 2U + DMR_INFO_LENGTH_SAMPLES_LOCAL / 2U - 1U;
}

/*
 * dmrRxDeliverFrame()
 *
 * Hands a finished, demodulated 33 byte DMR burst to the frame-mode protocol
 * logic, exactly as if it had arrived as a DMD1 or DML1 (slot 1) or DMD2 or
 * DML2 (slot 2) packet from the network.
 *
 * Parameters:
 *   slot        - Time slot index, 0 or 1.
 *   frame33     - The 33 byte burst.
 *   controlByte - Control byte that tells the kind of burst (data, voice or
 *                 none).
 *   lost        - true to deliver a "lost" frame instead of data.
 *
 * Returns: nothing.
 */
static void dmrRxDeliverFrame(uint8_t slot, const uint8_t* frame33, uint8_t controlByte, bool lost)
{
    // frame33 must point at the 33-byte frame PAYLOAD (no leading control
    // byte) -- matching what decodeDMR() expects at buffer[9..41]. Total
    // packet size is 4 (0x61/lenHi/lenLo/typeLen) + 4 (type) + 1 (control)
    // + 33 (frame) = 42 bytes -- i.e. 9U + DMR_FRAME_LENGTH_BYTES, NOT
    // 8U + DMR_FRAME_LENGTH_BYTES (that undercounts the control byte at
    // buf[8] and was a pre-existing off-by-one here: buf[41] is in bounds
    // for a 42-byte array but was one past the end of a 41-byte one).
    uint8_t buf[9U + DMR_FRAME_LENGTH_BYTES];
    const char* type = (slot == 0U) ? (lost ? TYPE_LOST1 : TYPE_DATA1) : (lost ? TYPE_LOST2 : TYPE_DATA2);

    buf[0U] = 0x61;
    buf[1U] = 0x00;
    buf[2U] = 9U + DMR_FRAME_LENGTH_BYTES;
    buf[3U] = 0x04;
    memcpy(buf + 4U, type, 4U);
    buf[8U] = controlByte;
    memcpy(buf + 9U, frame33, DMR_FRAME_LENGTH_BYTES);
    decodeDMR(buf, 9U + DMR_FRAME_LENGTH_BYTES, false);
}

/*
 * dmrSlotWatchdog()
 *
 * Ends a slot that has been marked active but has gone quiet.  Any frame
 * seen by decodeDMR() refreshes the slot's time stamp; a slot that is active
 * and has had no frame for DMR_SLOT_IDLE_TIMEOUT_NS gets a lost frame, which
 * runs the normal end-of-call cleanup.  This stops a slot that was activated
 * by a lone CSBK, with no call behind it, from keeping the repeater keyed.
 * It runs on the RX thread, and does nothing in frame mode.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void dmrSlotWatchdog(void)
{
    if (packetType != PACKET_TYPE_SAMP && packetType != PACKET_TYPE_BIT)
        return;

    int64_t now = dmrNowNs();
    for (uint8_t i = 0U; i < 2U; i++)
    {
        if (!slotActive[i])
            continue;

        pthread_mutex_lock(&dmrSlotActivityMutex);
        int64_t last = dmrSlotLastFrameNs[i];
        pthread_mutex_unlock(&dmrSlotActivityMutex);

        if (last != 0 && now - last < DMR_SLOT_IDLE_TIMEOUT_NS)
            continue;

        if (debugM)
            fprintf(stderr, "DMR: slot%u active but quiet, ending it\n", i + 1U);

        uint8_t blank[DMR_FRAME_LENGTH_BYTES];
        memset(blank, 0, sizeof(blank));
        dmrRxDeliverFrame(i, blank, 0x00U, true);
    }
}

// Read out and decode the burst a context has just finished receiving
// (dataPtr == c->endPtr), then either coast to the next burst on the same
// slot or go back to idle.
/*
 * dmrRxDiagMissingSync()
 *
 * Diagnostic only (debug mode).  Called when a followed voice call reaches
 * the position where burst A, and so a voice sync, should be but none was
 * found.  It demodulates the 24 symbol sync field at each sample offset
 * within 24 samples of the expected place, compares each with the sync
 * patterns of ETSI TS 102 361-1 Table 9.2 and prints the closest match.
 * That tells apart a different sync pattern (the pattern name), the right
 * sync outside the tracking window (the offset) and no sync at all (a high
 * bit error count).
 *
 * Parameters:
 *   c         - Receive context being followed.
 *   centre    - Centre level of the signal.
 *   threshold - Level that separates the inner and outer symbols.
 *
 * Returns: nothing.
 */
static void dmrRxDiagMissingSync(const DMR_RX_CTX* c, q15_t centre, q15_t threshold)
{
    static const struct
    {
        const char* name;
        uint64_t    bits;
    } patterns[] = {
        {"MS voice",      0x7F7D5DD57DFDULL},
        {"MS data",       0xD5D7F77FD757ULL},
        {"BS voice",      0x755FD7DF75F7ULL},
        {"BS data",       0xDFF57D75DF5DULL},
        {"RC",            0x77D55F7DFD77ULL},
        {"DM TS1 voice",  0x5D577F7757FFULL},
        {"DM TS1 data",   0xF7FDD5DDFD55ULL},
        {"DM TS2 voice",  0x7DFFD5F55D5FULL},
        {"DM TS2 data",   0xD7557F5FF7F5ULL},
        {"reserved",      0xDD7FF5D757DDULL},
    };
    const uint64_t mask48 = 0xFFFFFFFFFFFFULL;

    uint8_t  bestPat  = 0U;
    uint8_t  bestDist = 255U;
    int32_t  bestOff  = 0;
    uint8_t  msVoiceAt0 = 255U;

    uint32_t base = c->syncPtr - DMR_SYNC_LENGTH_SAMPLES_LOCAL + DMR_RADIO_SYMBOL_LENGTH;
    for (int32_t off = -24; off <= 24; off++)
    {
        uint8_t bytes[6];
        memset(bytes, 0, sizeof(bytes));
        dmrRxSamplesToBits((uint32_t)((int32_t)base + off), DMR_SYNC_LENGTH_SYMBOLS_LOCAL, bytes, 0U, centre, threshold);

        uint64_t v = 0ULL;
        for (uint8_t i = 0U; i < 6U; i++)
            v = (v << 8) | bytes[i];

        for (uint8_t p = 0U; p < sizeof(patterns) / sizeof(patterns[0]); p++)
        {
            uint8_t dist = dmrCountBits64((v ^ patterns[p].bits) & mask48);
            if (off == 0 && p == 0U)
                msVoiceAt0 = dist;
            if (dist < bestDist)
            {
                bestDist = dist;
                bestPat  = p;
                bestOff  = off;
            }
        }
    }

    fprintf(stderr, "DMR RX ctx%u slot%u: no sync where burst A expected -- closest: %s, %u/48 bits wrong at offset %+d (MS voice at offset 0: %u/48 wrong)\n",
            c->index, c->slot + 1U, patterns[bestPat].name, bestDist, (int)bestOff, msVoiceAt0);
}

/*
 * dmrRxCtxBurstEnd()
 *
 * Reads out and decodes the burst a context has just finished receiving
 * (dataPtr equals the context's endPtr), then either coasts to the next
 * burst on the same slot or goes back to idle.  A sync found while tracking
 * is folded into the drift estimate.  A simplex hotspot has no wake-up, and
 * a radio's repeated wake-up CSBK is handled here; completed bursts are
 * passed on with dmrRxDeliverFrame().
 *
 * Parameters:
 *   c - Receive context whose burst has ended.
 *
 * Returns: nothing.
 */
static void dmrRxCtxBurstEnd(DMR_RX_CTX* c)
{
    uint8_t slot = c->slot;

    // A sync found while tracking: fold its offset into the drift estimate.
    if (c->state != DMR_RXS_NONE && c->control != DMR_RX_CONTROL_NONE && c->sinceSync > 0U)
    {
        int32_t offset = (int32_t)(c->syncPtr - c->expectPtr);
        int32_t delta  = (offset * 256) / (int32_t)c->sinceSync / 2;
        if (delta > DMR_RX_MAX_DRIFT_STEP_Q8)
            delta = DMR_RX_MAX_DRIFT_STEP_Q8;
        if (delta < -DMR_RX_MAX_DRIFT_STEP_Q8)
            delta = -DMR_RX_MAX_DRIFT_STEP_Q8;
        c->driftQ8 += delta;
        if (c->driftQ8 > DMR_RX_MAX_DRIFT_Q8)
            c->driftQ8 = DMR_RX_MAX_DRIFT_Q8;
        if (c->driftQ8 < -DMR_RX_MAX_DRIFT_Q8)
            c->driftQ8 = -DMR_RX_MAX_DRIFT_Q8;
    }

    if (debugM)
    {
        fprintf(stderr, "DMR RX ctx%u slot%u: burst end, dataPtr=%u control=0x%02X state=%u syncCount=%u",
                c->index, slot + 1U, dmrRx.dataPtr, c->control, c->state, c->syncCount);
        if (c->state != DMR_RXS_NONE && c->control != DMR_RX_CONTROL_NONE)
            fprintf(stderr, " sync offset=%d drift=%d.%02d/burst", (int)(int32_t)(c->syncPtr - c->expectPtr),
                    (int)(c->driftQ8 / 256), (int)(((c->driftQ8 < 0 ? -c->driftQ8 : c->driftQ8) % 256) * 100 / 256));
        fprintf(stderr, "\n"); }
        if (c->control != DMR_RX_CONTROL_NONE)
            dmrRxLogSlotPhase(c);
  //  }

    q15_t centre    = (c->centre[0U]    + c->centre[1U]    + c->centre[2U]    + c->centre[3U])    >> 2;
    q15_t threshold = (c->threshold[0U] + c->threshold[1U] + c->threshold[2U] + c->threshold[3U]) >> 2;

    uint8_t frame[DMR_FRAME_LENGTH_BYTES + 1U];
    frame[0U] = c->control;

    uint32_t ptr = c->endPtr - DMR_FRAME_LENGTH_SAMPLES_LOCAL + DMR_RADIO_SYMBOL_LENGTH + 1U;
    dmrRxSamplesToBits(ptr, DMR_FRAME_LENGTH_SYMBOLS_LOCAL, frame, 8U, centre, threshold);

    if (debugM && txOn && c->state == DMR_RXS_VOICE && c->control == DMR_RX_CONTROL_NONE && c->n >= 5U)
        dmrRxDiagMissingSync(c, centre, threshold);

    // Default: go idle. If the decode below leaves a voice/data sequence
    // in progress, the coast block after it re-arms the window one TDMA
    // frame later instead.
    uint32_t burstEndPtr = c->endPtr;
    c->endPtr  = DMR_RX_NOENDPTR;
    c->maxCorr = 0;

    /* Simplex has no wake-up: a radio talking to a simplex hotspot starts its
     * call straight away with a voice header, so there is no idle state to
     * filter and the gate below only applies to a duplex repeater.
     *
     * Repeater un-keyed: behave like MMDVM's DMRIdleRX. A subscriber radio
     * isn't timed to our slots until it hears our downlink, so the only
     * thing that matters here is its wake-up CSBK (BS_Dwn_Act). Pass that
     * on flagged CONTROL_IDLE (0x80) -- 0xC3 -- exactly as MMDVM firmware
     * delivers it in FRAME mode, so decodeDMR() keys up without marking
     * any slot active. Everything else is ignored, and no call sequence
     * starts. Previously the wake-up arrived as a plain 0x43 CSBK, which
     * marked slot 1 active regardless of the radio's real slot; a call
     * that then ran on slot 2 left slot 1 active with no terminator ever
     * coming, i.e. a stuck key. */
    if (modem_duplex && !txOn && c->state == DMR_RXS_NONE)
    {
        if (c->control == DMR_RX_CONTROL_DATA)
        {
            uint8_t colorCode, dataType;
            DMRSlotType_decode(frame + 1U, &colorCode, &dataType);
            if (colorCode == dmrColorCode && dataType == DT_CSBK)
            {
                if (debugM)
                    fprintf(stderr, "DMR RX ctx%u: wake-up CSBK while idle\n", c->index);
                dmrRxDeliverFrame(0U, frame + 1U, DMR_RX_CONTROL_IDLE | DMR_RX_CONTROL_DATA | DT_CSBK, false);
            }
        }
        c->control = DMR_RX_CONTROL_NONE;
        return;
    }

    if (c->control == DMR_RX_CONTROL_DATA)
    {
        uint8_t colorCode, dataType;
        DMRSlotType_decode(frame + 1U, &colorCode, &dataType);

        if (colorCode == dmrColorCode)
        {
            c->syncCount = 0U;
            c->n         = 0U;

            frame[0U] |= dataType;

            switch (dataType)
            {
                case DT_TERMINATOR_WITH_LC:
                    if (c->state == DMR_RXS_VOICE)
                    {
                        dmrRxDeliverFrame(slot, frame + 1U, frame[0U], false);
                        c->state = DMR_RXS_NONE;
                    }
                    break;
                case DT_VOICE_LC_HEADER:
                    dmrRxDeliverFrame(slot, frame + 1U, frame[0U], false);
                    c->state = DMR_RXS_VOICE;
                    break;
                case DT_VOICE_PI_HEADER:
                    if (c->state == DMR_RXS_VOICE)
                        dmrRxDeliverFrame(slot, frame + 1U, frame[0U], false);
                    c->state = DMR_RXS_VOICE;
                    break;
                case DT_DATA_HEADER:
                    dmrRxDeliverFrame(slot, frame + 1U, frame[0U], false);
                    c->state = DMR_RXS_DATA;
                    c->type  = 0x00U;
                    break;
                case DT_RATE_12_DATA:
                case DT_RATE_34_DATA:
                case DT_RATE_1_DATA:
                    if (c->state == DMR_RXS_DATA)
                    {
                        dmrRxDeliverFrame(slot, frame + 1U, frame[0U], false);
                        c->type = dataType;
                    }
                    break;
                default:  // DT_CSBK and others
                    /* A radio repeats its wake-up CSBK (BS_Dwn_Act) until it
                     * hears the downlink, so one can still arrive after we've
                     * keyed -- not yet timed to our slots, so its slot label
                     * is meaningless. Relaying it marked a random slot active
                     * with no call behind it (seen on real hardware: the
                     * repeater never de-keyed). MMDVMHost ignores BS_Dwn_Act
                     * once keyed; so does this. */
                    if (dataType == DT_CSBK && CSBK_decode(frame + 1U, slot) && CSBK_getCSBKO(slot) == CSBKO_BSDWNACT)
                    {
                        if (debugM)
                            fprintf(stderr, "DMR RX ctx%u: repeated wake-up CSBK while keyed, ignored\n", c->index);
                    }
                    else
                    {
                        dmrRxDeliverFrame(slot, frame + 1U, frame[0U], false);
                    }
                    c->state = DMR_RXS_NONE;
                    break;
            }
        }
    }
    else if (c->control == DMR_RX_CONTROL_VOICE)
    {
        dmrRxDeliverFrame(slot, frame + 1U, frame[0U], false);
        c->state     = DMR_RXS_VOICE;
        c->syncCount = 0U;
        c->n         = 0U;
    }
    else
    {
        if (c->state != DMR_RXS_NONE)
        {
            c->syncCount++;
            if (c->syncCount >= DMR_MAX_SYNC_LOST_FRAMES)
            {
                dmrRxDeliverFrame(slot, frame + 1U, 0x00U, true);
                c->state = DMR_RXS_NONE;
            }
        }

        if (c->state == DMR_RXS_VOICE)
        {
            if (c->n >= 5U)
            {
                frame[0U] = DMR_RX_CONTROL_VOICE;
                c->n = 0U;
            }
            else
            {
                frame[0U] = ++c->n;
            }

            dmrRxDeliverFrame(slot, frame + 1U, frame[0U], false);
        }
        else if (c->state == DMR_RXS_DATA)
        {
            if (c->type != 0x00U)
                dmrRxDeliverFrame(slot, frame + 1U, DMR_RX_CONTROL_DATA | c->type, false);
        }
    }

    /* Coast: a voice/data sequence is still in progress, so the next
     * burst on this slot arrives exactly one TDMA frame (60ms) later.
     * Re-arm the window there instead of going idle. control goes back
     * to NONE so that, unless the +/-1-sample refine around the new
     * syncPtr finds a real sync (voice burst A, terminator, etc.), the
     * burst is read out as a sync-less voice B-F / data continuation by
     * the "else" branch above. Lost-sync handling (syncCount >=
     * DMR_MAX_SYNC_LOST_FRAMES) drops state to NONE, which ends coasting
     * and frees this context for the cold scan. */
    if (c->state != DMR_RXS_NONE)
    {
        // One TDMA frame on, plus the measured drift (see driftQ8).
        if (c->control != DMR_RX_CONTROL_NONE)
            c->sinceSync = 0U;
        c->driftAccQ8 += c->driftQ8;
        int32_t extra  = (c->driftAccQ8 >= 0) ? ((c->driftAccQ8 + 128) / 256) : -((-c->driftAccQ8 + 128) / 256);
        c->driftAccQ8 -= extra * 256;
        uint32_t step  = (uint32_t)((int32_t)DMR_RX_BURST_REPEAT_SAMPLES + extra);

        c->control   = DMR_RX_CONTROL_NONE;
        c->syncPtr  += step;
        c->startPtr += step;
        c->endPtr    = burstEndPtr + step;
        c->expectPtr = c->syncPtr;
        if (c->sinceSync < 255U)
            c->sinceSync++;
    }
}

// Diagnostic only (gated on debugM): raw incoming-sample level stats,
// reported once per DMR_RX_DIAG_PERIOD_SAMPLES_LOCAL. If this never
// shows meaningful swing (minVal/maxVal close together, or stuck near
// one extreme), the samples reaching dmrRxSamples() aren't a real
// demodulated 4FSK signal and the correlator has nothing to lock onto
// no matter how its thresholds are tuned.
static q15_t  dmrRxDiagMin = 32767;
static q15_t  dmrRxDiagMax = -32768;
static int64_t dmrRxDiagSumAbs = 0;
static uint32_t dmrRxDiagSampleWindowBase = 0U;

/* A real DMR burst can be far shorter than the 1-second window above --
 * a single 30ms-60ms burst averaged in with 940ms+ of surrounding
 * silence/noise would barely move a 1-second average at all. This
 * short-window "envelope" detector catches that case: it tracks a slow
 * moving baseline of the ambient noise level and flags (and only
 * flags, to avoid flooding the log) any short window that rises well
 * above that baseline -- i.e. "something happened here briefly",
 * whether or not the correlator ever locked onto it. */
static const uint32_t DMR_RX_DIAG_SHORT_WINDOW_SAMPLES = 240U;  // 10ms @ 24kHz
static q15_t   dmrRxDiagShortMin        = 32767;
static q15_t   dmrRxDiagShortMax        = -32768;
static int64_t dmrRxDiagShortSumAbs     = 0;
static uint32_t dmrRxDiagShortWindowBase = 0U;
static int32_t  dmrRxDiagBaselineAvgAbs  = -1;  // -1 = not yet seeded

/*
 * dmrRxProcessSample()
 *
 * Processes one receive sample.  There is one shared front end, then each
 * context's refine and burst handling, plus a free cold scan whenever a
 * context is idle.  It works like CDMRSlotRX::processSample() in MMDVM, but
 * runs freely: nothing rewinds dataPtr once per cycle.  In debug mode it
 * also gathers signal level statistics.
 *
 * Parameters:
 *   sample - The demodulated sample.
 *
 * Returns: true if the sample was handled.
 */
static bool dmrRxProcessSample(q15_t sample)
{
    DMR_RX_STATE* s = &dmrRx;

    if (debugM)
    {
        int64_t absSample = (sample < 0) ? -(int64_t)sample : (int64_t)sample;

        if (sample < dmrRxDiagMin) dmrRxDiagMin = sample;
        if (sample > dmrRxDiagMax) dmrRxDiagMax = sample;
        dmrRxDiagSumAbs += absSample;

        if (s->dataPtr - dmrRxDiagSampleWindowBase >= DMR_RX_DIAG_PERIOD_SAMPLES_LOCAL)
        {
            uint32_t n = s->dataPtr - dmrRxDiagSampleWindowBase;
            fprintf(stderr, "DMR RX diag: raw samples min=%d max=%d avgAbs=%lld (n=%u)\n",
                    (int)dmrRxDiagMin, (int)dmrRxDiagMax, (long long)(dmrRxDiagSumAbs / (n ? n : 1U)), n);
            dmrRxDiagMin = 32767;
            dmrRxDiagMax = -32768;
            dmrRxDiagSumAbs = 0;
            dmrRxDiagSampleWindowBase = s->dataPtr;
        }

        if (sample < dmrRxDiagShortMin) dmrRxDiagShortMin = sample;
        if (sample > dmrRxDiagShortMax) dmrRxDiagShortMax = sample;
        dmrRxDiagShortSumAbs += absSample;

        if (s->dataPtr - dmrRxDiagShortWindowBase >= DMR_RX_DIAG_SHORT_WINDOW_SAMPLES)
        {
            uint32_t n = s->dataPtr - dmrRxDiagShortWindowBase;
            int32_t shortAvgAbs = (int32_t)(dmrRxDiagShortSumAbs / (n ? n : 1U));

            if (dmrRxDiagBaselineAvgAbs < 0)
                dmrRxDiagBaselineAvgAbs = shortAvgAbs;  // seed on the very first window
            else
                dmrRxDiagBaselineAvgAbs = (dmrRxDiagBaselineAvgAbs * 15 + shortAvgAbs) / 16;  // slow EMA, ~160ms time constant

            if (dmrRxDiagBaselineAvgAbs > 50 && shortAvgAbs > (dmrRxDiagBaselineAvgAbs * 3) / 2)
                fprintf(stderr, "DMR RX diag: envelope bump at dataPtr=%u: short avgAbs=%d (baseline~%d) min=%d max=%d\n",
                        s->dataPtr, shortAvgAbs, dmrRxDiagBaselineAvgAbs, (int)dmrRxDiagShortMin, (int)dmrRxDiagShortMax);

            dmrRxDiagShortMin        = 32767;
            dmrRxDiagShortMax        = -32768;
            dmrRxDiagShortSumAbs     = 0;
            dmrRxDiagShortWindowBase = s->dataPtr;
        }
    }

    DMR_RX_BUF(s, s->dataPtr) = sample;

    s->bitBuffer[s->bitPtr % DMR_RADIO_SYMBOL_LENGTH] <<= 1;
    if (sample < 0)
        s->bitBuffer[s->bitPtr % DMR_RADIO_SYMBOL_LENGTH] |= 0x01U;

    // Refine: right after a cold lock, re-check within +/-1 sample of it
    // (same as MMDVM). While tracking a sequence, search the wider window
    // around where the next sync is expected (DMR_RX_TRACK_WINDOW_SAMPLES).
    for (uint8_t i = 0U; i < 2U; i++)
    {
        DMR_RX_CTX* c = &dmrRxCtx[i];
        if (c->endPtr == DMR_RX_NOENDPTR)
            continue;

        uint32_t centre = (c->state == DMR_RXS_NONE) ? c->syncPtr : c->expectPtr;
        int32_t  window = (c->state == DMR_RXS_NONE) ? 1 : DMR_RX_TRACK_WINDOW_SAMPLES;
        int32_t  d      = (int32_t)(s->dataPtr - centre);
        if (d >= -window && d <= window)
            dmrRxCorrelateSync(c, false);
    }

    // Cold scan: free-running over every sample whenever a context is
    // idle, but never at positions belonging to a stream already tracked
    // (those are the refine's job, and locking them twice would deliver
    // the same burst on both slots).
    DMR_RX_CTX* idle = NULL;
    for (uint8_t i = 0U; i < 2U; i++)
    {
        if (dmrRxCtx[i].endPtr == DMR_RX_NOENDPTR)
        {
            idle = &dmrRxCtx[i];
            break;
        }
    }
    if (idle != NULL && !dmrRxNearTrackedStream(s->dataPtr))
        dmrRxCorrelateSync(idle, true);

    for (uint8_t i = 0U; i < 2U; i++)
    {
        DMR_RX_CTX* c = &dmrRxCtx[i];
        if (c->endPtr != DMR_RX_NOENDPTR && s->dataPtr == c->endPtr)
            dmrRxCtxBurstEnd(c);
    }

    s->dataPtr++;

    s->bitPtr++;
    if (s->bitPtr >= DMR_RADIO_SYMBOL_LENGTH)
        s->bitPtr = 0U;

    return dmrRxCtxActive(&dmrRxCtx[0U]) || dmrRxCtxActive(&dmrRxCtx[1U]);
}

/*
 * dmrRxSamples()
 *
 * Entry point for SAMPLE mode receive.  Feeds demodulated samples through
 * dmrRxProcessSample() and runs the slot watchdog about once every 240
 * samples.
 *
 * Parameters:
 *   samples - Demodulated samples.
 *   length  - Number of samples.
 *
 * Returns: nothing.
 */
void dmrRxSamples(const q15_t* samples, uint16_t length)
{
    for (uint16_t i = 0U; i < length; i++)
        dmrRxProcessSample(samples[i]);

    // Roughly every 10ms of samples.
    if ((dmrRx.dataPtr % 240U) < length)
        dmrSlotWatchdog();
}


/* --- DMR TX scheduler: the real-time TDMA clock ---
 * This is the piece that was missing before: a free-running thread that
 * owns the actual on-air cadence, generating CACH bursts and idle-filler
 * data bursts so the transmitted signal has the continuous TDMA structure
 * a real radio needs to lock onto, and that also arms the dual-slot RX
 * correlators above at the right moments (see CDMRTX::process() /
 * CDMRTX::createCACH() in MMDVM, which this follows closely). Per the
 * user's answers: this is a true 2-slot repeater (both slots need
 * independent idle-filled bursts, not just whichever one has traffic),
 * and there is no external timing tick from modem_host, so this thread
 * is the only clock there is -- it has to pace itself in real time. */

// This codebase has no upstream source for short-LC content (no
// writeShortLC()-equivalent exists anywhere here), so the CACH's short-LC
// field is always the all-zero "empty" pattern -- a deliberate,
// documented simplification; CACH's AT/TC/LS0/LS1/Hamming bits (the part
// a radio actually needs for slot timing/activity) are still generated
// correctly below.
static const uint8_t DMR_EMPTY_SHORT_LC[12U] = {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                                                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
static const uint16_t DMR_CACH_LENGTH_BYTES_LOCAL = 3U;

static const uint32_t DMR_TX_STARTUP_COUNT = 20U;  // CDMRTX::STARTUP_COUNT
static const uint32_t DMR_TX_ABORT_COUNT   = 6U;   // CDMRTX::ABORT_COUNT

static uint8_t  dmrShortLC[12U];
static uint8_t  dmrCachPtr        = 0U;
static uint32_t dmrTxFrameCount   = 0U;
static uint32_t dmrTxAbortCount[2U] = {0U, 0U};

/* True while the downlink is actually being written to the modem. Only
 * dmrTxThread() changes it: it goes true at the first cycle start
 * (PRECACH1) after txOn goes true, and false as soon as txOn drops.
 *
 * Why not just gate on txOn: the modem starts transmitting once enough
 * samples have arrived, and how long that takes depends on which part of
 * the cycle we start writing in (a slot burst arrives as one ~1320-byte
 * block, a CACH as 120 bytes). Starting wherever the scheduler happened to
 * be when txOn went true made the on-air timing -- and so the RX slot
 * phase -- shift by up to ~20ms from one key-up to the next, while
 * staying rock-stable within a key-up. Always starting at the same point
 * in the cycle makes every key-up line up the same way. */
static bool dmrTxStreaming = false;

/*
 * dmrSendCACH()
 *
 * Creates and sends the CACH, as CDMRTX::createCACH() does in MMDVM.  The
 * CACH is only sent to the modem while keyed.
 *
 * Parameters:
 *   txSlotIndex - The slot that is sent after this CACH.
 *   rxSlotIndex - The slot whose correlator the last byte of this CACH arms,
 *                 as in MMDVM's own m_markBuffer[2] assignment.
 *
 * Returns: nothing.
 */
static void dmrSendCACH(uint8_t txSlotIndex, uint8_t rxSlotIndex)
{
    dmrTxFrameCount++;
    dmrTxAbortCount[0U]++;
    dmrTxAbortCount[1U]++;

    if (dmrCachPtr >= 12U)
        dmrCachPtr = 0U;

    if (dmrCachPtr == 0U)
        memcpy(dmrShortLC, DMR_EMPTY_SHORT_LC, 12U);

    uint8_t cach[3U];
    memcpy(cach, dmrShortLC + dmrCachPtr, 3U);

    bool at = false;
    if (dmrTxFrameCount >= DMR_TX_STARTUP_COUNT)
    {
        pthread_mutex_lock(&dmrTxFifoMutex[rxSlotIndex]);
        at = RingBuffer_dataSize(&dmrTxFifo[rxSlotIndex]) > 0;
        pthread_mutex_unlock(&dmrTxFifoMutex[rxSlotIndex]);
    }
    bool tc  = (txSlotIndex == 1U);
    bool ls0 = true;
    bool ls1 = true;
    if (dmrCachPtr == 0U)
        ls1 = false;
    else if (dmrCachPtr == 9U)
        ls0 = false;

    bool h0 = at ^ tc ^ ls1;
    bool h1 =      tc ^ ls1 ^ ls0;
    bool h2 = at ^ tc       ^ ls0;

    cach[0U] |= at  ? 0x80U : 0x00U;
    cach[0U] |= tc  ? 0x08U : 0x00U;
    cach[1U] |= ls1 ? 0x80U : 0x00U;
    cach[1U] |= ls0 ? 0x08U : 0x00U;
    cach[1U] |= h0  ? 0x02U : 0x00U;
    cach[2U] |= h1  ? 0x20U : 0x00U;
    cach[2U] |= h2  ? 0x02U : 0x00U;

    dmrCachPtr += 3U;

    // Only actually modulate/send when the repeater is keyed. The CACH
    // bookkeeping above (cachPtr, frameCount, abortCount) still has to
    // happen every cycle regardless -- see the dmrTxThread() comment on
    // why the scheduler can't gate on txOn.
    //
    // RX used to be "armed" from right here, once per half-cycle, the
    // same way real MMDVM hardware shares one ISR between its TX symbol
    // clock and its RX correlator window. That dependency is gone now:
    // dmrRxSamples()/dmrRxProcessSample() time themselves purely from
    // the real incoming RX sample stream and no longer need anything
    // from this scheduler (see the file comment above dmrRxProcessSample()
    // for why that External-clock design didn't survive contact with
    // real hardware).
    /* CACH is part of the same continuous TDMA stream as the slot bursts
     * that frame it -- isEOT=false, exactly like dmrSendSlotBurst() just
     * below. The old isEOT=true here flushed a short partial 'EOT' packet
     * twice per 60ms cycle (every CACH), which combined with the slot's
     * own EOT gave the modem ~4 "end of transmission" markers per cycle
     * and prevented it from ever actually entering DMR TX mode. */
    if (dmrTxStreaming)
        processTx(cach, DMR_CACH_LENGTH_BYTES_LOCAL, false);
}

/*
 * dmrSendSlotBurst()
 *
 * Sends the burst of one slot, as CDMRTX::createData() does in MMDVM.  It
 * takes a real frame from the slot's FIFO when there is one (and the start-
 * up and abort hysteresis allows it); otherwise the idle frame is sent, so
 * that the slot keeps its TDMA structure.  The burst is only sent to the
 * modem while keyed.
 *
 * Parameters:
 *   slotIndex - Time slot index, 0 or 1.
 *
 * Returns: nothing.
 */
static void dmrSendSlotBurst(uint8_t slotIndex)
{
    uint8_t frame[33U];  // DMR_FRAME_LENGTH_BYTES
    bool haveFrame = false;

    pthread_mutex_lock(&dmrTxFifoMutex[slotIndex]);
    if (RingBuffer_dataSize(&dmrTxFifo[slotIndex]) >= (int)DMR_FRAME_LENGTH_BYTES &&
        dmrTxFrameCount >= DMR_TX_STARTUP_COUNT && dmrTxAbortCount[slotIndex] >= DMR_TX_ABORT_COUNT)
    {
        RingBuffer_getData(&dmrTxFifo[slotIndex], frame, DMR_FRAME_LENGTH_BYTES);
        haveFrame = true;
    }
    pthread_mutex_unlock(&dmrTxFifoMutex[slotIndex]);

    if (!haveFrame)
        memcpy(frame, dmrIdleFrame, DMR_FRAME_LENGTH_BYTES);

    if (dmrTxStreaming)
    {
        if (debugM)
            fprintf(stderr, "DMR TX slot%u: sending %s frame\n", slotIndex + 1U, haveFrame ? "RELAYED" : "idle");
        /* isEOT=false: this burst is one chunk of a continuous keyed TDMA
         * stream, not the end of a transmission. The one-shot EOT flush
         * when txOn drops is handled by dmrTxThread()'s edge detector. */
        processTx(frame, DMR_FRAME_LENGTH_BYTES, false);
    }
}

/* --- Simplex (DMO) downlink ---
 * A simplex hotspot does not transmit the repeater's CACH + two slot
 * structure.  MMDVM's CDMRDMOTX (the firmware's simplex transmitter) sends one
 * 33 byte burst followed by 39 bytes of pseudo-random fill, 72 bytes or 60 ms
 * in all, and a subscriber radio simply locks to the MS sourced sync in each
 * burst.  A capture of a radio's own simplex transmission shows the same
 * 60 ms cadence: MS sync, a voice superframe of six bursts, 360 ms between
 * voice syncs.  Before the first burst the firmware sends a run of 0x5F bytes
 * (a plain two-symbol tone) so the receiving radio's squelch and AFC settle.
 *
 * Only used when modem_duplex is false.  Everything for the radio comes from
 * the slot 1 FIFO: whichever slot the gateway names, a simplex hotspot has
 * just the one (decodeDMR() queues slot 2 traffic there too). */
static const uint8_t DMR_DMO_PR_FILL[39U] = {
    0x63U, 0xEAU, 0x00U, 0x76U, 0x6CU, 0x76U, 0xC4U, 0x52U, 0xC8U, 0x78U,
    0x09U, 0x2DU, 0xB8U, 0x79U, 0x27U, 0x57U, 0x9BU, 0x31U, 0xBCU, 0x3EU,
    0xEAU, 0x45U, 0xC3U, 0x30U, 0x49U, 0x17U, 0x93U, 0xAEU, 0x8BU, 0x6DU,
    0xA4U, 0xA5U, 0xADU, 0xA2U, 0xF1U, 0x35U, 0xB5U, 0x3CU, 0x1EU};

static const uint8_t DMR_DMO_PREAMBLE_BYTE = 0x5FU;     // CDMRDMOTX DMR_SYNC
static const long    DMR_DMO_PERIOD_US     = 60000L;    // one burst + fill

/* [DMR] DmoPreamble, in ms. The frames the gateway sends meanwhile wait in
 * the FIFO (12 frames, 720 ms), so keep it well below that. */
static uint32_t dmrDmoPreambleMs = 360U;

/*
 * dmrSendDmoPreamble()
 *
 * Sends the preamble of a simplex (direct mode) transmission: bytes of
 * DMR_DMO_PREAMBLE_BYTE for dmrDmoPreambleMs milliseconds.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void dmrSendDmoPreamble(void)
{
    uint8_t chunk[32U];

    /* One byte is 4 symbols at 4800 baud, 5/6 ms. */
    uint32_t bytes = (dmrDmoPreambleMs * 6U) / 5U;

    memset(chunk, DMR_DMO_PREAMBLE_BYTE, sizeof chunk);
    while (bytes > 0U)
    {
        uint32_t n = (bytes > sizeof chunk) ? (uint32_t)sizeof chunk : bytes;
        processTx(chunk, (uint8_t)n, false);
        bytes -= n;
    }
    if (debugM)
        fprintf(stderr, "DMR TX (simplex): preamble %u ms sent\n", (unsigned)dmrDmoPreambleMs);
}

/*
 * dmrSendDmoPeriod()
 *
 * Sends one 60 ms period of a simplex transmission: a queued frame (or the
 * idle frame when there is none, so that the carrier and burst structure
 * carry on between frames) plus the fill bytes.  In simplex everything comes
 * from the slot 1 FIFO.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void dmrSendDmoPeriod(void)
{
    uint8_t frame[33U];  // DMR_FRAME_LENGTH_BYTES
    bool haveFrame = false;

    /* Keep the duplex scheduler's start-up bookkeeping advancing at its usual
     * rate (two CACHs per 60 ms), so switching to duplex later behaves as if
     * this thread had been running in duplex all along. */
    dmrTxFrameCount += 2U;
    dmrTxAbortCount[0U] += 2U;
    dmrTxAbortCount[1U] += 2U;

    if (!dmrTxStreaming)
        return;

    pthread_mutex_lock(&dmrTxFifoMutex[0U]);
    if (RingBuffer_dataSize(&dmrTxFifo[0U]) >= (int)DMR_FRAME_LENGTH_BYTES)
    {
        RingBuffer_getData(&dmrTxFifo[0U], frame, DMR_FRAME_LENGTH_BYTES);
        haveFrame = true;
    }
    pthread_mutex_unlock(&dmrTxFifoMutex[0U]);

    if (!haveFrame)
    {
        memcpy(frame, dmrIdleFrame, DMR_FRAME_LENGTH_BYTES);
        addDMRDataSync(frame, false);  // MS sourced, like everything else sent in simplex
    }

    if (debugM)
        fprintf(stderr, "DMR TX (simplex): sending %s frame\n", haveFrame ? "RELAYED" : "idle");

    processTx(frame, DMR_FRAME_LENGTH_BYTES, false);
    processTx(DMR_DMO_PR_FILL, sizeof DMR_DMO_PR_FILL, false);
}

/*
 * dmrTxThread()
 *
 * The TDMA scheduler: CACH(0,1), slot 1, CACH(1,0), slot 2, and repeat, like
 * the state machine of CDMRTX::process() in MMDVM.  It is paced by an
 * absolute-deadline clock so that the 30 ms slot and 60 ms cycle do not
 * drift.  The loop runs all the time, whether or not the transmitter is
 * keyed: dmrSendCACH() and dmrSendSlotBurst() decide what is sent to the
 * modem from txOn, so the on-air TDMA structure stays continuous while
 * nothing is relayed.  It also sends the end-of-transmission flush when txOn
 * goes off, and ends the TX hang.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL.
 */
void* dmrTxThread(void* arg)
{
    (void)arg;

    enum { DMR_TXSCHED_PRECACH1, DMR_TXSCHED_SLOT1, DMR_TXSCHED_PRECACH2, DMR_TXSCHED_SLOT2 } state = DMR_TXSCHED_PRECACH1;

    const long DMR_CACH_PERIOD_US = 2500L;   // 12 symbols (3 bytes) at 4800 sym/s
    const long DMR_SLOT_PERIOD_US = 27500L;  // 132 symbols (33 bytes) at 4800 sym/s

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    uint32_t debugCycleCount = 0U;

    while (connected)
    {
        if (debugM && state == DMR_TXSCHED_PRECACH1)
        {
            // Once per 60ms cycle -- confirms the scheduler thread itself
            // is still alive and ticking, independent of whether RX is
            // finding anything.
            debugCycleCount++;
            if ((debugCycleCount % 17U) == 0U)  // roughly once a second
                fprintf(stderr, "DMR TX scheduler: cycle %u, txOn=%d\n", debugCycleCount, (int)txOn);
        }

        /* TX hang expired: nothing has made a slot active since the hang
         * started (see dmrTxHangNs) -- un-key. */
        if (txOn && dmrTxHangNs != 0 && !slotActive[0] && !slotActive[1] &&
            dmrNowNs() - dmrTxHangNs > dmrTxHangDurNs)
        {
            uint8_t buf[2];
            buf[0] = TYPE_DMR_START;
            buf[1] = 0;
            txOn        = false;
            dmrTxHangNs = 0;
            sendDataToModem(buf, 2, 1, false);
            fprintf(stderr, "DMR TX: hang time over, un-keying\n");
        }

        /* Downlink stop: the first time txOn drops back to false while
         * streaming (both slots done), send one SAMP/BITS packet carrying
         * the EOT header to the modem so it stops keying and resets.
         * During a transmission every burst goes to the modem with
         * isEOT=false (see dmrSendSlotBurst() above) -- mirrors
         * P25/M17/D-STAR, where the EOT flush happens exactly once per
         * transmission. */
        if (dmrTxStreaming && !txOn)
        {
            dmrTxStreaming = false;
            pthread_mutex_lock(&dmrTxSlotTimeMutex);
            dmrTxRefValid = false;
            pthread_mutex_unlock(&dmrTxSlotTimeMutex);
            if (packetType == PACKET_TYPE_SAMP)
                writeSamples(0x00U, true);
            else if (packetType == PACKET_TYPE_BIT)
                modemWriteBits(sockfd, 0x00U, true);
            if (debugM)
                fprintf(stderr, "DMR TX: txOn dropped, flushed EOT to modem\n");
        }

        /* Downlink start: only ever at the top of a TDMA cycle, so every
         * key-up feeds the modem the same sequence (see dmrTxStreaming). */
        if (txOn && !dmrTxStreaming && state == DMR_TXSCHED_PRECACH1)
        {
            // New downlink: TX sample 0 is the CACH about to be sent, and
            // any earlier timing report belonged to the previous one.
            pthread_mutex_lock(&dmrTxSlotTimeMutex);
            dmrTxRefValid       = false;
            dmrTxSamplesWritten = 0U;
            pthread_mutex_unlock(&dmrTxSlotTimeMutex);
            dmrTxStreaming = true;
            if (debugM)
                fprintf(stderr, "DMR TX: downlink start at cycle boundary\n");

            if (!modem_duplex)
            {
                /* Simplex: preamble first, then the 60 ms cadence starts from
                 * the moment it is out (it took real time to write). */
                dmrSendDmoPreamble();
                clock_gettime(CLOCK_MONOTONIC, &next);
            }
        }

        long periodUs;
        if (!modem_duplex)
        {
            /* Simplex: no CACH, one burst per 60 ms. The duplex state
             * machine below is left alone and picks up from PRECACH1 if the
             * modem later says duplex. */
            state = DMR_TXSCHED_PRECACH1;
            dmrSendDmoPeriod();
            periodUs = DMR_DMO_PERIOD_US;
        }
        else
        switch (state)
        {
            case DMR_TXSCHED_PRECACH1:
                dmrSendCACH(0U, 1U);
                state = DMR_TXSCHED_SLOT1;
                periodUs = DMR_CACH_PERIOD_US;
                break;
            case DMR_TXSCHED_SLOT1:
            {
                // "next" is this state's scheduled start time -- record it
                // as the slot-1 reference for dmrRxLogSlotPhase().
                int64_t t = (int64_t)next.tv_sec * 1000000000LL + (int64_t)next.tv_nsec;
                pthread_mutex_lock(&dmrTxSlotTimeMutex);
                dmrTxSlot1StartNs = t;
                pthread_mutex_unlock(&dmrTxSlotTimeMutex);
            }
                dmrSendSlotBurst(0U);
                state = DMR_TXSCHED_PRECACH2;
                periodUs = DMR_SLOT_PERIOD_US;
                break;
            case DMR_TXSCHED_PRECACH2:
                dmrSendCACH(1U, 0U);
                state = DMR_TXSCHED_SLOT2;
                periodUs = DMR_CACH_PERIOD_US;
                break;
            case DMR_TXSCHED_SLOT2:
            default:
                dmrSendSlotBurst(1U);
                state = DMR_TXSCHED_PRECACH1;
                periodUs = DMR_SLOT_PERIOD_US;
                break;
        }

        next.tv_nsec += periodUs * 1000L;
        while (next.tv_nsec >= 1000000000L)
        {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long diffNs = (long)(next.tv_sec - now.tv_sec) * 1000000000L + (next.tv_nsec - now.tv_nsec);
        if (diffNs > 0)
        {
            struct timespec req = {diffNs / 1000000000L, diffNs % 1000000000L};
            nanosleep(&req, NULL);
        }
    }

    return NULL;
}

/* --- BIT-mode RX ---
 * In BIT mode the modem hardware does its own 4FSK symbol slicing and
 * just streams the resulting hard bit decisions; there is no amplitude
 * information, so burst timing is pure bit counting and sync is pure
 * bit-pattern correlation against the 48-bit MS-sourced sync words
 * (DMR_MS_DATA_SYNC_BITS / DMR_MS_VOICE_SYNC_BITS / DMR_SYNC_BITS_MASK).
 *
 * What the modem sends (openmt_hs firmware, DMR mode): it watches for the
 * MS data sync or its paired MS voice sync and, on a match, starts the
 * stream with the 192 bits up to and including that sync, then every
 * following bit. It keeps streaming while syncs keep arriving (a voice
 * superframe has one every 360 ms) and stops 1.5 s after the last one. The
 * modem's clock is recovered from the signal, so the stream is bit-exact:
 * a TDMA slot repeats every 576 bits, 60 ms.
 *
 * A DMR burst is 264 bits with its 48-bit sync in the middle (bits 108 to
 * 155), so the burst is only complete 108 bits after the sync match, and
 * its first 108 bits come from before it. This code therefore keeps the
 * last 1024 bits in a ring and, when a burst is due, cuts the 264 bits out
 * of it:
 *
 *   - a sync match at stream position p schedules a burst that ends at
 *     p + 108 (start = end - 264);
 *   - once a call is being followed (state VOICE or DATA), the next burst
 *     of the same slot is expected exactly 576 bits after the previous
 *     one. That covers voice bursts B-F and data blocks that carry no
 *     usable sync: at the expected position the burst is cut out whether
 *     or not a sync was seen there. Like the SAMPLE-mode path, DMR_MAX_
 *     SYNC_LOST_FRAMES such bursts in a row without a sync end the call;
 *   - sync matches that do not line up with the followed slot (the other
 *     slot's bursts, noise) are ignored while a call is being followed.
 *
 * The per-burst decisions (what to hand to decodeDMR() and with which
 * control byte) are the same as the per-context ones in the SAMPLE-mode RX
 * above, so decodeDMR() sees the same thing from either front end.
 *
 * Still single-slot: with no amplitude information there is nothing to
 * tell TS1 from TS2 against dmrTxThread()'s slot timing, so everything is
 * reported as slot 1 (slot argument 0).
 */
#define DMR_BIT_RING_BITS        1024U   // power of two; must hold one 264-bit burst
#define DMR_BIT_BURST_REPEAT     576U    // 60 ms at 9600 bit/s
#define DMR_BIT_SYNC_TO_END      108U    // bits after the sync until the burst is complete
#define DMR_BIT_SYNC_FROM_START  156U    // 108 + 48: burst start to the end of its sync
#define DMR_BIT_COAST_SLACK      16      // bits either side of the expected burst end
#define DMR_BIT_STREAM_GAP_NS    400000000LL  // longer than this between BITS packets: a new stream

static uint8_t  dmrBitRing[DMR_BIT_RING_BITS / 8U];
static uint64_t dmrBitWindow     = 0U;     // last 64 bits, newest in bit 0
static uint64_t dmrBitTotal      = 0U;     // bits received so far
static uint64_t dmrBitSinceStart = 0U;     // bits received since the stream (re)started
static int64_t  dmrBitLastNs     = 0;

/* Two independent call trackers, one per time slot. Their bursts are 288 bits
 * apart in the bit stream, so they never overlap: one pending burst at a time
 * is enough, and each slot has its own expected-next-burst position. */
typedef struct
{
    bool     coast;      // the next burst of this slot is expected at coastEnd
    uint64_t coastEnd;
    uint8_t  state;      // DMR_RXS_*
    uint8_t  n;          // voice burst counter, B-F = 1..5
    uint8_t  syncCount;  // bursts in a row without a sync
    uint8_t  dataType;   // data type of the data call being followed
} DmrBitCtx;

static DmrBitCtx dmrBitCtx[2];

static bool     dmrBitPending    = false;  // a burst found by sync is waiting for its last bits
static uint64_t dmrBitPendEnd    = 0U;     // stream position (exclusive) where it completes
static bool     dmrBitPendVoice  = false;
static uint8_t  dmrBitPendSlot   = 0U;     // the slot it belongs to

static int32_t  dmrBitPhaseNow   = -1;     // TX stream position (0..575) at the bit being processed, -1 unknown
static int32_t  dmrBitPendPhase  = -1;     // the same, at the end of the sync that started the pending burst
static int32_t  dmrSlotPhase     = -1;     // [DMR] slotPhase: TX phase at which a TS1 uplink sync ends, -1 unset

/*
 * dmrBitSlotFromPhase()
 *
 * Tells which time slot an uplink burst is in, in BIT mode.  The radio times
 * its uplink to the downlink the modem sends, so the burst's position in the
 * downlink's 60 ms two-slot cycle gives the slot.  The modem reports that
 * position with each BITS packet.  dmrSlotPhase is the position where a TS1
 * sync typically ends (measured once for each setup); TS2 is half a cycle
 * (288 bits) later.
 *
 * Parameters:
 *   phase - TX stream position (0 to 575) at the end of the sync.
 *
 * Returns: 0 for slot 1, 1 for slot 2; 0 if the phase or dmrSlotPhase is not
 *   known.
 */
static uint8_t dmrBitSlotFromPhase(int32_t phase)
{
    if (phase < 0 || dmrSlotPhase < 0)
        return 0U;

    int32_t d = (phase - dmrSlotPhase + 144 + 576) % 576;
    return (d < 288) ? 0U : 1U;
}

/*
 * dmrBitAnyCall()
 *
 * Tells whether a call is being received in BIT mode on either slot.
 *
 * Parameters: none.
 *
 * Returns: true if either slot's context is not idle.
 */
static bool dmrBitAnyCall(void)
{
    return dmrBitCtx[0].state != DMR_RXS_NONE || dmrBitCtx[1].state != DMR_RXS_NONE;
}

/*
 * dmrBitReset()
 *
 * Clears the BIT mode receive state: any pending burst and both slot
 * contexts.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void dmrBitReset(void)
{
    dmrBitPending = false;
    memset(dmrBitCtx, 0, sizeof(dmrBitCtx));
}

/*
 * dmrBitCutBurst()
 *
 * Copies the 264 bits that end at a stream position (exclusive) out of the
 * BIT mode bit ring.
 *
 * Parameters:
 *   end   - Stream position just after the burst's last bit.
 *   frame - Receives the 33 byte burst.
 *
 * Returns: nothing.
 */
static void dmrBitCutBurst(uint64_t end, uint8_t* frame)
{
    uint64_t start = end - DMR_FRAME_LENGTH_BITS;

    memset(frame, 0x00U, DMR_FRAME_LENGTH_BYTES);
    for (uint16_t i = 0U; i < DMR_FRAME_LENGTH_BITS; i++)
    {
        uint32_t pos = (uint32_t)((start + i) & (DMR_BIT_RING_BITS - 1U));
        bool b = (dmrBitRing[pos >> 3] & DMR_RX_BIT_MASK_TABLE[pos & 7U]) != 0U;
        if (b)
            frame[i >> 3] |= DMR_RX_BIT_MASK_TABLE[i & 7U];
    }
}

/*
 * dmrBitBurst()
 *
 * Handles one complete burst of a slot in BIT mode.  It mirrors the SAMPLE
 * mode burst handling (dmrRxCtxBurstEnd()) and delivers the burst through
 * dmrRxDeliverFrame().
 *
 * Parameters:
 *   frame   - The 33 byte burst.
 *   control - DMR_RX_CONTROL_DATA or DMR_RX_CONTROL_VOICE if a sync was
 *             found at the centre of the burst; DMR_RX_CONTROL_NONE if it
 *             was cut out at its expected position without one.
 *   end     - Stream position just after the burst.
 *   slot    - Time slot index, 0 or 1.
 *
 * Returns: nothing.
 */
static void dmrBitBurst(uint8_t* frame, uint8_t control, uint64_t end, uint8_t slot)
{
    DmrBitCtx* c = &dmrBitCtx[slot];
    uint8_t ctl = control;

    if (modem_duplex && !txOn && !dmrBitAnyCall())
    {
        /* Not keyed and no call in progress: like MMDVM's DMRIdleRX, the
         * only interesting thing is a radio's wake-up CSBK (BS_Dwn_Act),
         * passed on flagged CONTROL_IDLE so decodeDMR() keys up without
         * marking a slot active. Everything else is ignored. */
        if (debugM)
        {
            if (control == DMR_RX_CONTROL_DATA)
            {
                uint8_t cc, dt;
                DMRSlotType_decode(frame, &cc, &dt);
                bool ok = (dt == DT_CSBK) && CSBK_decode(frame, 0U);
                fprintf(stderr, "DMR RX bits: idle, data sync burst cc=%u (want %u) type=%u%s csbkOK=%d csbko=%u\n",
                        cc, dmrColorCode, dt, dt == DT_CSBK ? " (CSBK)" : "", ok ? 1 : 0, ok ? (unsigned)CSBK_getCSBKO(0U) : 0U);
            }
            else
            {
                fprintf(stderr, "DMR RX bits: idle, voice sync burst (ignored, no wake-up seen)\n");
            }
        }

        if (control == DMR_RX_CONTROL_DATA)
        {
            uint8_t colorCode, dataType;
            DMRSlotType_decode(frame, &colorCode, &dataType);
            if (colorCode == dmrColorCode && dataType == DT_CSBK)
            {
                if (debugM)
                    fprintf(stderr, "DMR RX bits: wake-up CSBK while idle\n");
                dmrRxDeliverFrame(0U, frame, DMR_RX_CONTROL_IDLE | DMR_RX_CONTROL_DATA | DT_CSBK, false);
            }
        }
        c->coast = false;
        return;
    }

    if (c->state == DMR_RXS_NONE && control != DMR_RX_CONTROL_NONE && debugM)
        fprintf(stderr, "DMR RX bits: call starts, sync end at TX phase %d, slot %u\n", (int)dmrBitPendPhase, slot + 1U);

    if (control == DMR_RX_CONTROL_DATA)
    {
        uint8_t colorCode, dataType;
        DMRSlotType_decode(frame, &colorCode, &dataType);

        if (debugM)
            fprintf(stderr, "DMR RX bits: slot %u data sync burst cc=%u (want %u) type=%u state=%u txOn=%d\n",
                    slot + 1U, colorCode, dmrColorCode, dataType, c->state, txOn ? 1 : 0);

        if (colorCode == dmrColorCode)
        {
            c->syncCount = 0U;
            c->n         = 0U;
            ctl          = DMR_RX_CONTROL_DATA | dataType;

            switch (dataType)
            {
                case DT_TERMINATOR_WITH_LC:
                    if (c->state == DMR_RXS_VOICE)
                    {
                        dmrRxDeliverFrame(slot, frame, ctl, false);
                        c->state = DMR_RXS_NONE;
                    }
                    break;
                case DT_VOICE_LC_HEADER:
                    dmrRxDeliverFrame(slot, frame, ctl, false);
                    c->state = DMR_RXS_VOICE;
                    break;
                case DT_VOICE_PI_HEADER:
                    if (c->state == DMR_RXS_VOICE)
                        dmrRxDeliverFrame(slot, frame, ctl, false);
                    c->state = DMR_RXS_VOICE;
                    break;
                case DT_DATA_HEADER:
                    dmrRxDeliverFrame(slot, frame, ctl, false);
                    c->state    = DMR_RXS_DATA;
                    c->dataType = 0x00U;
                    break;
                case DT_RATE_12_DATA:
                case DT_RATE_34_DATA:
                case DT_RATE_1_DATA:
                    if (c->state == DMR_RXS_DATA)
                    {
                        dmrRxDeliverFrame(slot, frame, ctl, false);
                        c->dataType = dataType;
                    }
                    break;
                default:  // DT_CSBK and others
                    if (dataType == DT_CSBK && CSBK_decode(frame, slot) && CSBK_getCSBKO(slot) == CSBKO_BSDWNACT)
                    {
                        if (debugM)
                            fprintf(stderr, "DMR RX bits: repeated wake-up CSBK while keyed, ignored\n");
                    }
                    else
                    {
                        dmrRxDeliverFrame(slot, frame, ctl, false);
                    }
                    c->state = DMR_RXS_NONE;
                    break;
            }
        }
    }
    else if (control == DMR_RX_CONTROL_VOICE)
    {
        if (debugM)
            fprintf(stderr, "DMR RX bits: slot %u voice sync burst\n", slot + 1U);
        dmrRxDeliverFrame(slot, frame, DMR_RX_CONTROL_VOICE, false);
        c->state     = DMR_RXS_VOICE;
        c->syncCount = 0U;
        c->n         = 0U;
    }
    else
    {
        if (c->state != DMR_RXS_NONE)
        {
            c->syncCount++;
            if (c->syncCount >= DMR_MAX_SYNC_LOST_FRAMES)
            {
                if (debugM)
                    fprintf(stderr, "DMR RX bits: slot %u: %u bursts in a row without sync, ending the call\n", slot + 1U, c->syncCount);
                dmrRxDeliverFrame(slot, frame, 0x00U, true);
                c->state = DMR_RXS_NONE;
            }
        }

        if (c->state == DMR_RXS_VOICE)
        {
            if (c->n >= 5U)
            {
                ctl  = DMR_RX_CONTROL_VOICE;
                c->n = 0U;
            }
            else
            {
                ctl = ++c->n;
            }

            dmrRxDeliverFrame(slot, frame, ctl, false);
        }
        else if (c->state == DMR_RXS_DATA)
        {
            if (c->dataType != 0x00U)
                dmrRxDeliverFrame(slot, frame, DMR_RX_CONTROL_DATA | c->dataType, false);
        }
    }

    // Still following a call: the next burst of this slot is 60 ms away
    if (c->state != DMR_RXS_NONE)
    {
        c->coast    = true;
        c->coastEnd = end + DMR_BIT_BURST_REPEAT;
    }
    else
    {
        c->coast = false;
    }
}

/*
 * dmrProcessBit()
 *
 * Processes one received bit in BIT mode.  The bit is stored in the bit ring
 * and the sync detector is advanced.  A sync match means that the burst it
 * belongs to ends 108 bits later, and the slot of the burst is decided (from
 * its position in the TDMA cycle, or from the slot being followed).  When a
 * burst is complete it is cut out and handled with dmrBitBurst(); a burst
 * whose expected position arrives with no sync is handled too, without one.
 *
 * Parameters:
 *   bit - The received bit.
 *
 * Returns: nothing.
 */
static void dmrProcessBit(bool bit)
{
    uint8_t frame[33U];  // DMR_FRAME_LENGTH_BYTES (must be a literal: global array size)

    uint32_t pos = (uint32_t)(dmrBitTotal & (DMR_BIT_RING_BITS - 1U));
    if (bit)
        dmrBitRing[pos >> 3] |= DMR_RX_BIT_MASK_TABLE[pos & 7U];
    else
        dmrBitRing[pos >> 3] &= (uint8_t)~DMR_RX_BIT_MASK_TABLE[pos & 7U];

    dmrBitTotal++;
    dmrBitSinceStart++;
    dmrBitWindow = (dmrBitWindow << 1) | (bit ? 1U : 0U);

    // A sync match this bit: the burst it belongs to ends 108 bits from now
    if (!dmrBitPending && dmrBitSinceStart >= DMR_BIT_SYNC_FROM_START)
    {
        uint64_t window = dmrBitWindow & DMR_SYNC_BITS_MASK;
        bool isData  = dmrCountBits64(window ^ DMR_MS_DATA_SYNC_BITS)  <= DMR_MAX_SYNC_BYTES_ERRS;
        bool isVoice = dmrCountBits64(window ^ DMR_MS_VOICE_SYNC_BITS) <= DMR_MAX_SYNC_BYTES_ERRS;

        if (isData || isVoice)
        {
            uint64_t end = dmrBitTotal + DMR_BIT_SYNC_TO_END;

            /* Which slot is it? A sync that lands where a followed slot's
             * next burst is expected belongs to that slot (this needs no
             * timing information at all). Otherwise it is the start of a
             * call: the modem's TX phase says which slot. A slot that is
             * already being followed rejects syncs outside its window. */
            int8_t  slot   = -1;
            int64_t bestDiff = 0;
            for (uint8_t sl = 0U; sl < 2U; sl++)
            {
                if (!dmrBitCtx[sl].coast)
                    continue;
                int64_t diff = (int64_t)(end - dmrBitCtx[sl].coastEnd);
                if (diff >= -DMR_BIT_COAST_SLACK && diff <= DMR_BIT_COAST_SLACK)
                {
                    slot     = (int8_t)sl;
                    bestDiff = diff;
                }
            }

            bool wanted = true;
            if (slot >= 0)
            {
                if (debugM && bestDiff != 0)
                    fprintf(stderr, "DMR RX bits: slot %d %s sync off the coast position by %lld bits\n",
                            slot + 1, isVoice && !isData ? "voice" : "data", (long long)bestDiff);
                /* No snapping to the expected position: the radio's burst
                 * rate differs a little from the bit count here (about 0.5
                 * bit per voice superframe was measured), and snapping let
                 * the error pile up. Re-align to where the sync really is. */
            }
            else
            {
                slot = (int8_t)dmrBitSlotFromPhase(dmrBitPhaseNow);
                if (dmrBitCtx[slot].coast)
                {
                    wanted = false;
                    if (debugM)
                        fprintf(stderr, "DMR RX bits: slot %d %s sync REJECTED, outside its window\n",
                                slot + 1, isVoice && !isData ? "voice" : "data");
                }
            }

            if (wanted)
            {
                dmrBitPending   = true;
                dmrBitPendPhase = dmrBitPhaseNow;
                dmrBitPendEnd   = end;
                dmrBitPendVoice = isVoice && !isData;
                dmrBitPendSlot  = (uint8_t)slot;
            }
        }
    }

    if (dmrBitPending && dmrBitTotal == dmrBitPendEnd)
    {
        dmrBitPending = false;
        dmrBitCutBurst(dmrBitPendEnd, frame);
        dmrBitBurst(frame, dmrBitPendVoice ? DMR_RX_CONTROL_VOICE : DMR_RX_CONTROL_DATA, dmrBitPendEnd, dmrBitPendSlot);
    }
    else if (!dmrBitPending)
    {
        // An expected burst's position, and no sync was found for it
        for (uint8_t sl = 0U; sl < 2U; sl++)
        {
            if (dmrBitCtx[sl].coast && dmrBitTotal == dmrBitCtx[sl].coastEnd)
            {
                uint64_t end = dmrBitCtx[sl].coastEnd;
                dmrBitCtx[sl].coast = false;
                dmrBitCutBurst(end, frame);
                dmrBitBurst(frame, DMR_RX_CONTROL_NONE, end, sl);
                break;
            }
        }
    }
}

/*
 * dmrRxBits()
 *
 * Entry point for BIT mode receive.  Feeds raw hard-sliced bytes through the
 * bit state machine (8 bits, which is 4 symbols, per byte, most significant
 * bit first), packed as modemWriteBits(sockfd, ) packs them.  A long gap in the stream
 * restarts the receiver and drops the old call state.
 *
 * Parameters:
 *   bytes  - Received bytes.
 *   length - Number of bytes in bytes.
 *
 * Returns: nothing.
 */
void dmrRxBits(const uint8_t* bytes, uint8_t length)
{
    int64_t now = dmrNowNs();

    /* The modem only streams while it hears sync; a gap in the BITS packets
     * means the stream ended, and what arrives next is a new one starting
     * with its own history. Forget the old stream. */
    if (dmrBitLastNs != 0 && now - dmrBitLastNs > DMR_BIT_STREAM_GAP_NS)
    {
        if (dmrBitAnyCall() && debugM)
            fprintf(stderr, "DMR RX bits: stream restarted, dropping the old call state\n");
        dmrBitReset();
        dmrBitSinceStart = 0U;
    }
    dmrBitLastNs = now;

    static int64_t lastPktNs = 0;
    if (debugM && lastPktNs != 0 && dmrBitAnyCall() && now - lastPktNs > 250000000LL)
        fprintf(stderr, "DMR RX bits: %lld ms between BITS packets during a call\n", (long long)((now - lastPktNs) / 1000000LL));
    lastPktNs = now;

    /* BIT mode firmware appends 2 bytes to every BITS packet: the TX stream
     * position (0..575) when the packet's last bit arrived, 0xFFFF while the
     * modem is not transmitting. */
    int32_t tailPhase = -1;
    if (length >= 2U)
    {
        uint16_t ph = (uint16_t)((bytes[length - 2U] << 8) | bytes[length - 1U]);
        tailPhase = (ph == 0xFFFFU) ? -1 : (int32_t)(ph % 576U);
        length   -= 2U;
    }
    int32_t totalBits = (int32_t)length * 8;
    int32_t bitIndex  = 0;

    for (uint8_t i = 0U; i < length; i++)
        for (int8_t b = 7; b >= 0; b--)
        {
            dmrBitPhaseNow = (tailPhase < 0) ? -1 : (tailPhase - (totalBits - 1 - bitIndex) % 576 + 576) % 576;
            bitIndex++;
            dmrProcessBit((bytes[i] & (1U << b)) != 0U);
        }

    if (debugM)
    {
        int64_t took = dmrNowNs() - now;
        if (took > 20000000LL)
            fprintf(stderr, "DMR RX bits: processing one BITS packet took %lld ms\n", (long long)(took / 1000000LL));
    }
}

/* RF voice regeneration.  Each voice burst received from the radio carries
 * three AMBE+2 frames protected by Golay codes.  They are decoded and encoded
 * again (see dmr_fec.h), which gives the number of channel bit errors for the
 * BER figure and, when DMR_FEC_REGENERATE is set, replaces correctable frames
 * with their corrected version before they are relayed.  Frames with more
 * errors than DMR_FEC_MAX_ERRORS go out as received, so the receiving radio's
 * own error handling still sees damage that can not be repaired.  Network
 * traffic never went over the air here, so it is left alone. */
#ifndef DMR_FEC_REGENERATE
#define DMR_FEC_REGENERATE 1
#endif

static unsigned int dmrRfErrs[2] = {0U, 0U}; // bit errors found in this RF call, per slot
static unsigned int dmrRfBits[2] = {0U, 0U}; // protected bits checked in this RF call, per slot

/*
 * dmrRfVoice()
 *
 * Adds the error bits of a received RF voice burst to the counts for the
 * slot, for the call's BER.  Network frames and frames that are too short
 * are ignored.
 *
 * Parameters:
 *   slot   - Time slot index, 0 or 1.
 *   buffer - The received frame (OpenMT header, control byte and 33 byte
 *            burst).
 *   length - Number of bytes in buffer.
 *   isNet  - true if the frame came from the network.
 *
 * Returns: nothing.
 */
static void dmrRfVoice(unsigned int slot, uint8_t* buffer, uint8_t length, bool isNet)
{
    if (isNet || length < 8U + 1U + DMR_FRAME_LENGTH_BYTES)
    {
        return;
    }

    dmrRfErrs[slot] += dmrFecErrors(buffer + 8 + 1, DMR_FEC_REGENERATE != 0);
    dmrRfBits[slot] += DMR_FEC_BITS_PER_BURST;
}

/*
 * dmrRfBer()
 *
 * Works out and prints the BER of the RF call that has just ended on a slot,
 * and clears the slot's counts.  A network call has no BER and leaves the
 * counts alone.
 *
 * Parameters:
 *   slot  - Time slot index, 0 or 1.
 *   isNet - true if the call came from the network.
 *
 * Returns: The BER as a fraction of bits in error, or 0 if there was none.
 */
static float dmrRfBer(unsigned int slot, bool isNet)
{
    float ber = 0.0f;

    /* A network call has no BER and must not disturb the counters of an RF
     * call on the same slot. */
    if (isNet)
    {
        return ber;
    }

    if (dmrRfBits[slot] > 0U)
    {
        ber = (float)dmrRfErrs[slot] / (float)dmrRfBits[slot];
        fprintf(stderr, "BER slot %u: %.6f (%u errors / %u bits)\n", slot + 1U, ber, dmrRfErrs[slot], dmrRfBits[slot]);
    }

    dmrRfErrs[slot] = 0U;
    dmrRfBits[slot] = 0U;

    return ber;
}

/* True while the link control of the call on a slot is known, so that the
 * embedded link control of its voice bursts can be rebuilt (see
 * dmrRebuildBurst()). */
static bool dmrRebuildLcValid[2] = {false, false};

/*
 * dmrRebuildLoadEmbeddedLc()
 *
 * Late entry: when a call is joined after its voice LC header, the link
 * control that arrives in the embedded data of the voice bursts is loaded for
 * the rebuild, so that the bursts that follow can be rebuilt like those of a
 * call with a header.  A link control already known from a header is kept.
 *
 * Parameters:
 *   slot - Time slot index, 0 or 1.
 *   lc   - The 9 bytes of the embedded link control.
 *
 * Returns: nothing.
 */
static void dmrRebuildLoadEmbeddedLc(uint8_t slot, const uint8_t* lc)
{
    if (dmrRebuildLcValid[slot])
        return;

    const uint8_t rs = DMR_REBUILD_SLOT(slot);

    DMRLC_decode(lc, rs);
    DMREmbeddedData_setLC(rs);
    dmrRebuildLcValid[slot] = true;
}

/*
 * dmrRebuildBurst()
 *
 * Rebuilds a burst that is about to be relayed to the radio, from what is
 * decoded from it, as MMDVMHost does in duplex mode: nothing that the radio
 * sent is repeated as it came, except the voice itself.
 *
 *   - Data bursts get their slot type encoded again, with the colour code of
 *     this host.
 *   - The voice LC header and the terminator with LC are decoded (BPTC and
 *     Reed-Solomon) and encoded again from the link control.  The header
 *     also gives the embedded link control for the voice bursts that follow.
 *   - CSBKs, data headers and rate 1/2 data blocks are decoded (BPTC) and
 *     encoded again; a CSBK is checked with its CRC and given a new one.
 *   - Voice bursts B to F get their EMB encoded again (with this host's
 *     colour code) and their embedded link control rebuilt from the link
 *     control of the header.  Burst A gets only its sync, which the caller
 *     adds.
 *   - Rate 3/4 data blocks are decoded (trellis) and encoded again; one that
 *     cannot be repaired goes out as received.
 *   - Rate 1 data blocks and the PI header are not decoded: they go out as
 *     received, with the slot type and sync rewritten.
 * A burst that does not decode is left as received.  The rebuild uses
 * slotData[] entries of its own (DMR_REBUILD_SLOT), so the state of a call
 * being decoded is not disturbed.
 *
 * Parameters:
 *   slot   - Time slot index, 0 or 1.
 *   buffer - The frame, starting with the OpenMT header; the control byte is
 *            at offset 8 and the burst from offset 9.  The burst is changed
 *            in place.
 *
 * Returns: nothing.
 */
static void dmrRebuildBurst(uint8_t slot, uint8_t* buffer)
{
    const uint8_t control = buffer[8];
    uint8_t*      frame   = buffer + 9;
    const uint8_t rs      = DMR_REBUILD_SLOT(slot);

    if ((control & DMR_SYNC_DATA) == DMR_SYNC_DATA)
    {
        uint8_t colorCode, dataType;

        DMRSlotType_decode(frame, &colorCode, &dataType);
        if (colorCode != dmrColorCode)
            return;

        switch (dataType)
        {
            case DT_VOICE_LC_HEADER:
            case DT_TERMINATOR_WITH_LC:
            {
                uint8_t lc[12];

                if (dataType == DT_TERMINATOR_WITH_LC)
                    dmrRebuildLcValid[slot] = false;

                if (DMRFullLC_decode(frame, dataType, lc, rs))
                {
                    DMRLC_decode(lc, rs);
                    DMRFullLC_encode(frame, dataType, rs);
                    if (dataType == DT_VOICE_LC_HEADER)
                    {
                        DMREmbeddedData_setLC(rs);
                        dmrRebuildLcValid[slot] = true;
                    }
                }
                break;
            }

            case DT_CSBK:
                if (CSBK_decode(frame, rs))
                    CSBK_encode(frame, rs);
                break;

            case DT_DATA_HEADER:
            case DT_RATE_12_DATA:
            {
                uint8_t data[12];

                BPTC19696_decode(frame, data, rs);
                BPTC19696_encode(data, frame, rs);
                break;
            }

            case DT_RATE_34_DATA:
            {
                uint8_t payload[18];

                /* A block that cannot be repaired is left as received */
                if (DMRTrellis_decode(frame, payload))
                    DMRTrellis_encode(payload, frame);
                break;
            }

            default:
                break;
        }

        DMRSlotType_encode(dmrColorCode, dataType, frame);
    }
    else if ((control & DMR_SYNC_AUDIO) != DMR_SYNC_AUDIO)
    {
        /* Voice burst B to F: the control byte carries its number */
        const uint8_t n = control & 0x0FU;

        if (n < 1U || n > 5U || !dmrRebuildLcValid[slot])
            return;

        DMREMB_decode(frame, rs);
        setColorCode(dmrColorCode, rs);
        setLCSS(DMREmbeddedData_getData(frame, n, rs), rs);
        DMREMB_encode(frame, rs);
    }
}

/*
 * dmrRelayBurst()
 *
 * Passes a data or voice burst on to the radio: gives data bursts the
 * base station or mobile station sync, then queues the burst in the slot's
 * FIFO (SAMPLE and BIT mode, where dmrTxThread() sends it in its own TDMA
 * slot) or sends it to the modem as a data frame (FRAME mode).  It is used
 * for every burst of a call, the terminator included.  In duplex mode the
 * burst is first rebuilt from what is decoded from it (dmrRebuildBurst()).
 * The terminator is
 * relayed before the call is closed, because closing the call un-keys the
 * transmitter in FRAME mode.
 *
 * Parameters:
 *   slot   - Time slot index, 0 or 1.
 *   buffer - The frame, starting with the OpenMT header; the control byte is
 *            at offset 8 and the burst from offset 9.  The sync of a data
 *            burst is changed in place.
 *   isNet  - true if the frame came from the network.
 *
 * Returns: nothing.
 */
static void dmrRelayBurst(uint8_t slot, uint8_t* buffer, bool isNet)
{
    /* In duplex the burst is rebuilt from what is decoded from it */
    if (modem_duplex)
        dmrRebuildBurst(slot, buffer);

    /* Re-transmitted data bursts (voice LC header, terminator, CSBK, data)
     * must carry the BS-sourced data sync, the same way voice burst A gets
     * the BS-sourced voice sync -- MMDVMHost does this for every data burst
     * it relays.  Without it a radio listening to a repeater finds no call
     * set-up. */
    if ((buffer[8] & DMR_SYNC_DATA) == DMR_SYNC_DATA)
        addDMRDataSync(buffer + 8 + 1, modem_duplex);

    if (packetType == PACKET_TYPE_SAMP || packetType == PACKET_TYPE_BIT)
    {
        /* Hand the frame to the slot's FIFO instead of modulating it at
         * once: a real DMR burst has to go out inside its own 30 ms TDMA
         * slot (see dmrTxThread()/dmrSendSlotBurst()).  In simplex there is
         * only the slot 1 FIFO (see dmrSendDmoPeriod()). */
        const uint8_t fifo = (slot == 0U || !modem_duplex) ? 0U : 1U;
        pthread_mutex_lock(&dmrTxFifoMutex[fifo]);
        RingBuffer_addData(&dmrTxFifo[fifo], buffer + 8 + 1, DMR_FRAME_LENGTH_BYTES);
        pthread_mutex_unlock(&dmrTxFifoMutex[fifo]);
        if (debugM)
            fprintf(stderr, "DMR relay slot%u: queued frame, control=0x%02X\n", slot + 1U, buffer[8]);
    }
    else
    {
        uint8_t buf[34 + 1];
        buf[0] = (slot == 0U) ? TYPE_DMR_DATA1 : TYPE_DMR_DATA2;
        memcpy(buf + 1, buffer + 8, 34);
        sendDataToModem(buf, 34 + 1, slot + 1U, isNet);
    }
}

/*
 * decodeDMR()
 *
 * Handles one DMR frame (DMD1, DML1, DMD2 or DML2) from the radio or the
 * network.  A simplex modem works on slot 1 only, so slot 2 frames are moved
 * to slot 1.  It then follows the call on that slot: the voice LC header
 * starts a call (the call is looked up and recorded on the dashboard, and
 * the modem is keyed), voice bursts are counted and forwarded, CSBK data is
 * handled, and the terminator or a "lost" frame ends the call.  Frames are
 * forwarded to the gateway, and to the modem when needed; in SAMPLE and BIT
 * mode they are queued in the slot's FIFO for dmrTxThread() instead of being
 * modulated at once, so that each burst leaves inside its own TDMA slot.
 *
 * Parameters:
 *   buffer - The frame, starting with the OpenMT header; the type is at
 *            offset 4 and the burst data from offset 8.
 *   length - Number of bytes in buffer.
 *   isNet  - true if the frame came from the network, false if from the
 *            radio.
 *
 * Returns: nothing.
 */
void decodeDMR(uint8_t* buffer, uint8_t length, bool isNet)
{
    static uint8_t activeSlots = 0;
    static uint8_t slotRow[2] = {1, 1};
    static bool isGroup[2] = {true, true};

    char cType[4] = "RF";
    if (isNet)
        strcpy(cType, "NET");

    /* Simplex works on slot 1 only. A radio talking to a simplex hotspot
     * has no slot (the modem's DMO receiver reports slot 2, and in SAMPLE and
     * BIT mode the slot would be guessed from timing against a downlink that
     * is not there), so everything heard from the radio is slot 1. It is
     * only passed on to the gateway, never retransmitted. */
    const bool simplexRF = !isNet && !modem_duplex;
    if (simplexRF)
    {
        if (memcmp((buffer + 4), TYPE_DATA2, 4) == 0)
        {
            memcpy(buffer + 4, TYPE_DATA1, 4);
            if (packetType == PACKET_TYPE_FRAME)
                sendCommToModem((uint8_t*)SETMODE, 5); // as the slot 2 branch did for simplex
        }
        else if (memcmp((buffer + 4), TYPE_LOST2, 4) == 0)
        {
            memcpy(buffer + 4, TYPE_LOST1, 4);
        }
    }

    // Any traffic for a slot keeps it alive (see dmrSlotWatchdog()).
    if (memcmp((buffer + 4), TYPE_DATA1, 4) == 0 || memcmp((buffer + 4), TYPE_LOST1, 4) == 0)
        dmrSlotNoteFrame(0U);
    else if (memcmp((buffer + 4), TYPE_DATA2, 4) == 0 || memcmp((buffer + 4), TYPE_LOST2, 4) == 0)
        dmrSlotNoteFrame(1U);

    if (memcmp((buffer + 4), TYPE_DATA1, 4) == 0)
    {
        if (!isNet && !slotActive[0] && !slotActive[1] && buffer[8] == 0xC3)
        {
            CSBK_decode(buffer + 8 + 1, 0);
         //   write(sockfd, SETMODE, 5);
            sendCommToModem((uint8_t*)SETMODE, 5);
            uint8_t buf[2];
            buf[0] = TYPE_DMR_START;
            buf[1] = 1;
            sendDataToModem(buf, 2, 1, isNet);
            isGroup[0] = true;

            /* In FRAME mode the MMDVM firmware starts the downlink itself on
             * TYPE_DMR_START. In SAMPLE/BIT mode this host generates the
             * downlink (dmrTxThread()), so key it here: the waking radio
             * needs to hear our CACH/idle bursts to time itself to a slot
             * before it sends its call. No slot is marked active -- the
             * call that follows does that, on whichever slot it's really
             * on. dmrTxThread() drops txOn again if no call follows. */
            if (!simplexRF && (packetType == PACKET_TYPE_SAMP || packetType == PACKET_TYPE_BIT))
            {
                dmrTxHangNs = dmrNowNs();
                txOn        = true;
                if (debugM)
                    fprintf(stderr, "DMR: wake-up CSBK, keying downlink\n");
            }
        }
        else
        {
            if (isNet && !slotActive[0] && !slotActive[1] && buffer[8] == 0x41)
            {
                CSBK_decode(buffer + 8 + 1, 0);
//                write(sockfd, SETMODE, 5);
                sendCommToModem((uint8_t*)SETMODE, 5);
                uint8_t buf[2];
                buf[0] = TYPE_DMR_START;
                buf[1] = 1;
                sendDataToModem(buf, 2, 1, isNet);
                isGroup[0] = true;
            }

            dataSync  = (buffer[8] & DMR_SYNC_DATA) == DMR_SYNC_DATA;
            audioSync = (buffer[8] & DMR_SYNC_AUDIO) == DMR_SYNC_AUDIO;

            if (dataSync)
            {
                if (!slotActive[0])
                {
                    slotActive[0] = true;
                    if (simplexRF)
                        dmrSimplexRf = true;
                    else
                        txOn = true;
                    if (activeSlots == 0)
                    {
                        activeSlots = 1;
                        slotRow[0]  = 1;
                    }
                    else
                    {
                        activeSlots = 2;
                        slotRow[0]  = 2;
                    }
                    fprintf(stderr, "Slot 1 active\n");
                    if (!isNet)
                    {
                        dmrRfErrs[0] = 0U;
                        dmrRfBits[0] = 0U;
                    }
                }

                uint8_t dataType = buffer[8] & 0x0FU;
                if (dataType == DT_VOICE_LC_HEADER)
                {
                    uint8_t LC[12];
                    slot1_srcId = CSBK_getSrcId(0);
                    if (DMRFullLC_decode(buffer + 8 + 1, 1, LC, 0))
                    {
                        slot1_starttime = time(NULL);
                        DMRLC_decode(LC, 0);
                        slot1_srcId = DMRLC_getSrcId(0);
                        slot1_dstId = DMRLC_getDstId(0);
                        char tmp[100];
                        bzero(tmp, 100);
                        if (findDMRId(slot1_srcId, slot1_call, sizeof(slot1_call), tmp, sizeof(tmp)))
                        {
                            bzero(slot1_name, 15);
                            memcpy(slot1_name, tmp, 14);
                            fprintf(stderr, "========= DMR Slot 1 Call: %s  Name: %s  Id: %u  Dst: %u\n", slot1_call, slot1_name, slot1_srcId, slot1_dstId);
                            sprintf(tmp, "%u", slot1_srcId);
                            char tmp1[11];
                            sprintf(tmp1, "TG %u", slot1_dstId);
                            saveLastCall(slotRow[0], modemName, "DMR TS1", cType, slot1_call, tmp, tmp1, "", NULL, "", true);
                        }
                        else
                        {
                            if (packetType == PACKET_TYPE_BIT)
                            {
                                strcpy(slot1_call, "N0CALL");
                                bzero(slot1_name, 15);
                                fprintf(stderr, "DMR Slot 1: Id %u not found in dmr_ids table, using N0CALL\n", slot1_srcId);
                            }
                            sprintf(tmp, "%u", slot1_srcId);
                            char tmp1[11];
                            sprintf(tmp1, "TG %u", slot1_dstId);
                            saveLastCall(slotRow[0], modemName, "DMR TS1", cType, "N0CALL", tmp, tmp1, "", NULL, "", true);
                        }
                    }
                    else
                    {
                        if (packetType == PACKET_TYPE_BIT)
                            fprintf(stderr, "DMR: voice LC header FullLC decode FAILED (slot 1)\n");
                    }
                }
                else if (dataType == DT_TERMINATOR_WITH_LC)
                {
                    /* Send the terminator to the radio while the call is still open */
                    if (txOn && slotActive[0] && !simplexRF)
                        dmrRelayBurst(0U, buffer, isNet);

                    slotActive[0] = false;
                    if (simplexRF)
                        dmrSimplexRf = false;
                    if (!slotActive[1] && !simplexRF)
                    {
                        dmrReleaseTx();
                        uint8_t buf[2];
                        buf[0] = TYPE_DMR_START;
                        buf[1] = 0;
                        sendDataToModem(buf, 2, 1, isNet);
                    }
                    char tmp[11];
                    sprintf(tmp, "%u", slot1_srcId);
                    char tmp1[11];
                    sprintf(tmp1, "TG %u", slot1_dstId);
                    float loss_BER = dmrRfBer(0, isNet);
                    slot1_duration = difftime(time(NULL), slot1_starttime);
                    saveLastCall(slotRow[0], modemName, "DMR TS1", cType, slot1_call, tmp, tmp1, "", NULL, "", false);
                    saveHistory(modemName, "DMR TS1", cType, slot1_call, tmp, tmp1, loss_BER, "", slot1_duration);
                    if (packetType == PACKET_TYPE_BIT)
                        slot1_starttime = 0;
                    if (activeSlots == 1)
                    {
                        activeSlots = 0;
                        slotRow[0] = 1;
                    }
                    else if (activeSlots == 2)
                    {
                        activeSlots = 1;
                        slotRow[0] = 1;
                    }
                    //    if (debugM)
                        fprintf(stderr, "======= EOT slot 1.\n\n");
                }
                else if (dataType == DT_CSBK)
                {
                }
            }
            else if (audioSync || txOn || dmrSimplexRf)
            {
                if (simplexRF)
                    dmrSimplexRf = true;
                else
                    txOn = true;
                dmrRfVoice(0U, buffer, length, isNet);
                if (buffer[8] == 0x20)
                {
                    /* NOW modify sync pattern (after EMB extracted) */
                    addDMRAudioSync(buffer + 8 + 1, modem_duplex);
                    //          fprintf(stderr, "Slot 1 Voice: ColorCode=%u PI=%d LCSS=%u\n", cc, pi, lcss);
                }
                else
                {
                    DMREMB_decode(buffer + 8 + 1, 0);
                    uint8_t lcss = getLCSS(0);
                    uint8_t cc   = getColorCode(0);
                    bool pi      = getPI(0);

                    if (DMREmbeddedData_decode(buffer + 8 + 1, lcss, 0))
                    {
                        unsigned int flco = DMRLC_getFLCO(0);
                        fprintf(stderr, "FLCO: %u\n", flco);
                        unsigned char data[9U];
                        DMREmbeddedData_getRawData(data, 0);
                        hexDump((char*)"DMR", data, 9);
                        if (flco == 0 || flco == 3)
                            dmrRebuildLoadEmbeddedLc(0, data);
                        if ((flco == 0 || flco == 3) && !slotActive[0] && !isNet)
                        {
                            /* late entry: the call is joined from its embedded
                             * link control, no header was seen, so the slot
                             * becomes active here and the bursts are relayed */
                            slotActive[0] = true;
                            if (activeSlots == 0)
                            {
                                activeSlots = 1;
                                slotRow[0]  = 1;
                            }
                            else
                            {
                                activeSlots = 2;
                                slotRow[0]  = 2;
                            }
                            if (slot1_starttime == 0)
                                slot1_starttime = time(NULL);
                            fprintf(stderr, "Slot 1 active (late entry)\n");
                        }
                        if (flco == 0)
                        {
                            isGroup[0] = true;
                            slot1_srcId = DMRLC_getSrcId(0);
                            if (packetType == PACKET_TYPE_BIT && slot1_starttime == 0)
                                slot1_starttime = time(NULL);
                            fprintf(stderr, "Slot 1 Voice: Src: %u ColorCode=%u PI=%d LCSS=%u\n", slot1_srcId, cc, pi, lcss);
                            slot1_dstId = DMRLC_getDstId(0);
                            char tmp[100];
                            bzero(tmp, 100);
                            if (findDMRId(slot1_srcId, slot1_call, sizeof(slot1_call), tmp, sizeof(tmp)))
                            {
                                bzero(slot1_name, 15);
                                memcpy(slot1_name, tmp, 14);
                                fprintf(stderr, "========= DMR Slot 1 Call: %s  Name: %s  Id: %u  Dst: %u\n", slot1_call, slot1_name, slot1_srcId, slot1_dstId);
                                sprintf(tmp, "%u", slot1_srcId);
                                char tmp1[11];
                                sprintf(tmp1, "TG %u", slot1_dstId);
                                saveLastCall(slotRow[0], modemName, "DMR TS1", cType, slot1_call, tmp, tmp1, "", NULL, "", true);
                            }
                            else
                            {
                                if (packetType == PACKET_TYPE_BIT)
                                {
                                    strcpy(slot1_call, "N0CALL");
                                    bzero(slot1_name, 15);
                                    fprintf(stderr, "DMR Slot 1: Id %u not found in dmr_ids table, using N0CALL\n", slot1_srcId);
                                }
                                sprintf(tmp, "%u", slot1_srcId);
                                char tmp1[11];
                                sprintf(tmp1, "TG %u", slot1_dstId);
                                saveLastCall(slotRow[0], modemName, "DMR TS1", cType, "N0CALL", tmp, tmp1, "", NULL, "", true);
                            }
                        }
                        else if (flco == 3)
                        {
                            isGroup[0] = false;
                        }
                    }
                }
            }

            if (txOn && slotActive[0] && !simplexRF)
                dmrRelayBurst(0U, buffer, isNet);

            if (dmrGWConnected && !isNet)
            {
                uint8_t hdr[7] = {0, 0, 0, 0, 0, 0, 0};
                hdr[0] = (slot1_dstId & 0xff0000) >> 16;
                hdr[1] = (slot1_dstId & 0x00ff00) >> 8;
                hdr[2] = (slot1_dstId & 0x0000ff);
                hdr[3] = (slot1_srcId & 0xff0000) >> 16;
                hdr[4] = (slot1_srcId & 0x00ff00) >> 8;
                hdr[5] = (slot1_srcId & 0x0000ff);
                hdr[6] = isGroup[0];
                buffer[2] = 8 + 34 + 7;
                pthread_mutex_lock(&gwBufMutex);
                RingBuffer_addData(&gwTxBuffer, buffer, 8 + 34);
                RingBuffer_addData(&gwTxBuffer, hdr, 7);
                pthread_mutex_unlock(&gwBufMutex);
            }
        }
    }
    else if (memcmp((buffer + 4), TYPE_LOST1, 4) == 0)
    {
        dmrRebuildLcValid[0] = false;
        if (simplexRF)
            dmrSimplexRf = false;
        if (!slotActive[1] && !simplexRF)
        {
            dmrReleaseTx();
            uint8_t buf[2];
            buf[0] = TYPE_DMR_START;
            buf[1] = 0;
            sendDataToModem(buf, 2, 1, isNet);
            isGroup[1] = true;
        }
        slotActive[0] = false;
        char tmp[11];
        sprintf(tmp, "%u", slot1_srcId);
        char tmp1[11];
        sprintf(tmp1, "TG %u", slot1_dstId);
        float loss_BER = dmrRfBer(0, isNet);
        slot1_duration = difftime(time(NULL), slot1_starttime);
        saveLastCall(slotRow[0], modemName, "DMR TS1", cType, slot1_call, tmp, tmp1, "", NULL, "", false);
        saveHistory(modemName, "DMR TS1", cType, slot1_call, tmp, tmp1, loss_BER, "", slot1_duration);
        if (packetType == PACKET_TYPE_BIT)
            slot1_starttime = 0;
        if (activeSlots == 1)
        {
            activeSlots = 0;
            slotRow[0] = 1;
        }
        else if (activeSlots == 2)
        {
            activeSlots = 1;
            slotRow[0] = 1;
        }
        //        if (debugM)
            fprintf(stderr, "Lost signal slot 1.\n");
    }
    else if (memcmp((buffer + 4), TYPE_DATA2, 4) == 0)
    {
        if (isNet && !slotActive[0] && !slotActive[1] && buffer[8] == 0x41)
        {
            CSBK_decode(buffer + 8 + 1, 0);
            sendCommToModem((uint8_t*)SETMODE, 5);
//            write(sockfd, SETMODE, 5);
            uint8_t buf[2];
            buf[0] = TYPE_DMR_START;
            buf[1] = 1;
            sendDataToModem(buf, 2, 2, isNet);
            isGroup[1] = true;
        }
        else if (!modem_duplex)
        {
            sendCommToModem((uint8_t*)SETMODE, 5);
        }

        dataSync  = (buffer[8] & DMR_SYNC_DATA) == DMR_SYNC_DATA;
        audioSync = (buffer[8] & DMR_SYNC_AUDIO) == DMR_SYNC_AUDIO;

        if (dataSync)
        {
            if (!slotActive[1])
            {
                slotActive[1] = true;
                txOn = true;
                if (activeSlots == 0)
                {
                    activeSlots = 1;
                    slotRow[1]  = 1;
                }
                else
                {
                    activeSlots = 2;
                    slotRow[1]  = 2;
                }
                fprintf(stderr, "Slot 2 active\n");
                if (!isNet)
                {
                    dmrRfErrs[1] = 0U;
                    dmrRfBits[1] = 0U;
                }
            }

            uint8_t dataType = buffer[8] & 0x0FU;
            if (dataType == DT_VOICE_LC_HEADER)
            {
                uint8_t LC[12];
                slot2_srcId = CSBK_getSrcId(1);
                if (DMRFullLC_decode(buffer + 8 + 1, 1, LC, 1))
                {
                    slot2_starttime = time(NULL);
                    DMRLC_decode(LC, 1);
                    slot2_srcId = DMRLC_getSrcId(1);
                    slot2_dstId = DMRLC_getDstId(1);
                    char tmp[100];
                    bzero(tmp, 100);
                    if (!findDMRId(slot2_srcId, slot2_call, sizeof(slot2_call), tmp, sizeof(tmp)) && packetType == PACKET_TYPE_BIT)
                    {
                        strcpy(slot2_call, "N0CALL");
                        tmp[0] = 0;
                        fprintf(stderr, "DMR Slot 2: Id %u not found in dmr_ids table, using N0CALL\n", slot2_srcId);
                    }
                    bzero(slot2_name, 15);
                    memcpy(slot2_name, tmp, 14);
                    fprintf(stderr, "========= DMR Slot 2 Call: %s  Name: %s  Id: %u  Dst: %u\n", slot2_call, slot2_name, slot2_srcId, slot2_dstId);
                    sprintf(tmp, "%u", slot2_srcId);
                    char tmp1[11];
                    sprintf(tmp1, "TG %u", slot2_dstId);
                    saveLastCall(slotRow[1], modemName, "DMR TS2", cType, slot2_call, tmp, tmp1, "", NULL, "", true);
                }
            }
            else if (dataType == DT_TERMINATOR_WITH_LC)
            {
                /* Send the terminator to the radio while the call is still open */
                if (txOn && slotActive[1])
                    dmrRelayBurst(1U, buffer, isNet);

                slotActive[1] = false;
                if (!slotActive[0])
                {
                    dmrReleaseTx();
                    uint8_t buf[2];
                    buf[0] = TYPE_DMR_START;
                    buf[1] = 0;
                    sendDataToModem(buf, 2, 2, isNet);
                }
                char tmp[11];
                sprintf(tmp, "%u", slot2_srcId);
                char tmp1[11];
                sprintf(tmp1, "TG %u", slot2_dstId);
                float loss_BER = dmrRfBer(1, isNet);
                slot2_duration = difftime(time(NULL), slot2_starttime);
                saveLastCall(slotRow[1], modemName, "DMR TS2", cType, slot2_call, tmp, tmp1, "", NULL, "", false);
                saveHistory(modemName, "DMR TS2", cType, slot2_call, tmp, tmp1, loss_BER, "", slot2_duration);
                if (packetType == PACKET_TYPE_BIT)
                    slot2_starttime = 0;
                if (activeSlots == 1)
                {
                    activeSlots = 0;
                    slotRow[1] = 1;
                }
                else if (activeSlots == 2)
                {
                    activeSlots = 1;
                    slotRow[1] = 1;
                }
                //   if (debugM)
                    fprintf(stderr, "======= EOT slot 2.\n\n");
            }
        }
        else if (audioSync || txOn)
        {
            txOn = true;
            dmrRfVoice(1U, buffer, length, isNet);

            if (buffer[8] == 0x20)
            {
                addDMRAudioSync(buffer + 8 + 1, modem_duplex);
            }
            else
            {
                DMREMB_decode(buffer + 8 + 1, 1);
                uint8_t lcss = getLCSS(1);
                uint8_t cc   = getColorCode(1);
                bool pi      = getPI(1);

                if (DMREmbeddedData_decode(buffer + 8 + 1, lcss, 1))
                {
                    unsigned int flco = DMRLC_getFLCO(1);
                    fprintf(stderr, "FLCO: %u\n", flco);
                    unsigned char data[9U];
                    DMREmbeddedData_getRawData(data, 1);
                    hexDump((char*)"DMR", data, 9);
                    if (flco == 0 || flco == 3)
                        dmrRebuildLoadEmbeddedLc(1, data);
                    if ((flco == 0 || flco == 3) && !slotActive[1] && !isNet)
                    {
                        /* late entry: see slot 1 */
                        slotActive[1] = true;
                        if (activeSlots == 0)
                        {
                            activeSlots = 1;
                            slotRow[1]  = 1;
                        }
                        else
                        {
                            activeSlots = 2;
                            slotRow[1]  = 2;
                        }
                        if (slot2_starttime == 0)
                            slot2_starttime = time(NULL);
                        fprintf(stderr, "Slot 2 active (late entry)\n");
                    }
                    if (flco == 0)
                    {
                        isGroup[1] = true;
                        DMRLC_decode(data, 1);
                        slot2_srcId = DMRLC_getSrcId(1);
                        fprintf(stderr, "Slot 2 Voice: Src: %u ColorCode=%u PI=%d LCSS=%u\n", slot2_srcId, cc, pi, lcss);
                        slot2_dstId = DMRLC_getDstId(1);
                        char tmp[100];
                        bzero(tmp, 100);
                        if (findDMRId(slot2_srcId, slot2_call, sizeof(slot2_call), tmp, sizeof(tmp)))
                        {
                            bzero(slot2_name, 15);
                            memcpy(slot2_name, tmp, 14);
                            fprintf(stderr, "========= DMR Slot 2 Call: %s  Name: %s  Id: %u  Dst: %u\n", slot2_call, slot2_name, slot2_srcId, slot2_dstId);
                            sprintf(tmp, "%u", slot2_srcId);
                            char tmp1[11];
                            sprintf(tmp1, "TG %u", slot2_dstId);
                            saveLastCall(slotRow[1], modemName, "DMR TS2", cType, slot2_call, tmp, tmp1, "", NULL, "", true);
                        }
                        else
                        {
                            if (packetType == PACKET_TYPE_BIT)
                            {
                                strcpy(slot2_call, "N0CALL");
                                bzero(slot2_name, 15);
                                fprintf(stderr, "DMR Slot 2: Id %u not found in dmr_ids table, using N0CALL\n", slot2_srcId);
                            }
                            sprintf(tmp, "%u", slot2_srcId);
                            char tmp1[11];
                            sprintf(tmp1, "TG %u", slot2_dstId);
                            saveLastCall(slotRow[1], modemName, "DMR TS2", cType, "N0CALL", tmp, tmp1, "", NULL, "", true);
                        }
                    }
                    else if (flco == 3)
                    {
                        isGroup[1] = false;
                    }
                }
            }
        }

        if (txOn && slotActive[1])
            dmrRelayBurst(1U, buffer, isNet);

        if (dmrGWConnected && !isNet)
        {
            uint8_t hdr[7] = {0, 0, 0, 0, 0, 0, 0};
            hdr[0] = (slot2_dstId & 0xff0000) >> 16;
            hdr[1] = (slot2_dstId & 0x00ff00) >> 8;
            hdr[2] = (slot2_dstId & 0x0000ff);
            hdr[3] = (slot2_srcId & 0xff0000) >> 16;
            hdr[4] = (slot2_srcId & 0x00ff00) >> 8;
            hdr[5] = (slot2_srcId & 0x0000ff);
            hdr[6] = isGroup[1];
            buffer[2] = 8 + 34 + 7;
            pthread_mutex_lock(&gwBufMutex);
            RingBuffer_addData(&gwTxBuffer, buffer, 8 + 34);
            RingBuffer_addData(&gwTxBuffer, hdr, 7);
            pthread_mutex_unlock(&gwBufMutex);
        }
    }
    else if (memcmp((buffer + 4), TYPE_LOST2, 4) == 0)
    {
        dmrRebuildLcValid[1] = false;
        if (!slotActive[0])
        {
            dmrReleaseTx();
            uint8_t buf[2];
            buf[0] = TYPE_DMR_START;
            buf[1] = 0;
            sendDataToModem(buf, 2, 2, isNet);
            isGroup[1] = true;
        }
        slotActive[1] = false;
        char tmp[11];
        sprintf(tmp, "%u", slot2_srcId);
        char tmp1[11];
        sprintf(tmp1, "TG %u", slot2_dstId);
        float loss_BER = dmrRfBer(1, isNet);
        slot2_duration = difftime(time(NULL), slot2_starttime);
        saveLastCall(slotRow[1], modemName, "DMR TS2", cType, slot2_call, tmp, tmp1, "", NULL, "", false);
        saveHistory(modemName, "DMR TS2", cType, slot2_call, tmp, tmp1, loss_BER, "", slot2_duration);
        if (packetType == PACKET_TYPE_BIT)
            slot2_starttime = 0;
        if (activeSlots == 1)
        {
            activeSlots = 0;
            slotRow[1] = 1;
        }
        else if (activeSlots == 2)
        {
            activeSlots = 1;
            slotRow[1] = 1;
        }
        //        if (debugM)
            fprintf(stderr, "Lost signal slot 2.\n");
    }
}

/*
 * decodeFrame()
 *
 * Passes a frame to decodeDMR() if it is a DMR frame and the active mode is
 * DMR or idle.
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
    char cType[4] = "RF";
    char active_mode[10] = "IDLE";

    readStatus(modemName, "main", "active_mode", active_mode);

    if (strcasecmp(active_mode, "DMR") == 0 || strcasecmp(active_mode, "IDLE") == 0)
    {
        if (isNet)
            strcpy(cType, "NET");

        if (memcmp(buffer + 4, "DM", 2) == 0)
        {
            decodeDMR(buffer, length, isNet);
        }
    }
}

/*
 * startClient()
 *
 * Thread that connects to the modem host socket.  It starts the gateway
 * server, receive and timer threads, sends the mode and radio configuration
 * packet (frequency, filters and, for BIT mode, the scan and slot settings),
 * then processes the packets the modem host returns: commands, TX timing
 * reports, the SAMP or BITS mode switch, and DMR frames.
 *
 * Parameters:
 *   arg - Address of the modem host, as a text string.
 *
 * Returns: Nothing useful; the thread ends by calling pthread_exit().
 */
void* startClient(void* arg)
{
    struct sockaddr_in serv_addr;
    uint8_t buffer[BUFFER_SIZE] = {0};
    char hostAddress[80];
    strcpy(hostAddress, (char*)arg);

    // Create socket file descriptor
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        reportError(modemName, "DMR", "Socket creation error: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1)
    {
        reportError(modemName, "DMR", "fcntl F_GETFL: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    // Prepare the sockaddr_in structure
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(clientPort);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (!hostToAddress(hostAddress, &serv_addr.sin_addr))
    {  // Connect to localhost
        reportError(modemName, "DMR", "Invalid address/ Address not supported");
        exit(EXIT_FAILURE);
    }

    // Connect to the server
    setNoDelay(sockfd);
    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        if (errno != EINPROGRESS)
        {
            reportError(modemName, "DMR", "connection failed: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    int err = pthread_create(&(gwHostid), NULL, &startTCPServer, NULL);
    if (err != 0)
    {
        reportError(modemName, "DMR", "Can't create gateway host thread :[%s]", strerror(err));
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
        reportError(modemName, "DMR", "Can't create rx thread :[%s]", strerror(err));
        return 0;
    }
    else
    {
        if (debugM) fprintf(stderr, "RX thread created successfully\n");
    }

    err = pthread_create(&(timerid), NULL, &timerThread, NULL);
    if (err != 0)
    {
        reportError(modemName, "DMR", "Can't create timer thread :[%s]", strerror(err));
        return 0;
    }
    else
    {
        if (debugM) fprintf(stderr, "Timer thread created successfully\n");
    }

    sleep(1);
    fprintf(stderr, "Connected to host.\n");

    ssize_t len       = 0;
    uint16_t offset   = 0;
    uint16_t respLen  = 0;
    uint8_t typeLen   = 0;
    uint8_t configLen = 4 + 4 + 11 + 6 + 1 + 40 + 16 + 1 + 1;  // 1 + 1 + 1 + 1 + 12 + 1 + 1 + 1 + 1 + 15;

    txOn = false;

    buffer[0] = 0x61;
    buffer[1] = 0x00;
    buffer[2] = configLen;
    buffer[3] = 0x04;
    memcpy(buffer + 4, TYPE_MODE, 4);
    memcpy(buffer + 8, MODE_NAME, 11);
    memcpy(buffer + 19, MODEM_TYPE, 6);
    buffer[25] = txLevel;
    uint8_t bytes[40];

    // Dev: 1200 Hz, symb rate = 4800
    uint32_t reg3  = 0x2A4C80D3;
    uint32_t reg10 = 0x049E472A;

    /* reg3/reg10 (PLL synth) and reg4's mode/invert bits below are
     * shared byte-for-byte with p25_host.c's confirmed-working ADF7021
     * config -- DMR and P25 are both 4-level-FSK-family, 12.5kHz-channel
     * protocols with comparable deviation, so that's the best available
     * reference for this radio. Post Dem BW and the IF filter select
     * were NOT carried over correctly, though: this host had Post Dem
     * BW = 80 and a 25kHz IF filter, wildly wider than EITHER confirmed
     * working protocol on this same hardware (D-STAR uses Post Dem
     * BW = 10, P25 uses 6; both use the 12.5kHz IF filter). That
     * mismatch -- not dmr_host.c's RX correlator -- is what explains
     * the symptom seen on real hardware: raw RX sample diagnostics
     * (added to investigate a stuck TX/no-RX-sync report) showed
     * noise-level statistics that never changed even while a real DMR
     * voice burst was confirmed on the air, exactly what an
     * excessively wide IF/post-demod filter bandwidth would produce
     * (far more out-of-band noise passed through than the discriminator
     * output a correctly-tuned narrowband demod should give). Lining
     * these two fields up with P25's confirmed-good values. */
    // K=32
    uint32_t reg4 = (uint32_t)0b0100 << 0;  // register 4
    reg4 |= (uint32_t)0b011 << 4;           // mode, 4FSK
    reg4 |= (uint32_t)0b0 << 7;
    reg4 |= (uint32_t)0b11 << 8;
    reg4 |= (uint32_t)393U << 10;  // Disc BW
    /* Receiver filters. Default as before: post-demod BW 6, 12.5 kHz IF.
     * MMDVM_HS uses post-demod BW 80 and the 25 kHz IF filter, which is much
     * more forgiving of a radio that is off frequency. Optional [DMR] keys,
     * BIT mode only:  rxWide=1  selects the MMDVM_HS values;
     *                 postDemBW (1..1023) and ifFilter (0=12.5k, 1=18.75k, 2=25k)
     * override them individually. */
    uint32_t postDemBW = 6U;
    uint32_t ifFilter  = 0U;
    if (packetType == PACKET_TYPE_BIT)
    {
        char v[16];
        memset(v, 0, sizeof(v));
        readHostConfig(modemName, "DMR", "rxWide", v);
        if (atoi(v) == 1)
        {
            postDemBW = 80U;
            ifFilter  = 2U;
        }
        memset(v, 0, sizeof(v));
        readHostConfig(modemName, "DMR", "postDemBW", v);
        if (atoi(v) >= 1 && atoi(v) <= 1023)
            postDemBW = (uint32_t)atoi(v);
        memset(v, 0, sizeof(v));
        readHostConfig(modemName, "DMR", "ifFilter", v);
        if (v[0] >= '0' && v[0] <= '2' && v[1] == 0)
            ifFilter = (uint32_t)(v[0] - '0');
        fprintf(stderr, "DMR: RX post-demod BW %u, IF filter %u (0=12.5k 1=18.75k 2=25k)\n", postDemBW, ifFilter);
    }
    reg4 |= postDemBW << 20;       // Post dem BW
    reg4 |= ifFilter << 30;        // IF filter

    uint32_t reg2 = (uint32_t)0b10 << 28;  // invert data (and RC alpha = 0.5)
    reg2 |= (uint32_t)0b111 << 4;          // modulation (4FSK)

    uint32_t reg13 = (uint32_t)0b1101 << 0;         // register 13
    reg13 |= (uint32_t)ADF7021_SLICER_TH_DMR << 4;  // slicer threshold

    ifConf(bytes, reg2, reg3, reg4, reg10, reg13, atol(modem_rxFrequency), atol(modem_txFrequency), txLevel, rfPower, 0, true);

    memcpy(buffer + 26, bytes, 40);
    uint64_t tmp[2] = {DMR_MS_DATA_SYNC_BITS, DMR_SYNC_BITS_MASK};
    memcpy(buffer + 66, (uint8_t*)tmp, 16);
    buffer[82] = 0x8a;
    /* BIT mode (openmths): bytes go out in symbol order, MSB first (0x12).
     * SAMPLE and FRAME mode keep the original value. */
    buffer[83] = (packetType == PACKET_TYPE_BIT) ? 0x12 : 0x02;
    if (packetType == PACKET_TYPE_BIT)
    {
        /* Optional [DMR] scanMult (1..15): how long the modem listens for DMR
         * each time it scans through its modes (low nibble of byte 83).
         * Default 2, as before. A longer dwell makes a short wake-up from
         * the idle repeater more likely to be heard. */
        char sm[16];
        memset(sm, 0, sizeof(sm));
        readHostConfig(modemName, "DMR", "scanMult", sm);
        int mult = atoi(sm);
        if (mult >= 1 && mult <= 15)
            buffer[83] = (uint8_t)(0x10 | mult);
        buffer[83] |= 0x20;   // ask the modem to append its TX stream position to each BITS packet
        memset(sm, 0, sizeof(sm));
        readHostConfig(modemName, "DMR", "slotPhase", sm);
        /* Default 197: measured on the first test setup (TS1 197, TS2 485).
         * The value comes from the modem's fixed timing, so it should hold
         * for the same firmware; override with [DMR] slotPhase if not. */
        dmrSlotPhase = (sm[0] != 0) ? (atoi(sm) % 576 + 576) % 576 : 197;
        fprintf(stderr, "DMR: slot phase %d (TS1 sync end; TS2 is 288 later)%s\n", (int)dmrSlotPhase, sm[0] != 0 ? "" : " [default]");
        fprintf(stderr, "DMR: BIT mode scan multiplier %d\n", (mult >= 1 && mult <= 15) ? mult : 2);
    }
    write(sockfd, buffer, configLen);

    hexDump((char*)"MODE", buffer, configLen);
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
            fprintf(stderr,
                    "DMR_Service: error when reading from server, errno=%d\n", errno);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "DMR_Service: unknown byte from server, 0x%02X\n", buffer[0]);
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

 //       if (debugM)
   //         hexDump((char*)"DMR host data", (unsigned char*)buffer, respLen);

        if (memcmp(type, TYPE_COMMAND, typeLen) == 0)
        {
            if (buffer[8] == COMM_SET_DUPLEX)
                modem_duplex = true;
            else if (buffer[8] == COMM_SET_SIMPLEX)
                modem_duplex = false;
            // Duplex decides whether relayed bursts get BS- or MS-sourced
            // sync, so a wrong setting here silences the whole downlink.
            fprintf(stderr, "DMR: modem_host says %s\n", modem_duplex ? "DUPLEX (BS sync on relayed bursts)" : "SIMPLEX (MS sync on relayed bursts)");
        }
        else if (typeLen == 4U && memcmp(type, "TXTM", 4) == 0 && respLen >= 8U + 8U)
        {
            /* TX timing report (see dmrTxRefValid). Offset is relative to
             * the next RX sample this host will receive -- which, since
             * every earlier SAMP packet has already been processed, is
             * dmrRx.dataPtr. */
            int32_t  offset = (int32_t)(((uint32_t)buffer[8] << 24) | ((uint32_t)buffer[9] << 16) |
                                        ((uint32_t)buffer[10] << 8) | (uint32_t)buffer[11]);
            uint32_t k      = ((uint32_t)buffer[12] << 24) | ((uint32_t)buffer[13] << 16) |
                              ((uint32_t)buffer[14] << 8) | (uint32_t)buffer[15];

            /* The modem repeats this report every ~0.3 s while keyed, so
             * RX samples lost on the way here (which shift dmrRx.dataPtr
             * against the TX clock) are corrected within a fraction of a
             * second.  Log only the first report and any that move the
             * reference by more than a few samples. */
            pthread_mutex_lock(&dmrTxSlotTimeMutex);
            bool plausible = dmrTxStreaming && k <= dmrTxSamplesWritten;
            bool hadRef    = dmrTxRefValid;
            int32_t oldPos = 0;
            int32_t newPos = 0;
            if (plausible)
            {
                uint32_t newRef = dmrRx.dataPtr + (uint32_t)offset;
                int64_t  m;
                m = ((int64_t)dmrTxRefK + (int64_t)(int32_t)(newRef - dmrTxRefRxPtr)) % DMR_CYCLE_SAMPLES;
                oldPos = (int32_t)(m < 0 ? m + DMR_CYCLE_SAMPLES : m);
                newPos = (int32_t)(k % DMR_CYCLE_SAMPLES);
                dmrTxRefRxPtr = newRef;
                dmrTxRefK     = k;
                dmrTxRefValid = true;
            }
            pthread_mutex_unlock(&dmrTxSlotTimeMutex);

            int32_t shift = newPos - oldPos;
            if (shift > DMR_CYCLE_SAMPLES / 2)
                shift -= DMR_CYCLE_SAMPLES;
            else if (shift < -DMR_CYCLE_SAMPLES / 2)
                shift += DMR_CYCLE_SAMPLES;
            if (!plausible || !hadRef || shift > 4 || shift < -4)
                fprintf(stderr, "DMR: TX timing report: tx sample %u on air at rx sample %u (offset %+d)%s",
                        k, dmrRx.dataPtr + (uint32_t)offset, (int)offset,
                        plausible ? "" : " -- ignored, doesn't match this downlink");
            if (plausible && hadRef && (shift > 4 || shift < -4))
                fprintf(stderr, " -- RX stream had drifted %+d samples, re-anchored", (int)-shift);
            if (!plausible || !hadRef || shift > 4 || shift < -4)
                fprintf(stderr, "\n");
        }
        else if (memcmp(type, TYPE_SAMPLE, typeLen) == 0)
        {
            packetType = PACKET_TYPE_SAMP;
            dmrRxNoteSamplePacket(dmrNowNs(), dmrRx.dataPtr + (uint32_t)((respLen - 8) / 2));
            q15_t in[2];
            for (uint16_t i = 8; i < respLen; i = i + 4)
            {
                memcpy(in, buffer + i, 4);
                q31_t res1 = in[0] * (50 * 128);
                in[0] = __SSAT((res1 >> 15), 16);
                res1 = in[1] * (50 * 128);
                in[1] = __SSAT((res1 >> 15), 16);

                if (USE_DC_FILTER)
                {
                    q31_t q31Samples[2];
                    arm_q15_to_q31(in, q31Samples, 2);

                    q31_t dcValues[2];
                    arm_biquad_cascade_df1_q31(&dcFilter, q31Samples, dcValues,
                                               2);

                    q31_t dcLevel = 0;
                    for (uint8_t i = 0U; i < 2; i++) dcLevel += dcValues[i];
                    dcLevel /= 2;

                    q15_t offset = (q15_t)(__SSAT((dcLevel >> 16), 16));

                    q15_t dcSamples[2];
                    for (uint8_t i = 0U; i < 2; i++)
                        dcSamples[i] = in[i] - offset;

                    dmrRxSamples(dcSamples, 2);
                }
                else
                {
                    dmrRxSamples(in, 2);
                }
            }
        }
        else if (memcmp(type, TYPE_BITS, typeLen) == 0)
        {
            packetType = PACKET_TYPE_BIT;
            dmrRxBits(buffer + 8, respLen - 8);
            dmrSlotWatchdog();
        }
        else if (memcmp(type, "DMD", 3) == 0)
        {
            pthread_mutex_lock(&rxBufMutex);
            RingBuffer_addData(&rxBuffer, buffer, respLen);
            pthread_mutex_unlock(&rxBufMutex);
        }
        else
        {
            //     if (debugM)
            hexDump((char*)"Host rec data", buffer, respLen);
        }
    }
    txOn = false;
    fprintf(stderr, "Disconnected from host.\n");
    // Close socket
    close(sockfd);
    connected = false;
    pthread_exit(NULL);
    return 0;
}

/* A call from the network is open on a slot from its first frame until its
 * terminator.  The gateway can be unlinked, or stop, without sending the
 * terminator; the host would then stay keyed (and the dashboard in TX) for
 * ever. */
static bool     dmrNetOpen[2] = {false, false};
static uint64_t dmrNetMs[2]   = {0U, 0U};

/*
 * dmrNetEnd()
 *
 * Ends the network calls that have no terminator: a "lost" packet is handed
 * to decodeFrame() for the slot as if the gateway had sent it, so the call is
 * closed the normal way (modem told to stop, last call and history saved).
 *
 * Parameters:
 *   force - true to end the open calls now, false to end only those that have
 *           had no frame from the gateway for 1.5 seconds.
 *
 * Returns: nothing.
 */
static void dmrNetEnd(bool force)
{
    for (uint8_t slot = 0U; slot < 2U; slot++)
    {
        if (!dmrNetOpen[slot] || (!force && nowMs() - dmrNetMs[slot] <= 1500U))
            continue;

        dmrNetOpen[slot] = false;

        uint8_t lost[64];

        memset(lost, 0x00, sizeof(lost));
        lost[0] = 0x61;
        lost[1] = 0x00;
        lost[2] = 0x08;
        lost[3] = 0x04;
        memcpy(lost + 4, slot == 0U ? "DML1" : "DML2", 4);

        fprintf(stderr, "DMR: network call on slot %u ended, no terminator from the gateway\n", slot + 1U);
        decodeFrame(lost, 8, true);
    }
}

/*
 * processGatewaySocket()
 *
 * Thread that serves the connected DMR gateway.  It reads the gateway's
 * packets: NAK, connect, disconnect, command and status packets are handled,
 * and DMR frames (when DMR is the active mode) are decoded as network
 * traffic with decodeFrame().
 *
 * Parameters:
 *   arg - Socket of the gateway connection, passed as a pointer sized
 *         integer.
 *
 * Returns: Nothing useful; the thread ends by calling pthread_exit().
 */
void* processGatewaySocket(void* arg)
{
    int childfd      = (intptr_t)arg;
    ssize_t len      = 0;
    uint8_t offset   = 0;
    uint16_t respLen = 0;
    uint8_t typeLen  = 0;
    uint8_t buffer[BUFFER_SIZE];
    char gps[50] = "";

    dmrGWConnected = true;
    addGateway(modemName, "main", "DMR");
    // Un-keys the repeater if a gateway (re)connects mid-call -- say so,
    // since it would otherwise look like a de-key with no cause.
    if (txOn)
        fprintf(stderr, "DMR gateway connected while keyed: dropping txOn\n");
    txOn = false;

    pthread_t txid;
    int err = pthread_create(&(txid), NULL, &gwTxThread, (void*)(intptr_t)childfd);
    if (err != 0)
        reportError(modemName, "DMR", "Can't create GW TX thread :[%s]", strerror(err));
    else
    {
        if (debugM) fprintf(stderr, "GW TX thread created successfully\n");
    }

    fprintf(stderr, "Gateway connected.\n");

    while (1)
    {
        /* Wait for the gateway; when it is quiet, see whether a network call
         * has gone silent (the gateway was unlinked or stopped sending). */
        struct pollfd gwPoll = {childfd, POLLIN, 0};
        int           gwWait = poll(&gwPoll, 1, 200);

        if (gwWait == 0)
        {
            dmrNetEnd(false);
            continue;
        }
        if (gwWait < 0 && errno == EINTR)
            continue;

        int len = read(childfd, buffer, 1);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            error((char*)"ERROR: DMR gateway connection closed remotely.");
            break;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            delay(5);
        }

        if (len != 1)
        {
            reportError(modemName, "DMR", "DMR_Service: error when reading from DMR gateway, errno=%d", errno);
            close(childfd);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "DMR_Service: unknown byte from DMR gateway, 0x%02X\n", buffer[0]);
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
            hexDump((char*)"DMR Gateway data:", (uint8_t*)buffer, respLen);

        if (memcmp(type, TYPE_NACK, typeLen) == 0)
        {
            dmrReflConnected = false;
            char tmp[8];
            bzero(tmp, 8);
            memcpy(tmp, buffer + 4 + typeLen, 6);
            ackDashbCommand(modemName, "reflLinkDMR", "failed");
            setReflectorStatus(modemName, "DMR", (const char*)tmp, false);
        }
        else if (memcmp(type, TYPE_CONNECT, typeLen) == 0)
        {
            dmrReflConnected = true;
            ackDashbCommand(modemName, "reflLinkDMR", "success");
            char tmp[13];
            bzero(tmp, 13);
            strcpy(tmp, "BM ");
            memcpy(tmp + 3, buffer + 4 + typeLen, 9);
            setReflectorStatus(modemName, "DMR", (const char*)tmp, true);
        }
        else if (memcmp(type, TYPE_DISCONNECT, typeLen) == 0)
        {
            dmrReflConnected = false;
            dmrNetEnd(true);
            char tmp[10];
            bzero(tmp, 10);
            memcpy(tmp, buffer + 4 + typeLen, 9);
            ackDashbCommand(modemName, "reflLinkDMR", "unlinked");
            setReflectorStatus(modemName, "DMR", (const char*)tmp, false);
        }
        else if (memcmp(type, TYPE_COMMAND, typeLen) == 0)
        {
            ackDashbCommand(modemName, "updateConfDMR", "success");
        }
        else if (memcmp(type, TYPE_STATUS, typeLen) == 0)
        {
        }
        else if (memcmp(type, "DM", 2) == 0 && isActiveMode())
        {
            reflBusy = true;
            if (typeLen == 4 && (type[3] == '1' || type[3] == '2'))
            {
                const uint8_t netSlot = (uint8_t)(type[3] - '1');

                if (type[2] == 'L' || (respLen > 8 && buffer[8] == 0x42))
                    dmrNetOpen[netSlot] = false; /* lost, or the terminator */
                else
                {
                    dmrNetOpen[netSlot] = true;
                    dmrNetMs[netSlot]   = nowMs();
                }
            }
            decodeFrame(buffer, respLen, true);
        }
        delay(5);
    }
    fprintf(stderr, "Gateway disconnected.\n");
    dmrNetEnd(true);
    dmrGWConnected   = false;
    dmrReflConnected = false;
    delGateway(modemName, "main", "DMR");
    clearReflLinkStatus(modemName, "DMR");
    pthread_exit(NULL);
    return 0;
}

/*
 * startTCPServer()
 *
 * Thread that listens for the DMR gateway on the server port.  For each
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
    struct hostent* hostp; /* client host info */
    int childfd;           /* child socket */
    int sockFd;

    sockFd = socket(AF_INET, SOCK_STREAM, 0);

    setNoDelay(sockFd);
    if (sockFd < 0)
    {
        reportError(modemName, "DMR", "DMR_Service: error when creating the socket: %s", strerror(errno));
        exit(1);
    }

    /* setsockopt: Handy debugging trick that lets
     * us rerun the server immediately after we kill it;
     * otherwise we have to wait about 20 secs.
     * Eliminates "ERROR on binding: Address already in use" error.
     */
    optval = 1;
    setsockopt(sockFd, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char*)&serveraddr, sizeof(serveraddr));

    /* this is an Internet address */
    serveraddr.sin_family = AF_INET;

    /* let the system figure out our IP address */
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* this is the port we will listen on */
    serveraddr.sin_port = htons((unsigned short)serverPort);

    if (bind(sockFd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
    {
        reportError(modemName, "DMR", "DMR_Service: error when binding the socket to port %u: %s", serverPort, strerror(errno));
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
        childfd = accept(sockFd, (struct sockaddr*)&clientaddr, &clientlen);
        if (childfd >= 0)
            setNoDelay(childfd);
        if (childfd < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            reportError(modemName, "DMR", "accept failed: %s", strerror(errno));
            break;
        }

        if (childfd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;

        hostaddrp = inet_ntoa(clientaddr.sin_addr);
        if (hostaddrp == NULL)
        {
            reportError(modemName, "DMR", "Can't get the address of the client.");
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

        int err = 0;
        /*
                pthread_t txid;
                err = pthread_create(&(txid), NULL, &txThread, (void*)(intptr_t)childfd);
                if (err != 0)
                    reportError(modemName, "DMR", "Can't create tx thread :[%s]", strerror(err));
                else
                {
                    if (debugM) fprintf(stderr, "TX thread created successfully\n");
                }
        */

        pthread_t procid;
        err = pthread_create(&(procid), NULL, &processGatewaySocket, (void*)(intptr_t)childfd);
        if (err != 0)
        {
            reportError(modemName, "DMR", "Can't create gateway process thread :[%s]", strerror(err));
            continue;
        }
        else
        {
            dmrGWConnected = true;
            if (debugM)
                fprintf(stderr, "Gateway process thread created successfully\n");
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
 * modem and DMR settings from the database (ID, colour code, levels, slot
 * phase and delay, DMO preamble, TX hang), starts the modem host client and
 * the DMR TX thread, and then watches for configuration changes until the
 * program is told to stop.
 *
 * Parameters:
 *   argc - Number of command line arguments.
 *   argv - Command line arguments.
 *
 * Returns: 0 on exit.
 */
int main(int argc, char** argv)
{
    bool daemon = false;
    int ret;
    int c;

    while ((c = getopt(argc, argv, ":m:a:s:u:p:c:dxv")) != -1)
    {
        switch (c)
        {
            case 'm':
            {
                modemId = atoi(optarg);
                if (modemId < 1 || modemId > 10)
                {
                    reportError(modemName, "DMR", "Invalid modem number.");
                    return 1;
                }
                sprintf(modemName, "modem%d", modemId);
                clientPort = 18000 + modemId - 1;
                serverPort = 18400 + modemId - 1;
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
                fprintf(stdout, "DMR_host: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: DMR_host [-m modem_number (1-10)] [-a address] [-s dbserver] [-u dbuser] [-p dbpassword] [-c configfile] [-d] [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            reportError(modemName, "DMR", "DMR_host: error in fork(), exiting");
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
    RingBuffer_Init(&txBuffer, 800);
    RingBuffer_Init(&rxBuffer, 1600);
    RingBuffer_Init(&gwTxBuffer, 800);
    RingBuffer_Init(&gwCommand, 200);

    /* Mutexes statically initialized with PTHREAD_MUTEX_INITIALIZER */

    initTimers();
    if (getTimer("status", 2000) < 0) return 0;
    if (getTimer("dbComm", 2000) < 0) return 0;

    memset(dcState, 0x00U, 4U * sizeof(q31_t));
    dcFilter.numStages = DC_FILTER_STAGES;
    dcFilter.pState    = dcState;
    dcFilter.pCoeffs   = DC_FILTER;
    dcFilter.postShift = 0;

    memset(dmrModState, 0x00U, 16U * sizeof(q15_t));
    dmrModFilter.L           = DMR_RADIO_SYMBOL_LENGTH;
    dmrModFilter.phaseLength = DMR_RRC_FILTER_PHASE_LEN;
    dmrModFilter.pCoeffs     = DMR_RRC_FILTER;
    dmrModFilter.pState      = dmrModState;

    char tmp[15];
    readHostConfig(modemName, "config", "streamType", tmp);   /* published by the bridge: FRAME, BIT or SAMPLE */
    if (strcasecmp(tmp, "SAMPLE") == 0)
        packetType = PACKET_TYPE_SAMP;
    else if (strcasecmp(tmp, "BIT") == 0)
        packetType = PACKET_TYPE_BIT;
    else
        packetType = PACKET_TYPE_FRAME;

    readHostConfig(modemName, "DMR", "ID", tmp);
    if (strlen(tmp) == 0)
    {
        setHostConfig(modemName, "DMR", "txLevel", "input", "75");
        setHostConfig(modemName, "DMR", "rfPower", "input", "128");
        setHostConfig(modemName, "DMR", "ID", "input", "1");
        setHostConfig(modemName, "DMR", "BM_Port", "input", "62031");
        setHostConfig(modemName, "DMR", "BM_Address", "input", "74.91.114.19");
        setHostConfig(modemName, "DMR", "BM_Pass", "input", "pass123");
        setHostConfig(modemName, "DMR", "BM_Name", "input", "3102");
        setHostConfig(modemName, "DMR", "ColorCode", "input", "1");
        setHostConfig(modemName, "DMR", "slots", "input", "3");
    }

    readHostConfig(modemName, "main", "callsign", stationCall);
    readHostConfig(modemName, "config", "rxFrequency", modem_rxFrequency);
    readHostConfig(modemName, "config", "txFrequency", modem_txFrequency);

    readHostConfig(modemName, "DMR", "txLevel", tmp);
    txLevel = atoi(tmp);
    readHostConfig(modemName, "DMR", "rfPower", tmp);
    rfPower = atoi(tmp);

    readHostConfig(modemName, "DMR", "ColorCode", tmp);
    if (strlen(tmp) > 0)
        dmrColorCode = (uint8_t)atoi(tmp);

    // Where TS1 bursts land relative to our TX slot-1 timing, in ms (see
    // dmrRxSlotFromPhase()). Per-installation; re-measure with debug on.
    tmp[0] = '\0';
    readHostConfig(modemName, "DMR", "SlotPhase", tmp);
    if (strlen(tmp) > 0)
        dmrRxTs1PhaseNs = (int64_t)(atof(tmp) * 1000000.0);

    // Shift of the sample-exact TS1/TS2 windows, in samples (see
    // dmrTxRefValid); like MMDVM's DMR delay setting. Normally 0.
    tmp[0] = '\0';
    readHostConfig(modemName, "DMR", "SlotDelay", tmp);
    if (strlen(tmp) > 0)
        dmrSlotDelaySamples = (int32_t)atoi(tmp);

    // Simplex only: length of the tone sent before the first burst, in ms
    // (see dmrSendDmoPreamble()).
    tmp[0] = '\0';
    readHostConfig(modemName, "DMR", "DmoPreamble", tmp);
    if (strlen(tmp) > 0)
    {
        long ms = atol(tmp);
        dmrDmoPreambleMs = (ms < 0L) ? 0U : (ms > 600L) ? 600U : (uint32_t)ms;
    }

    // How long the downlink stays up after the last call ends (SAMPLE/BIT
    // mode), in seconds -- see dmrTxHangNs.
    tmp[0] = '\0';
    readHostConfig(modemName, "DMR", "TxHang", tmp);
    if (strlen(tmp) > 0)
        dmrTxHangDurNs = (int64_t)(atof(tmp) * 1000000000.0);

    /* Dual-slot TX scheduler setup: per-slot FIFOs that decodeDMR() feeds
     * and dmrTxThread() drains, plus the colour-code-stamped idle frame
     * dmrSendSlotBurst() falls back to when a slot's FIFO is empty (see
     * CDMRTX::setColorCode() -- DMR_IDLE_DATA here is byte-for-byte the
     * same as MMDVM's IDLE_DATA[], just with a 2-byte TAG_DATA prefix). */
    // Slot 1 holds 12 frames: in simplex the preamble (dmrSendDmoPreamble())
    // delays the first burst, and the frames the gateway sends meanwhile
    // wait here. Slot 2's stays at 4.
    RingBuffer_Init(&dmrTxFifo[0], DMR_FRAME_LENGTH_BYTES * 12);
    RingBuffer_Init(&dmrTxFifo[1], DMR_FRAME_LENGTH_BYTES * 4);
    memcpy(dmrIdleFrame, DMR_IDLE_DATA + 2, DMR_FRAME_LENGTH_BYTES);
    DMRSlotType_encode(dmrColorCode, DT_IDLE, dmrIdleFrame);

    /* dmrRx is a plain static struct, so it starts out zero-filled --
     * that leaves endPtr at 0 instead of DMR_RX_NOENDPTR. Since dataPtr
     * also starts at 0, dmrRxProcessSample()'s very first sample sees
     * dataPtr == endPtr and treats it as a completed burst before any
     * real sync has ever been found, computing the frame's start position
     * as endPtr - DMR_FRAME_LENGTH_SAMPLES_LOCAL (an unsigned underflow)
     * and reading dmrRx.buffer far past its 900-sample bounds -- a
     * segfault on literally the first demodulated sample the modem ever
     * sends. dmrRxReset() sets the same safe starting state
     * (endPtr = DMR_RX_NOENDPTR, state = DMR_RXS_NONE, ...) that an
     * explicit "= 0xFFFFFFFFU" initializer would have given it. */
    dmrRxReset();

    clearReflLinkStatus(modemName, "DMR");

    int err = pthread_create(&(modemHostid), NULL, &startClient, modemHost);
    if (err != 0)
    {
        reportError(modemName, "DMR", "Can't create modem host thread :[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM) fprintf(stderr, "Modem host thread created successfully\n");
    }

    err = pthread_create(&(dmrTxThreadHandle), NULL, &dmrTxThread, NULL);
    if (err != 0)
    {
        reportError(modemName, "DMR", "Can't create DMR TX scheduler thread :[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM) fprintf(stderr, "DMR TX scheduler thread created successfully\n");
    }

    while (connected)
    {
        if (isTimerTriggered("dbComm"))
        {
            char parameter[31];

            readDashbCommand(modemName, "updateConfDMR", parameter);
            if (strlen(parameter) > 0)
            {
                readHostConfig(modemName, "main", "callsign", stationCall);
                if (dmrGWConnected)
                {
                    uint8_t buf[9];
                    buf[0] = 0x61;
                    buf[1] = 0x00;
                    buf[2] = 0x09;
                    buf[3] = 0x04;
                    memcpy(buf + 4, TYPE_COMMAND, 4);
                    buf[8] = COMM_UPDATE_CONF;
                    pthread_mutex_lock(&gwCommBufMutex);
                    RingBuffer_addData(&gwCommand, buf, 9);
                    pthread_mutex_unlock(&gwCommBufMutex);
                }

                /*
                 * Acknowledge now rather than when the gateway answers, so the
                 * dashboard does not time out if the gateway is slow, busy
                 * logging in, or not running. The gateway re-reads its own
                 * config when it receives the queued command.
                 */
                ackDashbCommand(modemName, "updateConfDMR", "success");
            }

            if (dmrGWConnected)
            {
                char parameter[31];
                readDashbCommand(modemName, "reflLinkDMR", parameter);
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
                    pthread_mutex_lock(&gwCommBufMutex);
                    RingBuffer_addData(&gwCommand, buf, 8);
                    pthread_mutex_unlock(&gwCommBufMutex);
                    sleep(3);
                    resetTimer("dbComm", 0);
                    continue;
                }
                else if (!dmrReflConnected)
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
                            token = strtok(NULL, ",");
                            char module[2];
                            strcpy(module, token);
                            uint8_t buf[8];
                            buf[0] = 0x61;
                            buf[1] = 0x00;
                            buf[2] = 0x15;
                            buf[3] = 0x04;
                            memcpy(buf + 4, TYPE_CONNECT, 4);
                            pthread_mutex_lock(&gwCommBufMutex);
                            RingBuffer_addData(&gwCommand, buf, 8);
                            char callsign[9];
                            bzero(callsign, 9);
                            readHostConfig(modemName, "main", "callsign", callsign);
                            RingBuffer_addData(&gwCommand, (uint8_t*)callsign, 6);
                            RingBuffer_addData(&gwCommand, (uint8_t*)name, 7);
                            pthread_mutex_unlock(&gwCommBufMutex);
                            sleep(3);
                        }
                        else
                            ackDashbCommand(modemName, "reflLinkDMR", "failed");
                    }
                    else
                        ackDashbCommand(modemName, "reflLinkDMR", "failed");
                }
                else
                    ackDashbCommand(modemName, "reflLinkDMR", "failed");
            }
            else
            {
                char parameter[31];
                readDashbCommand(modemName, "reflLinkDMR", parameter);
                if (strlen(parameter) > 0)
                {
                    ackDashbCommand(modemName, "reflLinkDMR", "No gateway");
                }
            }
            resetTimer("dbComm", 0);
        }
        delay(500000);
    }

    /* Cleanup RingBuffers */
    RingBuffer_Destroy(&txBuffer);
    RingBuffer_Destroy(&rxBuffer);
    RingBuffer_Destroy(&gwTxBuffer);
    RingBuffer_Destroy(&gwCommand);

    clearReflLinkStatus(modemName, "DMR");
    fprintf(stderr, "DMR Host terminated.\n");
    logError(modemName, "main", "DMR Host terminated.");
    return 0;
}
