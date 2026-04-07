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

#include <ADF7021.h>
#include <CCITTChecksumReverse.h>
#include <RingBuffer.h>
#include <arm_math.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
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
#include "dstar_gps.h"

/* External modemCommandBuffer from Modem_Host */
extern RingBuffer modemCommandBuffer;

#define VERSION "2025-09-13"
#define BUFFER_SIZE 500

q31_t DC_FILTER[]               = {3367972, 0, 3367972, 0, 2140747704, 0};  // {b0, 0, b1, b2, -a1, -a2}
const uint32_t DC_FILTER_STAGES = 1U;                        // One Biquad stage
bool USE_DC_FILTER              = true;

const int16_t TX_GAUSSIAN_0_35_FILTER[] = {0,     0,     0,     0,     1001, 3514, 9333, 18751,
                                           28499, 32767, 28499, 18751, 9333, 3514, 1001};    // numTaps = 15, L = 5
const uint8_t TX_GAUSSIAN_0_35_FILTER_PHASE_LEN = 3;  // phaseLength = numTaps/L

const uint8_t TX_FILTER_STATE_LEN               = 20;
const uint8_t TX_GAUSSIAN_0_35_FILTER_LEN       = 15;
const uint8_t TX_SYMBOL_LENGTH                  = 5;
const uint8_t RX_FILTER_STATE_LEN               = 40;

const int16_t RX_GAUSSIAN_0_5_FILTER[]   = {8,    104,  760, 3158, 7421, 9866,
                                            7421, 3158, 760, 104,  8,    0};
const uint8_t RX_GAUSSIAN_0_5_FILTER_LEN = 12;

#define DSTAR_RADIO_SYMBOL_LENGTH 5U  // At 24 kHz sample rate

#define DSTAR_HEADER_LENGTH_BYTES 41U

#define DSTAR_FEC_SECTION_LENGTH_BYTES 83U
#define DSTAR_FEC_SECTION_LENGTH_SYMBOLS 660U
#define DSTAR_FEC_SECTION_LENGTH_SAMPLES DSTAR_FEC_SECTION_LENGTH_SYMBOLS * DSTAR_RADIO_SYMBOL_LENGTH

#define DSTAR_DATA_LENGTH_BYTES 12U
#define DSTAR_DATA_LENGTH_BITS (DSTAR_DATA_LENGTH_BYTES * 8U)
#define DSTAR_DATA_LENGTH_SYMBOLS DSTAR_DATA_LENGTH_BYTES * 8U
#define DSTAR_DATA_LENGTH_SAMPLES DSTAR_DATA_LENGTH_SYMBOLS * DSTAR_RADIO_SYMBOL_LENGTH

#define DSTAR_END_SYNC_LENGTH_BYTES 6U
#define DSTAR_END_SYNC_LENGTH_BITS DSTAR_END_SYNC_LENGTH_BYTES * 8U

#define DSTAR_FRAME_SYNC_LENGTH_BYTES 3U
#define DSTAR_FRAME_SYNC_LENGTH_SYMBOLS DSTAR_FRAME_SYNC_LENGTH_BYTES * 8U
#define DSTAR_FRAME_SYNC_LENGTH_SAMPLES DSTAR_FRAME_SYNC_LENGTH_SYMBOLS * DSTAR_RADIO_SYMBOL_LENGTH

#define DSTAR_DATA_SYNC_LENGTH_BYTES 3U
#define DSTAR_DATA_SYNC_LENGTH_BITS (DSTAR_DATA_SYNC_LENGTH_BYTES * 8U)
#define DSTAR_DATA_SYNC_LENGTH_SYMBOLS DSTAR_DATA_SYNC_LENGTH_BYTES * 8U
#define DSTAR_DATA_SYNC_LENGTH_SAMPLES DSTAR_DATA_SYNC_LENGTH_SYMBOLS * DSTAR_RADIO_SYMBOL_LENGTH

const uint8_t DSTAR_DATA_MASK           = 0x80U;
const uint8_t DSTAR_REPEATER_MASK       = 0x40U;
const uint8_t DSTAR_INTERRUPTED_MASK    = 0x20U;
const uint8_t DSTAR_CONTROL_SIGNAL_MASK = 0x10U;
const uint8_t DSTAR_URGENT_MASK         = 0x08U;
const uint8_t DSTAR_REPEATER_CONTROL    = 0x07U;
const uint8_t DSTAR_AUTO_REPLY          = 0x06U;
const uint8_t DSTAR_RESEND_REQUESTED    = 0x04U;
const uint8_t DSTAR_ACK_FLAG            = 0x03U;
const uint8_t DSTAR_NO_RESPONSE         = 0x02U;
const uint8_t DSTAR_RELAY_UNAVAILABLE   = 0x01U;

const uint8_t DSTAR_DATA_SYNC_BYTES[] = {0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A,
                                         0x3F, 0x61, 0xE8, 0x55, 0x2D, 0x16};

// D-Star bit order version of 0x55 0x6E 0x0A
const uint32_t DSTAR_FRAME_SYNC_DATA  = 0x00557650U;
const uint32_t DSTAR_FRAME_SYNC_MASK  = 0x00FFFFFFU;
const bool DSTAR_FRAME_SYNC_SYMBOLS[] = {
    false, true, false, true, false, true,  false, true,
    false, true, true,  true, false, true,  true,  false,
    false, true, false, true, false, false, false, false};

// D-Star bit order version of 0x55 0x2D 0x16
const uint32_t DSTAR_DATA_SYNC_DATA  = 0x00AAB468U;
const uint32_t DSTAR_DATA_SYNC_MASK  = 0x00FFFFFFU;
const bool DSTAR_DATA_SYNC_SYMBOLS[] = {
    true,  false, true, false, true,  false, true,  false,
    true,  false, true, true,  false, true,  false, false,
    false, true,  true, false, true,  false, false, false};

// D-Star bit order version of 0x55 0x55 0xC8 0x7A
const uint32_t DSTAR_END_SYNC_DATA   = 0xAAAA135EU;
const uint32_t DSTAR_END_SYNC_MASK   = 0xFFFFFFFFU;
const uint8_t DSTAR_END_SYNC_BYTES[] = {0x55, 0x55, 0x55, 0x55, 0xC8, 0x7A,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t DSTAR_SLOW_DATA_TYPE_TEXT   = 0x40U;
const uint8_t DSTAR_SLOW_DATA_TYPE_HEADER = 0x50U;

const uint8_t DSTAR_SCRAMBLER_BYTES[] = {0x70U, 0x4FU, 0x93U};

#define MAX_FRAMES 150U

#define MAX_SYNC_BITS 100U * DSTAR_DATA_LENGTH_BITS

// D-Star preamble sequence (only 32 bits of 101010...)
const uint64_t PREAMBLE_MASK = 0x00000000FFFFFFFFU;
const uint64_t PREAMBLE_DATA = 0x00000000AAAAAAAAU;
const uint8_t PREAMBLE_ERRS  = 2U;

// D-Star bit order version of 0x55 0x55 0xC8 0x7A
const uint64_t END_SYNC_DATA = 0x0000AAAAAAAA135EU;
const uint64_t END_SYNC_MASK = 0x0000FFFFFFFFFFFFU;
const uint8_t END_SYNC_ERRS  = 1U;

// D-Star bit order version of 0x55 0x55 0x6E 0x0A
const uint64_t FRAME_SYNC_DATA = 0x0000000000557650U;
const uint64_t FRAME_SYNC_MASK = 0x0000000000FFFFFFU;
const uint8_t FRAME_SYNC_ERRS  = 2U;

// D-Star bit order version of 0x55 0x2D 0x16
const uint64_t DATA_SYNC_DATA = 0x0000000000AAB468U;
const uint64_t DATA_SYNC_MASK = 0x0000000000FFFFFFU;
const uint8_t DATA_SYNC_ERRS  = 2U;

const uint8_t BIT_MASK_TABLE0[] = {0x7FU, 0xBFU, 0xDFU, 0xEFU,
                                   0xF7U, 0xFBU, 0xFDU, 0xFEU};
const uint8_t BIT_MASK_TABLE1[] = {0x80U, 0x40U, 0x20U, 0x10U,
                                   0x08U, 0x04U, 0x02U, 0x01U};
const uint8_t BIT_MASK_TABLE2[] = {0xFEU, 0xFDU, 0xFBU, 0xF7U,
                                   0xEFU, 0xDFU, 0xBFU, 0x7FU};
const uint8_t BIT_MASK_TABLE3[] = {0x01U, 0x02U, 0x04U, 0x08U,
                                   0x10U, 0x20U, 0x40U, 0x80U};

#define WRITE_BIT1(p, i, b)                                      \
    p[(i) >> 3] = (b) ? (p[(i) >> 3] | BIT_MASK_TABLE1[(i) & 7]) \
                      : (p[(i) >> 3] & BIT_MASK_TABLE0[(i) & 7])
#define READ_BIT1(p, i) (p[(i) >> 3] & BIT_MASK_TABLE1[(i) & 7])

#define WRITE_BIT2(p, i, b)                                      \
    p[(i) >> 3] = (b) ? (p[(i) >> 3] | BIT_MASK_TABLE3[(i) & 7]) \
                      : (p[(i) >> 3] & BIT_MASK_TABLE2[(i) & 7])
#define READ_BIT2(p, i) (p[(i) >> 3] & BIT_MASK_TABLE3[(i) & 7])

const uint8_t INTERLEAVE_TABLE_RX[] = {
    0x00U, 0x00U, 0x03U, 0x00U, 0x06U, 0x00U, 0x09U, 0x00U, 0x0CU, 0x00U, 0x0FU,
    0x00U, 0x12U, 0x00U, 0x15U, 0x00U, 0x18U, 0x00U, 0x1BU, 0x00U, 0x1EU, 0x00U,
    0x21U, 0x00U, 0x24U, 0x00U, 0x27U, 0x00U, 0x2AU, 0x00U, 0x2DU, 0x00U, 0x30U,
    0x00U, 0x33U, 0x00U, 0x36U, 0x00U, 0x39U, 0x00U, 0x3CU, 0x00U, 0x3FU, 0x00U,
    0x42U, 0x00U, 0x45U, 0x00U, 0x48U, 0x00U, 0x4BU, 0x00U, 0x4EU, 0x00U, 0x51U,
    0x00U, 0x00U, 0x01U, 0x03U, 0x01U, 0x06U, 0x01U, 0x09U, 0x01U, 0x0CU, 0x01U,
    0x0FU, 0x01U, 0x12U, 0x01U, 0x15U, 0x01U, 0x18U, 0x01U, 0x1BU, 0x01U, 0x1EU,
    0x01U, 0x21U, 0x01U, 0x24U, 0x01U, 0x27U, 0x01U, 0x2AU, 0x01U, 0x2DU, 0x01U,
    0x30U, 0x01U, 0x33U, 0x01U, 0x36U, 0x01U, 0x39U, 0x01U, 0x3CU, 0x01U, 0x3FU,
    0x01U, 0x42U, 0x01U, 0x45U, 0x01U, 0x48U, 0x01U, 0x4BU, 0x01U, 0x4EU, 0x01U,
    0x51U, 0x01U, 0x00U, 0x02U, 0x03U, 0x02U, 0x06U, 0x02U, 0x09U, 0x02U, 0x0CU,
    0x02U, 0x0FU, 0x02U, 0x12U, 0x02U, 0x15U, 0x02U, 0x18U, 0x02U, 0x1BU, 0x02U,
    0x1EU, 0x02U, 0x21U, 0x02U, 0x24U, 0x02U, 0x27U, 0x02U, 0x2AU, 0x02U, 0x2DU,
    0x02U, 0x30U, 0x02U, 0x33U, 0x02U, 0x36U, 0x02U, 0x39U, 0x02U, 0x3CU, 0x02U,
    0x3FU, 0x02U, 0x42U, 0x02U, 0x45U, 0x02U, 0x48U, 0x02U, 0x4BU, 0x02U, 0x4EU,
    0x02U, 0x51U, 0x02U, 0x00U, 0x03U, 0x03U, 0x03U, 0x06U, 0x03U, 0x09U, 0x03U,
    0x0CU, 0x03U, 0x0FU, 0x03U, 0x12U, 0x03U, 0x15U, 0x03U, 0x18U, 0x03U, 0x1BU,
    0x03U, 0x1EU, 0x03U, 0x21U, 0x03U, 0x24U, 0x03U, 0x27U, 0x03U, 0x2AU, 0x03U,
    0x2DU, 0x03U, 0x30U, 0x03U, 0x33U, 0x03U, 0x36U, 0x03U, 0x39U, 0x03U, 0x3CU,
    0x03U, 0x3FU, 0x03U, 0x42U, 0x03U, 0x45U, 0x03U, 0x48U, 0x03U, 0x4BU, 0x03U,
    0x4EU, 0x03U, 0x51U, 0x03U, 0x00U, 0x04U, 0x03U, 0x04U, 0x06U, 0x04U, 0x09U,
    0x04U, 0x0CU, 0x04U, 0x0FU, 0x04U, 0x12U, 0x04U, 0x15U, 0x04U, 0x18U, 0x04U,
    0x1BU, 0x04U, 0x1EU, 0x04U, 0x21U, 0x04U, 0x24U, 0x04U, 0x27U, 0x04U, 0x2AU,
    0x04U, 0x2DU, 0x04U, 0x30U, 0x04U, 0x33U, 0x04U, 0x36U, 0x04U, 0x39U, 0x04U,
    0x3CU, 0x04U, 0x3FU, 0x04U, 0x42U, 0x04U, 0x45U, 0x04U, 0x48U, 0x04U, 0x4BU,
    0x04U, 0x4EU, 0x04U, 0x51U, 0x04U, 0x00U, 0x05U, 0x03U, 0x05U, 0x06U, 0x05U,
    0x09U, 0x05U, 0x0CU, 0x05U, 0x0FU, 0x05U, 0x12U, 0x05U, 0x15U, 0x05U, 0x18U,
    0x05U, 0x1BU, 0x05U, 0x1EU, 0x05U, 0x21U, 0x05U, 0x24U, 0x05U, 0x27U, 0x05U,
    0x2AU, 0x05U, 0x2DU, 0x05U, 0x30U, 0x05U, 0x33U, 0x05U, 0x36U, 0x05U, 0x39U,
    0x05U, 0x3CU, 0x05U, 0x3FU, 0x05U, 0x42U, 0x05U, 0x45U, 0x05U, 0x48U, 0x05U,
    0x4BU, 0x05U, 0x4EU, 0x05U, 0x51U, 0x05U, 0x00U, 0x06U, 0x03U, 0x06U, 0x06U,
    0x06U, 0x09U, 0x06U, 0x0CU, 0x06U, 0x0FU, 0x06U, 0x12U, 0x06U, 0x15U, 0x06U,
    0x18U, 0x06U, 0x1BU, 0x06U, 0x1EU, 0x06U, 0x21U, 0x06U, 0x24U, 0x06U, 0x27U,
    0x06U, 0x2AU, 0x06U, 0x2DU, 0x06U, 0x30U, 0x06U, 0x33U, 0x06U, 0x36U, 0x06U,
    0x39U, 0x06U, 0x3CU, 0x06U, 0x3FU, 0x06U, 0x42U, 0x06U, 0x45U, 0x06U, 0x48U,
    0x06U, 0x4BU, 0x06U, 0x4EU, 0x06U, 0x51U, 0x06U, 0x00U, 0x07U, 0x03U, 0x07U,
    0x06U, 0x07U, 0x09U, 0x07U, 0x0CU, 0x07U, 0x0FU, 0x07U, 0x12U, 0x07U, 0x15U,
    0x07U, 0x18U, 0x07U, 0x1BU, 0x07U, 0x1EU, 0x07U, 0x21U, 0x07U, 0x24U, 0x07U,
    0x27U, 0x07U, 0x2AU, 0x07U, 0x2DU, 0x07U, 0x30U, 0x07U, 0x33U, 0x07U, 0x36U,
    0x07U, 0x39U, 0x07U, 0x3CU, 0x07U, 0x3FU, 0x07U, 0x42U, 0x07U, 0x45U, 0x07U,
    0x48U, 0x07U, 0x4BU, 0x07U, 0x4EU, 0x07U, 0x51U, 0x07U, 0x01U, 0x00U, 0x04U,
    0x00U, 0x07U, 0x00U, 0x0AU, 0x00U, 0x0DU, 0x00U, 0x10U, 0x00U, 0x13U, 0x00U,
    0x16U, 0x00U, 0x19U, 0x00U, 0x1CU, 0x00U, 0x1FU, 0x00U, 0x22U, 0x00U, 0x25U,
    0x00U, 0x28U, 0x00U, 0x2BU, 0x00U, 0x2EU, 0x00U, 0x31U, 0x00U, 0x34U, 0x00U,
    0x37U, 0x00U, 0x3AU, 0x00U, 0x3DU, 0x00U, 0x40U, 0x00U, 0x43U, 0x00U, 0x46U,
    0x00U, 0x49U, 0x00U, 0x4CU, 0x00U, 0x4FU, 0x00U, 0x52U, 0x00U, 0x01U, 0x01U,
    0x04U, 0x01U, 0x07U, 0x01U, 0x0AU, 0x01U, 0x0DU, 0x01U, 0x10U, 0x01U, 0x13U,
    0x01U, 0x16U, 0x01U, 0x19U, 0x01U, 0x1CU, 0x01U, 0x1FU, 0x01U, 0x22U, 0x01U,
    0x25U, 0x01U, 0x28U, 0x01U, 0x2BU, 0x01U, 0x2EU, 0x01U, 0x31U, 0x01U, 0x34U,
    0x01U, 0x37U, 0x01U, 0x3AU, 0x01U, 0x3DU, 0x01U, 0x40U, 0x01U, 0x43U, 0x01U,
    0x46U, 0x01U, 0x49U, 0x01U, 0x4CU, 0x01U, 0x4FU, 0x01U, 0x52U, 0x01U, 0x01U,
    0x02U, 0x04U, 0x02U, 0x07U, 0x02U, 0x0AU, 0x02U, 0x0DU, 0x02U, 0x10U, 0x02U,
    0x13U, 0x02U, 0x16U, 0x02U, 0x19U, 0x02U, 0x1CU, 0x02U, 0x1FU, 0x02U, 0x22U,
    0x02U, 0x25U, 0x02U, 0x28U, 0x02U, 0x2BU, 0x02U, 0x2EU, 0x02U, 0x31U, 0x02U,
    0x34U, 0x02U, 0x37U, 0x02U, 0x3AU, 0x02U, 0x3DU, 0x02U, 0x40U, 0x02U, 0x43U,
    0x02U, 0x46U, 0x02U, 0x49U, 0x02U, 0x4CU, 0x02U, 0x4FU, 0x02U, 0x52U, 0x02U,
    0x01U, 0x03U, 0x04U, 0x03U, 0x07U, 0x03U, 0x0AU, 0x03U, 0x0DU, 0x03U, 0x10U,
    0x03U, 0x13U, 0x03U, 0x16U, 0x03U, 0x19U, 0x03U, 0x1CU, 0x03U, 0x1FU, 0x03U,
    0x22U, 0x03U, 0x25U, 0x03U, 0x28U, 0x03U, 0x2BU, 0x03U, 0x2EU, 0x03U, 0x31U,
    0x03U, 0x34U, 0x03U, 0x37U, 0x03U, 0x3AU, 0x03U, 0x3DU, 0x03U, 0x40U, 0x03U,
    0x43U, 0x03U, 0x46U, 0x03U, 0x49U, 0x03U, 0x4CU, 0x03U, 0x4FU, 0x03U, 0x52U,
    0x03U, 0x01U, 0x04U, 0x04U, 0x04U, 0x07U, 0x04U, 0x0AU, 0x04U, 0x0DU, 0x04U,
    0x10U, 0x04U, 0x13U, 0x04U, 0x16U, 0x04U, 0x19U, 0x04U, 0x1CU, 0x04U, 0x1FU,
    0x04U, 0x22U, 0x04U, 0x25U, 0x04U, 0x28U, 0x04U, 0x2BU, 0x04U, 0x2EU, 0x04U,
    0x31U, 0x04U, 0x34U, 0x04U, 0x37U, 0x04U, 0x3AU, 0x04U, 0x3DU, 0x04U, 0x40U,
    0x04U, 0x43U, 0x04U, 0x46U, 0x04U, 0x49U, 0x04U, 0x4CU, 0x04U, 0x4FU, 0x04U,
    0x01U, 0x05U, 0x04U, 0x05U, 0x07U, 0x05U, 0x0AU, 0x05U, 0x0DU, 0x05U, 0x10U,
    0x05U, 0x13U, 0x05U, 0x16U, 0x05U, 0x19U, 0x05U, 0x1CU, 0x05U, 0x1FU, 0x05U,
    0x22U, 0x05U, 0x25U, 0x05U, 0x28U, 0x05U, 0x2BU, 0x05U, 0x2EU, 0x05U, 0x31U,
    0x05U, 0x34U, 0x05U, 0x37U, 0x05U, 0x3AU, 0x05U, 0x3DU, 0x05U, 0x40U, 0x05U,
    0x43U, 0x05U, 0x46U, 0x05U, 0x49U, 0x05U, 0x4CU, 0x05U, 0x4FU, 0x05U, 0x01U,
    0x06U, 0x04U, 0x06U, 0x07U, 0x06U, 0x0AU, 0x06U, 0x0DU, 0x06U, 0x10U, 0x06U,
    0x13U, 0x06U, 0x16U, 0x06U, 0x19U, 0x06U, 0x1CU, 0x06U, 0x1FU, 0x06U, 0x22U,
    0x06U, 0x25U, 0x06U, 0x28U, 0x06U, 0x2BU, 0x06U, 0x2EU, 0x06U, 0x31U, 0x06U,
    0x34U, 0x06U, 0x37U, 0x06U, 0x3AU, 0x06U, 0x3DU, 0x06U, 0x40U, 0x06U, 0x43U,
    0x06U, 0x46U, 0x06U, 0x49U, 0x06U, 0x4CU, 0x06U, 0x4FU, 0x06U, 0x01U, 0x07U,
    0x04U, 0x07U, 0x07U, 0x07U, 0x0AU, 0x07U, 0x0DU, 0x07U, 0x10U, 0x07U, 0x13U,
    0x07U, 0x16U, 0x07U, 0x19U, 0x07U, 0x1CU, 0x07U, 0x1FU, 0x07U, 0x22U, 0x07U,
    0x25U, 0x07U, 0x28U, 0x07U, 0x2BU, 0x07U, 0x2EU, 0x07U, 0x31U, 0x07U, 0x34U,
    0x07U, 0x37U, 0x07U, 0x3AU, 0x07U, 0x3DU, 0x07U, 0x40U, 0x07U, 0x43U, 0x07U,
    0x46U, 0x07U, 0x49U, 0x07U, 0x4CU, 0x07U, 0x4FU, 0x07U, 0x02U, 0x00U, 0x05U,
    0x00U, 0x08U, 0x00U, 0x0BU, 0x00U, 0x0EU, 0x00U, 0x11U, 0x00U, 0x14U, 0x00U,
    0x17U, 0x00U, 0x1AU, 0x00U, 0x1DU, 0x00U, 0x20U, 0x00U, 0x23U, 0x00U, 0x26U,
    0x00U, 0x29U, 0x00U, 0x2CU, 0x00U, 0x2FU, 0x00U, 0x32U, 0x00U, 0x35U, 0x00U,
    0x38U, 0x00U, 0x3BU, 0x00U, 0x3EU, 0x00U, 0x41U, 0x00U, 0x44U, 0x00U, 0x47U,
    0x00U, 0x4AU, 0x00U, 0x4DU, 0x00U, 0x50U, 0x00U, 0x02U, 0x01U, 0x05U, 0x01U,
    0x08U, 0x01U, 0x0BU, 0x01U, 0x0EU, 0x01U, 0x11U, 0x01U, 0x14U, 0x01U, 0x17U,
    0x01U, 0x1AU, 0x01U, 0x1DU, 0x01U, 0x20U, 0x01U, 0x23U, 0x01U, 0x26U, 0x01U,
    0x29U, 0x01U, 0x2CU, 0x01U, 0x2FU, 0x01U, 0x32U, 0x01U, 0x35U, 0x01U, 0x38U,
    0x01U, 0x3BU, 0x01U, 0x3EU, 0x01U, 0x41U, 0x01U, 0x44U, 0x01U, 0x47U, 0x01U,
    0x4AU, 0x01U, 0x4DU, 0x01U, 0x50U, 0x01U, 0x02U, 0x02U, 0x05U, 0x02U, 0x08U,
    0x02U, 0x0BU, 0x02U, 0x0EU, 0x02U, 0x11U, 0x02U, 0x14U, 0x02U, 0x17U, 0x02U,
    0x1AU, 0x02U, 0x1DU, 0x02U, 0x20U, 0x02U, 0x23U, 0x02U, 0x26U, 0x02U, 0x29U,
    0x02U, 0x2CU, 0x02U, 0x2FU, 0x02U, 0x32U, 0x02U, 0x35U, 0x02U, 0x38U, 0x02U,
    0x3BU, 0x02U, 0x3EU, 0x02U, 0x41U, 0x02U, 0x44U, 0x02U, 0x47U, 0x02U, 0x4AU,
    0x02U, 0x4DU, 0x02U, 0x50U, 0x02U, 0x02U, 0x03U, 0x05U, 0x03U, 0x08U, 0x03U,
    0x0BU, 0x03U, 0x0EU, 0x03U, 0x11U, 0x03U, 0x14U, 0x03U, 0x17U, 0x03U, 0x1AU,
    0x03U, 0x1DU, 0x03U, 0x20U, 0x03U, 0x23U, 0x03U, 0x26U, 0x03U, 0x29U, 0x03U,
    0x2CU, 0x03U, 0x2FU, 0x03U, 0x32U, 0x03U, 0x35U, 0x03U, 0x38U, 0x03U, 0x3BU,
    0x03U, 0x3EU, 0x03U, 0x41U, 0x03U, 0x44U, 0x03U, 0x47U, 0x03U, 0x4AU, 0x03U,
    0x4DU, 0x03U, 0x50U, 0x03U, 0x02U, 0x04U, 0x05U, 0x04U, 0x08U, 0x04U, 0x0BU,
    0x04U, 0x0EU, 0x04U, 0x11U, 0x04U, 0x14U, 0x04U, 0x17U, 0x04U, 0x1AU, 0x04U,
    0x1DU, 0x04U, 0x20U, 0x04U, 0x23U, 0x04U, 0x26U, 0x04U, 0x29U, 0x04U, 0x2CU,
    0x04U, 0x2FU, 0x04U, 0x32U, 0x04U, 0x35U, 0x04U, 0x38U, 0x04U, 0x3BU, 0x04U,
    0x3EU, 0x04U, 0x41U, 0x04U, 0x44U, 0x04U, 0x47U, 0x04U, 0x4AU, 0x04U, 0x4DU,
    0x04U, 0x50U, 0x04U, 0x02U, 0x05U, 0x05U, 0x05U, 0x08U, 0x05U, 0x0BU, 0x05U,
    0x0EU, 0x05U, 0x11U, 0x05U, 0x14U, 0x05U, 0x17U, 0x05U, 0x1AU, 0x05U, 0x1DU,
    0x05U, 0x20U, 0x05U, 0x23U, 0x05U, 0x26U, 0x05U, 0x29U, 0x05U, 0x2CU, 0x05U,
    0x2FU, 0x05U, 0x32U, 0x05U, 0x35U, 0x05U, 0x38U, 0x05U, 0x3BU, 0x05U, 0x3EU,
    0x05U, 0x41U, 0x05U, 0x44U, 0x05U, 0x47U, 0x05U, 0x4AU, 0x05U, 0x4DU, 0x05U,
    0x50U, 0x05U, 0x02U, 0x06U, 0x05U, 0x06U, 0x08U, 0x06U, 0x0BU, 0x06U, 0x0EU,
    0x06U, 0x11U, 0x06U, 0x14U, 0x06U, 0x17U, 0x06U, 0x1AU, 0x06U, 0x1DU, 0x06U,
    0x20U, 0x06U, 0x23U, 0x06U, 0x26U, 0x06U, 0x29U, 0x06U, 0x2CU, 0x06U, 0x2FU,
    0x06U, 0x32U, 0x06U, 0x35U, 0x06U, 0x38U, 0x06U, 0x3BU, 0x06U, 0x3EU, 0x06U,
    0x41U, 0x06U, 0x44U, 0x06U, 0x47U, 0x06U, 0x4AU, 0x06U, 0x4DU, 0x06U, 0x50U,
    0x06U, 0x02U, 0x07U, 0x05U, 0x07U, 0x08U, 0x07U, 0x0BU, 0x07U, 0x0EU, 0x07U,
    0x11U, 0x07U, 0x14U, 0x07U, 0x17U, 0x07U, 0x1AU, 0x07U, 0x1DU, 0x07U, 0x20U,
    0x07U, 0x23U, 0x07U, 0x26U, 0x07U, 0x29U, 0x07U, 0x2CU, 0x07U, 0x2FU, 0x07U,
    0x32U, 0x07U, 0x35U, 0x07U, 0x38U, 0x07U, 0x3BU, 0x07U, 0x3EU, 0x07U, 0x41U,
    0x07U, 0x44U, 0x07U, 0x47U, 0x07U, 0x4AU, 0x07U, 0x4DU, 0x07U, 0x50U, 0x07U,
};

const uint8_t SCRAMBLE_TABLE_RX[] = {
    0x70U, 0x4FU, 0x93U, 0x40U, 0x64U, 0x74U, 0x6DU, 0x30U, 0x2BU, 0xE7U, 0x2DU,
    0x54U, 0x5FU, 0x8AU, 0x1DU, 0x7FU, 0xB8U, 0xA7U, 0x49U, 0x20U, 0x32U, 0xBAU,
    0x36U, 0x98U, 0x95U, 0xF3U, 0x16U, 0xAAU, 0x2FU, 0xC5U, 0x8EU, 0x3FU, 0xDCU,
    0xD3U, 0x24U, 0x10U, 0x19U, 0x5DU, 0x1BU, 0xCCU, 0xCAU, 0x79U, 0x0BU, 0xD5U,
    0x97U, 0x62U, 0xC7U, 0x1FU, 0xEEU, 0x69U, 0x12U, 0x88U, 0x8CU, 0xAEU, 0x0DU,
    0x66U, 0xE5U, 0xBCU, 0x85U, 0xEAU, 0x4BU, 0xB1U, 0xE3U, 0x0FU, 0xF7U, 0x34U,
    0x09U, 0x44U, 0x46U, 0xD7U, 0x06U, 0xB3U, 0x72U, 0xDEU, 0x42U, 0xF5U, 0xA5U,
    0xD8U, 0xF1U, 0x87U, 0x7BU, 0x9AU, 0x04U, 0x22U, 0xA3U, 0x6BU, 0x83U, 0x59U,
    0x39U, 0x6FU, 0x00U};

const uint16_t CCITT16_TABLE[] = {
    0x0000U, 0x1189U, 0x2312U, 0x329bU, 0x4624U, 0x57adU, 0x6536U, 0x74bfU,
    0x8c48U, 0x9dc1U, 0xaf5aU, 0xbed3U, 0xca6cU, 0xdbe5U, 0xe97eU, 0xf8f7U,
    0x1081U, 0x0108U, 0x3393U, 0x221aU, 0x56a5U, 0x472cU, 0x75b7U, 0x643eU,
    0x9cc9U, 0x8d40U, 0xbfdbU, 0xae52U, 0xdaedU, 0xcb64U, 0xf9ffU, 0xe876U,
    0x2102U, 0x308bU, 0x0210U, 0x1399U, 0x6726U, 0x76afU, 0x4434U, 0x55bdU,
    0xad4aU, 0xbcc3U, 0x8e58U, 0x9fd1U, 0xeb6eU, 0xfae7U, 0xc87cU, 0xd9f5U,
    0x3183U, 0x200aU, 0x1291U, 0x0318U, 0x77a7U, 0x662eU, 0x54b5U, 0x453cU,
    0xbdcbU, 0xac42U, 0x9ed9U, 0x8f50U, 0xfbefU, 0xea66U, 0xd8fdU, 0xc974U,
    0x4204U, 0x538dU, 0x6116U, 0x709fU, 0x0420U, 0x15a9U, 0x2732U, 0x36bbU,
    0xce4cU, 0xdfc5U, 0xed5eU, 0xfcd7U, 0x8868U, 0x99e1U, 0xab7aU, 0xbaf3U,
    0x5285U, 0x430cU, 0x7197U, 0x601eU, 0x14a1U, 0x0528U, 0x37b3U, 0x263aU,
    0xdecdU, 0xcf44U, 0xfddfU, 0xec56U, 0x98e9U, 0x8960U, 0xbbfbU, 0xaa72U,
    0x6306U, 0x728fU, 0x4014U, 0x519dU, 0x2522U, 0x34abU, 0x0630U, 0x17b9U,
    0xef4eU, 0xfec7U, 0xcc5cU, 0xddd5U, 0xa96aU, 0xb8e3U, 0x8a78U, 0x9bf1U,
    0x7387U, 0x620eU, 0x5095U, 0x411cU, 0x35a3U, 0x242aU, 0x16b1U, 0x0738U,
    0xffcfU, 0xee46U, 0xdcddU, 0xcd54U, 0xb9ebU, 0xa862U, 0x9af9U, 0x8b70U,
    0x8408U, 0x9581U, 0xa71aU, 0xb693U, 0xc22cU, 0xd3a5U, 0xe13eU, 0xf0b7U,
    0x0840U, 0x19c9U, 0x2b52U, 0x3adbU, 0x4e64U, 0x5fedU, 0x6d76U, 0x7cffU,
    0x9489U, 0x8500U, 0xb79bU, 0xa612U, 0xd2adU, 0xc324U, 0xf1bfU, 0xe036U,
    0x18c1U, 0x0948U, 0x3bd3U, 0x2a5aU, 0x5ee5U, 0x4f6cU, 0x7df7U, 0x6c7eU,
    0xa50aU, 0xb483U, 0x8618U, 0x9791U, 0xe32eU, 0xf2a7U, 0xc03cU, 0xd1b5U,
    0x2942U, 0x38cbU, 0x0a50U, 0x1bd9U, 0x6f66U, 0x7eefU, 0x4c74U, 0x5dfdU,
    0xb58bU, 0xa402U, 0x9699U, 0x8710U, 0xf3afU, 0xe226U, 0xd0bdU, 0xc134U,
    0x39c3U, 0x284aU, 0x1ad1U, 0x0b58U, 0x7fe7U, 0x6e6eU, 0x5cf5U, 0x4d7cU,
    0xc60cU, 0xd785U, 0xe51eU, 0xf497U, 0x8028U, 0x91a1U, 0xa33aU, 0xb2b3U,
    0x4a44U, 0x5bcdU, 0x6956U, 0x78dfU, 0x0c60U, 0x1de9U, 0x2f72U, 0x3efbU,
    0xd68dU, 0xc704U, 0xf59fU, 0xe416U, 0x90a9U, 0x8120U, 0xb3bbU, 0xa232U,
    0x5ac5U, 0x4b4cU, 0x79d7U, 0x685eU, 0x1ce1U, 0x0d68U, 0x3ff3U, 0x2e7aU,
    0xe70eU, 0xf687U, 0xc41cU, 0xd595U, 0xa12aU, 0xb0a3U, 0x8238U, 0x93b1U,
    0x6b46U, 0x7acfU, 0x4854U, 0x59ddU, 0x2d62U, 0x3cebU, 0x0e70U, 0x1ff9U,
    0xf78fU, 0xe606U, 0xd49dU, 0xc514U, 0xb1abU, 0xa022U, 0x92b9U, 0x8330U,
    0x7bc7U, 0x6a4eU, 0x58d5U, 0x495cU, 0x3de3U, 0x2c6aU, 0x1ef1U, 0x0f78U};

const q15_t DSTAR_LEVEL0 = -841;
const q15_t DSTAR_LEVEL1 = 841;

const uint8_t BIT_MASK_TABLE[] = {0x80U, 0x40U, 0x20U, 0x10U,
                                  0x08U, 0x04U, 0x02U, 0x01U};

const uint8_t INTERLEAVE_TABLE_TX[] = {
    0x00U, 0x04U, 0x04U, 0x00U, 0x07U, 0x04U, 0x0BU, 0x00U, 0x0EU, 0x04U,
    0x12U, 0x00U, 0x15U, 0x04U, 0x19U, 0x00U, 0x1CU, 0x04U, 0x20U, 0x00U,
    0x23U, 0x04U, 0x27U, 0x00U, 0x2AU, 0x04U, 0x2DU, 0x07U, 0x31U, 0x02U,
    0x34U, 0x05U, 0x38U, 0x00U, 0x3BU, 0x03U, 0x3EU, 0x06U, 0x42U, 0x01U,
    0x45U, 0x04U, 0x48U, 0x07U, 0x4CU, 0x02U, 0x4FU, 0x05U, 0x00U, 0x05U,
    0x04U, 0x01U, 0x07U, 0x05U, 0x0BU, 0x01U, 0x0EU, 0x05U, 0x12U, 0x01U,
    0x15U, 0x05U, 0x19U, 0x01U, 0x1CU, 0x05U, 0x20U, 0x01U, 0x23U, 0x05U,
    0x27U, 0x01U, 0x2AU, 0x05U, 0x2EU, 0x00U, 0x31U, 0x03U, 0x34U, 0x06U,
    0x38U, 0x01U, 0x3BU, 0x04U, 0x3EU, 0x07U, 0x42U, 0x02U, 0x45U, 0x05U,
    0x49U, 0x00U, 0x4CU, 0x03U, 0x4FU, 0x06U, 0x00U, 0x06U, 0x04U, 0x02U,
    0x07U, 0x06U, 0x0BU, 0x02U, 0x0EU, 0x06U, 0x12U, 0x02U, 0x15U, 0x06U,
    0x19U, 0x02U, 0x1CU, 0x06U, 0x20U, 0x02U, 0x23U, 0x06U, 0x27U, 0x02U,
    0x2AU, 0x06U, 0x2EU, 0x01U, 0x31U, 0x04U, 0x34U, 0x07U, 0x38U, 0x02U,
    0x3BU, 0x05U, 0x3FU, 0x00U, 0x42U, 0x03U, 0x45U, 0x06U, 0x49U, 0x01U,
    0x4CU, 0x04U, 0x4FU, 0x07U, 0x00U, 0x07U, 0x04U, 0x03U, 0x07U, 0x07U,
    0x0BU, 0x03U, 0x0EU, 0x07U, 0x12U, 0x03U, 0x15U, 0x07U, 0x19U, 0x03U,
    0x1CU, 0x07U, 0x20U, 0x03U, 0x23U, 0x07U, 0x27U, 0x03U, 0x2AU, 0x07U,
    0x2EU, 0x02U, 0x31U, 0x05U, 0x35U, 0x00U, 0x38U, 0x03U, 0x3BU, 0x06U,
    0x3FU, 0x01U, 0x42U, 0x04U, 0x45U, 0x07U, 0x49U, 0x02U, 0x4CU, 0x05U,
    0x50U, 0x00U, 0x01U, 0x00U, 0x04U, 0x04U, 0x08U, 0x00U, 0x0BU, 0x04U,
    0x0FU, 0x00U, 0x12U, 0x04U, 0x16U, 0x00U, 0x19U, 0x04U, 0x1DU, 0x00U,
    0x20U, 0x04U, 0x24U, 0x00U, 0x27U, 0x04U, 0x2BU, 0x00U, 0x2EU, 0x03U,
    0x31U, 0x06U, 0x35U, 0x01U, 0x38U, 0x04U, 0x3BU, 0x07U, 0x3FU, 0x02U,
    0x42U, 0x05U, 0x46U, 0x00U, 0x49U, 0x03U, 0x4CU, 0x06U, 0x50U, 0x01U,
    0x01U, 0x01U, 0x04U, 0x05U, 0x08U, 0x01U, 0x0BU, 0x05U, 0x0FU, 0x01U,
    0x12U, 0x05U, 0x16U, 0x01U, 0x19U, 0x05U, 0x1DU, 0x01U, 0x20U, 0x05U,
    0x24U, 0x01U, 0x27U, 0x05U, 0x2BU, 0x01U, 0x2EU, 0x04U, 0x31U, 0x07U,
    0x35U, 0x02U, 0x38U, 0x05U, 0x3CU, 0x00U, 0x3FU, 0x03U, 0x42U, 0x06U,
    0x46U, 0x01U, 0x49U, 0x04U, 0x4CU, 0x07U, 0x50U, 0x02U, 0x01U, 0x02U,
    0x04U, 0x06U, 0x08U, 0x02U, 0x0BU, 0x06U, 0x0FU, 0x02U, 0x12U, 0x06U,
    0x16U, 0x02U, 0x19U, 0x06U, 0x1DU, 0x02U, 0x20U, 0x06U, 0x24U, 0x02U,
    0x27U, 0x06U, 0x2BU, 0x02U, 0x2EU, 0x05U, 0x32U, 0x00U, 0x35U, 0x03U,
    0x38U, 0x06U, 0x3CU, 0x01U, 0x3FU, 0x04U, 0x42U, 0x07U, 0x46U, 0x02U,
    0x49U, 0x05U, 0x4DU, 0x00U, 0x50U, 0x03U, 0x01U, 0x03U, 0x04U, 0x07U,
    0x08U, 0x03U, 0x0BU, 0x07U, 0x0FU, 0x03U, 0x12U, 0x07U, 0x16U, 0x03U,
    0x19U, 0x07U, 0x1DU, 0x03U, 0x20U, 0x07U, 0x24U, 0x03U, 0x27U, 0x07U,
    0x2BU, 0x03U, 0x2EU, 0x06U, 0x32U, 0x01U, 0x35U, 0x04U, 0x38U, 0x07U,
    0x3CU, 0x02U, 0x3FU, 0x05U, 0x43U, 0x00U, 0x46U, 0x03U, 0x49U, 0x06U,
    0x4DU, 0x01U, 0x50U, 0x04U, 0x01U, 0x04U, 0x05U, 0x00U, 0x08U, 0x04U,
    0x0CU, 0x00U, 0x0FU, 0x04U, 0x13U, 0x00U, 0x16U, 0x04U, 0x1AU, 0x00U,
    0x1DU, 0x04U, 0x21U, 0x00U, 0x24U, 0x04U, 0x28U, 0x00U, 0x2BU, 0x04U,
    0x2EU, 0x07U, 0x32U, 0x02U, 0x35U, 0x05U, 0x39U, 0x00U, 0x3CU, 0x03U,
    0x3FU, 0x06U, 0x43U, 0x01U, 0x46U, 0x04U, 0x49U, 0x07U, 0x4DU, 0x02U,
    0x50U, 0x05U, 0x01U, 0x05U, 0x05U, 0x01U, 0x08U, 0x05U, 0x0CU, 0x01U,
    0x0FU, 0x05U, 0x13U, 0x01U, 0x16U, 0x05U, 0x1AU, 0x01U, 0x1DU, 0x05U,
    0x21U, 0x01U, 0x24U, 0x05U, 0x28U, 0x01U, 0x2BU, 0x05U, 0x2FU, 0x00U,
    0x32U, 0x03U, 0x35U, 0x06U, 0x39U, 0x01U, 0x3CU, 0x04U, 0x3FU, 0x07U,
    0x43U, 0x02U, 0x46U, 0x05U, 0x4AU, 0x00U, 0x4DU, 0x03U, 0x50U, 0x06U,
    0x01U, 0x06U, 0x05U, 0x02U, 0x08U, 0x06U, 0x0CU, 0x02U, 0x0FU, 0x06U,
    0x13U, 0x02U, 0x16U, 0x06U, 0x1AU, 0x02U, 0x1DU, 0x06U, 0x21U, 0x02U,
    0x24U, 0x06U, 0x28U, 0x02U, 0x2BU, 0x06U, 0x2FU, 0x01U, 0x32U, 0x04U,
    0x35U, 0x07U, 0x39U, 0x02U, 0x3CU, 0x05U, 0x40U, 0x00U, 0x43U, 0x03U,
    0x46U, 0x06U, 0x4AU, 0x01U, 0x4DU, 0x04U, 0x50U, 0x07U, 0x01U, 0x07U,
    0x05U, 0x03U, 0x08U, 0x07U, 0x0CU, 0x03U, 0x0FU, 0x07U, 0x13U, 0x03U,
    0x16U, 0x07U, 0x1AU, 0x03U, 0x1DU, 0x07U, 0x21U, 0x03U, 0x24U, 0x07U,
    0x28U, 0x03U, 0x2BU, 0x07U, 0x2FU, 0x02U, 0x32U, 0x05U, 0x36U, 0x00U,
    0x39U, 0x03U, 0x3CU, 0x06U, 0x40U, 0x01U, 0x43U, 0x04U, 0x46U, 0x07U,
    0x4AU, 0x02U, 0x4DU, 0x05U, 0x51U, 0x00U, 0x02U, 0x00U, 0x05U, 0x04U,
    0x09U, 0x00U, 0x0CU, 0x04U, 0x10U, 0x00U, 0x13U, 0x04U, 0x17U, 0x00U,
    0x1AU, 0x04U, 0x1EU, 0x00U, 0x21U, 0x04U, 0x25U, 0x00U, 0x28U, 0x04U,
    0x2CU, 0x00U, 0x2FU, 0x03U, 0x32U, 0x06U, 0x36U, 0x01U, 0x39U, 0x04U,
    0x3CU, 0x07U, 0x40U, 0x02U, 0x43U, 0x05U, 0x47U, 0x00U, 0x4AU, 0x03U,
    0x4DU, 0x06U, 0x51U, 0x01U, 0x02U, 0x01U, 0x05U, 0x05U, 0x09U, 0x01U,
    0x0CU, 0x05U, 0x10U, 0x01U, 0x13U, 0x05U, 0x17U, 0x01U, 0x1AU, 0x05U,
    0x1EU, 0x01U, 0x21U, 0x05U, 0x25U, 0x01U, 0x28U, 0x05U, 0x2CU, 0x01U,
    0x2FU, 0x04U, 0x32U, 0x07U, 0x36U, 0x02U, 0x39U, 0x05U, 0x3DU, 0x00U,
    0x40U, 0x03U, 0x43U, 0x06U, 0x47U, 0x01U, 0x4AU, 0x04U, 0x4DU, 0x07U,
    0x51U, 0x02U, 0x02U, 0x02U, 0x05U, 0x06U, 0x09U, 0x02U, 0x0CU, 0x06U,
    0x10U, 0x02U, 0x13U, 0x06U, 0x17U, 0x02U, 0x1AU, 0x06U, 0x1EU, 0x02U,
    0x21U, 0x06U, 0x25U, 0x02U, 0x28U, 0x06U, 0x2CU, 0x02U, 0x2FU, 0x05U,
    0x33U, 0x00U, 0x36U, 0x03U, 0x39U, 0x06U, 0x3DU, 0x01U, 0x40U, 0x04U,
    0x43U, 0x07U, 0x47U, 0x02U, 0x4AU, 0x05U, 0x4EU, 0x00U, 0x51U, 0x03U,
    0x02U, 0x03U, 0x05U, 0x07U, 0x09U, 0x03U, 0x0CU, 0x07U, 0x10U, 0x03U,
    0x13U, 0x07U, 0x17U, 0x03U, 0x1AU, 0x07U, 0x1EU, 0x03U, 0x21U, 0x07U,
    0x25U, 0x03U, 0x28U, 0x07U, 0x2CU, 0x03U, 0x2FU, 0x06U, 0x33U, 0x01U,
    0x36U, 0x04U, 0x39U, 0x07U, 0x3DU, 0x02U, 0x40U, 0x05U, 0x44U, 0x00U,
    0x47U, 0x03U, 0x4AU, 0x06U, 0x4EU, 0x01U, 0x51U, 0x04U, 0x02U, 0x04U,
    0x06U, 0x00U, 0x09U, 0x04U, 0x0DU, 0x00U, 0x10U, 0x04U, 0x14U, 0x00U,
    0x17U, 0x04U, 0x1BU, 0x00U, 0x1EU, 0x04U, 0x22U, 0x00U, 0x25U, 0x04U,
    0x29U, 0x00U, 0x2CU, 0x04U, 0x2FU, 0x07U, 0x33U, 0x02U, 0x36U, 0x05U,
    0x3AU, 0x00U, 0x3DU, 0x03U, 0x40U, 0x06U, 0x44U, 0x01U, 0x47U, 0x04U,
    0x4AU, 0x07U, 0x4EU, 0x02U, 0x51U, 0x05U, 0x02U, 0x05U, 0x06U, 0x01U,
    0x09U, 0x05U, 0x0DU, 0x01U, 0x10U, 0x05U, 0x14U, 0x01U, 0x17U, 0x05U,
    0x1BU, 0x01U, 0x1EU, 0x05U, 0x22U, 0x01U, 0x25U, 0x05U, 0x29U, 0x01U,
    0x2CU, 0x05U, 0x30U, 0x00U, 0x33U, 0x03U, 0x36U, 0x06U, 0x3AU, 0x01U,
    0x3DU, 0x04U, 0x40U, 0x07U, 0x44U, 0x02U, 0x47U, 0x05U, 0x4BU, 0x00U,
    0x4EU, 0x03U, 0x51U, 0x06U, 0x02U, 0x06U, 0x06U, 0x02U, 0x09U, 0x06U,
    0x0DU, 0x02U, 0x10U, 0x06U, 0x14U, 0x02U, 0x17U, 0x06U, 0x1BU, 0x02U,
    0x1EU, 0x06U, 0x22U, 0x02U, 0x25U, 0x06U, 0x29U, 0x02U, 0x2CU, 0x06U,
    0x30U, 0x01U, 0x33U, 0x04U, 0x36U, 0x07U, 0x3AU, 0x02U, 0x3DU, 0x05U,
    0x41U, 0x00U, 0x44U, 0x03U, 0x47U, 0x06U, 0x4BU, 0x01U, 0x4EU, 0x04U,
    0x51U, 0x07U, 0x02U, 0x07U, 0x06U, 0x03U, 0x09U, 0x07U, 0x0DU, 0x03U,
    0x10U, 0x07U, 0x14U, 0x03U, 0x17U, 0x07U, 0x1BU, 0x03U, 0x1EU, 0x07U,
    0x22U, 0x03U, 0x25U, 0x07U, 0x29U, 0x03U, 0x2CU, 0x07U, 0x30U, 0x02U,
    0x33U, 0x05U, 0x37U, 0x00U, 0x3AU, 0x03U, 0x3DU, 0x06U, 0x41U, 0x01U,
    0x44U, 0x04U, 0x47U, 0x07U, 0x4BU, 0x02U, 0x4EU, 0x05U, 0x52U, 0x00U,
    0x03U, 0x00U, 0x06U, 0x04U, 0x0AU, 0x00U, 0x0DU, 0x04U, 0x11U, 0x00U,
    0x14U, 0x04U, 0x18U, 0x00U, 0x1BU, 0x04U, 0x1FU, 0x00U, 0x22U, 0x04U,
    0x26U, 0x00U, 0x29U, 0x04U, 0x2DU, 0x00U, 0x30U, 0x03U, 0x33U, 0x06U,
    0x37U, 0x01U, 0x3AU, 0x04U, 0x3DU, 0x07U, 0x41U, 0x02U, 0x44U, 0x05U,
    0x48U, 0x00U, 0x4BU, 0x03U, 0x4EU, 0x06U, 0x52U, 0x01U, 0x03U, 0x01U,
    0x06U, 0x05U, 0x0AU, 0x01U, 0x0DU, 0x05U, 0x11U, 0x01U, 0x14U, 0x05U,
    0x18U, 0x01U, 0x1BU, 0x05U, 0x1FU, 0x01U, 0x22U, 0x05U, 0x26U, 0x01U,
    0x29U, 0x05U, 0x2DU, 0x01U, 0x30U, 0x04U, 0x33U, 0x07U, 0x37U, 0x02U,
    0x3AU, 0x05U, 0x3EU, 0x00U, 0x41U, 0x03U, 0x44U, 0x06U, 0x48U, 0x01U,
    0x4BU, 0x04U, 0x4EU, 0x07U, 0x52U, 0x02U, 0x03U, 0x02U, 0x06U, 0x06U,
    0x0AU, 0x02U, 0x0DU, 0x06U, 0x11U, 0x02U, 0x14U, 0x06U, 0x18U, 0x02U,
    0x1BU, 0x06U, 0x1FU, 0x02U, 0x22U, 0x06U, 0x26U, 0x02U, 0x29U, 0x06U,
    0x2DU, 0x02U, 0x30U, 0x05U, 0x34U, 0x00U, 0x37U, 0x03U, 0x3AU, 0x06U,
    0x3EU, 0x01U, 0x41U, 0x04U, 0x44U, 0x07U, 0x48U, 0x02U, 0x4BU, 0x05U,
    0x4FU, 0x00U, 0x52U, 0x03U, 0x03U, 0x03U, 0x06U, 0x07U, 0x0AU, 0x03U,
    0x0DU, 0x07U, 0x11U, 0x03U, 0x14U, 0x07U, 0x18U, 0x03U, 0x1BU, 0x07U,
    0x1FU, 0x03U, 0x22U, 0x07U, 0x26U, 0x03U, 0x29U, 0x07U, 0x2DU, 0x03U,
    0x30U, 0x06U, 0x34U, 0x01U, 0x37U, 0x04U, 0x3AU, 0x07U, 0x3EU, 0x02U,
    0x41U, 0x05U, 0x45U, 0x00U, 0x48U, 0x03U, 0x4BU, 0x06U, 0x4FU, 0x01U,
    0x52U, 0x04U, 0x03U, 0x04U, 0x07U, 0x00U, 0x0AU, 0x04U, 0x0EU, 0x00U,
    0x11U, 0x04U, 0x15U, 0x00U, 0x18U, 0x04U, 0x1CU, 0x00U, 0x1FU, 0x04U,
    0x23U, 0x00U, 0x26U, 0x04U, 0x2AU, 0x00U, 0x2DU, 0x04U, 0x30U, 0x07U,
    0x34U, 0x02U, 0x37U, 0x05U, 0x3BU, 0x00U, 0x3EU, 0x03U, 0x41U, 0x06U,
    0x45U, 0x01U, 0x48U, 0x04U, 0x4BU, 0x07U, 0x4FU, 0x02U, 0x52U, 0x05U,
    0x03U, 0x05U, 0x07U, 0x01U, 0x0AU, 0x05U, 0x0EU, 0x01U, 0x11U, 0x05U,
    0x15U, 0x01U, 0x18U, 0x05U, 0x1CU, 0x01U, 0x1FU, 0x05U, 0x23U, 0x01U,
    0x26U, 0x05U, 0x2AU, 0x01U, 0x2DU, 0x05U, 0x31U, 0x00U, 0x34U, 0x03U,
    0x37U, 0x06U, 0x3BU, 0x01U, 0x3EU, 0x04U, 0x41U, 0x07U, 0x45U, 0x02U,
    0x48U, 0x05U, 0x4CU, 0x00U, 0x4FU, 0x03U, 0x52U, 0x06U, 0x03U, 0x06U,
    0x07U, 0x02U, 0x0AU, 0x06U, 0x0EU, 0x02U, 0x11U, 0x06U, 0x15U, 0x02U,
    0x18U, 0x06U, 0x1CU, 0x02U, 0x1FU, 0x06U, 0x23U, 0x02U, 0x26U, 0x06U,
    0x2AU, 0x02U, 0x2DU, 0x06U, 0x31U, 0x01U, 0x34U, 0x04U, 0x37U, 0x07U,
    0x3BU, 0x02U, 0x3EU, 0x05U, 0x42U, 0x00U, 0x45U, 0x03U, 0x48U, 0x06U,
    0x4CU, 0x01U, 0x4FU, 0x04U, 0x52U, 0x07U, 0x03U, 0x07U, 0x07U, 0x03U,
    0x0AU, 0x07U, 0x0EU, 0x03U, 0x11U, 0x07U, 0x15U, 0x03U, 0x18U, 0x07U,
    0x1CU, 0x03U, 0x1FU, 0x07U, 0x23U, 0x03U, 0x26U, 0x07U, 0x2AU, 0x03U};

const uint8_t SCRAMBLE_TABLE_TX[] = {
    0x00U, 0xF7U, 0x34U, 0x09U, 0x44U, 0x46U, 0xD7U, 0x06U, 0xB3U, 0x72U, 0xDEU,
    0x42U, 0xF5U, 0xA5U, 0xD8U, 0xF1U, 0x87U, 0x7BU, 0x9AU, 0x04U, 0x22U, 0xA3U,
    0x6BU, 0x83U, 0x59U, 0x39U, 0x6FU, 0xA1U, 0xFAU, 0x52U, 0xECU, 0xF8U, 0xC3U,
    0x3DU, 0x4DU, 0x02U, 0x91U, 0xD1U, 0xB5U, 0xC1U, 0xACU, 0x9CU, 0xB7U, 0x50U,
    0x7DU, 0x29U, 0x76U, 0xFCU, 0xE1U, 0x9EU, 0x26U, 0x81U, 0xC8U, 0xE8U, 0xDAU,
    0x60U, 0x56U, 0xCEU, 0x5BU, 0xA8U, 0xBEU, 0x14U, 0x3BU, 0xFEU, 0x70U, 0x4FU,
    0x93U, 0x40U, 0x64U, 0x74U, 0x6DU, 0x30U, 0x2BU, 0xE7U, 0x2DU, 0x54U, 0x5FU,
    0x8AU, 0x1DU, 0x7FU, 0xB8U, 0xA7U, 0x49U, 0x20U, 0x32U, 0xBAU, 0x36U, 0x98U,
    0x95U, 0xF3U, 0x06U};

const uint8_t BIT_SYNC     = 0xAAU;
const uint8_t FRAME_SYNC[] = {0xEAU, 0xA6U, 0x00U};
const uint8_t DSTAR_HEADER = 0x00U;
const uint8_t DSTAR_DATA   = 0x01U;
const uint8_t DSTAR_EOT2   = 0x02U;

#define NOENDPTR 9999U

enum DSRX_STATE
{
    DSRXS_NONE,
    DSRXS_HEADER,
    DSRXS_DATA
};

#define DSTAR_BUFFER_LENGTH_BITS 800U
uint8_t rxBitBuffer[DSTAR_BUFFER_LENGTH_BITS / 8U];
unsigned int dataBits;
unsigned int rxBufferBits;

char MODE_NAME[11] = "DSTAR";
char MODEM_TYPE[6] = "GMSK";
bool USE_LP_FILTER = false;

enum DSRX_STATE rxState;
uint64_t bitBuffer[DSTAR_RADIO_SYMBOL_LENGTH];
q15_t headerBuffer[DSTAR_FEC_SECTION_LENGTH_SAMPLES + 2U * DSTAR_RADIO_SYMBOL_LENGTH];
q15_t dataBuffer[DSTAR_DATA_LENGTH_SAMPLES];
uint16_t bitPtr;
uint16_t headerPtr;
uint16_t dataPtr;
uint16_t startPtr;
uint16_t syncPtr;
uint16_t minSyncPtr;
uint16_t maxSyncPtr;
q31_t maxFrameCorr;
q31_t maxDataCorr;
uint16_t frameCount;
uint8_t countdown;
unsigned int mar;
int pathMetric[4U];
unsigned int pathMemory0[42U];
unsigned int pathMemory1[42U];
unsigned int pathMemory2[42U];
unsigned int pathMemory3[42U];
uint8_t fecOutput[42U];
uint32_t rssiAccum;
uint16_t rssiCount;

arm_biquad_casd_df1_inst_q31 dcFilter;
q31_t dcState[4];

arm_fir_interpolate_instance_q15 txModFilter;
arm_fir_instance_q15 rxGaussianFilter;
q15_t rxGaussianState[40U];  // NoTaps + BlockSize - 1, 12 + 20 - 1 plus some
                             // spare

q15_t txModState[20U];  // blockSize + phaseLength - 1, 8 + 9 - 1 plus some spare
uint8_t symBuffer[600U];
uint16_t symLen;
uint16_t symPtr;
uint16_t txDelay;  // In bytes

const char* TYPE_HEADER     = "DSTH";
const char* TYPE_DATA       = "DSTD";
const char* TYPE_EOT        = "DSTE";
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

const uint8_t AMBE_SILENCE[] = {0x9e, 0x8d, 0x32, 0x32, 0x26,
                                0x1a, 0x3f, 0x61, 0xe8};
const uint8_t DSTAR_SYNC[]   = {0x55, 0x2d, 0x16};
const uint8_t DSTAR_EOT[]    = {0x55, 0x55, 0x55};

uint8_t packetType       = 0;
uint8_t modemId          = 1;
char modemName[8]        = "modem1";
char myCall[9]           = "N0CALL";
char urCall[9]           = "CQCQCQ";
char rpt1Call[9]         = "";
char rpt2Call[9]         = "";
char station_myCall[9]   = "N0CALL";
char station_rpt1Call[9] = "";
char station_rpt2Call[9] = "";
char suffix[5]           = "";
char metaText[23]        = "";
char linkedReflector[10] = "";
bool txOn                = false;
bool validSSHeader       = false;
bool slowSpeedUpdate     = false;
bool validFrame          = false;
bool validHeader         = false;
bool headerSent          = false;
bool debugM              = false;
bool connected           = true;
bool modem_duplex        = false;
bool dstarReflConnected  = false;
bool dstarGWConnected    = false;
uint8_t duration         = 0;
uint8_t dstar_space      = 0;
uint16_t streamId        = 0;
uint16_t modeHang        = 30000;
uint8_t txLevel          = 50;
uint8_t rfPower          = 128;
uint8_t header[49]       = "";
time_t start_time;
char modem_rxFrequency[11] = "435000000";
char modem_txFrequency[11] = "435000000";
uint32_t syncErrors        = 0;

// BER tracking variables
uint32_t totalBitsDecoded = 0;
uint32_t totalBitErrors   = 0;

uint8_t ssHeader[DSTAR_HEADER_LENGTH_BYTES] = "";

char sGps[256]      = "";
char sGPSCall[256]  = "";
float fLat          = 0.0f;
float fLong         = 0.0f;
uint16_t altitude   = 0;
char gps[50]        = "";

char modemHost[80]  = "127.0.0.1";
int sockfd          = 0;
uint16_t serverPort = 18100;
uint16_t clientPort = 18000;
unsigned int clientlen;         //< byte size of client's address
char* hostaddrp;                //< dotted decimal host addr string
int optval;                     //< flag value for setsockopt
struct sockaddr_in serveraddr;  //< server's addr
struct sockaddr_in clientaddr;  //< client addr

RingBuffer rxBuffer;
RingBuffer gwTxBuffer;
RingBuffer gwCommand;

pthread_mutex_t timerMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t rxBufMutex   = PTHREAD_MUTEX_INITIALIZER;  /* Protects rxBuffer */
pthread_mutex_t gwTxBufMutex = PTHREAD_MUTEX_INITIALIZER;  /* Protects gwTxBuffer */
pthread_mutex_t gwCmdMutex   = PTHREAD_MUTEX_INITIALIZER;  /* Protects gwCommand */

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

typedef union crc
{
    uint16_t m_crc16;
    uint8_t m_crc8[2];
} m_crc;
m_crc mcrc;

// Forward declarations
void processBitNone(bool bit);
void decodeBitHeader(bool bit);
void decodeBitData(bool bit);
void processNone(q15_t sample);
void decodeHeader(q15_t sample);
void decodeData();
bool correlateFrameSync();
bool correlateDataSync();
void samplesToBits(const q15_t* inBuffer, uint16_t start, uint16_t count, uint8_t* outBuffer, uint16_t limit);
bool rxHeader(uint8_t* in, uint8_t* out);
void acs(int* metric);
void viterbiDecode(int* data);
void traceBack();
bool checksum(const uint8_t* header);
int slowSpeedDataDecode(unsigned char a, unsigned char b, unsigned char c, char* metaText);
void slowSpeedDataEncode(char* cMessage, unsigned char* ucBytes, unsigned char ucMode);
void processHeader(uint8_t* data, uint8_t length, bool isNet);
void processData(uint8_t* data, uint8_t length, bool genSync, bool isNet);
void processEOT(bool isNet);


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

    if (strcasecmp(active_mode, "DSTAR") == 0 || strcasecmp(active_mode, "IDLE") == 0)
        return true;
    else
        return false;
}

void reset()
{
    rxState      = DSRXS_NONE;
    headerPtr    = 0U;
    dataPtr      = 0U;
    bitPtr       = 0U;
    maxFrameCorr = 0;
    maxDataCorr  = 0;
    startPtr     = NOENDPTR;
    syncPtr      = NOENDPTR;
    minSyncPtr   = NOENDPTR;
    maxSyncPtr   = NOENDPTR;
    frameCount   = 0U;
    countdown    = 0U;
    rssiAccum    = 0U;
    rssiCount    = 0U;

    symLen       = 0U;
    symPtr       = 0U;
    txDelay      = 180U;  // TX preamble length in bytes.
    rxBufferBits = 0U;
    dataBits     = 0U;
    bitBuffer[0] = 0;
    validSSHeader = false;
}

uint32_t calcRawBERFromSync(const uint8_t *rxBits)
{
    uint32_t errors = 0;

    for (uint32_t i = 0; i < 3U; i++)
    {
        uint8_t diff = rxBits[i] ^ DSTAR_SYNC[i];
        errors += __builtin_popcount(diff);
    }

    totalBitErrors   += errors;
    totalBitsDecoded += 24U;

    return errors;
}

bool checkCCITT161(const unsigned char* in, unsigned int length)
{
    assert(in != NULL);
    assert(length > 2U);

    union
    {
        uint16_t crc16;
        uint8_t crc8[2U];
    } crc;
    crc.crc16 = 0xFFFFU;

    for (unsigned int i = 0U; i < (length - 2U); i++)
        crc.crc16 = (uint16_t)(crc.crc8[1U]) ^ CCITT16_TABLE[crc.crc8[0U] ^ in[i]];

    crc.crc16 = ~crc.crc16;
    /* fprintf(stderr, "crc: %02X  %02X    %02X  %02X\n", crc.crc8[0], crc.crc8[1],
    in[length -2], in[length - 1]); */
    return crc.crc8[0U] == in[length - 2U] && crc.crc8[1U] == in[length - 1U];
}

void txHeader(const uint8_t* in, uint8_t* out)
{
    uint8_t intermediate[84U];
    uint32_t i;

    for (i = 0U; i < 83U; i++)
    {
        intermediate[i] = 0x00U;
        out[i]          = 0x00U;
    }

    // Convolve the header
    uint8_t d, d1 = 0U, d2 = 0U, g0, g1;
    uint32_t k = 0U;
    for (i = 0U; i < 42U; i++)
    {
        for (uint8_t j = 0U; j < 8U; j++)
        {
            uint8_t mask = (0x01U << j);
            d            = 0U;

            if (in[i] & mask) d = 1U;

            g0 = (d + d2) & 1;
            g1 = (d + d1 + d2) & 1;
            d2 = d1;
            d1 = d;

            if (g1) intermediate[k >> 3] |= BIT_MASK_TABLE[k & 7];
            k++;

            if (g0) intermediate[k >> 3] |= BIT_MASK_TABLE[k & 7];
            k++;
        }
    }

    // Interleave the header
    i = 0U;
    while (i < 660U)
    {
        unsigned char d = intermediate[i >> 3];

        if (d & 0x80U)
            out[INTERLEAVE_TABLE_TX[i * 2U]] |= (0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
        i++;

        if (d & 0x40U)
            out[INTERLEAVE_TABLE_TX[i * 2U]] |= (0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
        i++;

        if (d & 0x20U)
            out[INTERLEAVE_TABLE_TX[i * 2U]] |= (0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
        i++;

        if (d & 0x10U)
            out[INTERLEAVE_TABLE_TX[i * 2U]] |= (0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
        i++;

        if (i < 660U)
        {
            if (d & 0x08U)
                out[INTERLEAVE_TABLE_TX[i * 2U]] |= (0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
            i++;

            if (d & 0x04U)
                out[INTERLEAVE_TABLE_TX[i * 2U]] |= (0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
            i++;

            if (d & 0x02U)
                out[INTERLEAVE_TABLE_TX[i * 2U]] |= (0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
            i++;

            if (d & 0x01U)
                out[INTERLEAVE_TABLE_TX[i * 2U]] |= (0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
            i++;
        }
    }

    // Scramble the header
    for (i = 0U; i < 83U; i++)
        out[i] ^= SCRAMBLE_TABLE_TX[i];
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

            switch (rxState)
            {
                case DSRXS_NONE:
                    processBitNone(bit);
                break;

                case DSRXS_HEADER:
                    decodeBitHeader(bit);
                break;

                case DSRXS_DATA:
                    decodeBitData(bit);
                break;

                default:
                break;
            }
        }
    }
}

void processBitNone(bool bit)
{
    // Fuzzy matching of the preamble sync sequence
    bitBuffer[0] <<= 1;
    if (bit)
        bitBuffer[0] |= 0x01U;

    if (countBits64((bitBuffer[0] & PREAMBLE_MASK) ^ PREAMBLE_DATA) <= PREAMBLE_ERRS)
    {
        // Extend scan period in D-Star, once preamble is detected

        rxState = DSRXS_NONE;

        return;
    }

    // Fuzzy matching of the frame sync sequence
    if (countBits64((bitBuffer[0] & FRAME_SYNC_MASK) ^ FRAME_SYNC_DATA) <= FRAME_SYNC_ERRS)
    {
        memset(rxBitBuffer, 0x00U, DSTAR_FEC_SECTION_LENGTH_BYTES);
        rxBufferBits = 0U;
    //    fprintf(stderr, "Detected header sync.\n");

        rxState = DSRXS_HEADER;
        return;
    }

    // Exact matching of the data sync bit sequence
    if (countBits64((bitBuffer[0] & DATA_SYNC_MASK) ^ DATA_SYNC_DATA) == 0U)
    {
    //    fprintf(stderr, "Detected data sync.\n");
        processData(rxBitBuffer, DSTAR_DATA_LENGTH_BYTES, false, false);

        memset(rxBitBuffer, 0x00U, DSTAR_DATA_LENGTH_BYTES);
        rxBufferBits = 0U;

        dataBits = MAX_SYNC_BITS;
        rxState    = DSRXS_DATA;
        return;
    }
}

void decodeBitHeader(bool bit)
{
    bitBuffer[0] <<= 1;
    if (bit)
        bitBuffer[0] |= 0x01U;

    WRITE_BIT2(rxBitBuffer, rxBufferBits, bit);

    rxBufferBits++;
    if (rxBufferBits > DSTAR_BUFFER_LENGTH_BITS)
        reset();

    // A full FEC header
    if (rxBufferBits == DSTAR_FEC_SECTION_LENGTH_SYMBOLS)
    {
        // Process the scrambling, interleaving and FEC, then return if the chcksum was correct
        unsigned char header[DSTAR_HEADER_LENGTH_BYTES];
        bool ok = rxHeader(rxBitBuffer, header);
        if (ok)
        {
            processHeader(header, DSTAR_HEADER_LENGTH_BYTES, false);

            memset(rxBitBuffer, 0x00U, DSTAR_DATA_LENGTH_BYTES);
            rxBufferBits = 0U;

            rxState    = DSRXS_DATA;
            dataBits = MAX_SYNC_BITS;
        }
        else
        {
            // The checksum failed, return to looking for syncs
            rxState = DSRXS_NONE;
        //    fprintf(stderr, "Header checksum failed.\n");
        }
    }
}

void decodeBitData(bool bit)
{
    bitBuffer[0] <<= 1;
    if (bit)
        bitBuffer[0] |= 0x01U;

    WRITE_BIT2(rxBitBuffer, rxBufferBits, bit);

    rxBufferBits++;
    if (rxBufferBits > DSTAR_BUFFER_LENGTH_BITS)
        reset();

    // Fuzzy matching of the end frame sequences
    if (countBits64((bitBuffer[0] & END_SYNC_MASK) ^ END_SYNC_DATA) <= END_SYNC_ERRS)
    {
    //    serial.writeDStarEOT();
    //    fprintf(stderr, "EOT dectected.\n");
        processEOT(false);

        rxState = DSRXS_NONE;
        return;
    }

    // Fuzzy matching of the data sync bit sequence
    bool syncSeen = false;
    if (rxBufferBits >= (DSTAR_DATA_LENGTH_BITS - 3U))
    {
        if (countBits64((bitBuffer[0] & DATA_SYNC_MASK) ^ DATA_SYNC_DATA) <= DATA_SYNC_ERRS)
        {
       //     fprintf(stderr, "Detected data sync.\n");
            rxBufferBits = DSTAR_DATA_LENGTH_BITS;
            dataBits     = MAX_SYNC_BITS;
            syncSeen       = true;
        }
    }

    // Check to see if the sync is arriving late
    if (rxBufferBits == DSTAR_DATA_LENGTH_BITS && !syncSeen)
    {
        for (uint8_t i = 1U; i <= 3U; i++)
        {
            uint64_t syncMask = DATA_SYNC_MASK >> i;
            uint64_t syncData = DATA_SYNC_DATA >> i;
            if (countBits64((bitBuffer[0] & syncMask) ^ syncData) <= DATA_SYNC_ERRS)
            {
                rxBufferBits -= i;
                break;
            }
        }
    }

    dataBits--;

    // We've not seen a data sync for too long, signal RXLOST and change to RX_NONE
    if (dataBits == 0U)
    {
    //    fprintf(stderr, "Signal lost.\n");
        processEOT(false);

        rxState = DSRXS_NONE;
        return;
    }

    // Send a data frame to the host if the required number of bits have been received, or if a data sync has been seen
    if (rxBufferBits == DSTAR_DATA_LENGTH_BITS)
    {
        if (syncSeen)
        {
            rxBitBuffer[9U]  = DSTAR_DATA_SYNC_BYTES[9U];
            rxBitBuffer[10U] = DSTAR_DATA_SYNC_BYTES[10U];
            rxBitBuffer[11U] = DSTAR_DATA_SYNC_BYTES[11U];

            processData(rxBitBuffer, DSTAR_DATA_LENGTH_BYTES, false, false);
        }
        else
        {
            processData(rxBitBuffer, DSTAR_DATA_LENGTH_BYTES, false, false);
        }

        // Start the next frame
        memset(rxBitBuffer, 0x00U, DSTAR_DATA_LENGTH_BYTES);
        rxBufferBits = 0U;
    }
}

void writeSamples(uint8_t c, bool isEOT)
{
    static uint8_t bytes[200] = {0x61, 0x00, 0xc8, 0x04, 'S', 'A', 'M', 'P'}; // setup 8 byte header
    static uint8_t bytePos = 8; // set first position after header
    q15_t          inBuffer[8U];
    q15_t          outBuffer[DSTAR_RADIO_SYMBOL_LENGTH * 8U];

    uint8_t mask = 0x01U;

    for (uint8_t i = 0U; i < 8U; i++)
    {
        if ((c & mask) == mask)
            inBuffer[i] = DSTAR_LEVEL0;
        else
            inBuffer[i] = DSTAR_LEVEL1;

        mask <<= 1;
    }

    arm_fir_interpolate_q15(&txModFilter, inBuffer, outBuffer, 8U);

    for (uint16_t i = 0U; i < DSTAR_RADIO_SYMBOL_LENGTH * 8U; i++)
    {
        bytes[bytePos++] = (outBuffer[i] & 0x00ff);
        bytes[bytePos++] = (outBuffer[i] & 0xff00) >> 8;
        if (bytePos == 200)
        {
            write(sockfd, bytes, 200);
            bytePos = 8;
            delay(650);
        }
    }

    if (bytePos > 8 && isEOT) // If EOT and number of new bytes less than 200 pad out with 0.
    {
        memset(bytes+bytePos, 0, 200 - bytePos);
        write(sockfd, bytes, 200);
        bytePos = 8;
    }
}

void samples(const q15_t* samples, const uint16_t* rssi, uint8_t length)
{
    for (uint16_t i = 0U; i < length; i++)
    {
        rssiAccum = 0;  //+= rssi[i];
        rssiCount++;

        q15_t sample = samples[i];  //((samples[i] & 0x00ff) << 8) |
                                    //((samples[i] & 0xff00) >> 8);

        bitBuffer[bitPtr] <<= 1;
        if (sample < 0) bitBuffer[bitPtr] |= 0x01U;

        dataBuffer[dataPtr] = sample;

        switch (rxState)
        {
            case DSRXS_HEADER:
                decodeHeader(sample);
                break;
            case DSRXS_DATA:
                decodeData();
                break;
            default:
                processNone(sample);
                break;
        }

        dataPtr++;
        if (dataPtr >= DSTAR_DATA_LENGTH_SAMPLES) dataPtr = 0U;

        bitPtr++;
        if (bitPtr >= DSTAR_RADIO_SYMBOL_LENGTH) bitPtr = 0U;
    }
}

void processNone(q15_t sample)
{
    // Fuzzy matching of the frame sync sequence
    bool ret = correlateFrameSync();
    if (ret)
    {
        countdown = 5U;

        headerBuffer[headerPtr] = sample;
        headerPtr++;

        rssiAccum = 0U;
        rssiCount = 0U;
        rxState = DSRXS_HEADER;

        return;
    }

    // Fuzzy matching of the data sync bit sequence
    ret = correlateDataSync();
    if (ret)
    {
        //    io.setDecode(true);
        rxState = DSRXS_DATA;
    }
}

void decodeHeader(q15_t sample)
{
    if (countdown > 0U)
    {
        correlateFrameSync();
        countdown--;
    }

    headerBuffer[headerPtr] = sample;
    headerPtr++;

    // A full FEC header
    if (headerPtr == (DSTAR_FEC_SECTION_LENGTH_SAMPLES + DSTAR_RADIO_SYMBOL_LENGTH))
    {
        uint8_t buffer[DSTAR_FEC_SECTION_LENGTH_BYTES];
        samplesToBits(headerBuffer, DSTAR_RADIO_SYMBOL_LENGTH, DSTAR_FEC_SECTION_LENGTH_SYMBOLS, buffer, DSTAR_FEC_SECTION_LENGTH_SAMPLES);

        // Process the scrambling, interleaving and FEC, then return true if the
        // chcksum was correct
        uint8_t header[DSTAR_HEADER_LENGTH_BYTES];
        bool ok = rxHeader(buffer, header);
        if (!ok)
        {
            // The checksum failed, return to looking for syncs
            rxState      = DSRXS_NONE;
            maxFrameCorr = 0;
            maxDataCorr  = 0;
        }
        else
        {
            processHeader(header, 41, false);
        }
    }

    // Ready to start the first data section
    if (headerPtr == (DSTAR_FEC_SECTION_LENGTH_SAMPLES + 2U * DSTAR_RADIO_SYMBOL_LENGTH))
    {
        frameCount = 0U;
        dataPtr    = 0U;

        startPtr   = 476U;
        syncPtr    = 471U;
        maxSyncPtr = 472U;
        minSyncPtr = 470U;

        rxState = DSRXS_DATA;
    }
}

void decodeData()
{
    // Fuzzy matching of the end frame sequences
    if (countBits64((bitBuffer[bitPtr] & DSTAR_END_SYNC_MASK) ^ DSTAR_END_SYNC_DATA) <= END_SYNC_ERRS)
    {
        processEOT(false);

        maxFrameCorr = 0;
        maxDataCorr  = 0;

        rxState = DSRXS_NONE;
        return;
    }

    // Fuzzy matching of the data sync bit sequence
    if (minSyncPtr < maxSyncPtr)
    {
        if (dataPtr >= minSyncPtr && dataPtr <= maxSyncPtr)
            correlateDataSync();
    }
    else
    {
        if (dataPtr >= minSyncPtr || dataPtr <= maxSyncPtr)
            correlateDataSync();
    }

    // We've not seen a data sync for too long, signal RXLOST and change to RX_NONE
    if (frameCount >= MAX_FRAMES)
    {
        processEOT(false);

        maxFrameCorr = 0;
        maxDataCorr  = 0;

        rxState = DSRXS_NONE;
        return;
    }

    // Send a data frame to the host if the required number of bits have been received
    if (dataPtr == maxSyncPtr)
    {
        uint8_t buffer[DSTAR_DATA_LENGTH_BYTES];
        samplesToBits(dataBuffer, startPtr, DSTAR_DATA_LENGTH_SYMBOLS, buffer, DSTAR_DATA_LENGTH_SAMPLES);
        processData(buffer, DSTAR_DATA_LENGTH_BYTES, true, false);

        frameCount++;

        maxFrameCorr = 0;
        maxDataCorr  = 0;
    }
}

bool correlateFrameSync()
{
    if (countBits64((bitBuffer[bitPtr] & DSTAR_FRAME_SYNC_MASK) ^ DSTAR_FRAME_SYNC_DATA) <= FRAME_SYNC_ERRS)
    {
        uint16_t ptr = dataPtr + DSTAR_DATA_LENGTH_SAMPLES - DSTAR_FRAME_SYNC_LENGTH_SAMPLES + DSTAR_RADIO_SYMBOL_LENGTH;
        if (ptr >= DSTAR_DATA_LENGTH_SAMPLES)
            ptr -= DSTAR_DATA_LENGTH_SAMPLES;

        q31_t corr = 0;

        for (uint8_t i = 0U; i < DSTAR_FRAME_SYNC_LENGTH_SYMBOLS; i++)
        {
            q15_t val = dataBuffer[ptr];

            if (DSTAR_FRAME_SYNC_SYMBOLS[i])
                corr -= val;
            else
                corr += val;

            ptr += DSTAR_RADIO_SYMBOL_LENGTH;
            if (ptr >= DSTAR_DATA_LENGTH_SAMPLES)
                ptr -= DSTAR_DATA_LENGTH_SAMPLES;
        }

        if (corr > maxFrameCorr)
        {
            maxFrameCorr = corr;
            headerPtr    = 0U;
            return true;
        }
    }

    return false;
}

bool correlateDataSync()
{
    uint8_t maxErrs = 0U;
    if (rxState == DSRXS_DATA) maxErrs = DATA_SYNC_ERRS;

    if (countBits64((bitBuffer[bitPtr] & DSTAR_DATA_SYNC_MASK) ^ DSTAR_DATA_SYNC_DATA) <= maxErrs)
    {
        uint16_t ptr = dataPtr + DSTAR_DATA_LENGTH_SAMPLES - DSTAR_DATA_SYNC_LENGTH_SAMPLES + DSTAR_RADIO_SYMBOL_LENGTH;
        if (ptr >= DSTAR_DATA_LENGTH_SAMPLES)
            ptr -= DSTAR_DATA_LENGTH_SAMPLES;

        q31_t corr = 0;

        for (uint8_t i = 0U; i < DSTAR_DATA_SYNC_LENGTH_SYMBOLS; i++)
        {
            q15_t val = dataBuffer[ptr];

            if (DSTAR_DATA_SYNC_SYMBOLS[i])
                corr -= val;
            else
                corr += val;

            ptr += DSTAR_RADIO_SYMBOL_LENGTH;
            if (ptr >= DSTAR_DATA_LENGTH_SAMPLES)
                ptr -= DSTAR_DATA_LENGTH_SAMPLES;
        }

        if (corr > maxDataCorr)
        {
            maxDataCorr = corr;
            frameCount  = 0U;

            syncPtr = dataPtr;

            startPtr = dataPtr + DSTAR_RADIO_SYMBOL_LENGTH;
            if (startPtr >= DSTAR_DATA_LENGTH_SAMPLES)
                startPtr -= DSTAR_DATA_LENGTH_SAMPLES;

            maxSyncPtr = syncPtr + 1U;
            if (maxSyncPtr >= DSTAR_DATA_LENGTH_SAMPLES)
                maxSyncPtr -= DSTAR_DATA_LENGTH_SAMPLES;

            minSyncPtr = syncPtr + DSTAR_DATA_LENGTH_SAMPLES - 1U;
            if (minSyncPtr >= DSTAR_DATA_LENGTH_SAMPLES)
                minSyncPtr -= DSTAR_DATA_LENGTH_SAMPLES;

            return true;
        }
    }

    return false;
}

void samplesToBits(const q15_t* inBuffer, uint16_t start, uint16_t count, uint8_t* outBuffer, uint16_t limit)
{
    for (uint16_t i = 0U; i < count; i++)
    {
        q15_t sample = inBuffer[start];

        if (sample < 0)
            WRITE_BIT2(outBuffer, i, true);
        else
            WRITE_BIT2(outBuffer, i, false);

        start += DSTAR_RADIO_SYMBOL_LENGTH;
        if (start >= limit) start -= limit;
    }
}

void acs(int* metric)
{
    int tempMetric[4U];

    unsigned int j = mar >> 3;
    unsigned int k = mar & 7;

    // Pres. state = S0, Prev. state = S0 & S2
    int m1         = metric[0U] + pathMetric[0U];
    int m2         = metric[4U] + pathMetric[2U];
    tempMetric[0U] = m1 < m2 ? m1 : m2;

    // Count bit errors based on path metric difference
    if (m1 < m2)
    {
        pathMemory0[j] &= BIT_MASK_TABLE0[k];
        if (metric[4U] != 0) totalBitErrors++;  // Error detected
    }
    else
    {
        pathMemory0[j] |= BIT_MASK_TABLE1[k];
        if (metric[0U] != 0) totalBitErrors++;  // Error detected
    }

    // Pres. state = S1, Prev. state = S0 & S2
    m1             = metric[1U] + pathMetric[0U];
    m2             = metric[5U] + pathMetric[2U];
    tempMetric[1U] = m1 < m2 ? m1 : m2;
    if (m1 < m2)
    {
        pathMemory1[j] &= BIT_MASK_TABLE0[k];
        if (metric[5U] != 0) totalBitErrors++;
    }
    else
    {
        pathMemory1[j] |= BIT_MASK_TABLE1[k];
        if (metric[1U] != 0) totalBitErrors++;
    }

    // Pres. state = S2, Prev. state = S2 & S3
    m1             = metric[2U] + pathMetric[1U];
    m2             = metric[6U] + pathMetric[3U];
    tempMetric[2U] = m1 < m2 ? m1 : m2;
    if (m1 < m2)
    {
        pathMemory2[j] &= BIT_MASK_TABLE0[k];
        if (metric[6U] != 0) totalBitErrors++;
    }
    else
    {
        pathMemory2[j] |= BIT_MASK_TABLE1[k];
        if (metric[2U] != 0) totalBitErrors++;
    }

    // Pres. state = S3, Prev. state = S1 & S3
    m1             = metric[3U] + pathMetric[1U];
    m2             = metric[7U] + pathMetric[3U];
    tempMetric[3U] = m1 < m2 ? m1 : m2;
    if (m1 < m2)
    {
        pathMemory3[j] &= BIT_MASK_TABLE0[k];
        if (metric[7U] != 0) totalBitErrors++;
    }
    else
    {
        pathMemory3[j] |= BIT_MASK_TABLE1[k];
        if (metric[3U] != 0) totalBitErrors++;
    }

    for (unsigned int i = 0U; i < 4U; i++) pathMetric[i] = tempMetric[i];

    mar++;
    totalBitsDecoded += 2;  // We decode 2 bits per call
}

void viterbiDecode(int* data)
{
    int metric[8U];

    metric[0] = (data[1] ^ 0) + (data[0] ^ 0);
    metric[1] = (data[1] ^ 1) + (data[0] ^ 1);
    metric[2] = (data[1] ^ 1) + (data[0] ^ 0);
    metric[3] = (data[1] ^ 0) + (data[0] ^ 1);
    metric[4] = (data[1] ^ 1) + (data[0] ^ 1);
    metric[5] = (data[1] ^ 0) + (data[0] ^ 0);
    metric[6] = (data[1] ^ 0) + (data[0] ^ 1);
    metric[7] = (data[1] ^ 1) + (data[0] ^ 0);

    acs(metric);
}

void traceBack()
{
    // Start from the S0, t=31
    unsigned int j = 0U;
    unsigned int k = 0U;
    for (int i = 329; i >= 0; i--)
    {
        switch (j)
        {
            case 0U:  // if state = S0
                if (!READ_BIT1(pathMemory0, i))
                    j = 0U;
                else
                    j = 2U;
                WRITE_BIT1(fecOutput, k, false);
                k++;
                break;

            case 1U:  // if state = S1
                if (!READ_BIT1(pathMemory1, i))
                    j = 0U;
                else
                    j = 2U;
                WRITE_BIT1(fecOutput, k, true);
                k++;
                break;

            case 2U:  // if state = S1
                if (!READ_BIT1(pathMemory2, i))
                    j = 1U;
                else
                    j = 3U;
                WRITE_BIT1(fecOutput, k, false);
                k++;
                break;

            case 3U:  // if state = S1
                if (!READ_BIT1(pathMemory3, i))
                    j = 1U;
                else
                    j = 3U;
                WRITE_BIT1(fecOutput, k, true);
                k++;
                break;
        }
    }
}

bool checksum(const uint8_t* header)
{
    union
    {
        uint16_t crc16;
        uint8_t crc8[2U];
    } crc;
    crc.crc16 = 0xFFFFU;
    for (uint8_t i = 0U; i < (DSTAR_HEADER_LENGTH_BYTES - 2U); i++)
        crc.crc16 = ((uint16_t)(crc.crc8[1U])) ^ CCITT16_TABLE[crc.crc8[0U] ^ header[i]];

    crc.crc16 = ~crc.crc16;

    return crc.crc8[0U] == header[DSTAR_HEADER_LENGTH_BYTES - 2U] &&
           crc.crc8[1U] == header[DSTAR_HEADER_LENGTH_BYTES - 1U];
}

bool rxHeader(uint8_t* in, uint8_t* out)
{
    int i;

    // Descramble the header
    for (i = 0; i < (int)(DSTAR_FEC_SECTION_LENGTH_BYTES); i++)
        in[i] ^= SCRAMBLE_TABLE_RX[i];

    unsigned char intermediate[84U];
    for (i = 0; i < 84; i++) intermediate[i] = 0x00U;

    // Deinterleave the header
    i = 0;
    while (i < 660)
    {
        unsigned char d = in[i / 8];

        if (d & 0x01U)
            intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |=
                (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
        i++;

        if (d & 0x02U)
            intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |=
                (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
        i++;

        if (d & 0x04U)
            intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |=
                (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
        i++;

        if (d & 0x08U)
            intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |=
                (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
        i++;

        if (i < 660)
        {
            if (d & 0x10U)
                intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |=
                    (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
            i++;

            if (d & 0x20U)
                intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |=
                    (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
            i++;

            if (d & 0x40U)
                intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |=
                    (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
            i++;

            if (d & 0x80U)
                intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |=
                    (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
            i++;
        }
    }

    for (i = 0; i < 4; i++) pathMetric[i] = 0;

    int decodeData[2U];

    mar = 0U;
    for (i = 0; i < 660; i += 2)
    {
        if (intermediate[i >> 3] & (0x80U >> (i & 7)))
            decodeData[1U] = 1U;
        else
            decodeData[1U] = 0U;

        if (intermediate[i >> 3] & (0x40U >> (i & 7)))
            decodeData[0U] = 1U;
        else
            decodeData[0U] = 0U;

        viterbiDecode(decodeData);
    }

    traceBack();

    for (i = 0; i < (int)(DSTAR_HEADER_LENGTH_BYTES); i++) out[i] = 0x00U;

    unsigned int j = 0;
    for (i = 329; i >= 0; i--)
    {
        if (READ_BIT1(fecOutput, i)) out[j >> 3] |= (0x01U << (j & 7));

        j++;
    }

    return checksum(out);
}

// Decode GPS packets in slow-speed bytes.
/* Forward declaration for C++ GPS parsing module */
extern bool dstar_parse_gps(unsigned char c,
                           char* gps_buffer, int* gps_idx,
                           char* gps_call_out,
                           float* lat_out, float* lon_out, uint16_t* alt_out);

/* Wrapper function that calls C++ GPS parser */
bool decodeGPS(unsigned char c)
{
    static char tgps[201];
    static int gpsidx = 0;
    
    return dstar_parse_gps(c, tgps, &gpsidx, sGPSCall, &fLat, &fLong, &altitude);
}

// Decode slow-speed data bytes.
int slowSpeedDataDecode(unsigned char a, unsigned char b, unsigned char c, char* metaText)
{
    static bool bSyncFound;
    static bool bFirstSection;
    static bool bHeaderActive;
    static int iSection;
    static unsigned char ucType;
    int iRet = 0;
    static char cText[30];
    static uint8_t header[50];
    static uint8_t ptr = 0;
    static uint8_t hBuffer[5];

    // Unscramble
    a ^= 0x70;
    b ^= 0x4f;
    c ^= 0x93;

    if (a == 0x25 && b == 0x62 && c == 0x85)
    {
        bSyncFound    = true;
        bFirstSection = true;
        bHeaderActive = false;
        bzero(cText, 30);
        memset(cText, 0x20, 29);
        ptr = 0;
        ucType = 0x00;
        memset(hBuffer, 0x00U, 5);
        memset(header, 0x00U, DSTAR_HEADER_LENGTH_BYTES);
        return 20;
    }

    if (bFirstSection)
    {
        ucType = (unsigned char)(a & 0xf0);

        // DV Header start
        if (bSyncFound == true && ucType == 0x50)
        {
            bSyncFound    = false;
            bHeaderActive = true;
        }

        bFirstSection = false;

        switch (ucType)
        {
            case 0x50:  // Header
            {
                if (bHeaderActive)
                {
                    hBuffer[0] = b;
                    hBuffer[1] = c;
                }
            }
                break;

            case 0x30:  // GPS
                if (decodeGPS(b))
                    iRet = 2;
                else if (decodeGPS(c))
                    iRet = 2;
                break;

            case 0x40:  // Text
                iSection = a & 0x0f;
                if (iSection >= 4)
                {
                    break;
                }
                cText[iSection * 5 + 0] = b & 0x7FU;
                cText[iSection * 5 + 1] = c & 0x7FU;
                break;

            case 0xc0:  // Code Squelch
                break;

            default:
                break;
        }
    }
    else
    {
        switch (ucType)
        {
            case 0x50:  // Header
            {
                if (bHeaderActive)
                {
                    hBuffer[2] = a;
                    hBuffer[3] = b;
                    hBuffer[4] = c;

                    if (ptr < 45U)
                    {
                     //   if ((a & 0x0f) == 0x05)
                        memcpy(header + ptr, hBuffer, 5U);
                        ptr += 5U;

                        // Clean up the data
                        header[0U] &= (DSTAR_INTERRUPTED_MASK | DSTAR_URGENT_MASK | DSTAR_REPEATER_MASK);
                        header[1U] = 0x00U;
                        header[2U] = 0x00U;

                        for (unsigned int i = 3U; i < 39U; i++)
                            header[i] &= 0x7FU;

                        // Check the CRC
                        bool ret = checkCCITT161(header, DSTAR_HEADER_LENGTH_BYTES);
                        if (!ret)
                        {
                            if (ptr >= 45U)
                            {
                                fprintf(stderr, "D-Star, invalid slow data header\n");
                                ptr = 0;
                            }

                            validSSHeader = false;
                            bFirstSection = true;
                            return 0;
                        }
//                        dump((char*)"SS Header", header, DSTAR_HEADER_LENGTH_BYTES);
                        validSSHeader = true;
                        memcpy(ssHeader, header, DSTAR_HEADER_LENGTH_BYTES);
                        bHeaderActive = false;
                        ptr = 0;
                        iRet = 1;
                    }
                }
            }
                break;

            case 0x30:  // GPS
                if (decodeGPS(a))
                    iRet = 2;
                else if (decodeGPS(b))
                    iRet = 2;
                else if (decodeGPS(c))
                    iRet = 2;
                break;

            case 0x40:  // Text
                cText[iSection * 5 + 2] = a & 0x7FU;
                cText[iSection * 5 + 3] = b & 0x7FU;
                cText[iSection * 5 + 4] = c & 0x7FU;
                if (iSection == 3)
                {
                    if (memcmp(cText, metaText, 20) != 0)
                    {
                        slowSpeedUpdate = true;
                        memcpy(metaText, cText, 20);
                        metaText[20]    = 0;
                        slowSpeedUpdate = false;
                    }
                    iRet = 3;
                    memset(cText, 0x20, 30);
                }
                break;

            case 0xc0:  // Code Squelch
                break;
        }
        bFirstSection = true;
    }
    return iRet;
}

// Encode message to slow-speed bytes.
void slowSpeedDataEncode(char* cMessage, unsigned char* ucBytes, unsigned char ucMode)
{
    static int iIndex;
    static unsigned char ucFrameCount;

    if (ucMode == 10)
    {
        ucFrameCount = 0;
        return;
    }

    int iSyncfrm = ucFrameCount % 21;

    if (ucFrameCount > 251)
        ucFrameCount = 0;
    else
        ucFrameCount++;

    if (iSyncfrm == 0)
    {
        ucBytes[0] = 0x25 ^ 0x70;
        ucBytes[1] = 0x62 ^ 0x4f;
        ucBytes[2] = 0x85 ^ 0x93;
        iIndex     = 0;
        return;
    }

    if (ucMode == 0)
    {
        char slowMessage[40];
        memset(slowMessage, 0x20, 39);

        if (iIndex <= 36)
        {
            int iSection = iIndex % 5;
            if (iSection == 0)
            {
                if (iIndex == 36)
                    ucBytes[0] = 0x51;
                else
                    ucBytes[0] = 0x55;
                ucBytes[1] = (unsigned char)slowMessage[iIndex++];
                ucBytes[2] = (unsigned char)slowMessage[iIndex++];
            }
            else
            {
                ucBytes[0] = (unsigned char)slowMessage[iIndex++];
                ucBytes[1] = (unsigned char)slowMessage[iIndex++];
                ucBytes[2] = (unsigned char)slowMessage[iIndex++];
            }
        }
        else
        {
            ucBytes[0] = 0x66;
            ucBytes[1] = 0x66;
            ucBytes[2] = 0x66;
        }
    }

    if (ucMode == 1)
    {
        if (iIndex <= 17)
        {
            int iSection = iIndex % 5;
            if (iSection == 0)
            {
                ucBytes[0] = (unsigned char)((0x40 + iIndex / 5) ^ 0x70);
                ucBytes[1] = (unsigned char)cMessage[iIndex++] ^ 0x4f;
                ucBytes[2] = (unsigned char)cMessage[iIndex++] ^ 0x93;
            }
            else
            {
                ucBytes[0] = (unsigned char)cMessage[iIndex++] ^ 0x70;
                ucBytes[1] = (unsigned char)cMessage[iIndex++] ^ 0x4f;
                ucBytes[2] = (unsigned char)cMessage[iIndex++] ^ 0x93;
            }
        }
        else
        {
            ucBytes[0] = 0x66 ^ 0x70;
            ucBytes[1] = 0x66 ^ 0x4f;
            ucBytes[2] = 0x66 ^ 0x93;
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
                        idle = true;
                        resetTimer("modeHang");
                        fprintf(stderr, "Sending set IDLE mode.\n");
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

void processHeader(uint8_t* data, uint8_t length, bool isNet)
{
    uint8_t buf[49];
    buf[0] = 0x61;
    buf[1] = 0x00;
    buf[2] = 0x31;
    buf[3] = 0x04;
    memcpy(buf + 4, TYPE_HEADER, 4);
    memcpy(buf + 8, data, length);
    memset(header, 0x00, 49);
    memcpy(header, data, length);

    bzero(myCall, 9);
    bzero(urCall, 9);
    bzero(suffix, 5);
    bzero(rpt1Call, 9);
    bzero(rpt2Call, 9);
    memcpy(rpt2Call, buf + 11, 8);
    memcpy(rpt1Call, buf + 19, 8);
    memcpy(urCall, buf + 27, 8);
    memcpy(myCall, buf + 35, 8);
    memcpy(suffix, buf + 43, 4);

    if (strcasecmp(rpt1Call, "DIRECT") == 0 || strcasecmp(rpt2Call, "DIRECT") == 0) return;

    if (strcasecmp(rpt1Call, station_rpt1Call) == 0)
    {
        validFrame = true;

        // Reset BER counters for new transmission
        totalBitsDecoded = 0;
        totalBitErrors = 0;

        if (!txOn)
            write(sockfd, SETMODE, 5);

        txOn = true;

        validHeader = true;
        frameCount = 0;

        metaText[0] = 0;
        start_time = time(NULL);

        char type[4] = "RF";
        if (isNet)
            strcpy(type, "NET");

        saveLastCall(1, modemName, "DSTAR", type, myCall, suffix, urCall, metaText, NULL, gps, true);

        fprintf(stderr, "DSTAR Header: %s  %s  %s  %s  %s\n", rpt1Call, rpt2Call, urCall, myCall, suffix);

        if (dstarGWConnected && RingBuffer_freeSpace(&gwTxBuffer) >= 49 && (strcasecmp(rpt2Call, station_rpt2Call) == 0) && !isNet)
        {
            pthread_mutex_lock(&gwTxBufMutex);
            RingBuffer_addData(&gwTxBuffer, buf, 49);
            pthread_mutex_unlock(&gwTxBufMutex);
            headerSent = true;
        }

        // only retransmit RF data if in duplex mode
        if (modem_duplex || isNet)
        {
            if (packetType == PACKET_TYPE_FRAME)
                write(sockfd, buf, length + 8);
            else
            {
                pthread_mutex_lock(&rxBufMutex);
                RingBuffer_addData(&rxBuffer, buf, length + 8);
                pthread_mutex_unlock(&rxBufMutex);
            }

            headerSent = true;
        }
    }
}

void processData(uint8_t* data, uint8_t length, bool genSync, bool isNet)
{
    if (!txOn && validFrame)
    {
        write(sockfd, SETMODE, 5);
        txOn = true;
        frameCount = 0;
    }

    uint8_t buffer[DSTAR_DATA_LENGTH_BYTES + 8U];
    buffer[0] = 0x61;
    buffer[1] = 0x00;
    buffer[2] = 0x14;
    buffer[3] = 0x04;
    memcpy(buffer + 4, TYPE_DATA, 4);
    memcpy(buffer + 8, data, length);

    if ((frameCount % 21U) == 0 && genSync)
    {
        if (frameCount == 0U)
        {
            validFrame  = true;
            txOn        = true;
            memcpy(buffer + 17, DSTAR_DATA_SYNC_BYTES + 9, 3);
        }
        else
        {
            /* Measure raw BER against the known sync pattern.
             * buffer + 17 holds the 3 received sync bytes (24 bits). */
            syncErrors = calcRawBERFromSync(buffer + 17);
            totalBitErrors += syncErrors;
            totalBitsDecoded += 24;
            if (debugM)
                fprintf(stderr, "Sync BER: %u/24 errors\n", syncErrors);
        }
    }

    int ret = slowSpeedDataDecode(buffer[17], buffer[18], buffer[19], metaText);
    if (ret == 20 && !genSync)
        frameCount = 21;

    if ((frameCount % 21) == 0 && !genSync)
    {
        /* Measure raw BER against the known sync pattern.
         * buffer + 17 holds the 3 received sync bytes (24 bits). */
        syncErrors = calcRawBERFromSync(buffer + 17);
        totalBitErrors += syncErrors;
        totalBitsDecoded += 24;
        if (debugM)
            fprintf(stderr, "Sync BER: %u/24 errors\n", syncErrors);
        frameCount++;
    }

    if (isTimerTriggered("status"))
    {
        if (validFrame)
        {
            char type[4] = "RF";
            if (isNet)
                strcpy(type, "NET");

            saveLastCall(1, modemName, "DSTAR", type, myCall, suffix, urCall, metaText, NULL, gps, true);
        }
        resetTimer("status");
    }

    if (!validFrame && validSSHeader)
    {
        bzero(myCall, 9);
        bzero(urCall, 9);
        bzero(suffix, 5);
        bzero(rpt1Call, 9);
        bzero(rpt2Call, 9);
        memcpy(rpt2Call, ssHeader + 3, 8);
        memcpy(rpt1Call, ssHeader + 11, 8);
        memcpy(urCall, ssHeader + 19, 8);
        memcpy(myCall, ssHeader + 27, 8);
        memcpy(suffix, ssHeader + 35, 4);
        memcpy(header, ssHeader, 41);

        if (strcasecmp(rpt1Call, "DIRECT") == 0 ||
            strcasecmp(rpt2Call, "DIRECT") == 0 ||
            strcasecmp(rpt1Call, station_rpt1Call) != 0) return;

        validFrame = true;
        validHeader = true;
    }

    if (validFrame)
    {
        if (RingBuffer_freeSpace(&gwTxBuffer) >= 20 && (strcasecmp(rpt1Call, station_rpt1Call) == 0))
        {
            if (validHeader && !headerSent && header[0] != 0)
            {
                uint8_t buf[49];
                buf[0] = 0x61;
                buf[1] = 0x00;
                buf[2] = 0x31;
                buf[3] = 0x04;
                memcpy(buf + 4, TYPE_HEADER, 4);
                memcpy(buf + 8, header, 41);

                if (dstarGWConnected && RingBuffer_freeSpace(&gwTxBuffer) >= 49 && (strcasecmp(rpt2Call, station_rpt2Call) == 0) && !isNet)
                {
                    pthread_mutex_lock(&gwTxBufMutex);
                    RingBuffer_addData(&gwTxBuffer, buf, 49);
                    pthread_mutex_unlock(&gwTxBufMutex);
                }

                if (modem_duplex || isNet)
                {
                    if (packetType == PACKET_TYPE_FRAME)
                        write(sockfd, buf, 49);
                    else
                    {
                        pthread_mutex_lock(&rxBufMutex);
                        RingBuffer_addData(&rxBuffer, buffer, length + 8);
                        pthread_mutex_unlock(&rxBufMutex);
                    }
                }

                headerSent = true;
                start_time = time(NULL);

                char type[4] = "RF";
                if (isNet)
                    strcpy(type, "NET");

                saveLastCall(1, modemName, "DSTAR", type, myCall, suffix, urCall, metaText, NULL, gps, true);

                fprintf(stderr, "DSTAR SS Header: %s  %s  %s  %s  %s\n", rpt1Call, rpt2Call, urCall, myCall, suffix);
            }

            if (!isNet)
            {
                pthread_mutex_lock(&gwTxBufMutex);
                RingBuffer_addData(&gwTxBuffer, buffer, 20);
                pthread_mutex_unlock(&gwTxBufMutex);
            }
        }

        if ((strcasecmp(rpt1Call, station_rpt1Call) == 0) && (modem_duplex || isNet))
        {
            if (packetType == PACKET_TYPE_FRAME)
                write(sockfd, buffer, length + 8);
            else
            {
                pthread_mutex_lock(&rxBufMutex);
                RingBuffer_addData(&rxBuffer, buffer, length + 8);
                pthread_mutex_unlock(&rxBufMutex);
            }
        }
    }

    if (genSync)
        frameCount++;
}

void processEOT(bool isNet)
{
    float loss_BER = 0.0f;

    if (validFrame)
    {
        gps[0] = 0;
        if (fLat != 0.0f && fLong != 0.0f)
        {
            fprintf(stderr, "Lat: %f  Long: %f  Alt: %d\n", fLat, fLong, altitude);
            sprintf(gps, "%f %f %d 0 0", fLat, fLong, altitude);
        }

        // Calculate BER
     //   float loss_BER = ((float)syncErrors / 24.0f) * 100.0f;
        if (totalBitsDecoded > 0)
        {
            loss_BER = (float)totalBitErrors / (float)totalBitsDecoded;
            fprintf(stderr, "BER: %.6f (%u errors / %u bits)\n", loss_BER, totalBitErrors, totalBitsDecoded);
        }

        duration = difftime(time(NULL), start_time);

        char type[4] = "RF";
        if (isNet)
            strcpy(type, "NET");

        saveLastCall(1, modemName, "DSTAR", type, myCall, suffix, urCall, metaText, NULL, gps, false);
        saveHistory(modemName, "DSTAR", type, myCall, suffix, urCall, loss_BER, metaText, duration);

        fprintf(stderr, "Text: %s\n", metaText);
    }

    if (validFrame)
    {
        uint8_t buf[8];
        buf[0] = 0x61;
        buf[1] = 0x00;
        buf[2] = 0x08;
        buf[3] = 0x04;
        memcpy(buf + 4, TYPE_EOT, 4);

        if (!isNet && dstarGWConnected && RingBuffer_freeSpace(&gwTxBuffer) >= 8 && (strcasecmp(rpt2Call, station_rpt2Call) == 0))
        {
            pthread_mutex_lock(&gwTxBufMutex);
            RingBuffer_addData(&gwTxBuffer, buf, 8);
            pthread_mutex_unlock(&gwTxBufMutex);
        }

        if ((strcasecmp(rpt1Call, station_rpt1Call) == 0) && (modem_duplex || isNet))
        {
            write(sockfd, buf, 8);
            delay(300000);

            // Canned EOT sending BER message.
            // ***********************************************************************
            uint8_t buf2[49] = {0x61, 0x00, 0x31, 0x04, 'D', 'S', 'T', 'H'};
            memcpy(buf2 + 8, header, 41);
            write(sockfd, buf2, 49);
            delay(18000);

            buf2[2] = 0x14;
            buf2[7] = 'D';
            memcpy(buf2 + 8, AMBE_SILENCE, 9);
            write(sockfd, buf2, 20);
            delay(18000);

            //strcpy(metaText, "Have a nice day.   ");
            bzero(metaText, 23);
            sprintf(metaText, "Your BER: %2.1f      ", loss_BER);
            for (uint8_t i = 0; i < 19; i++)
            {
                slowSpeedDataEncode(metaText, buf2 + 17, 1);
                write(sockfd, buf2, 20);
                delay(18000);
            }

            memcpy(buf2 + 4, TYPE_EOT, 4);
            memcpy(buf2 + 17, DSTAR_EOT, 3);
            write(sockfd, buf2, 20);
            // ***********************************************************************
        }
    }

    bzero(myCall, 9);
    bzero(urCall, 9);
    bzero(suffix, 5);
    bzero(metaText, 23);
    strcpy(rpt1Call, "DIRECT");
    strcpy(rpt2Call, "DIRECT");
    bzero(header, 49);
    bzero(ssHeader, 41);
    fLat = 0.0f;
    fLong = 0.0f;
    txOn          = false;
    validHeader   = false;
    validSSHeader = false;
    validFrame    = false;
    headerSent    = false;
}

void processTx(uint8_t* data, const uint8_t length, const uint8_t type, bool m_tx)
{
    if (type == DSTAR_HEADER && symLen == 0U)
    {
        if (!m_tx)
        {
            for (uint16_t i = 0U; i < txDelay; i++)
                symBuffer[symLen++] = BIT_SYNC;
        }

        if (length != DSTAR_HEADER_LENGTH_BYTES) return;

        uint8_t header[DSTAR_HEADER_LENGTH_BYTES];
        memcpy(header, data, DSTAR_HEADER_LENGTH_BYTES);

        uint8_t buffer[86U];
        txHeader(header, buffer + 2U);

        buffer[0U]  = FRAME_SYNC[0U];
        buffer[1U]  = FRAME_SYNC[1U];
        buffer[2U] |= FRAME_SYNC[2U];

        for (uint8_t i = 0U; i < 85U; i++)
            symBuffer[symLen++] = buffer[i];

        symPtr = 0U;
        fprintf(stderr, "Header\n");
    }

    if (type == DSTAR_DATA && symLen == 0U)
    {
        if (length != DSTAR_DATA_LENGTH_BYTES) return;

        for (uint8_t i = 0U; i < DSTAR_DATA_LENGTH_BYTES; i++)
            symBuffer[symLen++] = data[i];

        symPtr = 0U;
        fprintf(stderr, "Data\n");
    }

    if (type == DSTAR_EOT2 && symLen == 0U)
    {
        for (uint8_t j = 0U; j < 3U; j++)
        {
            for (uint8_t i = 0U; i < DSTAR_END_SYNC_LENGTH_BYTES; i++)
                symBuffer[symLen++] = DSTAR_END_SYNC_BYTES[i];
        }

        symPtr = 0U;
        fprintf(stderr, "EOT\n");
    }

    while (symLen > 0U)
    {
        uint8_t c = symBuffer[symPtr++];
        if (packetType == PACKET_TYPE_SAMP)
        {
            if (type == DSTAR_EOT2 && symPtr >= symLen)
                writeSamples(c, true);
            else
                writeSamples(c, false);
        }
        else if (packetType == PACKET_TYPE_BIT)
        {
            if (type == DSTAR_EOT2 && symPtr >= symLen)
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
                    if (len == 5)
                    {
                        if (write(sockfd, buf, len) < 0)
                        {
                            fprintf(stderr, "ERROR: remote disconnect\n");
                            pthread_mutex_unlock(&rxBufMutex);
                            break;
                        }
                        pthread_mutex_unlock(&rxBufMutex);
                        continue;
                    }
                    uint8_t type[4];
                    memcpy(type, buf + 4, 4);
                    if (memcmp(type, TYPE_HEADER, 4) == 0)
                    {
                        processTx(buf + 8, 41, DSTAR_HEADER, false);
                    }
                    else if (memcmp(type, TYPE_DATA, 4) == 0)
                    {
                        processTx(buf + 8, 12, DSTAR_DATA, true);
                    }
                    else if (memcmp(type, TYPE_EOT, 4) == 0)
                    {
                        processTx(buf + 8, 12, DSTAR_EOT2, true);
                    }
                }
            }
            loop = 0;
            pthread_mutex_unlock(&rxBufMutex);
        }
    }

    fprintf(stderr, "RX thread exited.\n");
    int iRet         = 500;
    dstarGWConnected = false;
    connected        = false;
    pthread_exit(&iRet);
    return NULL;
}

// Thread to send out going bytes to gateway.
void* txThread(void* arg)
{
    int     sockfd = (intptr_t)arg;
    uint8_t loop   = 0;

    while (connected && dstarGWConnected)
    {
        delay(100);
        loop++;

        if (loop > 100)
        {
            pthread_mutex_lock(&gwTxBufMutex);
            if (RingBuffer_dataSize(&gwTxBuffer) >= 5)
            {
                uint8_t buf[1];
                RingBuffer_peek(&gwTxBuffer, buf, 1);
                if (buf[0] != 0x61)
                {
                    fprintf(stderr, "TX invalid header.\n");
                    pthread_mutex_unlock(&gwTxBufMutex);
                    continue;
                }
                uint8_t byte[3];
                uint16_t len = 0;
                RingBuffer_peek(&gwTxBuffer, byte, 3);
                len = (byte[1] << 8) + byte[2];
                ;
                if (RingBuffer_dataSize(&gwTxBuffer) >= len)
                {
                    uint8_t buf[len];
                    RingBuffer_getData(&gwTxBuffer, buf, len);
                    if (write(sockfd, buf, len) < 0)
                    {
                        fprintf(stderr, "ERROR: remote disconnect\n");
                        RingBuffer_clear(&gwTxBuffer);
                        pthread_mutex_unlock(&gwTxBufMutex);
                        pthread_mutex_lock(&gwCmdMutex);
                        RingBuffer_clear(&gwCommand);
                        pthread_mutex_unlock(&gwCmdMutex);
                        break;
                    }
                }
            }
            loop = 0;
            pthread_mutex_unlock(&gwTxBufMutex);
        }

        // Send command data to gateway.
        pthread_mutex_lock(&gwCmdMutex);
        if (RingBuffer_dataSize(&gwCommand) >= 5)
        {
            uint8_t buf[1];
            RingBuffer_peek(&gwCommand, buf, 1);
            if (buf[0] != 0x61)
            {
                fprintf(stderr, "Command invalid header.\n");
            }
            else
            {
                uint8_t byte[3];
                uint16_t len = 0;
                RingBuffer_peek(&gwCommand, byte, 3);
                len = (byte[1] << 8) + byte[2];
                ;
                if (RingBuffer_dataSize(&gwCommand) >= len)
                {
                    uint8_t buf[len];
                    RingBuffer_getData(&gwCommand, buf, len);
                    if (write(sockfd, buf, len) < 0)
                    {
                        fprintf(stderr, "Command ERROR: remote disconnect\n");
                        pthread_mutex_unlock(&gwCmdMutex);
                        pthread_mutex_lock(&gwTxBufMutex);
                        RingBuffer_clear(&gwTxBuffer);
                        pthread_mutex_unlock(&gwTxBufMutex);
                        pthread_mutex_lock(&gwCmdMutex);
                        RingBuffer_clear(&gwCommand);
                        pthread_mutex_unlock(&gwCmdMutex);
                        break;
                    }
                }
            }
        }
        pthread_mutex_unlock(&gwCmdMutex);
    }

    fprintf(stderr, "TX thread exited.\n");
    int iRet         = 500;
    dstarGWConnected = false;
    pthread_exit(&iRet);
    return NULL;
}

// Process incoming gateway bytes.
void* processGatewaySocket(void* arg)
{
    int      childfd      = (intptr_t)arg;
    ssize_t  len          = 0;
    uint8_t  offset       = 0;
    uint16_t respLen      = 0;
    uint8_t  typeLen      = 0;
    uint8_t  buffer[BUFFER_SIZE];
    char     gps[50]      = "";
    char     myCall[9];
    char     urCall[9];
    char     suffix[5];
    char     rpt1Call[9];
    char     rpt2Call[9];
    char     metaText[23] = "";

    dstarGWConnected = true;
    addGateway(modemName, "main", "DSTAR");

    while (dstarGWConnected)
    {
        int len = read(childfd, buffer, 1);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            error((char*)"ERROR: DSTAR gateway connection closed remotely.");
            break;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            delay(5);
       //     continue;
        }

        if (len != 1)
        {
            fprintf(stderr, "DSTAR_Service: error when reading from DSTAR gateway, errno=%d\n", errno);
            close(childfd);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "DSTAR_Service: unknown byte from DTAR gateway, 0x%02X\n", buffer[0]);
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
            dump((char*)"DSTAR Gateway data:", (unsigned char*)buffer, respLen);

        if (memcmp(type, TYPE_NACK, typeLen) == 0)
        {
            dstarReflConnected = false;
            char tmp[8];
            bzero(tmp, 8);
            memcpy(tmp, buffer + 4 + typeLen, 7);
            ackDashbCommand(modemName, "reflLinkDSTAR", "failed");
            setReflectorStatus(modemName, "DSTAR", (const char*)tmp, false);
        }
        else if (memcmp(type, TYPE_CONNECT, typeLen) == 0)
        {
            dstarReflConnected = true;
            ackDashbCommand(modemName, "reflLinkDSTAR", "success");
            char tmp[10];
            bzero(tmp, 10);
            memcpy(tmp, buffer + 4 + typeLen, 7);
            tmp[7] = ' ';
            tmp[8] = buffer[15];
            setReflectorStatus(modemName, "DSTAR", (const char*)tmp, true);
            strcpy(linkedReflector, tmp);
        }
        else if (memcmp(type, TYPE_DISCONNECT, typeLen) == 0)
        {
            dstarReflConnected = false;
            char tmp[10];
            bzero(tmp, 10);
            memcpy(tmp, buffer + 4 + typeLen, 7);
            tmp[7] = ' ';
            tmp[8] = buffer[15];
            ackDashbCommand(modemName, "reflLinkDSTAR", "success");
            setReflectorStatus(modemName, "DSTAR", (const char*)tmp, false);
        }
        else if (memcmp(type, TYPE_COMMAND, typeLen) == 0)
        {
            ackDashbCommand(modemName, "updateConfDSTAR", "success");
        }
        else if (memcmp(type, TYPE_STATUS, typeLen) == 0)
        {
        }
        else if (memcmp(type, TYPE_HEADER,typeLen) == 0 && isActiveMode())
        {
            processHeader(buffer + 8, respLen - 8, true);
        }
        else if (memcmp(type, TYPE_DATA, typeLen) == 0 && isActiveMode())
        {
            processData(buffer + 8, respLen - 8, false, true);
        }
        else if (memcmp(type, TYPE_EOT, typeLen) == 0 && isActiveMode())
        {
            processEOT(true);
        }
      //  delay(5000);
    }

    fprintf(stderr, "Gateway disconnected.\n");
    dstarGWConnected   = false;
    dstarReflConnected = false;
    setReflectorStatus(modemName, "DSTAR", (const char*)linkedReflector, false);
    delGateway(modemName, "main", "DSTAR");

    /* Flush stale data so a reconnecting gateway gets a clean slate */
    pthread_mutex_lock(&gwTxBufMutex);
    RingBuffer_clear(&gwTxBuffer);
    pthread_mutex_unlock(&gwTxBufMutex);

    pthread_mutex_lock(&gwCmdMutex);
    RingBuffer_clear(&gwCommand);
    pthread_mutex_unlock(&gwCmdMutex);

    pthread_mutex_lock(&rxBufMutex);
    RingBuffer_clear(&rxBuffer);
    pthread_mutex_unlock(&rxBufMutex);

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
        fprintf(stderr, "DSTAR_Service: error when creating the socket: %s\n", strerror(errno));
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
        fprintf(stderr, "DSTAR_Service: error when binding the socket to port %u: %s\n", serverPort, strerror(errno));
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

        pthread_t procid;
        int err = pthread_create(&(procid), NULL, &processGatewaySocket, (void*)(intptr_t)childfd);
        if (err != 0)
        {
            fprintf(stderr, "Can't create gateway process thread :[%s]", strerror(err));
            continue;
        }
        else
        {
            dstarGWConnected = true;
            if (debugM)
                fprintf(stderr, "Client process thread created successfully\n");
        }

        pthread_t txid;
        err = pthread_create(&(txid), NULL, &txThread, (void*)(intptr_t)childfd);
        if (err != 0)
            fprintf(stderr, "Can't create tx thread :[%s]", strerror(err));
        else
        {
            if (debugM) fprintf(stderr, "TX thread created successfully\n");
        }
        delay(1000);
    }
    int iRet = 100;
    pthread_exit(&iRet);
    return NULL;
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

    pthread_t rxid;
    int err = pthread_create(&(rxid), NULL, &rxThread, NULL);
    if (err != 0)
        fprintf(stderr, "Can't create rx thread :[%s]", strerror(err));
    else
    {
        if (debugM) fprintf(stderr, "RX thread created successfully\n");
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
    uint32_t reg3  = 0x2A4C4193;
    uint32_t reg10 = 0x0C96473A;

    // K=32
    uint32_t reg4 = (uint32_t)0b0100 << 0;  // register 4
    reg4 |= (uint32_t)0b001 << 4;           // mode, GMSK
    reg4 |= (uint32_t)0b1 << 7;
    reg4 |= (uint32_t)0b10 << 8;
    reg4 |= (uint32_t)522U << 10;  // Disc BW
    reg4 |= (uint32_t)10U << 20;   // Post dem BW
    reg4 |= (uint32_t)0b00 << 30;  // IF filter (12.5 kHz)

    uint32_t reg2 = (uint32_t)0b00 << 28;  // invert data (and RC alpha = 0.5)
    reg2 |= (uint32_t)0b001 << 4;          // modulation (GMSK)

    uint32_t reg13 = (uint32_t)0b1101 << 0;           // register 13
    reg13 |= (uint32_t)ADF7021_SLICER_TH_DSTAR << 4;  // slicer threshold

    ifConf(bytes, reg2, reg3, reg4, reg10, reg13, atol(modem_rxFrequency),
           atol(modem_txFrequency), txLevel, rfPower, 0, true);

    memcpy(buffer + 26, bytes, 40);
    uint64_t tmp[2] = {PREAMBLE_DATA, PREAMBLE_MASK};
    memcpy(buffer + 66, (uint8_t*)tmp, 16);
    buffer[82] = 0x8a;
    buffer[83] = 0x01;  // TX LSB fisrt / scan multiplier
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
                    "DSTAR_Service: error when reading from server, errno=%d\n",
                    errno);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "DSTAR_Service: unknown byte from server, 0x%02X\n",
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
            dump((char*)"DSTAR host data:", (unsigned char*)buffer, respLen);

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
                    arm_fir_fast_q15(&rxGaussianFilter, dcSamples, smp, 2);
                }
                else
                    arm_fir_fast_q15(&rxGaussianFilter, in, smp, 2);

                samples(smp, NULL, 2);
            }
            //       write(sockfd, buffer, respLen);
        }
        else if (memcmp(type, TYPE_BITS, typeLen) == 0)
        {
            packetType = PACKET_TYPE_BIT;
            processBits(buffer + 8, respLen - 8);
        }
        else if (memcmp(type, TYPE_HEADER, typeLen) == 0)
        {
            processHeader(buffer + 8, respLen - 8, false);
        }
        else if (memcmp(type, TYPE_DATA, typeLen) == 0)
        {
            processData(buffer + 8, respLen - 8, false, false);
        }
        else if (memcmp(type, TYPE_EOT, typeLen) == 0)
        {
            processEOT(false);
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

int main(int argc, char** argv)
{
    bool daemon = 0;
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
                serverPort = 18100 + modemId - 1;
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
                fprintf(stdout, "DSTAR_Service: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: DSTAR_Service [-m modem_number (1-10)] [-d] [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            fprintf(stderr, "DSTAR_Service: error in fork(), exiting\n");
            return 1;
        }

        // If this is the parent, exit
        if (pid > 0) return 0;

        // We are the child from here onwards
        setsid();

        umask(0);
    }

    memset(dcState, 0x00U, 4U * sizeof(q31_t));
    dcFilter.numStages = DC_FILTER_STAGES;
    dcFilter.pState    = dcState;
    dcFilter.pCoeffs   = DC_FILTER;
    dcFilter.postShift = 0;

	memset(rxGaussianState, 0x00U, 40U * sizeof(q15_t));
	rxGaussianFilter.numTaps = RX_GAUSSIAN_0_5_FILTER_LEN;
	rxGaussianFilter.pState  = rxGaussianState;
	rxGaussianFilter.pCoeffs = RX_GAUSSIAN_0_5_FILTER;

    memset(txModState, 0x00U, 20U * sizeof(q15_t));
    txModFilter.L           = DSTAR_RADIO_SYMBOL_LENGTH;
    txModFilter.phaseLength = TX_GAUSSIAN_0_35_FILTER_PHASE_LEN;
    txModFilter.pCoeffs     = TX_GAUSSIAN_0_35_FILTER;
    txModFilter.pState      = txModState;

    /* Initialize RingBuffers */
    RingBuffer_Init(&rxBuffer, 1600);
    RingBuffer_Init(&gwTxBuffer, 800);
    RingBuffer_Init(&gwCommand, 200);

    initTimers();
    if (getTimer("status", 2000) < 0) return 0;
    if (getTimer("dbComm", 2000) < 0) return 0;

    reset();

    char tmp[15];
    readHostConfig(modemName, "config", "modem", tmp);
    if (strcasecmp(tmp, "openmt") == 0)
        packetType = PACKET_TYPE_SAMP;
    else if (strcasecmp(tmp, "openmths") == 0)
        packetType = PACKET_TYPE_BIT;
    else
        packetType = PACKET_TYPE_FRAME;

    readHostConfig(modemName, "DSTAR", "rpt1Call", rpt1Call);
    if (strlen(rpt1Call) == 0)
    {
        setHostConfig(modemName, "DSTAR", "rpt1Call", "input", "DIRECT");
        setHostConfig(modemName, "DSTAR", "rpt2Call", "input", "DIRECT");
        setHostConfig(modemName, "DSTAR", "callsign", "input", "N0CALL");
        setHostConfig(modemName, "DSTAR", "txLevel", "input", "50");
        setHostConfig(modemName, "DSTAR", "rfPower", "input", "128");
        setHostConfig(modemName, "DSTAR", "gwPortOut", "input", "20010");
        setHostConfig(modemName, "DSTAR", "gwPortIn", "input", "20011");
        setHostConfig(modemName, "DSTAR", "gwAddress", "input", "127.0.0.1");
    }

    readHostConfig(modemName, "config", "rxFrequency", modem_rxFrequency);
    readHostConfig(modemName, "config", "txFrequency", modem_txFrequency);

    readHostConfig(modemName, "main", "callsign", station_myCall);
    readHostConfig(modemName, "DSTAR", "rpt1Call", station_rpt1Call);
    readHostConfig(modemName, "DSTAR", "rpt2Call", station_rpt2Call);

    readHostConfig(modemName, "DSTAR", "txLevel", tmp);
    txLevel = atoi(tmp);
    readHostConfig(modemName, "DSTAR", "rfPower", tmp);
    rfPower = atoi(tmp);

    //  clearDashbCommands();
    clearReflLinkStatus(modemName, "DSTAR");

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
        if (debugM) fprintf(stderr, "Timer thread created successfully\n");
    }

    while (connected)
    {
        if (isTimerTriggered("dbComm"))
        {
            char parameter[31];

            readDashbCommand(modemName, "updateConfDSTAR", parameter);
            if (strlen(parameter) > 0)
            {
                readHostConfig(modemName, "DSTAR", "rpt1Call", rpt1Call);
                readHostConfig(modemName, "DSTAR", "rpt2Call", rpt2Call);
                readHostConfig(modemName, "main", "callsign", myCall);
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

            if (dstarGWConnected)
            {
                char parameter[31];
                readDashbCommand(modemName, "reflLinkDSTAR", parameter);
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
                else if (!dstarReflConnected)
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
                            buf[2] = 0x16;
                            buf[3] = 0x04;
                            memcpy(buf + 4, TYPE_CONNECT, 4);
                            char callsign[9];
                            bzero(callsign, 9);
                            readHostConfig(modemName, "main", "callsign", callsign);
                            pthread_mutex_lock(&gwCmdMutex);
                            RingBuffer_addData(&gwCommand, buf, 8);
                            RingBuffer_addData(&gwCommand, (uint8_t*)callsign, 6);
                            RingBuffer_addData(&gwCommand, (uint8_t*)name, 7);
                            RingBuffer_addData(&gwCommand, (uint8_t*)module, 1);
                            pthread_mutex_unlock(&gwCmdMutex);
                            sleep(3);
                        }
                        else
                            ackDashbCommand(modemName, "reflLinkDSTAR", "failed");
                    }
                    else
                        ackDashbCommand(modemName, "reflLinkDSTAR", "failed");
                }
                else
                    ackDashbCommand(modemName, "reflLinkDSTAR", "failed");
            }
            else
            {
                char parameter[31];
                readDashbCommand(modemName, "reflLinkDSTAR", parameter);
                if (strlen(parameter) > 0)
                {
                    ackDashbCommand(modemName, "reflLinkDSTAR", "No gateway");
                }
            }
            resetTimer("dbComm");
        }
        delay(500000);
    }
    clearReflLinkStatus(modemName, "DSTAR");
    fprintf(stderr, "DSTAR service terminated.\n");
    logError(modemName, "main", "DSTAR host terminated.");
    
    /* Cleanup RingBuffers */
    RingBuffer_Destroy(&rxBuffer);
    RingBuffer_Destroy(&gwTxBuffer);
    RingBuffer_Destroy(&gwCommand);
    
    return 0;
}
