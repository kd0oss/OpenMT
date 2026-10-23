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

#ifndef YSF_DSP_H
#define YSF_DSP_H

#include <stdbool.h>
#include <stdint.h>

#include <dsp_tools.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * YSF C4FM, 9600 bit/s: 4800 baud 4-level FSK, 24 kHz sample rate, 5 samples
 * per symbol, 480 symbols (2400 samples, 100 ms) per frame.
 *
 * Sample polarity follows the P25 host: a negative sample is the +3 symbol.
 * Dibits 01 -> +3, 00 -> +1, 10 -> -1, 11 -> -3.
 */
#define YSF_SAMPLES_PER_SYMBOL      5U
#define YSF_SAMPLES_PER_FRAME       (YSF_SAMPLES_PER_SYMBOL * 480U)
#define YSF_FILTER_SPAN            6U      /* symbols each side of the pulse */
#define YSF_FILTER_TAPS            (2U * YSF_FILTER_SPAN * YSF_SAMPLES_PER_SYMBOL + 1U)

#define YSF_SHAPE_RRC              FSK4_SHAPE_RRC
#define YSF_SHAPE_RC               FSK4_SHAPE_RC

/*
 * Outer symbol (+-3) amplitude in counts at txLevel 50: 2.7 kHz deviation for
 * the normal setting and 1.35 kHz for the low deviation setting (the same
 * +-0.9 kHz and +-0.45 kHz per level as the MMDVM_HS YSF_H and YSF_L).
 */
#define YSF_LEVEL_OUTER_H           1890.0F
#define YSF_LEVEL_OUTER_L           945.0F

/* Transmit */
typedef Fsk4Tx YsfTx;

void ysfTxInit(YsfTx* tx, float level, uint8_t shape);
void ysfTxSetLevel(YsfTx* tx, float level);
unsigned int ysfTxPreamble(YsfTx* tx, unsigned int symbols, int16_t* out);
unsigned int ysfTxFrame(YsfTx* tx, const uint8_t* frame, int16_t* out);
unsigned int ysfTxFlush(YsfTx* tx, int16_t* out);

/* Receive */
#define YSF_RX_RING            FSK4_RX_RING
#define YSF_RX_MAX_CAND        FSK4_RX_MAX_CAND
#define YSF_RX_MAX_MISS        1U      /* one frame without a sync is still passed on, the second is a loss */

typedef Fsk4FrameFn YsfRxFrameFn;
typedef Fsk4Cand YsfCand;
typedef Fsk4Rx YsfRx;

void ysfRxInit(YsfRx* rx, YsfRxFrameFn cb, void* ctx, uint8_t shape);
void ysfRxSamples(YsfRx* rx, const int16_t* samples, unsigned int length);
void ysfRxReset(YsfRx* rx);

/*
 * Receive from a bit stream (OpenMT_HS / ADF7021).
 *
 * The ADF7021 slices the C4FM itself and streams the hard bits, most
 * significant bit first (two bits per symbol, 9600 bit/s). The state machine is
 * that of the MMDVM_HS YSFRX: find the 40 bit frame sync word (two bit errors
 * allowed), collect the rest of the 960 bit frame, then look for the next sync
 * word where it should be (+-2 bits, four bit errors allowed) and report the
 * signal lost when a frame has gone by without one. The frames passed to the
 * callback are the 120 bytes the MMDVM firmware sends after its flag byte.
 */
#define YSF_BIT_MAX_FRAMES          2U

typedef Fsk4BitRx YsfBitRx;

void ysfBitRxInit(YsfBitRx* rx, YsfRxFrameFn cb, void* ctx);
void ysfBitRxReset(YsfBitRx* rx);
void ysfBitRxBytes(YsfBitRx* rx, const uint8_t* bytes, unsigned int length);

/* Transmit to a bit stream: the preamble and the frames are the bytes as sent */
unsigned int ysfBitTxPreamble(uint8_t* out, unsigned int bytes);

#ifdef __cplusplus
}
#endif

#endif
