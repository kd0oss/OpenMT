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
 * ysf_dsp.c
 *
 * The YSF part of the 4-level FSK modem.  The modem itself is the shared
 * engine in tools/dsp_tools.c; this file holds what is particular to YSF
 * (the frame sync word and the FICH check of a frame) and the YSF named
 * functions that call the engine.
 */

#include <string.h>

#include "ysf_dsp.h"
#include "ysf_func.h"

/*
 * The sync word in the sample domain: q = -(symbol), made from the sync bytes
 * the first time it is needed.  Dibits 01 -> +3, 00 -> +1, 10 -> -1, 11 -> -3.
 */
static float SYNC_Q[YSF_SYNC_LENGTH_SYMBOLS];
static uint8_t SYNC_BYTES[YSF_SYNC_LENGTH_BYTES];
static bool syncReady = false;

static bool frameLooksValid(const uint8_t* raw);

/* Description of YSF for the engine */
static Fsk4Cfg ysfCfg =
{
    .sps           = YSF_SAMPLES_PER_SYMBOL,
    .filterSpan    = YSF_FILTER_SPAN,
    .frameSymbols  = YSF_FRAME_LENGTH_SYMBOLS,
    .syncSymbols   = YSF_SYNC_LENGTH_SYMBOLS,
    .syncQ         = SYNC_Q,
    .maxMiss       = YSF_RX_MAX_MISS,
    .holdOnMiss    = true,
    .valid         = frameLooksValid,

    .syncBitsValue = 0x000000D471C9634DULL,
    .syncBitsMask  = 0x000000FFFFFFFFFFULL,
    .syncBits      = YSF_SYNC_LENGTH_BITS,
    .frameBits     = YSF_FRAME_LENGTH_BITS,
    .startErrs     = 2U,       /* allowed to start a lock */
    .runErrs       = 4U,       /* allowed once locked     */
    .maxFrames     = YSF_BIT_MAX_FRAMES,
    .syncBytes     = SYNC_BYTES
};

/*
 * makeSync()
 *
 * Makes the sync word in the sample domain and the sync bytes from the YSF
 * sync word.  It is done once; later calls do nothing.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void makeSync(void)
{
    uint8_t sync[YSF_FRAME_LENGTH_BYTES];

    if (syncReady)
        return;

    ysfAddSync(sync);
    memcpy(SYNC_BYTES, sync, YSF_SYNC_LENGTH_BYTES);

    for (unsigned int i = 0U; i < YSF_SYNC_LENGTH_SYMBOLS; i++)
    {
        unsigned int dibit = (sync[i >> 2] >> (6U - 2U * (i & 3U))) & 0x03U;
        float q;

        switch (dibit)
        {
            case 1U: q = -3.0F; break;
            case 0U: q = -1.0F; break;
            case 2U: q = 1.0F; break;
            default: q = 3.0F; break;
        }

        SYNC_Q[i] = q;
    }

    syncReady = true;
}

/*
 * ysfCfgGet()
 *
 * Gives the engine description of YSF, after making sure the sync word is
 * set up.
 *
 * Parameters: none.
 *
 * Returns: The description.
 */
static Fsk4Cfg* ysfCfgGet(void)
{
    makeSync();

    return &ysfCfg;
}

/*
 * frameLooksValid()
 *
 * A hunting candidate is accepted only if the FICH (Golay and CRC) agrees.
 *
 * Parameters:
 *   raw - The sliced frame.
 *
 * Returns: true if the FICH of the frame could be decoded.
 */
static bool frameLooksValid(const uint8_t* raw)
{
    YsfFich fich;

    ysfFichInit(&fich);

    return ysfFichDecode(&fich, raw);
}

/* ------------------------------------------------------------------------ */
/* Transmit                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * ysfTxInit()
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
void ysfTxInit(YsfTx* tx, float level, uint8_t shape)
{
    fsk4TxInit(tx, ysfCfgGet(), level, shape);
}

/*
 * ysfTxSetLevel()
 *
 * Changes the transmit level of a modulator.
 *
 * Parameters:
 *   tx    - The modulator.
 *   level - Peak level of the outer symbols.
 *
 * Returns: nothing.
 */
void ysfTxSetLevel(YsfTx* tx, float level)
{
    fsk4TxSetLevel(tx, level);
}

/*
 * ysfTxPreamble()
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
unsigned int ysfTxPreamble(YsfTx* tx, unsigned int symbols, int16_t* out)
{
    return fsk4TxPreamble(tx, symbols, out);
}

/*
 * ysfTxFrame()
 *
 * Modulates a frame: 480 symbols from the 120 bytes of a frame (sync and
 * all), 2400 samples.  The dibit 01 is symbol +3, which is a negative
 * sample.
 *
 * Parameters:
 *   tx    - The modulator.
 *   frame - The 120 byte frame.
 *   out   - Receives the samples.
 *
 * Returns: Number of samples made.
 */
unsigned int ysfTxFrame(YsfTx* tx, const uint8_t* frame, int16_t* out)
{
    return fsk4TxFrame(tx, frame, out);
}

/*
 * ysfTxFlush()
 *
 * Pushes zero symbols so the last real symbol leaves the filter.
 *
 * Parameters:
 *   tx  - The modulator.
 *   out - Receives the samples.
 *
 * Returns: Number of samples made.
 */
unsigned int ysfTxFlush(YsfTx* tx, int16_t* out)
{
    return fsk4TxFlush(tx, out);
}

/* ------------------------------------------------------------------------ */
/* Receive                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * ysfRxInit()
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
void ysfRxInit(YsfRx* rx, YsfRxFrameFn cb, void* ctx, uint8_t shape)
{
    fsk4RxInit(rx, ysfCfgGet(), cb, ctx, shape);
}

/*
 * ysfRxReset()
 *
 * Puts the receiver back to searching for a sync word.  The candidates,
 * tracking and pending frame are cleared.
 *
 * Parameters:
 *   rx - The receiver.
 *
 * Returns: nothing.
 */
void ysfRxReset(YsfRx* rx)
{
    fsk4RxReset(rx);
}

/*
 * ysfRxSamples()
 *
 * Feeds demodulated samples to the receiver.  Every complete frame found is
 * passed to the callback as the 120 bytes that came off the air (still
 * scrambled); lost = true reports that the signal has gone.
 *
 * Parameters:
 *   rx      - The receiver.
 *   samples - Demodulated samples.
 *   length  - Number of samples.
 *
 * Returns: nothing.
 */
void ysfRxSamples(YsfRx* rx, const int16_t* samples, unsigned int length)
{
    fsk4RxSamples(rx, samples, length);
}

/* ------------------------------------------------------------------------ */
/* Bit stream receive and transmit                                           */
/* ------------------------------------------------------------------------ */

/*
 * ysfBitRxInit()
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
void ysfBitRxInit(YsfBitRx* rx, YsfRxFrameFn cb, void* ctx)
{
    fsk4BitRxInit(rx, ysfCfgGet(), cb, ctx);
}

/*
 * ysfBitRxReset()
 *
 * Back to searching for a sync word.
 *
 * Parameters:
 *   rx - The bit receiver.
 *
 * Returns: nothing.
 */
void ysfBitRxReset(YsfBitRx* rx)
{
    fsk4BitRxReset(rx);
}

/*
 * ysfBitRxBytes()
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
void ysfBitRxBytes(YsfBitRx* rx, const uint8_t* bytes, unsigned int length)
{
    fsk4BitRxBytes(rx, bytes, length);
}

/*
 * ysfBitTxPreamble()
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
unsigned int ysfBitTxPreamble(uint8_t* out, unsigned int bytes)
{
    return fsk4BitTxPreamble(out, bytes);
}
