/* Created by Rick KD0OSS */
/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef YSF_FUNC_H
#define YSF_FUNC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Yaesu System Fusion, 9600 bit/s C4FM: 4800 baud, 100 ms frames of 480
 * symbols (960 bits, 120 bytes).  A frame is the 40 bit sync word, the 100 bit
 * FICH (coded with a Golay code and a rate 1/2 convolutional code, then
 * interleaved) and five 180 bit channels of two halves each; the 18 bytes of a
 * channel are 9 bytes of the first half (CSD1 or the data channel) and 9 bytes
 * of the second half (CSD2 or the voice channel).
 */
#define YSF_FRAME_LENGTH_BYTES      120U
#define YSF_FRAME_LENGTH_BITS       960U
#define YSF_FRAME_LENGTH_SYMBOLS    480U
#define YSF_FRAME_TIME_MS           100U

#define YSF_SYNC_LENGTH_BYTES       5U
#define YSF_SYNC_LENGTH_BITS        40U
#define YSF_SYNC_LENGTH_SYMBOLS     20U
#define YSF_FICH_LENGTH_BYTES       25U
#define YSF_CALLSIGN_LENGTH         10U

#define YSF_FI_HEADER               0U
#define YSF_FI_COMMUNICATIONS       1U
#define YSF_FI_TERMINATOR           2U
#define YSF_FI_TEST                 3U

#define YSF_DT_VD_MODE1             0U
#define YSF_DT_DATA_FR_MODE         1U
#define YSF_DT_VD_MODE2             2U
#define YSF_DT_VOICE_FR_MODE        3U

#define YSF_CM_GROUP1               0U
#define YSF_CM_GROUP2               1U
#define YSF_CM_INDIVIDUAL           3U

#define YSF_MR_DIRECT               0U
#define YSF_MR_NOT_BUSY             1U
#define YSF_MR_BUSY                 2U

/*
 * Reflector packets (YSFReflector / YSFGateway): "YSFD" gateway(10) source(10)
 * destination(10) counter(1) frame(120) is 155 bytes; the counter byte is the
 * frame number in bits 7..1 and the end of transmission flag in bit 0.
 * Between this host and the YSF gateway the packet travels without the "YSFD"
 * (a body of 151 bytes).
 */
#define YSF_NET_PACKET_LENGTH       155U
#define YSF_NET_BODY_LENGTH         151U
#define YSF_NET_POLL_LENGTH         14U

/* ------------------------------------------------------------------------ */
/* Frame level coding                                                        */
/* ------------------------------------------------------------------------ */

void ysfAddSync(uint8_t* frame);
bool ysfHasSync(const uint8_t* frame, unsigned int maxErrors);

/* The rate 1/2 constraint length 5 convolutional code of the FICH and the data channels */
void ysfConvEncode(const uint8_t* in, uint8_t* out, unsigned int nBits);

/*
 * FICH.  The six bytes are the FICH as sent: byte 0 FI(2) CS(2) CM(2) BN(2),
 * byte 1 BT(2) FN(3) FT(3), byte 2 Rv(1) Dev(1) MR(3) VoIP(1) DT(2) (the bit
 * layout of MMDVMHost), byte 3 SQL(1) DGId(7), then the CRC.
 */
typedef struct
{
    uint8_t b[6];
} YsfFich;

void ysfFichInit(YsfFich* f);
bool ysfFichDecode(YsfFich* f, const uint8_t* frame);
void ysfFichEncode(YsfFich* f, uint8_t* frame);

uint8_t ysfFichGetFI(const YsfFich* f);
uint8_t ysfFichGetCM(const YsfFich* f);
uint8_t ysfFichGetBN(const YsfFich* f);
uint8_t ysfFichGetBT(const YsfFich* f);
uint8_t ysfFichGetFN(const YsfFich* f);
uint8_t ysfFichGetFT(const YsfFich* f);
uint8_t ysfFichGetDT(const YsfFich* f);
uint8_t ysfFichGetMR(const YsfFich* f);
bool    ysfFichGetDev(const YsfFich* f);
uint8_t ysfFichGetDGId(const YsfFich* f);

void ysfFichSetFI(YsfFich* f, uint8_t fi);
void ysfFichSetCM(YsfFich* f, uint8_t cm);
void ysfFichSetBN(YsfFich* f, uint8_t bn);
void ysfFichSetBT(YsfFich* f, uint8_t bt);
void ysfFichSetFN(YsfFich* f, uint8_t fn);
void ysfFichSetFT(YsfFich* f, uint8_t ft);
void ysfFichSetDT(YsfFich* f, uint8_t dt);
void ysfFichSetMR(YsfFich* f, uint8_t mr);
void ysfFichSetVoIP(YsfFich* f, bool on);
void ysfFichSetDev(YsfFich* f, bool on);
void ysfFichSetDGId(YsfFich* f, uint8_t id);

/* ------------------------------------------------------------------------ */
/* Payload: the callsigns carried in the data channels                       */
/* ------------------------------------------------------------------------ */

typedef struct
{
    uint8_t uplink[YSF_CALLSIGN_LENGTH];
    uint8_t downlink[YSF_CALLSIGN_LENGTH];
    uint8_t source[YSF_CALLSIGN_LENGTH];
    uint8_t dest[YSF_CALLSIGN_LENGTH];
    bool hasUplink;
    bool hasDownlink;
    bool hasSource;
    bool hasDest;
} YsfPayload;

void ysfPayloadInit(YsfPayload* p);
void ysfPayloadSetUplink(YsfPayload* p, const char* callsign);
void ysfPayloadSetDownlink(YsfPayload* p, const char* callsign);
void ysfPayloadReset(YsfPayload* p);

/*
 * The process functions work on a whole 120 byte frame.  They read the
 * callsigns of the call (the source and the destination), put the downlink and
 * uplink callsigns of this station into the second half of the header, and
 * re-code the channel.  They return true when the call's callsigns were found
 * in this frame (a valid header, or the first frame(s) of a voice call).
 * 'gateway' says that the frame came from a gateway that has already done this,
 * so the downlink and uplink are left alone.
 */
bool ysfPayloadHeader(YsfPayload* p, uint8_t* frame);
bool ysfPayloadVDMode1Data(YsfPayload* p, uint8_t* frame, uint8_t fn, bool gateway);
bool ysfPayloadVDMode2Data(YsfPayload* p, uint8_t* frame, uint8_t fn, bool gateway);
bool ysfPayloadDataFRData(YsfPayload* p, uint8_t* frame, uint8_t fn, bool gateway);
bool ysfPayloadVoiceFRData(YsfPayload* p, uint8_t* frame);

/* V/D mode 2 repeats each voice bit three times: majority vote, returns the number of corrected triplets */
unsigned int ysfPayloadVDMode2Audio(YsfPayload* p, uint8_t* frame);

/*
 * One 20 byte block of a data FR mode frame (Wires-X): half 0 is the first
 * data channel (CSD1), half 1 the second (CSD2).  The read returns false, and
 * spaces, when the CRC is bad.
 */
bool ysfPayloadReadCsd(const uint8_t* frame, unsigned int half, uint8_t* csd);
void ysfPayloadWriteCsd(uint8_t* frame, unsigned int half, const uint8_t* csd);

/* A header or terminator frame with the given 20 byte CSD1 (destination, source) and CSD2 (downlink, uplink) */
void ysfPayloadWriteHeader(uint8_t* frame, const uint8_t* csd1, const uint8_t* csd2);

/* ------------------------------------------------------------------------ */
/* Control                                                                   */
/* ------------------------------------------------------------------------ */

typedef enum
{
    YSF_RF_LISTENING = 0,
    YSF_RF_AUDIO,
    YSF_RF_DATA,
    YSF_RF_REJECTED
} YsfRfState;

typedef enum
{
    YSF_NET_IDLE = 0,
    YSF_NET_AUDIO
} YsfNetState;

typedef enum
{
    YSF_EV_RF_START = 1,        /* header or late entry                */
    YSF_EV_RF_LATE,
    YSF_EV_RF_END,
    YSF_EV_RF_LOST,
    YSF_EV_RF_REJECTED,
    YSF_EV_NET_START,
    YSF_EV_NET_END,
    YSF_EV_NET_LOST,
    YSF_EV_NET_CSD,             /* the callsigns inside the voice frames: source, dest; the packet's source is in 'gateway' */
    YSF_EV_NET_SWITCH           /* another station started to talk before the end of the call: that call is over */
} YsfEvent;

typedef struct
{
    YsfEvent event;
    char source[YSF_CALLSIGN_LENGTH + 1U];
    char dest[YSF_CALLSIGN_LENGTH + 1U];
    char gateway[YSF_CALLSIGN_LENGTH + 1U];     /* network calls: the gateway the call came from */
    uint8_t dgid;
    uint8_t dt;
    bool data;
    float seconds;
    float ber;                  /* percent, RF only */
} YsfCall;

typedef struct YsfCtrl YsfCtrl;

typedef void (*YsfNetFn)(void* ctx, const uint8_t* body, bool end);
typedef void (*YsfRfFn)(void* ctx, const uint8_t* frame, bool eot);
typedef void (*YsfEventFn)(void* ctx, const YsfCall* call);

struct YsfCtrl
{
    char callsign[YSF_CALLSIGN_LENGTH + 1U];    /* this station, padded with spaces */
    bool selfOnly;
    bool remoteGateway;
    bool lowDeviation;
    bool localRepeat;           /* queue received RF frames for TX */
    uint32_t rfTimeoutMs;
    uint32_t netWatchdogMs;

    YsfNetFn sendNet;
    YsfRfFn sendRf;
    YsfEventFn event;
    void* ctx;

    YsfRfState rfState;
    YsfNetState netState;
    YsfFich lastFich;
    YsfPayload rfPayload;
    YsfPayload netPayload;
    char rfSource[YSF_CALLSIGN_LENGTH + 1U];
    char rfDest[YSF_CALLSIGN_LENGTH + 1U];
    char netSource[YSF_CALLSIGN_LENGTH + 1U];
    char netDest[YSF_CALLSIGN_LENGTH + 1U];
    char netGateway[YSF_CALLSIGN_LENGTH + 1U];
    uint8_t rfDgid;
    uint8_t netDgid;
    uint8_t netDt;
    uint8_t netN;
    uint32_t rfFrames;
    uint32_t rfErrs;
    uint32_t rfBits;
    uint32_t netFrames;
    uint32_t netLost;
    bool netAnnounced;
    bool netCsdLogged;                          /* the callsigns inside the frames have been reported for this call */
    uint32_t rfTimer;
    bool rfTimerRunning;
    uint32_t netTimer;
    bool netTimerRunning;
    uint32_t netWatchdog;
    bool netHeldValid;                          /* a packet from another station, kept until the next one confirms it */
    uint8_t netHeld[YSF_NET_BODY_LENGTH];
    bool netWatchdogRunning;
};

void ysfCtrlInit(YsfCtrl* c);
void ysfCtrlSetCallsign(YsfCtrl* c, const char* callsign);
void ysfCtrlReset(YsfCtrl* c);

/* frame: the 120 bytes from the receiver; lost = true tells the control that the signal has gone */
void ysfCtrlRf(YsfCtrl* c, const uint8_t* frame, bool lost);

/* body: YSF_NET_BODY_LENGTH bytes from the gateway */
void ysfCtrlNet(YsfCtrl* c, const uint8_t* body);
void ysfCtrlClock(YsfCtrl* c, uint32_t ms);
bool ysfCtrlBusy(const YsfCtrl* c);

#ifdef __cplusplus
}
#endif

#endif
