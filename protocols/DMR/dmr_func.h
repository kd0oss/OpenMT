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


#if !defined(DMR_func_H)
#define	DMR_func_H

#include <stdint.h>
#include <stdbool.h>

/* SLOTDATA structure for slot state */
typedef struct SLOTDATA
{
    bool PF;
    bool R;
    bool piFlag;  /* Renamed from PI to avoid conflict with math.h constant */
    uint8_t colorCode;
    uint8_t LCSS;
    unsigned int FLCO;
    bool FLCO_valid;
    unsigned char options;
    unsigned int CSBK;
    unsigned char FID;
    bool GI;
    unsigned int bsId;
    unsigned int srcId;
    unsigned int dstId;
    bool dataContent;
    unsigned char CBF;
    bool OVCM;
    bool rawData[195];
    bool deInterData[196];
    unsigned char tdata[12];
	bool EMB_raw[128];
	bool EMB_data[72];
} SLOTDATA;

extern SLOTDATA slotData[2];

static const unsigned char TAG_DATA   = 0x01U;

static const unsigned int DMR_FRAME_LENGTH_BITS  = 264U;
static const unsigned int DMR_FRAME_LENGTH_BYTES = 33U;

static const unsigned int DMR_SYNC_LENGTH_BITS  = 48U;
static const unsigned int DMR_SYNC_LENGTH_BYTES = 6U;

static const unsigned int DMR_EMB_LENGTH_BITS  = 8U;
static const unsigned int DMR_EMB_LENGTH_BYTES = 1U;

static const unsigned int DMR_SLOT_TYPE_LENGTH_BITS  = 8U;
static const unsigned int DMR_SLOT_TYPE_LENGTH_BYTES = 1U;

static const unsigned int DMR_EMBEDDED_SIGNALLING_LENGTH_BITS  = 32U;
static const unsigned int DMR_EMBEDDED_SIGNALLING_LENGTH_BYTES = 4U;

static const unsigned int DMR_AMBE_LENGTH_BITS  = 108U * 2U;
static const unsigned int DMR_AMBE_LENGTH_BYTES = 27U;

static const unsigned char BS_SOURCED_AUDIO_SYNC[]   = {0x07U, 0x55U, 0xFDU, 0x7DU, 0xF7U, 0x5FU, 0x70U};
static const unsigned char BS_SOURCED_DATA_SYNC[]    = {0x0DU, 0xFFU, 0x57U, 0xD7U, 0x5DU, 0xF5U, 0xD0U};

static const unsigned char MS_SOURCED_AUDIO_SYNC[]   = {0x07U, 0xF7U, 0xD5U, 0xDDU, 0x57U, 0xDFU, 0xD0U};
static const unsigned char MS_SOURCED_DATA_SYNC[]    = {0x0DU, 0x5DU, 0x7FU, 0x77U, 0xFDU, 0x75U, 0x70U};

static const unsigned char DIRECT_SLOT1_AUDIO_SYNC[] = {0x05U, 0xD5U, 0x77U, 0xF7U, 0x75U, 0x7FU, 0xF0U};
static const unsigned char DIRECT_SLOT1_DATA_SYNC[]  = {0x0FU, 0x7FU, 0xDDU, 0x5DU, 0xDFU, 0xD5U, 0x50U};

static const unsigned char DIRECT_SLOT2_AUDIO_SYNC[] = {0x07U, 0xDFU, 0xFDU, 0x5FU, 0x55U, 0xD5U, 0xF0U};
static const unsigned char DIRECT_SLOT2_DATA_SYNC[]  = {0x0DU, 0x75U, 0x57U, 0xF5U, 0xFFU, 0x7FU, 0x50U};

static const unsigned char SYNC_MASK[]               = {0x0FU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xF0U};

// The PR FILL and Data Sync pattern.
static const unsigned char DMR_IDLE_DATA[] = {
    TAG_DATA, 0x00U, 0x53U, 0xC2U, 0x5EU, 0xABU, 0xA8U, 0x67U, 0x1DU,
    0xC7U,    0x38U, 0x3BU, 0xD9U, 0x36U, 0x00U, 0x0DU, 0xFFU, 0x57U,
    0xD7U,    0x5DU, 0xF5U, 0xD0U, 0x03U, 0xF6U, 0xE4U, 0x65U, 0x17U,
    0x1BU,    0x48U, 0xCAU, 0x6DU, 0x4FU, 0xC6U, 0x10U, 0xB4U};

// A silence frame only
static const unsigned char DMR_SILENCE_DATA[] = {
    TAG_DATA, 0x00U, 0xB9U, 0xE8U, 0x81U, 0x52U, 0x61U, 0x73U, 0x00U,
    0x2AU,    0x6BU, 0xB9U, 0xE8U, 0x81U, 0x52U, 0x60U, 0x00U, 0x00U,
    0x00U,    0x00U, 0x00U, 0x01U, 0x73U, 0x00U, 0x2AU, 0x6BU, 0xB9U,
    0xE8U,    0x81U, 0x52U, 0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU};

static const unsigned char PAYLOAD_LEFT_MASK[]  = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
                                            0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
                                            0xFFU, 0xFFU, 0xFFU, 0xF0U};
static const unsigned char PAYLOAD_RIGHT_MASK[] = {0x0FU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
                                            0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
                                            0xFFU, 0xFFU, 0xFFU, 0xFFU};

static const unsigned char VOICE_LC_HEADER_CRC_MASK[]    = {0x96U, 0x96U, 0x96U};
static const unsigned char TERMINATOR_WITH_LC_CRC_MASK[] = {0x99U, 0x99U, 0x99U};
static const unsigned char PI_HEADER_CRC_MASK[]          = {0x69U, 0x69U};
static const unsigned char DATA_HEADER_CRC_MASK[]        = {0xCCU, 0xCCU};
static const unsigned char CSBK_CRC_MASK[]               = {0xA5U, 0xA5U};

static const unsigned int DMR_SLOT_TIME = 60U;
static const unsigned int AMBE_PER_SLOT = 3U;

#define DT_MASK 0x0FU
#define DT_VOICE_PI_HEADER 0x00U
#define DT_VOICE_LC_HEADER 0x01U
#define DT_TERMINATOR_WITH_LC 0x02U
#define DT_CSBK 0x03U
#define DT_DATA_HEADER 0x06U
#define DT_RATE_12_DATA 0x07U
#define DT_RATE_34_DATA 0x08U
#define DT_IDLE 0x09U
#define DT_RATE_1_DATA 0x0AU

// Dummy values
#define DT_VOICE_SYNC 0xF0U
#define DT_VOICE 0xF1U

static const unsigned char DMR_IDLE_RX    = 0x80U;
static const unsigned char DMR_SYNC_DATA  = 0x40U;
static const unsigned char DMR_SYNC_AUDIO = 0x20U;

static const unsigned char DMR_SLOT1 = 0x00U;
static const unsigned char DMR_SLOT2 = 0x80U;

static const unsigned char DPF_UDT              = 0x00U;
static const unsigned char DPF_RESPONSE         = 0x01U;
static const unsigned char DPF_UNCONFIRMED_DATA = 0x02U;
static const unsigned char DPF_CONFIRMED_DATA   = 0x03U;
static const unsigned char DPF_DEFINED_SHORT    = 0x0DU;
static const unsigned char DPF_DEFINED_RAW      = 0x0EU;
static const unsigned char DPF_PROPRIETARY      = 0x0FU;

static const unsigned char FID_ETSI = 0U;
static const unsigned char FID_DMRA = 16U;

enum FLCO
{
    FLCO_GROUP               = 0,
    FLCO_USER_USER           = 3,
    FLCO_TALKER_ALIAS_HEADER = 4,
    FLCO_TALKER_ALIAS_BLOCK1 = 5,
    FLCO_TALKER_ALIAS_BLOCK2 = 6,
    FLCO_TALKER_ALIAS_BLOCK3 = 7,
    FLCO_GPS_INFO            = 8
};

enum CSBKO
{
    CSBKO_NONE           = 0x00,
    CSBKO_UUVREQ         = 0x04,
    CSBKO_UUANSRSP       = 0x05,
    CSBKO_CTCSBK         = 0x07,
    CSBKO_CALL_ALERT     = 0x1F,
    CSBKO_CALL_ALERT_ACK = 0x20,
    CSBKO_RADIO_CHECK    = 0x24,
    CSBKO_NACKRSP        = 0x26,
    CSBKO_CALL_EMERGENCY = 0x27,
    CSBKO_BSDWNACT       = 0x38,
    CSBKO_PRECCSBK       = 0x3D
};

typedef enum LC_STATE
{
    LCS_NONE,
    LCS_FIRST,
    LCS_SECOND,
    LCS_THIRD
} LC_STATE;

bool CSBK_decode(const unsigned char* bytes, const uint8_t slot);

void CSBK_encode(unsigned char* bytes, const uint8_t slot);

// Generic fields
unsigned int CSBK_getCSBKO(const uint8_t slot);
unsigned char CSBK_getFID(const uint8_t slot);

// Set/Get the OVCM bit in the supported CSBKs
bool CSBK_getOVCM(const uint8_t slot);
void CSBK_setOVCM(bool ovcm, const uint8_t slot);

// For BS Dwn Act
unsigned int CSBK_getBSId(const uint8_t slot);

// For Pre
bool CSBK_getGI(const uint8_t slot);

unsigned int CSBK_getSrcId(const uint8_t slot);
unsigned int CSBK_getDstId(const uint8_t slot);

bool CSBK_getDataContent(const uint8_t slot);
unsigned char CSBK_getCBF(const uint8_t slot);

void CSBK_setCBF(unsigned char cbf, const uint8_t slot);

void BPTC19696_decode(const unsigned char* in, unsigned char* out, const uint8_t slot);

void BPTC19696_encode(const unsigned char* in, unsigned char* out, const uint8_t slot);

void BPTC19696_decodeExtractBinary(const unsigned char* in, const uint8_t slot);
void BPTC19696_decodeErrorCheck(const uint8_t slot);
void BPTC19696_decodeDeInterleave(const uint8_t slot);
void BPTC19696_decodeExtractData(unsigned char* data, const uint8_t slot);

void BPTC19696_encodeExtractData(const unsigned char* in, const uint8_t slot);
void BPTC19696_encodeInterleave(const uint8_t slot);
void BPTC19696_encodeErrorCheck(const uint8_t slot);
void BPTC19696_encodeExtractBinary(unsigned char* data, const uint8_t slot);

void DMRLC_encodeBytes(unsigned char* bytes, const uint8_t slot);
void DMRLC_encodeBits(bool* bits, const uint8_t slot);

bool DMRFullLC_decode(const unsigned char* data, unsigned char type, unsigned char* lcData, const uint8_t slot);

void DMRFullLC_encode(unsigned char* data, unsigned char type, const uint8_t slot);

void DMRLC_decode(const unsigned char* bytes, uint8_t slot);

bool DMRLC_getPF(const uint8_t slot);
void DMRLC_setPF(bool pf, const uint8_t slot);

unsigned int DMRLC_getFLCO(const uint8_t slot);
void DMRLC_setFLCO(unsigned int flco, const uint8_t slot);

bool DMRLC_getOVCM(const uint8_t slot);
void DMRLC_setOVCM(bool ovcm, const uint8_t slot);

unsigned char DMRLC_getFID(const uint8_t slot);
void DMRLC_setFID(unsigned char fid, const uint8_t slot);

unsigned int DMRLC_getSrcId(const uint8_t slot);
void DMRLC_setSrcId(unsigned int id, const uint8_t slot);

unsigned int DMRLC_getDstId(const uint8_t slot);
void DMRLC_setDstId(unsigned int id, const uint8_t slot);

void addDMRAudioSync(unsigned char* data, bool duplex);
void addDMRDataSync(unsigned char* data, bool duplex);

uint8_t getColorCode(const uint8_t slot);
void setColorCode(uint8_t code, const uint8_t slot);

bool getPI(const uint8_t slot);
void setPI(bool pi, const uint8_t slot);

uint8_t getLCSS(const uint8_t slot);
void setLCSS(uint8_t lcss, const uint8_t slot);

void DMREMB_decode(const unsigned char* data,  const uint8_t slot);
void DMREMB_encode(unsigned char* data, const uint8_t slot);

bool DMREmbeddedData_decode(const unsigned char* data, unsigned char lcss, const uint8_t slot);
bool DMREmbeddedData_getRawData(unsigned char* data, const uint8_t slot);

void decodeGPS(const unsigned char* data,  const uint8_t slot);

#endif
