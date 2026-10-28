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

#ifndef NXDN_FUNC_H
#define NXDN_FUNC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXDN_FRAME_LENGTH_BITS      384U
#define NXDN_FRAME_LENGTH_BYTES     48U
#define NXDN_FRAME_LENGTH_SYMBOLS   192U

#define NXDN_FSW_LENGTH_BITS        20U
#define NXDN_FSW_LENGTH_SYMBOLS     10U
#define NXDN_LICH_LENGTH_BITS       16U
#define NXDN_SACCH_LENGTH_BITS      60U
#define NXDN_FACCH1_LENGTH_BITS     144U
#define NXDN_FACCH2_LENGTH_BITS     348U

#define NXDN_FSW_LICH_SACCH_LENGTH_BITS   (NXDN_FSW_LENGTH_BITS + NXDN_LICH_LENGTH_BITS + NXDN_SACCH_LENGTH_BITS)
#define NXDN_FSW_LICH_SACCH_LENGTH_BYTES  (NXDN_FSW_LICH_SACCH_LENGTH_BITS / 8U)

#define NXDN_FACCH1_OFFSET_1        (NXDN_FSW_LICH_SACCH_LENGTH_BITS)
#define NXDN_FACCH1_OFFSET_2        (NXDN_FSW_LICH_SACCH_LENGTH_BITS + NXDN_FACCH1_LENGTH_BITS)

#define NXDN_LICH_RFCT_RCCH         0U
#define NXDN_LICH_RFCT_RTCH         1U
#define NXDN_LICH_RFCT_RDCH         2U
#define NXDN_LICH_RFCT_RTCH_C       3U

#define NXDN_LICH_USC_SACCH_NS      0U
#define NXDN_LICH_USC_UDCH          1U
#define NXDN_LICH_USC_SACCH_SS      2U
#define NXDN_LICH_USC_SACCH_IDLE    3U

#define NXDN_LICH_STEAL_NONE        3U
#define NXDN_LICH_STEAL_FACCH1_2    2U
#define NXDN_LICH_STEAL_FACCH1_1    1U
#define NXDN_LICH_STEAL_FACCH       0U

#define NXDN_LICH_DIRECTION_INBOUND   0U
#define NXDN_LICH_DIRECTION_OUTBOUND  1U

#define NXDN_SR_SINGLE              0U
#define NXDN_SR_4_4                 0U
#define NXDN_SR_3_4                 1U
#define NXDN_SR_2_4                 2U
#define NXDN_SR_1_4                 3U

#define NXDN_MESSAGE_TYPE_VCALL           0x01U
#define NXDN_MESSAGE_TYPE_VCALL_IV        0x03U
#define NXDN_MESSAGE_TYPE_DCALL_HDR       0x09U
#define NXDN_MESSAGE_TYPE_DCALL_DATA      0x0BU
#define NXDN_MESSAGE_TYPE_DCALL_ACK       0x0CU
#define NXDN_MESSAGE_TYPE_TX_REL          0x08U
#define NXDN_MESSAGE_TYPE_HEAD_DLY        0x0FU
#define NXDN_MESSAGE_TYPE_IDLE            0x10U

/* Messages carried between this host and the NXDN gateway (netData[40]) */
#define NXDN_NET_VOICE_HEADER       0U
#define NXDN_NET_VOICE_BODY         1U
#define NXDN_NET_VOICE_TRAILER      2U
#define NXDN_NET_DATA_HEADER        3U
#define NXDN_NET_DATA_BODY          4U
#define NXDN_NET_DATA_TRAILER       5U

#define NXDN_NET_DATA_LENGTH        40U

/*
 * Low level channel coding.  All "frame" arguments are the 48 byte (384 bit)
 * NXDN frame; the scrambler is NOT applied by these functions.
 */
void nxdnScramble(uint8_t* frame);
void nxdnAddSync(uint8_t* frame);
bool nxdnHasSync(const uint8_t* frame);

uint8_t nxdnCrc6(const uint8_t* in, unsigned int lengthBits);
uint16_t nxdnCrc12(const uint8_t* in, unsigned int lengthBits);
uint16_t nxdnCrc15(const uint8_t* in, unsigned int lengthBits);

bool nxdnLichDecode(const uint8_t* frame, uint8_t* lich);
void nxdnLichEncode(uint8_t lich, uint8_t* frame);
uint8_t nxdnLichParity(uint8_t lich);
#define NXDN_LICH_RFCT(l)       (((l) >> 6) & 0x03U)
#define NXDN_LICH_FCT(l)        (((l) >> 4) & 0x03U)
#define NXDN_LICH_OPTION(l)     (((l) >> 2) & 0x03U)
#define NXDN_LICH_DIRECTION(l)  (((l) >> 1) & 0x01U)
uint8_t nxdnLichMake(uint8_t rfct, uint8_t fct, uint8_t option, uint8_t direction);

/* sacch[5]: byte 0 = structure(2) | RAN(6), then 18 data bits, then CRC6 */
bool nxdnSacchDecode(const uint8_t* frame, uint8_t* sacch);
void nxdnSacchEncode(const uint8_t* sacch, uint8_t* frame);
void nxdnSacchGetData(const uint8_t* sacch, uint8_t* data);
void nxdnSacchSetData(uint8_t* sacch, const uint8_t* data);
void nxdnSacchGetRaw(const uint8_t* sacch, uint8_t* raw4);
#define NXDN_SACCH_RAN(s)        ((s)[0] & 0x3FU)
#define NXDN_SACCH_STRUCTURE(s)  (((s)[0] >> 6) & 0x03U)
void nxdnSacchSetRAN(uint8_t* sacch, uint8_t ran);
void nxdnSacchSetStructure(uint8_t* sacch, uint8_t structure);

/* facch[12]: 80 data bits followed by CRC12 */
bool nxdnFacch1Decode(const uint8_t* frame, unsigned int offset, uint8_t* facch);
void nxdnFacch1Encode(const uint8_t* facch10, uint8_t* frame, unsigned int offset);
void nxdnFacch1GetRaw(const uint8_t* facch10, uint8_t* raw12);

/* udch[25]: RAN byte, 22 bytes of layer 3, then CRC15 */
bool nxdnUdchDecode(const uint8_t* frame, uint8_t* udch);
void nxdnUdchEncode(const uint8_t* udch23, uint8_t* frame);

/*
 * Voice.  A voice block is 72 bits on air (bytes at frame + 12 + n * 9) and
 * 49 bits (12 + 12 + 25) in the network form.  The decode returns the number
 * of corrected channel bits (out of 47).
 */
unsigned int nxdnAudioDecodeBlock(const uint8_t* in, uint8_t* out, unsigned int offset);
void nxdnAudioEncodeBlock(const uint8_t* in, uint8_t* out, unsigned int offset);
/*
 * Vocoder chip (AMBE3000R at its 72 bit rate) block, 9 bytes, to or from the 49 bit
 * network form in 7 bytes.  The chip block carries no FEC parity.
 */
void nxdnAudioChipToNet49(const uint8_t* chip, uint8_t* net7);
void nxdnAudioNet49ToChip(const uint8_t* net7, uint8_t* chip);
unsigned int nxdnAudioChipPadding(const uint8_t* chip);

/*
 * The same with the FEC of the chip on.  'layout' is how the chip arranges the 72 bit block:
 *   NXDN_CHIP_NOFEC   49 bits, no parity (nxdnAudioChipToNet49), returns the number of set padding bits
 *   NXDN_CHIP_AIR     the block as on the air, returns the number of corrected bits
 *   NXDN_CHIP_CODING  24 + 23 + 25 bits in coding order, returns the number of corrected bits
 */
#define NXDN_CHIP_NOFEC   0U
#define NXDN_CHIP_AIR     1U
#define NXDN_CHIP_CODING  2U
unsigned int nxdnAudioChipDecode(unsigned int layout, const uint8_t* chip, uint8_t* net7);
void nxdnAudioChipEncode(unsigned int layout, const uint8_t* net7, uint8_t* chip);
void nxdnAudioDecode(const uint8_t* in, uint8_t* out, unsigned int* errors);
void nxdnAudioEncode(const uint8_t* in, uint8_t* out);

/* Layer 3 (22 bytes) */
void nxdnL3Decode(uint8_t* l3, const uint8_t* bytes, unsigned int lengthBits, unsigned int offset);
#define NXDN_L3_TYPE(l)    ((l)[0] & 0x3FU)
#define NXDN_L3_SRC(l)     ((uint16_t)(((l)[3] << 8) | (l)[4]))
#define NXDN_L3_DST(l)     ((uint16_t)(((l)[5] << 8) | (l)[6]))
#define NXDN_L3_GROUP(l)   ((((l)[2] >> 5) & 0x07U) <= 1U)   /* call type: broadcast or group */
#define NXDN_L3_BLOCKS(l)  (((l)[8] & 0x0FU) + 1U)

/*
 * Control logic (the NXDNControl of MMDVMHost without the modem, timers and
 * network classes).  RF frames come in with nxdnCtrlRf(), network data with
 * nxdnCtrlNet(); results leave through the callbacks.
 */
typedef enum
{
    NXDN_RF_LISTENING = 0,
    NXDN_RF_AUDIO,
    NXDN_RF_DATA,
    NXDN_RF_REJECTED
} NxdnRfState;

typedef enum
{
    NXDN_NET_IDLE = 0,
    NXDN_NET_AUDIO,
    NXDN_NET_DATA
} NxdnNetState;

typedef enum
{
    NXDN_EV_RF_START = 1,       /* header or late entry                */
    NXDN_EV_RF_LATE,
    NXDN_EV_RF_END,             /* normal end of transmission          */
    NXDN_EV_RF_LOST,            /* signal lost                         */
    NXDN_EV_RF_REJECTED,        /* selfOnly rejection                  */
    NXDN_EV_NET_START,
    NXDN_EV_NET_END,
    NXDN_EV_NET_LOST            /* network watchdog                    */
} NxdnEvent;

typedef struct
{
    NxdnEvent event;
    uint16_t src;
    uint16_t dst;
    bool group;
    bool data;
    float seconds;
    float ber;                  /* percent, RF only                    */
} NxdnCall;

typedef struct NxdnCtrl NxdnCtrl;

typedef void (*NxdnNetFn)(void* ctx, const uint8_t* netData, uint8_t type);
typedef void (*NxdnRfFn)(void* ctx, const uint8_t* frame, bool eot);
typedef void (*NxdnEventFn)(void* ctx, const NxdnCall* call);

struct NxdnCtrl
{
    uint8_t ran;
    uint16_t id;
    bool selfOnly;
    bool remoteGateway;
    bool duplex;                /* repeater: LICH direction outbound   */
    bool localRepeat;           /* queue received RF frames for TX     */
    uint32_t rfTimeoutMs;
    uint32_t netWatchdogMs;

    NxdnNetFn sendNet;
    NxdnRfFn sendRf;
    NxdnEventFn event;
    void* ctx;

    NxdnRfState rfState;
    NxdnNetState netState;
    uint8_t rfMask;
    uint8_t netMask;
    uint8_t rfLayer3[22];
    uint8_t netLayer3[22];
    uint8_t lastLich;
    uint32_t rfFrames;
    uint32_t rfErrs;
    uint32_t rfBits;
    uint32_t netFrames;
    uint32_t rfTimer;
    bool rfTimerRunning;
    uint32_t netTimer;
    uint32_t netWatchdog;
    bool netWatchdogRunning;
};

void nxdnCtrlInit(NxdnCtrl* c);
void nxdnCtrlReset(NxdnCtrl* c);
void nxdnCtrlRf(NxdnCtrl* c, const uint8_t* frame, bool lost);
void nxdnCtrlNet(NxdnCtrl* c, const uint8_t* netData);
void nxdnCtrlClock(NxdnCtrl* c, uint32_t ms);
bool nxdnCtrlBusy(const NxdnCtrl* c);

#ifdef __cplusplus
}
#endif

#endif
