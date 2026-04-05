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

#ifndef P25FUNC_H
#define P25FUNC_H

#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <arm_math.h>

static const uint8_t BIT_MASK_TABLE[] = { 0x80U, 0x40U, 0x20U, 0x10U, 0x08U, 0x04U, 0x02U, 0x01U };

#define WRITE_BIT(p,i,b) p[(i)>>3] = (b) ? (p[(i)>>3] | BIT_MASK_TABLE[(i)&7]) : (p[(i)>>3] & ~BIT_MASK_TABLE[(i)&7])
#define READ_BIT(p,i)   (p[(i)>>3] & BIT_MASK_TABLE[(i)&7])

#define P25_RADIO_SYMBOL_LENGTH 5U      // At 24 kHz sample rate

#define P25_HDR_FRAME_LENGTH_BYTES 99U
#define P25_HDR_FRAME_LENGTH_BITS P25_HDR_FRAME_LENGTH_BYTES * 8U
#define P25_HDR_FRAME_LENGTH_SYMBOLS P25_HDR_FRAME_LENGTH_BYTES * 4U
#define P25_HDR_FRAME_LENGTH_SAMPLES P25_HDR_FRAME_LENGTH_SYMBOLS * P25_RADIO_SYMBOL_LENGTH

#define P25_LDU_FRAME_LENGTH_BYTES 216U
#define P25_LDU_FRAME_LENGTH_BITS P25_LDU_FRAME_LENGTH_BYTES * 8U
#define P25_LDU_FRAME_LENGTH_SYMBOLS P25_LDU_FRAME_LENGTH_BYTES * 4U
#define P25_LDU_FRAME_LENGTH_SAMPLES P25_LDU_FRAME_LENGTH_SYMBOLS * P25_RADIO_SYMBOL_LENGTH

#define P25_TERMLC_FRAME_LENGTH_BYTES 54U
#define P25_TERMLC_FRAME_LENGTH_BITS P25_TERMLC_FRAME_LENGTH_BYTES * 8U
#define P25_TERMLC_FRAME_LENGTH_SYMBOLS P25_TERMLC_FRAME_LENGTH_BYTES * 4U
#define P25_TERMLC_FRAME_LENGTH_SAMPLES P25_TERMLC_FRAME_LENGTH_SYMBOLS * P25_RADIO_SYMBOL_LENGTH

#define P25_TERM_FRAME_LENGTH_BYTES 18U
#define P25_TERM_FRAME_LENGTH_BITS P25_TERM_FRAME_LENGTH_BYTES * 8U
#define P25_TERM_FRAME_LENGTH_SYMBOLS P25_TERM_FRAME_LENGTH_BYTES * 4U
#define P25_TERM_FRAME_LENGTH_SAMPLES P25_TERM_FRAME_LENGTH_SYMBOLS * P25_RADIO_SYMBOL_LENGTH

#define P25_TSDU_FRAME_LENGTH_BYTES 45U
#define P25_TSDU_FRAME_LENGTH_BITS P25_TSDU_FRAME_LENGTH_BYTES * 8U
#define P25_TSDU_FRAME_LENGTH_SYMBOLS P25_TSDU_FRAME_LENGTH_BYTES * 4U
#define P25_TSDU_FRAME_LENGTH_SAMPLES P25_TSDU_FRAME_LENGTH_SYMBOLS * P25_RADIO_SYMBOL_LENGTH

#define P25_PDU_HDR_FRAME_LENGTH_BYTES 45U
#define P25_PDU_HDR_FRAME_LENGTH_BITS P25_PDU_HDR_FRAME_LENGTH_BYTES * 8U
#define P25_PDU_HDR_FRAME_LENGTH_SYMBOLS P25_PDU_HDR_FRAME_LENGTH_BYTES * 4U
#define P25_PDU_HDR_FRAME_LENGTH_SAMPLES P25_PDU_HDR_FRAME_LENGTH_SYMBOLS * P25_RADIO_SYMBOL_LENGTH

#define P25_SYNC_LENGTH_BYTES 6U
#define P25_SYNC_LENGTH_BITS P25_SYNC_LENGTH_BYTES * 8U
#define P25_SYNC_LENGTH_SYMBOLS P25_SYNC_LENGTH_BYTES * 4U
#define P25_SYNC_LENGTH_SAMPLES P25_SYNC_LENGTH_SYMBOLS * P25_RADIO_SYMBOL_LENGTH

#define P25_NID_LENGTH_BYTES 8U
#define P25_NID_LENGTH_BITS P25_NID_LENGTH_BYTES * 8U
#define P25_NID_LENGTH_SYMBOLS P25_NID_LENGTH_BYTES * 4U
#define P25_NID_LENGTH_SAMPLES P25_NID_LENGTH_SYMBOLS * P25_RADIO_SYMBOL_LENGTH

static const uint8_t P25_SYNC_BYTES[] = {0x55U, 0x75U, 0xF5U, 0xFFU, 0x77U, 0xFFU};
static const uint8_t P25_SYNC_BYTES_LENGTH  = 6U;

static const uint64_t P25_SYNC_BITS      = 0x00005575F5FF77FFU;
static const uint64_t P25_SYNC_BITS_MASK = 0x0000FFFFFFFFFFFFU;

// 5     5      7     5      F     5      F     F      7     7      F     F
// 01 01 01 01  01 11 01 01  11 11 01 01  11 11 11 11  01 11 01 11  11 11 11 11
// +3 +3 +3 +3  +3 -3 +3 +3  -3 -3 +3 +3  -3 -3 -3 -3  +3 -3 +3 -3  -3 -3 -3 -3

static const int8_t P25_SYNC_SYMBOLS_VALUES[] = {+3, +3, +3, +3, +3, -3, +3, +3, -3, -3, +3, +3, -3, -3, -3, -3, +3, -3, +3, -3, -3, -3, -3, -3};

static const uint32_t P25_SYNC_SYMBOLS      = 0x00FB30A0U;
static const uint32_t P25_SYNC_SYMBOLS_MASK = 0x00FFFFFFU;

#define P25_DUID_HDU 0x00U  /*              // Header Data Unit */
#define P25_DUID_TDU 0x03U  /*              // Simple Terminator Data Unit */
#define P25_DUID_LDU1 0x05U  /*             // Logical Link Data Unit 1 */
#define P25_DUID_TSDU 0x07U  /*             // Trunking System Data Unit */
#define P25_DUID_LDU2 0x0AU  /*             // Logical Link Data Unit 2 */
#define P25_DUID_PDU 0x0CU  /*              // Packet Data Unit */
#define P25_DUID_TDULC 0x0FU  /*            // Terminator Data Unit with Link Control */
#define P25_DUID_HEADER 0x00U  /*  */
#define P25_DUID_TERM 0x03U  /*  */
#define P25_DUID_TERM_LC 0x0FU  /*  */

#define P25_MI_LENGTH_BYTES 9U

#define P25_LCF_GROUP   0x00U
#define P25_LCF_PRIVATE 0x03U

static const unsigned int  P25_SS0_START    = 70U;
static const unsigned int  P25_SS1_START    = 71U;
static const unsigned int  P25_SS_INCREMENT = 72U;

static const uint8_t P25_ALGO_UNENCRYPT = 0x80U;

static const uint8_t P25_NULL_IMBE[] = {0x04U, 0x0CU, 0xFDU, 0x7BU, 0xFBU, 0x7DU, 0xF2U, 0x7BU, 0x3DU, 0x9EU, 0x45U};

static const q15_t SCALING_FACTOR = 18750;      // Q15(0.57)

static const uint8_t CORRELATION_COUNTDOWN = 10U;//5U;

static const uint8_t MAX_SYNC_BIT_START_ERRS = 2U;
static const uint8_t MAX_SYNC_BIT_RUN_ERRS   = 4U;

static const uint8_t MAX_SYNC_SYMBOLS_ERRS = 2U;

static const uint8_t NOAVEPTR = 99U;

static const uint16_t NOENDPTR = 9999U;

#define MAX_NID_ERRS 5U

#define MAX_SYNC_FRAMES 4U + 1U

// One symbol boxcar filter
static const q15_t   BOXCAR5_FILTER[] = {12000, 12000, 12000, 12000, 12000, 0};
static const uint16_t BOXCAR5_FILTER_LEN = 6U;

// Generated using rcosdesign(0.2, 8, 5, 'normal') in MATLAB
// numTaps = 40, L = 5
static q15_t RC_0_2_FILTER[] = {-897, -1636, -1840, -1278, 0, 1613, 2936, 3310, 2315, 0, -3011, -5627, -6580, -4839,
                                0, 7482, 16311, 24651, 30607, 32767, 30607, 24651, 16311, 7482, 0, -4839, -6580, -5627,
                               -3011, 0, 2315, 3310, 2936, 1613, 0, -1278, -1840, -1636, -897, 0}; // numTaps = 40, L = 5
static const uint16_t RC_0_2_FILTER_PHASE_LEN = 8U; // phaseLength = numTaps/L

static const q15_t LOWPASS_FILTER[] = {124, -188, -682, 1262, 556, -621, -1912, -911, 2058, 3855, 1234, -4592, -7692, -2799,
                                8556, 18133, 18133, 8556, -2799, -7692, -4592, 1234, 3855, 2058, -911, -1912, -621,
                                556, 1262, -682, -188, 124};
static const uint16_t LOWPASS_FILTER_LEN = 32U;

static const q15_t P25_LEVELA =  1260;
static const q15_t P25_LEVELB =   420;
static const q15_t P25_LEVELC =  -420;
static const q15_t P25_LEVELD = -1260;

static const uint8_t P25_START_SYNC = 0x77U;

static const uint8_t DUMMY_HEADER[] = {
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x08U, 0xDCU, 0x60U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x93U, 0xE7U, 0x73U, 0x77U, 0x57U,
    0xD6U, 0xD3U, 0xCFU, 0x77U, 0xEEU, 0x82U, 0x93U, 0xE2U, 0x2FU, 0xF3U, 0xD5U,
    0xF5U, 0xBEU, 0xBCU, 0x54U, 0x0DU, 0x9CU, 0x29U, 0x3EU, 0x46U, 0xE3U, 0x28U,
    0xB0U, 0xB7U, 0x73U, 0x76U, 0x1EU, 0x26U, 0x0CU, 0x75U, 0x5BU, 0xF7U, 0x4DU,
    0x5FU, 0x5AU, 0x37U, 0x18U};

static const uint8_t DUMMY_LDU2[] = {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                              0x00U, 0x00U, 0x00U, 0x80U, 0x00U, 0x00U,
                              0xACU, 0xB8U, 0xA4U, 0x9BU, 0xDCU, 0x75U};

static const uint8_t CCS_PARITY[] = {
    0x00U, 0x39U, 0x72U, 0x4BU, 0xE4U, 0xDDU, 0x96U, 0xAFU, 0xF1U, 0xC8U, 0x83U,
    0xBAU, 0x15U, 0x2CU, 0x67U, 0x5EU, 0xDBU, 0xE2U, 0xA9U, 0x90U, 0x3FU, 0x06U,
    0x4DU, 0x74U, 0x2AU, 0x13U, 0x58U, 0x61U, 0xCEU, 0xF7U, 0xBCU, 0x85U, 0x8FU,
    0xB6U, 0xFDU, 0xC4U, 0x6BU, 0x52U, 0x19U, 0x20U, 0x7EU, 0x47U, 0x0CU, 0x35U,
    0x9AU, 0xA3U, 0xE8U, 0xD1U, 0x54U, 0x6DU, 0x26U, 0x1FU, 0xB0U, 0x89U, 0xC2U,
    0xFBU, 0xA5U, 0x9CU, 0xD7U, 0xEEU, 0x41U, 0x78U, 0x33U, 0x0AU, 0x27U, 0x1EU,
    0x55U, 0x6CU, 0xC3U, 0xFAU, 0xB1U, 0x88U, 0xD6U, 0xEFU, 0xA4U, 0x9DU, 0x32U,
    0x0BU, 0x40U, 0x79U, 0xFCU, 0xC5U, 0x8EU, 0xB7U, 0x18U, 0x21U, 0x6AU, 0x53U,
    0x0DU, 0x34U, 0x7FU, 0xE9U, 0xD0U, 0x9BU, 0xA2U, 0xA8U, 0x91U, 0xDAU, 0xE3U,
    0x4CU, 0x75U, 0x3EU, 0x07U, 0x59U, 0x60U, 0x2BU, 0x12U, 0xBDU, 0x84U, 0xCFU,
    0xF6U, 0x73U, 0x4AU, 0x01U, 0x38U, 0x97U, 0xAEU, 0xE5U, 0xDCU, 0x82U, 0xBBU,
    0xF0U, 0xC9U, 0x66U, 0x5FU, 0x14U, 0x2DU, 0x4EU, 0x77U, 0x3CU, 0x05U, 0xAAU,
    0x93U, 0xD8U, 0xE1U, 0xBFU, 0x86U, 0xCDU, 0xF4U, 0x5BU, 0x62U, 0x29U, 0x10U,
    0x95U, 0xACU, 0xE7U, 0xDEU, 0x71U, 0x48U, 0x03U, 0x3AU, 0x64U, 0x5DU, 0x16U,
    0x2FU, 0x80U, 0xB9U, 0xF2U, 0xCBU, 0xC1U, 0xF8U, 0xB3U, 0x8AU, 0x25U, 0x1CU,
    0x57U, 0x6EU, 0x30U, 0x09U, 0x42U, 0x7BU, 0xD4U, 0xEDU, 0xA6U, 0x9FU, 0x1AU,
    0x23U, 0x68U, 0x51U, 0xFEU, 0xC7U, 0x8CU, 0xB5U, 0xEBU, 0xD2U, 0x99U, 0xA0U,
    0x0FU, 0x36U, 0x7DU, 0x44U, 0x69U, 0x50U, 0x1BU, 0x22U, 0x8DU, 0xB4U, 0xFFU,
    0xC6U, 0x98U, 0xA1U, 0xEAU, 0xD3U, 0x7CU, 0x45U, 0x0EU, 0x37U, 0xB2U, 0x8BU,
    0xC0U, 0xF9U, 0x56U, 0x6FU, 0x24U, 0x1DU, 0x43U, 0x7AU, 0x31U, 0x08U, 0xA7U,
    0x9EU, 0xD5U, 0xECU, 0xE6U, 0xDFU, 0x94U, 0xADU, 0x02U, 0x3BU, 0x70U, 0x49U,
    0x17U, 0x2EU, 0x65U, 0x5CU, 0xF3U, 0xCAU, 0x81U, 0xB8U, 0x3DU, 0x04U, 0x4FU,
    0x76U, 0xD9U, 0xE0U, 0xABU, 0x92U, 0xCCU, 0xF5U, 0xBEU, 0x87U, 0x28U, 0x11U,
    0x5AU, 0x63U};

#define MAX_CCS_ERRS 4U

static const unsigned int IMBE_INTERLEAVE[] = {
    0,   7,   12,  19,  24,  31,  36,  43,  48,  55,  60,  67,  72,  79,  84,
    91,  96,  103, 108, 115, 120, 127, 132, 139, 1,   6,   13,  18,  25,  30,
    37,  42,  49,  54,  61,  66,  73,  78,  85,  90,  97,  102, 109, 114, 121,
    126, 133, 138, 2,   9,   14,  21,  26,  33,  38,  45,  50,  57,  62,  69,
    74,  81,  86,  93,  98,  105, 110, 117, 122, 129, 134, 141, 3,   8,   15,
    20,  27,  32,  39,  44,  51,  56,  63,  68,  75,  80,  87,  92,  99,  104,
    111, 116, 123, 128, 135, 140, 4,   11,  16,  23,  28,  35,  40,  47,  52,
    59,  64,  71,  76,  83,  88,  95,  100, 107, 112, 119, 124, 131, 136, 143,
    5,   10,  17,  22,  29,  34,  41,  46,  53,  58,  65,  70,  77,  82,  89,
    94,  101, 106, 113, 118, 125, 130, 137, 142};

static const unsigned char REC62[] = {0x62U, 0x02U, 0x02U, 0x0CU, 0x0BU, 0x12U,
                               0x64U, 0x00U, 0x00U, 0x80U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U};

static const unsigned char REC63[] = {0x63U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC64[] = {0x64U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC65[] = {0x65U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC66[] = {0x66U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC67[] = {0x67U, 0xF0U, 0x9DU, 0x6AU, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC68[] = {0x68U, 0x19U, 0xD4U, 0x26U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC69[] = {0x69U, 0xE0U, 0xEBU, 0x7BU, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC6A[] = {0x6AU, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U};

static const unsigned char REC6B[] = {0x6BU, 0x02U, 0x02U, 0x0CU, 0x0BU, 0x12U,
                               0x64U, 0x00U, 0x00U, 0x80U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U};

static const unsigned char REC6C[] = {0x6CU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC6D[] = {0x6DU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC6E[] = {0x6EU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC6F[] = {0x6FU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC70[] = {0x70U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC71[] = {0x71U, 0xACU, 0xB8U, 0xA4U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC72[] = {0x72U, 0x9BU, 0xDCU, 0x75U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

static const unsigned char REC73[] = {0x73U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U};

static const unsigned char REC80[] = {0x80U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                               0x00U, 0x00U, 0x00U, 0x00U, 0x00U};

extern uint8_t        p25mi[P25_MI_LENGTH_BYTES];
extern uint8_t        p25mfId;
extern uint8_t        p25algId;
extern uint8_t        p25lcf;
extern uint8_t        p25lastDUID;
extern bool           p25emergency;
extern unsigned int   p25kId;
extern unsigned int   p25srcId;
extern unsigned int   p25dstId;
extern uint8_t        p25duid;
extern uint8_t        p25lsd1;
extern uint8_t        p25lsd2;
extern uint8_t        p25hdr[P25_NID_LENGTH_BYTES];
extern uint8_t        p25ldu1[P25_NID_LENGTH_BYTES];
extern uint8_t        p25ldu2[P25_NID_LENGTH_BYTES];
extern uint8_t        p25termlc[P25_NID_LENGTH_BYTES];
extern uint8_t        p25term[P25_NID_LENGTH_BYTES];
extern unsigned int   p25nac;

void p25Reset();
void addSync(uint8_t* data);
void setBusyBits(uint8_t* data, unsigned int ssOffset, bool b1, bool b2);
void addBusyBits(uint8_t* data, unsigned int length, bool b1, bool b2);
void decode(const uint8_t* in, uint8_t* out, unsigned int start, unsigned int stop);
void encode(const uint8_t* in, uint8_t* out, unsigned int start, unsigned int stop);
unsigned int compare(const uint8_t* data1, const uint8_t* data2, unsigned int length);
uint8_t lowSpeedData_encode(uint8_t in);
void lowSpeedData_process(uint8_t* data);
void lowSpeedData_encode1(uint8_t* data);
uint8_t lowSpeedData_getLSD1();
void lowSpeedData_setLSD1(uint8_t lsd1);
uint8_t lowSpeedData_getLSD2();
void lowSpeedData_setLSD2(uint8_t lsd2);
void resetNID(unsigned int nac);
bool decodeNID(const uint8_t* data);
void encodeNID(uint8_t* data, uint8_t duid);
uint8_t getDUID();
void decodeLDUHamming(const uint8_t* data, uint8_t* raw);
void getMI(uint8_t* mi);
uint8_t getMFId();
uint8_t getAlgId();
unsigned int getKId();
unsigned int getSrcId();
bool getEmergency();
unsigned int getNAC();
uint8_t getLCF();
unsigned int getDstId();
bool decodeLDU1(const uint8_t* data);
bool decodeLDU2(const uint8_t* data);
void encodeLDU1(uint8_t* data);
void encodeLDU2(uint8_t* data);
void encodeHeader(uint8_t* data);
void setMI(const uint8_t* mi);
void setMFId(uint8_t id);
void setAlgId(uint8_t id);
void setKId(unsigned int id);
void setSrcId(unsigned int id);
void setEmergency(bool on);
void setLCF(uint8_t lcf);
void setDstId(unsigned int id);
void audip25encode(uint8_t* data, const uint8_t* imbe, unsigned int n);
void audip25decode(const uint8_t* data, uint8_t* imbe, unsigned int n);

#endif
