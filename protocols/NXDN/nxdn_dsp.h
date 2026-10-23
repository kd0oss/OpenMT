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

#ifndef NXDN_DSP_H
#define NXDN_DSP_H

#include <stdbool.h>
#include <stdint.h>

#include <dsp_tools.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NXDN 4800 bps: 2400 baud 4FSK, 24 kHz sample rate, 10 samples per symbol,
 * 192 symbols (1920 samples, 80 ms) per frame.
 *
 * Sample polarity follows the P25 host: a negative sample is the +3 symbol.
 * Dibits 01 -> +3, 00 -> +1, 10 -> -1, 11 -> -3.
 */
#define NXDN_SAMPLES_PER_SYMBOL     10U
#define NXDN_SAMPLES_PER_FRAME      (NXDN_SAMPLES_PER_SYMBOL * 192U)
#define NXDN_FILTER_SPAN            6U      /* symbols each side of the pulse */
#define NXDN_FILTER_TAPS            (2U * NXDN_FILTER_SPAN * NXDN_SAMPLES_PER_SYMBOL + 1U)

#define NXDN_SHAPE_RRC              FSK4_SHAPE_RRC
#define NXDN_SHAPE_RC               FSK4_SHAPE_RC

/* Outer symbol (+-3) amplitude for 1.05 kHz deviation, in counts at txLevel 50 */
#define NXDN_LEVEL_OUTER            735.0F

/* Transmit */
typedef Fsk4Tx NxdnTx;

void nxdnTxInit(NxdnTx* tx, float level, uint8_t shape);
void nxdnTxSetLevel(NxdnTx* tx, float level);
unsigned int nxdnTxPreamble(NxdnTx* tx, unsigned int symbols, int16_t* out);
unsigned int nxdnTxFrame(NxdnTx* tx, const uint8_t* frame, int16_t* out);
unsigned int nxdnTxFlush(NxdnTx* tx, int16_t* out);

/* Receive */
#define NXDN_RX_RING            FSK4_RX_RING
#define NXDN_RX_MAX_CAND        FSK4_RX_MAX_CAND
#define NXDN_RX_MAX_MISS        4U

typedef Fsk4FrameFn NxdnRxFrameFn;
typedef Fsk4Cand NxdnCand;
typedef Fsk4Rx NxdnRx;

void nxdnRxInit(NxdnRx* rx, NxdnRxFrameFn cb, void* ctx, uint8_t shape);
void nxdnRxSamples(NxdnRx* rx, const int16_t* samples, unsigned int length);
void nxdnRxReset(NxdnRx* rx);

/*
 * Receive from a bit stream (OpenMT_HS / ADF7021).
 *
 * The ADF7021 slices the 4FSK itself and streams the hard bits, most
 * significant bit first (two bits per symbol, 4800 bit/s). The state machine is
 * that of the MMDVM_HS NXDNRX: find the 20 bit frame sync word, collect the
 * rest of the 384 bit frame, then look for the next sync word where it should
 * be (+-2 bits) and report the frame lost when it has not been seen for
 * NXDN_BIT_MAX_FRAMES frames. The frames passed to the callback are the same
 * 48 bytes the MMDVM firmware sends.
 */
#define NXDN_BIT_MAX_FRAMES         6U

typedef Fsk4BitRx NxdnBitRx;

void nxdnBitRxInit(NxdnBitRx* rx, NxdnRxFrameFn cb, void* ctx);
void nxdnBitRxReset(NxdnBitRx* rx);
void nxdnBitRxBytes(NxdnBitRx* rx, const uint8_t* bytes, unsigned int length);

/* Transmit to a bit stream: the preamble and the frames are the bytes as sent */
unsigned int nxdnBitTxPreamble(uint8_t* out, unsigned int bytes);

#ifdef __cplusplus
}
#endif

#endif
