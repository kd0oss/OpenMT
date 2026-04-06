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
#include <arm_math.h>
#include <arpa/inet.h>
#include <ctype.h>
//#include <dmr_emb.h>
#include <dmr_func.h>
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
#include <tools.h>
#include <unistd.h>

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
char slot1_call[9]       = "";
char slot2_call[9]       = "";
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

pthread_mutex_t timerMutex     = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t rxBufMutex     = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gwBufMutex     = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gwCommBufMutex = PTHREAD_MUTEX_INITIALIZER;

static const uint8_t SETMODE[] = {0x61, 0x00, 0x05, 0x01, COMM_SET_MODE};
static const uint8_t SETIDLE[] = {0x61, 0x00, 0x05, 0x01, COMM_SET_IDLE};

arm_biquad_casd_df1_inst_q31 dcFilter;
q31_t dcState[4];

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

void decodeFrame(uint8_t* buffer, uint8_t length, bool isNet);
void sendDataToModem(uint8_t* data, uint8_t length, uint8_t slot, bool isNet);
void sendCommToModem(uint8_t* data, uint8_t length);
void* startTCPServer(void* arg);

// error - wrapper for perror
void error(char* msg)
{
    perror(msg);
    exit(1);
}

// Wait for 'delay' miroseconds
void delay(uint32_t delay)
{
    struct timespec req, rem;
    req.tv_sec  = 0;
    req.tv_nsec = delay * 1000;
    nanosleep(&req, &rem);
};

// Print debug data.
void dump(char* text, unsigned char* data, unsigned int length)
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

    fprintf(stdout, "%s: %llu\n", text, milliseconds);

    while (length > 0U)
    {
        unsigned int bytes = (length > 16U) ? 16U : length;

        fprintf(stdout, "%04X:  ", offset);

        for (i = 0U; i < bytes; i++) fprintf(stdout, "%02X ", data[offset + i]);

        for (i = bytes; i < 16U; i++) fputs("   ", stdout);

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

    if (strcasecmp(active_mode, "DMR") == 0 || strcasecmp(active_mode, "IDLE") == 0)
        return true;
    else
        return false;
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
void* timerThread(void* arg)
{
    bool idle = true;

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
    if (getTimer("sendSlot", 30) < 0)
    {
        fprintf(stderr, "Failed to get timer. RX thread exited.\n");
        int iRet  = 500;
        connected = false;
        pthread_exit(&iRet);
        return NULL;
    }

    while (connected)
    {
        delay(500);

        if (isTimerTriggered("sendSlot"))
        {
            pthread_mutex_lock(&rxBufMutex);
            if (RingBuffer_dataSize(&rxBuffer) >= 5)
            {
                uint8_t buf[1];
                RingBuffer_peek(&rxBuffer, buf, 1);
                if (buf[0] != 0x61)
                {
                    fprintf(stderr, "RX invalid header.\n");
                    resetTimer("sendSlot");
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
                    resetTimer("sendSlot");
                    pthread_mutex_unlock(&rxBufMutex);
                    if (len == 5)
                    {
                        if (write(sockfd, buf, len) < 0)
                        {
                            fprintf(stderr, "ERROR: remote disconnect\n");
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
            resetTimer("sendSlot");
            pthread_mutex_unlock(&rxBufMutex);
        }
    }

    fprintf(stderr, "RX thread exited.\n");
    int iRet  = 500;
    connected = false;
    pthread_exit(&iRet);
    return NULL;
}

void* gwTxThread(void* arg)
{
    int sockfd = (intptr_t)arg;
    int8_t tId = getTimer("sendGW", 30);

    if (tId < 0)
    {
        fprintf(stderr, "Failed to get timer. GW TX thread exited.\n");
        int iRet  = 600;
        connected = false;
        pthread_exit(&iRet);
        return NULL;
    }

    while (dmrGWConnected)
    {
        delay(500);

        if (isTimerTriggered("sendGW"))
        {
            pthread_mutex_lock(&gwBufMutex);
            if (RingBuffer_dataSize(&gwTxBuffer) >= 5)
            {
                uint8_t buf[1];
                RingBuffer_peek(&gwTxBuffer, buf, 1);
                if (buf[0] != 0x61)
                {
                    fprintf(stderr, "GW TX invalid header.\n");
                    resetTimer("sendGW");
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
                    resetTimer("sendGW");
                    pthread_mutex_unlock(&gwBufMutex);
                    if (write(sockfd, buf, len) < 0)
                    {
                        fprintf(stderr, "ERROR: remote disconnect\n");
                        break;
                    }
                    continue;
                }
            }
            resetTimer("sendGW");
            pthread_mutex_unlock(&gwBufMutex);
        }
    }

    fprintf(stderr, "GW TX thread exited.\n");
    if (tId > 0)
        disableTimer(tId);
    int iRet  = 600;
    pthread_exit(&iRet);
    return NULL;
}

void sendDataToModem(uint8_t* data, uint8_t length, uint8_t slot, bool isNet)
{
    if (!isNet && !modem_duplex) return;

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

void sendCommToModem(uint8_t* data, uint8_t length)
{
    write(sockfd, data, length);
}

void decodeDMR(uint8_t* buffer, uint8_t length, bool isNet)
{
    static uint8_t activeSlots = 0;
    static uint8_t slotRow[2] = {1, 1};
    static bool isGroup[2] = {true, true};

    char cType[4] = "RF";
    if (isNet)
        strcpy(cType, "NET");

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
                        if (findDMRId(slot1_srcId, slot1_call, tmp))
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
                            sprintf(tmp, "%u", slot1_srcId);
                            char tmp1[11];
                            sprintf(tmp1, "TG %u", slot1_dstId);
                            saveLastCall(slotRow[0], modemName, "DMR TS1", cType, "N0CALL", tmp, tmp1, "", NULL, "", true);
                        }
                    }
                }
                else if (dataType == DT_TERMINATOR_WITH_LC)
                {
                    slotActive[0] = false;
                    if (!slotActive[1])
                    {
                        txOn = false;
                        uint8_t buf[2];
                        buf[0] = TYPE_DMR_START;
                        buf[1] = 0;
                        sendDataToModem(buf, 2, 1, isNet);
                    }
                    char tmp[11];
                    sprintf(tmp, "%u", slot1_srcId);
                    char tmp1[11];
                    sprintf(tmp1, "TG %u", slot1_dstId);
                    float loss_BER = 0.0f;
                    slot1_duration = difftime(time(NULL), slot1_starttime);
                    saveLastCall(slotRow[0], modemName, "DMR TS1", cType, slot1_call, tmp, tmp1, "", NULL, "", false);
                    saveHistory(modemName, "DMR TS1", cType, slot1_call, tmp, tmp1, loss_BER, "", slot1_duration);
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
            else if (audioSync || txOn)
            {
                txOn = true;
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
                        dump((char*)"DMR", data, 9);
                        if (flco == 0)
                        {
                            isGroup[0] = true;
                            slot1_srcId = DMRLC_getSrcId(0);
                            fprintf(stderr, "Slot 1 Voice: Src: %u ColorCode=%u PI=%d LCSS=%u\n", slot1_srcId, cc, pi, lcss);
                            slot1_dstId = DMRLC_getDstId(0);
                            char tmp[100];
                            bzero(tmp, 100);
                            if (findDMRId(slot1_srcId, slot1_call, tmp))
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

            if (txOn && slotActive[0])
            {
                uint8_t buf[34 + 1];
                buf[0] = TYPE_DMR_DATA1;
                memcpy(buf + 1, buffer + 8, 34);
                sendDataToModem(buf, 34 + 1, 1, isNet);
            }

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
        if (!slotActive[1])
        {
            txOn = false;
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
        float loss_BER = 0.0f;
        slot1_duration = difftime(time(NULL), slot1_starttime);
        saveLastCall(slotRow[0], modemName, "DMR TS1", cType, slot1_call, tmp, tmp1, "", NULL, "", false);
        saveHistory(modemName, "DMR TS1", cType, slot1_call, tmp, tmp1, loss_BER, "", slot1_duration);
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
                    findDMRId(slot2_srcId, slot2_call, tmp);
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
                slotActive[1] = false;
                if (!slotActive[0])
                {
                    txOn = false;
                    uint8_t buf[2];
                    buf[0] = TYPE_DMR_START;
                    buf[1] = 0;
                    sendDataToModem(buf, 2, 2, isNet);
                }
                char tmp[11];
                sprintf(tmp, "%u", slot2_srcId);
                char tmp1[11];
                sprintf(tmp1, "TG %u", slot2_dstId);
                float loss_BER = 0.0f;
                slot2_duration = difftime(time(NULL), slot2_starttime);
                saveLastCall(slotRow[1], modemName, "DMR TS2", cType, slot2_call, tmp, tmp1, "", NULL, "", false);
                saveHistory(modemName, "DMR TS2", cType, slot2_call, tmp, tmp1, loss_BER, "", slot2_duration);
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
                    dump((char*)"DMR", data, 9);
                    if (flco == 0)
                    {
                        isGroup[1] = true;
                        DMRLC_decode(data, 1);
                        slot2_srcId = DMRLC_getSrcId(1);
                        fprintf(stderr, "Slot 2 Voice: Src: %u ColorCode=%u PI=%d LCSS=%u\n", slot2_srcId, cc, pi, lcss);
                        slot2_dstId = DMRLC_getDstId(1);
                        char tmp[100];
                        bzero(tmp, 100);
                        if (findDMRId(slot2_srcId, slot2_call, tmp))
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
        {
            uint8_t buf[34 + 1];
            buf[0] = TYPE_DMR_DATA2;
            memcpy(buf + 1, buffer + 8, 34);
            sendDataToModem(buf, 34 + 1, 2, isNet);
        }

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
        if (!slotActive[0])
        {
            txOn = false;
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
        float loss_BER = 0.0f;
        slot2_duration = difftime(time(NULL), slot2_starttime);
        saveLastCall(slotRow[1], modemName, "DMR TS2", cType, slot2_call, tmp, tmp1, "", NULL, "", false);
        saveHistory(modemName, "DMR TS2", cType, slot2_call, tmp, tmp1, loss_BER, "", slot2_duration);
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

// Start up connection to modem host.
void* startClient(void* arg)
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
    {  // Connect to localhost
        perror("Invalid address/ Address not supported");
        exit(EXIT_FAILURE);
    }

    // Connect to the server
    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
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

    ssize_t len       = 0;
    uint16_t offset   = 0;
    uint16_t respLen  = 0;
    uint8_t typeLen   = 0;
    uint8_t configLen = 4 + 4 + 11 + 6 + 1 + 40 + 16 + 1 +
                        1;  // 1 + 1 + 1 + 1 + 12 + 1 + 1 + 1 + 1 + 15;

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

    // K=32
    uint32_t reg4 = (uint32_t)0b0100 << 0;  // register 4
    reg4 |= (uint32_t)0b011 << 4;           // mode, 4FSK
    reg4 |= (uint32_t)0b0 << 7;
    reg4 |= (uint32_t)0b11 << 8;
    reg4 |= (uint32_t)393U << 10;  // Disc BW
    reg4 |= (uint32_t)80U << 20;   // Post dem BW
    reg4 |= (uint32_t)0b10 << 30;  // IF filter (25 kHz)

    uint32_t reg2 = (uint32_t)0b10 << 28;  // invert data (and RC alpha = 0.5)
    reg2 |= (uint32_t)0b111 << 4;          // modulation (4FSK)

    uint32_t reg13 = (uint32_t)0b1101 << 0;         // register 13
    reg13 |= (uint32_t)ADF7021_SLICER_TH_DMR << 4;  // slicer threshold

    ifConf(bytes, reg2, reg3, reg4, reg10, reg13, atol(modem_rxFrequency),
           atol(modem_txFrequency), txLevel, rfPower, 0, true);

    memcpy(buffer + 26, bytes, 40);
    uint64_t tmp[2] = {DMR_MS_DATA_SYNC_BITS, DMR_SYNC_BITS_MASK};
    memcpy(buffer + 66, (uint8_t*)tmp, 16);
    buffer[82] = 0x8a;
    buffer[83] = 0x02;  // TX LSB fisrt / scan multiplier
    write(sockfd, buffer, configLen);

    dump((char*)"MODE", buffer, configLen);
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
                    "DMR_Service: error when reading from server, errno=%d\n",
                    errno);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "DMR_Service: unknown byte from server, 0x%02X\n",
                    buffer[0]);
            continue;
            ;
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
            dump((char*)"DMR host data", (unsigned char*)buffer, respLen);

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
                    arm_biquad_cascade_df1_q31(&dcFilter, q31Samples, dcValues,
                                               2);

                    q31_t dcLevel = 0;
                    for (uint8_t i = 0U; i < 2; i++) dcLevel += dcValues[i];
                    dcLevel /= 2;

                    q15_t offset = (q15_t)(__SSAT((dcLevel >> 16), 16));

                    q15_t dcSamples[2];
                    for (uint8_t i = 0U; i < 2; i++)
                        dcSamples[i] = in[i] - offset;
                    //           arm_fir_fast_q15(&rxGaussianFilter, dcSamples, smp, 2);
                }
                //       else
                //           arm_fir_fast_q15(&rxGaussianFilter, in, smp, 2);

                //        samples(smp, NULL, 2);
            }
            //       write(sockfd, buffer, respLen);
        }
        else if (memcmp(type, TYPE_BITS, typeLen) == 0)
        {
            packetType = PACKET_TYPE_BIT;
            //   processBits(buffer + 8, respLen - 8);
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
            dump((char*)"Host rec data", buffer, respLen);
        }
    }
    txOn = false;
    fprintf(stderr, "Disconnected from host.\n");
    // Close socket
    close(sockfd);
    connected = false;
    int iRet  = 100;
    pthread_exit(&iRet);
    return 0;
}

// Process bytes from gateway.
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
    txOn = false;

    pthread_t txid;
    int err = pthread_create(&(txid), NULL, &gwTxThread, (void*)(intptr_t)childfd);
    if (err != 0)
        fprintf(stderr, "Can't create GW TX thread :[%s]", strerror(err));
    else
    {
        if (debugM) fprintf(stderr, "GW TX thread created successfully\n");
    }

    fprintf(stderr, "Gateway connected.\n");

    while (1)
    {
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
            fprintf(stderr, "DMR_Service: error when reading from DMR gateway, errno=%d\n", errno);
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
            dump((char*)"DMR Gateway data:", (uint8_t*)buffer, respLen);

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
        //    ackDashbCommand(modemName, "reflLinkDMR", "success");
            char tmp[13];
            bzero(tmp, 13);
            strcpy(tmp, "BM ");
            memcpy(tmp + 3, buffer + 4 + typeLen, 9);
            setReflectorStatus(modemName, "DMR", (const char*)tmp, true);
        }
        else if (memcmp(type, TYPE_DISCONNECT, typeLen) == 0)
        {
            dmrReflConnected = false;
            char tmp[10];
            bzero(tmp, 10);
            memcpy(tmp, buffer + 4 + typeLen, 9);
            ackDashbCommand(modemName, "reflLinkDMR", "success");
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
            decodeFrame(buffer, respLen, true);
        }
        delay(5);
    }
    fprintf(stderr, "Gateway disconnected.\n");
    dmrGWConnected   = false;
    dmrReflConnected = false;
    delGateway(modemName, "main", "DMR");
    clearReflLinkStatus(modemName, "DMR");
    int iRet = 100;
    pthread_exit(&iRet);
    return 0;
}

// Listen for incoming gateway connection.
void* startTCPServer(void* arg)
{
    struct hostent* hostp; /* client host info */
    int childfd;           /* child socket */
    int sockFd;

    sockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd < 0)
    {
        fprintf(stderr, "DMR_Service: error when creating the socket: %s\n", strerror(errno));
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
        fprintf(stderr, "DMR_Service: error when binding the socket to port %u: %s\n", serverPort, strerror(errno));
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
        if (childfd < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            perror("accept failed");
            break;
        }

        if (childfd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;

        /*
         * gethostbyaddr: determine who sent the message
         */
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

        int err = 0;
        /*
                pthread_t txid;
                err = pthread_create(&(txid), NULL, &txThread, (void*)(intptr_t)childfd);
                if (err != 0)
                    fprintf(stderr, "Can't create tx thread :[%s]", strerror(err));
                else
                {
                    if (debugM) fprintf(stderr, "TX thread created successfully\n");
                }
        */

        pthread_t procid;
        err = pthread_create(&(procid), NULL, &processGatewaySocket, (void*)(intptr_t)childfd);
        if (err != 0)
        {
            fprintf(stderr, "Can't create gateway process thread :[%s]", strerror(err));
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
    int iRet = 100;
    pthread_exit(&iRet);
    return NULL;
}

int main(int argc, char** argv)
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
                clientPort = 18000 + modemId - 1;
                serverPort = 18400 + modemId - 1;
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
                fprintf(stdout, "DMR_host: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: DMR_host [-m modem_number (1-10)] [-d] [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            fprintf(stderr, "DMR_host: error in fork(), exiting\n");
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

    char tmp[15];
    readHostConfig(modemName, "config", "modem", tmp);
    if (strcasecmp(tmp, "openmt") == 0)
        packetType = PACKET_TYPE_SAMP;
    else if (strcasecmp(tmp, "openmths") == 0)
        packetType = PACKET_TYPE_BIT;
    else
        packetType = PACKET_TYPE_FRAME;

    readHostConfig(modemName, "DMR", "callsign", stationCall);
    if (strlen(stationCall) == 0)
    {
        setHostConfig(modemName, "DMR", "callsign", "input", "N0CALL");
        readHostConfig(modemName, "DMR", "callsign", stationCall);
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

    readHostConfig(modemName, "config", "rxFrequency", modem_rxFrequency);
    readHostConfig(modemName, "config", "txFrequency", modem_txFrequency);

    readHostConfig(modemName, "DMR", "txLevel", tmp);
    txLevel = atoi(tmp);
    readHostConfig(modemName, "DMR", "rfPower", tmp);
    rfPower = atoi(tmp);

    clearReflLinkStatus(modemName, "DMR");

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
        if (isTimerTriggered("dbComm"))
        {
            char parameter[31];

            readDashbCommand(modemName, "updateConfDMR", parameter);
            if (strlen(parameter) > 0)
            {
                readHostConfig(modemName, "main", "callsign", stationCall);
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

            if (dmrGWConnected)
            {
                char parameter[31];
                readDashbCommand(modemName, "reflLinkDMR", parameter);
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
                    pthread_mutex_lock(&gwCommBufMutex);
                    RingBuffer_addData(&gwCommand, buf, 8);
                    pthread_mutex_unlock(&gwCommBufMutex);
                    sleep(3);
                    resetTimer("dbComm");
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
            resetTimer("dbComm");
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
