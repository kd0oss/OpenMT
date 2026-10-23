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

/*
 * nxdn_dsp.c
 *
 * The NXDN part of the 4-level FSK modem.  The modem itself is the shared
 * engine in tools/dsp_tools.c; this file holds what is particular to NXDN
 * (the frame sync word and the channel code check of a frame) and the NXDN
 * named functions that call the engine.
 */

#include <string.h>

#include "nxdn_dsp.h"
#include "nxdn_func.h"

/* FSW in the sample domain: q = -(symbol), -3 +1 -3 +3 -3 -3 +3 +3 -1 +3 */
static const float SYNC_Q[10] = { 3.0F, -1.0F, 3.0F, -3.0F, 3.0F, 3.0F, -3.0F, -3.0F, 1.0F, -3.0F };

/* The 20 bit frame sync word as it starts a frame */
static const uint8_t FSW_BYTES[3] = { 0xCDU, 0xF5U, 0x90U };

static bool frameLooksValid(const uint8_t* raw);

/* Description of NXDN for the engine */
static Fsk4Cfg nxdnCfg =
{
    .sps           = NXDN_SAMPLES_PER_SYMBOL,
    .filterSpan    = NXDN_FILTER_SPAN,
    .frameSymbols  = NXDN_FRAME_LENGTH_SYMBOLS,
    .syncSymbols   = NXDN_FSW_LENGTH_SYMBOLS,
    .syncQ         = SYNC_Q,
    .maxMiss       = NXDN_RX_MAX_MISS,
    .holdOnMiss    = false,
    .valid         = frameLooksValid,

    .syncBitsValue = 0x000CDF59ULL,
    .syncBitsMask  = 0x000FFFFFULL,
    .syncBits      = NXDN_FSW_LENGTH_BITS,
    .frameBits     = NXDN_FRAME_LENGTH_BITS,
    .startErrs     = 0U,       /* none to start a lock */
    .runErrs       = 3U,       /* allowed once locked  */
    .maxFrames     = NXDN_BIT_MAX_FRAMES,
    .syncBytes     = FSW_BYTES
};

/*
 * frameLooksValid()
 *
 * A hunting candidate is accepted only if the channel coding agrees.
 *
 * Parameters:
 *   raw - The 48 byte frame as it came off the air (still scrambled).
 *
 * Returns: true if the LICH and one of the channel codes (UDCH, SACCH or
 *   FACCH1) decode, false if not.
 */
static bool frameLooksValid(const uint8_t* raw)
{
    uint8_t data[NXDN_FRAME_LENGTH_BYTES];
    uint8_t lich;
    uint8_t tmp[26];
    uint8_t facch[12];

    memcpy(data, raw, NXDN_FRAME_LENGTH_BYTES);
    nxdnScramble(data);

    if (!nxdnLichDecode(data, &lich))
        return false;

    if (NXDN_LICH_FCT(lich) == NXDN_LICH_USC_UDCH)
        return nxdnUdchDecode(data, tmp);

    if (nxdnSacchDecode(data, tmp))
        return true;

    return nxdnFacch1Decode(data, NXDN_FACCH1_OFFSET_1, facch) ||
           nxdnFacch1Decode(data, NXDN_FACCH1_OFFSET_2, facch);
}

/* ------------------------------------------------------------------------ */
/* Transmit                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * nxdnTxInit()
 *
 * Prepares a transmit modulator: the symbol history is cleared, the level
 * and shape are stored and the pulse shaping filter taps are made.
 *
 * Parameters:
 *   tx    - The modulator.
 *   level - Peak level of the outer symbols.
 *   shape - Pulse shape: root raised cosine or raised cosine.
 *
 * Returns: nothing.
 */
void nxdnTxInit(NxdnTx* tx, float level, uint8_t shape)
{
    fsk4TxInit(tx, &nxdnCfg, level, shape);
}

/*
 * nxdnTxSetLevel()
 *
 * Changes the transmit level of a modulator.
 *
 * Parameters:
 *   tx    - The modulator.
 *   level - Peak level of the outer symbols.
 *
 * Returns: nothing.
 */
void nxdnTxSetLevel(NxdnTx* tx, float level)
{
    fsk4TxSetLevel(tx, level);
}

/*
 * nxdnTxPreamble()
 *
 * Alternating +3 -3 symbols (0x77 bytes) so the receiver can settle.
 *
 * Parameters:
 *   tx      - The modulator.
 *   symbols - Number of symbols to make.
 *   out     - Receives the samples.
 *
 * Returns: Number of samples made.
 */
unsigned int nxdnTxPreamble(NxdnTx* tx, unsigned int symbols, int16_t* out)
{
    return fsk4TxPreamble(tx, symbols, out);
}

/*
 * nxdnTxFrame()
 *
 * 192 symbols from the 48 bytes of a frame (already scrambled), 1920
 * samples.  The dibit 01 is symbol +3, which is a negative sample.
 *
 * Parameters:
 *   tx    - The modulator.
 *   frame - The 48 byte frame.
 *   out   - Receives the samples.
 *
 * Returns: Number of samples made.
 */
unsigned int nxdnTxFrame(NxdnTx* tx, const uint8_t* frame, int16_t* out)
{
    return fsk4TxFrame(tx, frame, out);
}

/*
 * nxdnTxFlush()
 *
 * Pushes zero symbols so the last real symbol leaves the filter.
 *
 * Parameters:
 *   tx  - The modulator.
 *   out - Receives the samples.
 *
 * Returns: Number of samples made.
 */
unsigned int nxdnTxFlush(NxdnTx* tx, int16_t* out)
{
    return fsk4TxFlush(tx, out);
}

/* ------------------------------------------------------------------------ */
/* Receive                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * nxdnRxInit()
 *
 * Prepares a receiver: the state is cleared, the filter taps are made, the
 * minimum signal amplitude is set and the callback is stored.
 *
 * Parameters:
 *   rx    - The receiver.
 *   cb    - Function called for each frame received, and when the signal is
 *           lost.
 *   ctx   - Context pointer passed to the callback.
 *   shape - Pulse shape of the receive filter.
 *
 * Returns: nothing.
 */
void nxdnRxInit(NxdnRx* rx, NxdnRxFrameFn cb, void* ctx, uint8_t shape)
{
    fsk4RxInit(rx, &nxdnCfg, cb, ctx, shape);
}

/*
 * nxdnRxReset()
 *
 * Puts the receiver back to searching for a sync word.  The candidates,
 * tracking and pending frame are cleared.
 *
 * Parameters:
 *   rx - The receiver.
 *
 * Returns: nothing.
 */
void nxdnRxReset(NxdnRx* rx)
{
    fsk4RxReset(rx);
}

/*
 * nxdnRxSamples()
 *
 * Feeds demodulated samples to the receiver.  Every complete frame found is
 * passed to the callback as the 48 bytes that came off the air (still
 * scrambled); lost = true reports that the signal has gone.
 *
 * Parameters:
 *   rx      - The receiver.
 *   samples - Demodulated samples.
 *   length  - Number of samples.
 *
 * Returns: nothing.
 */
void nxdnRxSamples(NxdnRx* rx, const int16_t* samples, unsigned int length)
{
    fsk4RxSamples(rx, samples, length);
}

/* ------------------------------------------------------------------------ */
/* Bit stream receive and transmit                                           */
/* ------------------------------------------------------------------------ */

/*
 * nxdnBitRxInit()
 *
 * Prepares the bit receiver. cb gets every frame (and the loss of the
 * signal).
 *
 * Parameters:
 *   rx  - The bit receiver.
 *   cb  - Function called for each frame received, and when the signal is
 *         lost.
 *   ctx - Context pointer passed to the callback.
 *
 * Returns: nothing.
 */
void nxdnBitRxInit(NxdnBitRx* rx, NxdnRxFrameFn cb, void* ctx)
{
    fsk4BitRxInit(rx, &nxdnCfg, cb, ctx);
}

/*
 * nxdnBitRxReset()
 *
 * Back to searching for a sync word.
 *
 * Parameters:
 *   rx - The bit receiver.
 *
 * Returns: nothing.
 */
void nxdnBitRxReset(NxdnBitRx* rx)
{
    fsk4BitRxReset(rx);
}

/*
 * nxdnBitRxBytes()
 *
 * Feeds bytes of the bit stream, most significant bit first.
 *
 * Parameters:
 *   rx     - The bit receiver.
 *   bytes  - The received bytes.
 *   length - Number of bytes in bytes.
 *
 * Returns: nothing.
 */
void nxdnBitRxBytes(NxdnBitRx* rx, const uint8_t* bytes, unsigned int length)
{
    fsk4BitRxBytes(rx, bytes, length);
}

/*
 * nxdnBitTxPreamble()
 *
 * Fills 'bytes' bytes with the 0x77 pattern (dibits 01 11 = +3 -3 symbols),
 * the same alternating outer symbols as the sample preamble.
 *
 * Parameters:
 *   out   - Receives the preamble bytes.
 *   bytes - Number of bytes to make.
 *
 * Returns: Number of bytes made.
 */
unsigned int nxdnBitTxPreamble(uint8_t* out, unsigned int bytes)
{
    return fsk4BitTxPreamble(out, bytes);
}
