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
 * dsp_tools.c
 *
 * The 4-level FSK modem engine shared by the NXDN and YSF hosts.  See
 * dsp_tools.h for what it does and how a protocol describes itself.
 */

#include <math.h>
#include <string.h>

#include "dsp_tools.h"

#define ALPHA               0.2F    /* roll-off of the pulse */
#define HUNT_THRESHOLD      0.80F   /* correlation needed to start a candidate */
#define TRACK_THRESHOLD     0.55F   /* correlation needed to follow the sync word once locked */
#define MAX_SYNC_SYMBOLS    32U     /* most sync symbols a protocol may have */

/*
 * fsk4CfgPrepare()
 *
 * Works out the values of a protocol description that follow from the values
 * the protocol set: filter length, frame size in bytes and samples, the
 * distances used to find the sync word and frame, and the energy and the
 * outer symbol counts of the sync word.  It is done once; later calls do
 * nothing.
 *
 * Parameters:
 *   cfg - The protocol description.
 *
 * Returns: nothing.
 */
void fsk4CfgPrepare(Fsk4Cfg* cfg)
{
    if (cfg->ready)
        return;

    cfg->filterTaps = 2U * cfg->filterSpan * cfg->sps + 1U;
    cfg->frameBytes = cfg->frameSymbols / 4U;
    cfg->samplesPerFrame = cfg->sps * cfg->frameSymbols;
    cfg->syncSpan = (cfg->syncSymbols - 1U) * cfg->sps;
    cfg->frameLast = (cfg->frameSymbols - cfg->syncSymbols) * cfg->sps;

    cfg->syncEnergy = 0.0F;
    cfg->syncHi = 0U;
    cfg->syncLo = 0U;

    for (unsigned int i = 0U; i < cfg->syncSymbols; i++)
    {
        float q = cfg->syncQ[i];

        cfg->syncEnergy += q * q;

        if (q > 2.0F)
            cfg->syncHi++;
        else if (q < -2.0F)
            cfg->syncLo++;
    }

    cfg->ready = true;
}

/*
 * makeTaps()
 *
 * Makes the pulse shaping filter: a root raised cosine (or raised cosine
 * when shape is FSK4_SHAPE_RC) pulse, alpha 0.2, scaled so that the taps sum
 * to the number of samples per symbol.
 *
 * Parameters:
 *   cfg   - The protocol description.
 *   taps  - Receives cfg->filterTaps taps.
 *   shape - The pulse shape, FSK4_SHAPE_RC or the root raised cosine.
 *
 * Returns: nothing.
 */
static void makeTaps(const Fsk4Cfg* cfg, float* taps, uint8_t shape)
{
    const float pi = 3.14159265358979F;
    int mid = (int)(cfg->filterTaps / 2U);
    float sum = 0.0F;

    for (int i = 0; i < (int)cfg->filterTaps; i++)
    {
        float t = (float)(i - mid) / (float)cfg->sps;
        float h;

        if (shape == FSK4_SHAPE_RC)
        {
            float s = (fabsf(t) < 1.0e-6F) ? 1.0F : sinf(pi * t) / (pi * t);
            float d = 1.0F - (2.0F * ALPHA * t) * (2.0F * ALPHA * t);

            if (fabsf(d) < 1.0e-5F)
                h = (pi / 4.0F) * ((fabsf(t) < 1.0e-6F) ? 1.0F : sinf(pi * t) / (pi * t));
            else
                h = s * cosf(pi * ALPHA * t) / d;
        }
        else
        {
            if (fabsf(t) < 1.0e-6F)
            {
                h = 1.0F - ALPHA + 4.0F * ALPHA / pi;
            }
            else if (fabsf(fabsf(t) - 1.0F / (4.0F * ALPHA)) < 1.0e-5F)
            {
                h = (ALPHA / sqrtf(2.0F)) * ((1.0F + 2.0F / pi) * sinf(pi / (4.0F * ALPHA)) +
                                              (1.0F - 2.0F / pi) * cosf(pi / (4.0F * ALPHA)));
            }
            else
            {
                float num = sinf(pi * t * (1.0F - ALPHA)) + 4.0F * ALPHA * t * cosf(pi * t * (1.0F + ALPHA));
                float den = pi * t * (1.0F - (4.0F * ALPHA * t) * (4.0F * ALPHA * t));
                h = num / den;
            }
        }

        taps[i] = h;
        sum += h;
    }

    for (unsigned int i = 0U; i < cfg->filterTaps; i++)
        taps[i] *= (float)cfg->sps / sum;
}

/* ------------------------------------------------------------------------ */
/* Transmit                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * fsk4TxInit()
 *
 * Prepares a transmitter: the symbol history is cleared, the level and shape
 * are stored and the pulse shaping filter taps are made.
 *
 * Parameters:
 *   tx    - The transmitter.
 *   cfg   - The protocol description.
 *   level - Amplitude of the outer (+3, -3) symbols, as a sample value.
 *   shape - The pulse shape, FSK4_SHAPE_RRC or FSK4_SHAPE_RC.
 *
 * Returns: nothing.
 */
void fsk4TxInit(Fsk4Tx* tx, Fsk4Cfg* cfg, float level, uint8_t shape)
{
    fsk4CfgPrepare(cfg);
    tx->cfg = cfg;
    memset(tx->sym, 0x00, sizeof(tx->sym));
    tx->level = level;
    tx->shape = shape;
    makeTaps(cfg, tx->taps, shape);
}

/*
 * fsk4TxSetLevel()
 *
 * Changes the amplitude of the transmitted symbols.
 *
 * Parameters:
 *   tx    - The transmitter.
 *   level - Amplitude of the outer (+3, -3) symbols, as a sample value.
 *
 * Returns: nothing.
 */
void fsk4TxSetLevel(Fsk4Tx* tx, float level)
{
    tx->level = level;
}

/*
 * clip16()
 *
 * Rounds a value to a 16 bit sample, limiting it to the range of the sample.
 *
 * Parameters:
 *   v - The value.
 *
 * Returns: The sample, limited to -32768 to 32767.
 */
static int16_t clip16(float v)
{
    if (v > 32767.0F)
        return 32767;
    if (v < -32768.0F)
        return -32768;

    return (int16_t)lrintf(v);
}

/*
 * txSymbol()
 *
 * Pushes one symbol in the sample domain (q = +-3, +-1) through the pulse
 * shaping filter and makes the samples for it.
 *
 * Parameters:
 *   tx  - The transmitter.
 *   q   - The symbol in the sample domain (a +3 symbol is -3, as in the
 *         sample).
 *   out - Receives cfg->sps samples.
 *
 * Returns: nothing.
 */
static void txSymbol(Fsk4Tx* tx, float q, int16_t* out)
{
    const Fsk4Cfg* cfg = tx->cfg;
    const unsigned int count = cfg->filterSpan * 2U + 1U;

    memmove(&tx->sym[1], &tx->sym[0], (count - 1U) * sizeof(float));
    tx->sym[0] = q * (tx->level / 3.0F);

    for (unsigned int j = 0U; j < cfg->sps; j++)
    {
        float y = 0.0F;

        for (unsigned int m = 0U; m < count; m++)
        {
            unsigned int idx = j + cfg->sps * m;

            if (idx < cfg->filterTaps)
                y += tx->sym[m] * tx->taps[idx];
        }

        out[j] = clip16(y);
    }
}

/*
 * fsk4TxPreamble()
 *
 * Makes the preamble: alternating +3 -3 symbols (0x77 bytes) so the receiver
 * can settle.
 *
 * Parameters:
 *   tx      - The transmitter.
 *   symbols - Number of symbols.
 *   out     - Receives the samples (symbols * samples per symbol).
 *
 * Returns: The number of samples made.
 */
unsigned int fsk4TxPreamble(Fsk4Tx* tx, unsigned int symbols, int16_t* out)
{
    for (unsigned int i = 0U; i < symbols; i++)
        txSymbol(tx, (i & 1U) ? 3.0F : -3.0F, out + i * tx->cfg->sps);

    return symbols * tx->cfg->sps;
}

/*
 * fsk4TxFrame()
 *
 * Modulates a frame (already scrambled or coded as it goes on the air).  The
 * dibit 01 is symbol +3, which is a negative sample.
 *
 * Parameters:
 *   tx    - The transmitter.
 *   frame - The frame, cfg->frameBytes bytes.
 *   out   - Receives cfg->samplesPerFrame samples.
 *
 * Returns: The number of samples made, cfg->samplesPerFrame.
 */
unsigned int fsk4TxFrame(Fsk4Tx* tx, const uint8_t* frame, int16_t* out)
{
    for (unsigned int i = 0U; i < tx->cfg->frameSymbols; i++)
    {
        unsigned int dibit = (frame[i >> 2] >> (6U - 2U * (i & 3U))) & 0x03U;
        float q;

        switch (dibit)
        {
            case 1U: q = -3.0F; break;      /* +3 symbol  */
            case 0U: q = -1.0F; break;      /* +1 symbol  */
            case 2U: q = 1.0F; break;       /* -1 symbol  */
            default: q = 3.0F; break;       /* -3 symbol  */
        }

        txSymbol(tx, q, out + i * tx->cfg->sps);
    }

    return tx->cfg->samplesPerFrame;
}

/*
 * fsk4TxFlush()
 *
 * Pushes zero symbols so the last real symbol leaves the filter.
 *
 * Parameters:
 *   tx  - The transmitter.
 *   out - Receives the samples, (filterSpan + 1) * samples per symbol of them.
 *
 * Returns: The number of samples made.
 */
unsigned int fsk4TxFlush(Fsk4Tx* tx, int16_t* out)
{
    const unsigned int symbols = tx->cfg->filterSpan + 1U;

    for (unsigned int i = 0U; i < symbols; i++)
        txSymbol(tx, 0.0F, out + i * tx->cfg->sps);

    return symbols * tx->cfg->sps;
}

/* ------------------------------------------------------------------------ */
/* Receive                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * fsk4RxInit()
 *
 * Prepares a receiver: the state is cleared, the filter taps are made, the
 * minimum signal amplitude is set and the callback is stored.
 *
 * Parameters:
 *   rx    - The receiver.
 *   cfg   - The protocol description.
 *   cb    - Called with every frame found, and when the signal is lost.
 *   ctx   - Passed to cb.
 *   shape - The pulse shape of the receive filter.
 *
 * Returns: nothing.
 */
void fsk4RxInit(Fsk4Rx* rx, Fsk4Cfg* cfg, Fsk4FrameFn cb, void* ctx, uint8_t shape)
{
    fsk4CfgPrepare(cfg);
    memset(rx, 0x00, sizeof(*rx));
    rx->cfg = cfg;
    makeTaps(cfg, rx->taps, shape);
    rx->minAmp = 30.0F;
    rx->cb = cb;
    rx->ctx = ctx;
}

/*
 * fsk4RxReset()
 *
 * Forgets any lock and candidates, so the receiver searches for a sync word
 * again.
 *
 * Parameters:
 *   rx - The receiver.
 *
 * Returns: nothing.
 */
void fsk4RxReset(Fsk4Rx* rx)
{
    rx->candCount = 0U;
    rx->tracking = false;
    rx->prov = false;
    rx->dueValid = false;
    rx->windowDone = false;
    rx->held = false;
    rx->miss = 0U;
}

/*
 * ringY()
 *
 * Reads a filter output from the receiver's sample ring.
 *
 * Parameters:
 *   rx  - The receiver.
 *   idx - Absolute sample position.
 *
 * Returns: The filter output at that position.
 */
static inline float ringY(const Fsk4Rx* rx, uint64_t idx)
{
    return rx->y[idx & (FSK4_RX_RING - 1U)];
}

/*
 * ringR()
 *
 * Reads a sync correlation from the receiver's ring.
 *
 * Parameters:
 *   rx  - The receiver.
 *   idx - Absolute sample position.
 *
 * Returns: The correlation at that position.
 */
static inline float ringR(const Fsk4Rx* rx, uint64_t idx)
{
    return rx->r[idx & (FSK4_RX_RING - 1U)];
}

/*
 * correlate()
 *
 * Correlates the filter outputs at the symbol positions of the sync word
 * that ends at a sample against the sync word.
 *
 * Parameters:
 *   rx - The receiver.
 *   n  - Position of the newest sample, the last symbol of the sync word.
 *
 * Returns: The normalised correlation, from -1 to 1 (1 is a perfect match),
 *          or 0 when the samples hardly vary.
 */
static float correlate(const Fsk4Rx* rx, uint64_t n)
{
    const Fsk4Cfg* cfg = rx->cfg;
    float sx = 0.0F, sxx = 0.0F, sqx = 0.0F;

    for (unsigned int k = 0U; k < cfg->syncSymbols; k++)
    {
        float x = ringY(rx, n - cfg->syncSpan + k * cfg->sps);
        sx += x;
        sxx += x * x;
        sqx += cfg->syncQ[k] * x;
    }

    float var = sxx - sx * sx / (float)cfg->syncSymbols;

    if (var < 1.0F)
        return 0.0F;

    return sqx / sqrtf(cfg->syncEnergy * var);
}

/*
 * syncLevels()
 *
 * Measures the high and low levels of the sync word ending at a sample and
 * checks that every sync symbol is on the right side of its slicing level.
 *
 * Parameters:
 *   rx     - The receiver.
 *   nb     - Position of the last symbol of the sync word.
 *   centre - Receives the middle between the levels.
 *   amp    - Receives the amplitude, half the difference between the levels.
 *
 * Returns: true if the amplitude is at least the minimum and every sync
 *          symbol fits, false otherwise.
 */
static bool syncLevels(const Fsk4Rx* rx, uint64_t nb, float* centre, float* amp)
{
    const Fsk4Cfg* cfg = rx->cfg;
    float hi = 0.0F, lo = 0.0F;
    float x[MAX_SYNC_SYMBOLS];

    for (unsigned int k = 0U; k < cfg->syncSymbols; k++)
    {
        x[k] = ringY(rx, nb - cfg->syncSpan + k * cfg->sps);

        if (cfg->syncQ[k] > 2.0F)
            hi += x[k];
        else if (cfg->syncQ[k] < -2.0F)
            lo += x[k];
    }

    hi /= (float)cfg->syncHi;
    lo /= (float)cfg->syncLo;

    float c = (hi + lo) / 2.0F;
    float a = (hi - lo) / 2.0F;

    *centre = c;
    *amp = a;

    if (a < rx->minAmp)
        return false;

    /* Every sync symbol has to be on the right side of its slicing level */
    for (unsigned int k = 0U; k < cfg->syncSymbols; k++)
    {
        float v = x[k] - c;

        if (cfg->syncQ[k] > 2.0F && v < a / 3.0F)
            return false;
        if (cfg->syncQ[k] < -2.0F && v > -a / 3.0F)
            return false;
        if (cfg->syncQ[k] > 0.0F && cfg->syncQ[k] < 2.0F && v < -a / 3.0F)
            return false;
        if (cfg->syncQ[k] < 0.0F && cfg->syncQ[k] > -2.0F && v > a / 3.0F)
            return false;
    }

    return true;
}

/*
 * sliceFrame()
 *
 * Slices the symbols of the frame whose last sync symbol is at nb into
 * dibits.
 *
 * Parameters:
 *   rx     - The receiver.
 *   nb     - Position of the last symbol of the sync word.
 *   centre - Middle level of the signal.
 *   amp    - Amplitude of the outer symbols.
 *   frame  - Receives the cfg->frameBytes bytes of the frame.
 *
 * Returns: nothing.
 */
static void sliceFrame(const Fsk4Rx* rx, uint64_t nb, float centre, float amp, uint8_t* frame)
{
    const Fsk4Cfg* cfg = rx->cfg;
    float th = amp * 2.0F / 3.0F;
    uint64_t first = nb - cfg->syncSpan;

    memset(frame, 0x00, cfg->frameBytes);

    for (unsigned int i = 0U; i < cfg->frameSymbols; i++)
    {
        float v = ringY(rx, first + (uint64_t)i * cfg->sps) - centre;
        unsigned int dibit;

        if (v < -th)
            dibit = 1U;         /* +3 */
        else if (v < 0.0F)
            dibit = 0U;         /* +1 */
        else if (v < th)
            dibit = 2U;         /* -1 */
        else
            dibit = 3U;         /* -3 */

        frame[i >> 2] |= (uint8_t)(dibit << (6U - 2U * (i & 3U)));
    }
}

/*
 * addCandidate()
 *
 * Remembers a sync word found while hunting.  Up to FSK4_RX_MAX_CAND
 * candidates are kept; when the list is full the one with the lowest
 * correlation is replaced, if the new one is better.  Each candidate is due
 * for decoding once the whole frame has arrived.
 *
 * Parameters:
 *   rx - The receiver.
 *   nb - Position of the last symbol of the sync word.
 *   r  - Correlation of the candidate.
 *
 * Returns: nothing.
 */
static void addCandidate(Fsk4Rx* rx, uint64_t nb, float r)
{
    unsigned int slot = rx->candCount;

    rx->statCandidates++;

    if (rx->candCount >= FSK4_RX_MAX_CAND)
    {
        unsigned int worst = 0U;

        for (unsigned int i = 1U; i < rx->candCount; i++)
            if (rx->cand[i].r < rx->cand[worst].r)
                worst = i;

        if (rx->cand[worst].r >= r)
            return;

        memmove(&rx->cand[worst], &rx->cand[worst + 1U], (rx->candCount - worst - 1U) * sizeof(Fsk4Cand));
        slot = rx->candCount - 1U;
    }
    else
    {
        rx->candCount++;
    }

    rx->cand[slot].nb = nb;
    rx->cand[slot].due = nb + rx->cfg->frameLast;
    rx->cand[slot].r = r;
}

/*
 * emitFrame()
 *
 * Slices the frame that is due and passes it on.  A provisional first frame
 * is released only if this next frame also decodes; if it does not, the
 * provisional lock is dropped as noise.  For a protocol that asks for it
 * (holdOnMiss) a frame without a sync word is held until the next frame has
 * one.
 *
 * Parameters:
 *   rx - The receiver.
 *   nb - Position of the last symbol of the sync word of the frame.
 *
 * Returns: nothing.
 */
static void emitFrame(Fsk4Rx* rx, uint64_t nb)
{
    const Fsk4Cfg* cfg = rx->cfg;
    uint8_t frame[FSK4_MAX_FRAME_BYTES];

    sliceFrame(rx, nb, rx->centre, rx->amp, frame);

    if (rx->prov)
    {
        if (!cfg->valid(frame))
        {
            /* Not a second frame: it was noise */
            rx->prov = false;
            rx->tracking = false;
            rx->dueValid = false;
            rx->statDropped++;
            return;
        }

        rx->prov = false;
        rx->statLocks++;
        rx->statFrames++;

        if (rx->cb != NULL)
            rx->cb(rx->ctx, rx->provFrame, false);
    }

    if (cfg->holdOnMiss && rx->miss > 0U)
    {
        /* No sync word was found for this frame: only pass it on once the next one is there */
        memcpy(rx->heldFrame, frame, cfg->frameBytes);
        rx->held = true;
        return;
    }

    rx->statFrames++;

    if (rx->cb != NULL)
        rx->cb(rx->ctx, frame, false);
}

/*
 * processCandidates()
 *
 * Takes the sync word candidates whose frames have fully arrived, oldest
 * first.  A candidate whose levels are usable and whose frame passes the
 * channel code check gives a provisional lock: the frame is held back until
 * the next one also decodes.  All other candidates are dropped.
 *
 * Parameters:
 *   rx - The receiver.
 *   n  - Position of the newest sample.
 *
 * Returns: nothing.
 */
static void processCandidates(Fsk4Rx* rx, uint64_t n)
{
    const Fsk4Cfg* cfg = rx->cfg;

    while (rx->candCount > 0U && rx->cand[0].due <= n)
    {
        Fsk4Cand c = rx->cand[0];
        float centre, amp;

        memmove(&rx->cand[0], &rx->cand[1], (rx->candCount - 1U) * sizeof(Fsk4Cand));
        rx->candCount--;

        if (!syncLevels(rx, c.nb, &centre, &amp))
            continue;

        uint8_t frame[FSK4_MAX_FRAME_BYTES];

        sliceFrame(rx, c.nb, centre, amp, frame);

        if (!cfg->valid(frame))
            continue;

        /*
         * Provisional lock.  The frame is held back until the next frame also
         * shows up where it should be and decodes, so noise that happens to
         * look like a frame cannot reach the control logic.
         */
        rx->tracking = true;
        rx->prov = true;
        memcpy(rx->provFrame, frame, cfg->frameBytes);
        rx->miss = 0U;
        rx->centre = centre;
        rx->amp = amp;
        rx->exp = c.nb + cfg->samplesPerFrame;
        rx->windowDone = false;
        rx->dueValid = false;
        rx->candCount = 0U;
        rx->statAmp = amp;
    }
}

/*
 * track()
 *
 * Follows a locked signal.  It decodes the frame scheduled by the last
 * window, then looks for the next sync word within two samples of where it
 * should be and adjusts the centre and amplitude.  A provisional lock that
 * misses a sync is dropped; more than cfg->maxMiss misses in a row end the
 * lock and the callback is told that the signal is lost.
 *
 * Parameters:
 *   rx - The receiver.
 *   n  - Position of the newest sample.
 *
 * Returns: nothing.
 */
static void track(Fsk4Rx* rx, uint64_t n)
{
    const Fsk4Cfg* cfg = rx->cfg;

    /* Decode the frame scheduled by the last window */
    if (rx->dueValid && n >= rx->due)
    {
        rx->dueValid = false;
        emitFrame(rx, rx->nb);
    }

    /* Look for the next sync word around where it should be */
    if (n == rx->exp + 2U)
    {
        uint64_t best = rx->exp;
        float bestR = -1.0F;

        for (uint64_t p = rx->exp - 2U; p <= rx->exp + 2U; p++)
        {
            float r = ringR(rx, p);

            if (r > bestR)
            {
                bestR = r;
                best = p;
            }
        }

        rx->statLastR = bestR;

        float c, a;

        if (bestR >= TRACK_THRESHOLD && syncLevels(rx, best, &c, &a))
        {
            rx->miss = 0U;

            if (rx->held)
            {
                rx->held = false;
                rx->statFrames++;

                if (rx->cb != NULL)
                    rx->cb(rx->ctx, rx->heldFrame, false);
            }

            rx->centre = 0.5F * rx->centre + 0.5F * c;
            rx->amp = 0.5F * rx->amp + 0.5F * a;
            rx->statAmp = rx->amp;
            rx->nb = best;
        }
        else
        {
            rx->miss++;
            rx->nb = rx->exp;
        }

        if (rx->prov && rx->miss > 0U)
        {
            rx->prov = false;
            rx->tracking = false;
            rx->dueValid = false;
            rx->statDropped++;
            return;
        }

        if (rx->miss > cfg->maxMiss)
        {
            rx->tracking = false;
            rx->dueValid = false;
            rx->held = false;
            rx->statLost++;

            if (rx->cb != NULL)
                rx->cb(rx->ctx, NULL, true);

            return;
        }

        rx->due = rx->nb + cfg->frameLast;
        rx->dueValid = true;
        rx->exp = rx->nb + cfg->samplesPerFrame;
    }
}

/*
 * hunt()
 *
 * Looks for the start of a signal.  A local peak of the sync word
 * correlation above HUNT_THRESHOLD, with usable levels, is added as a
 * candidate.
 *
 * Parameters:
 *   rx - The receiver.
 *   n  - Position of the newest sample.
 *
 * Returns: nothing.
 */
static void hunt(Fsk4Rx* rx, uint64_t n)
{
    if (n < rx->cfg->syncSpan + 8U)
        return;

    uint64_t p = n - 2U;
    float r = ringR(rx, p);

    if (r < HUNT_THRESHOLD)
        return;

    if (r < ringR(rx, p - 1U) || r < ringR(rx, p - 2U) || r <= ringR(rx, p + 1U) || r <= ringR(rx, p + 2U))
        return;

    float c, a;

    if (!syncLevels(rx, p, &c, &a))
        return;

    addCandidate(rx, p, r);
}

/*
 * fsk4RxSamples()
 *
 * Feeds demodulated samples to the receiver.  Every complete frame found is
 * passed to the callback as the bytes that came off the air (still
 * scrambled); lost = true reports that the signal has gone.
 *
 * Parameters:
 *   rx      - The receiver.
 *   samples - The demodulated samples.
 *   length  - Number of samples.
 *
 * Returns: nothing.
 */
void fsk4RxSamples(Fsk4Rx* rx, const int16_t* samples, unsigned int length)
{
    const Fsk4Cfg* cfg = rx->cfg;

    for (unsigned int i = 0U; i < length; i++)
    {
        /* matched filter */
        rx->inbuf[rx->inpos] = (float)samples[i];
        rx->inbuf[rx->inpos + cfg->filterTaps] = (float)samples[i];

        const float* w = &rx->inbuf[rx->inpos + 1U];
        float y = 0.0F;

        for (unsigned int k = 0U; k < cfg->filterTaps; k++)
            y += w[k] * rx->taps[k];

        y /= (float)cfg->sps;

        rx->inpos++;
        if (rx->inpos >= cfg->filterTaps)
            rx->inpos = 0U;

        uint64_t n = rx->n++;

        rx->y[n & (FSK4_RX_RING - 1U)] = y;
        rx->r[n & (FSK4_RX_RING - 1U)] = (n >= cfg->syncSpan) ? correlate(rx, n) : 0.0F;

        if (rx->tracking)
            track(rx, n);
        else
        {
            hunt(rx, n);
            processCandidates(rx, n);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Bit stream receive and transmit                                           */
/* ------------------------------------------------------------------------ */

/*
 * bitCount64()
 *
 * Counts the bits that are set in a 64 bit value.
 *
 * Parameters:
 *   v - The value.
 *
 * Returns: The number of one bits in v.
 */
static unsigned int bitCount64(uint64_t v)
{
    unsigned int n = 0U;

    while (v != 0ULL)
    {
        n += (unsigned int)(v & 1ULL);
        v >>= 1;
    }

    return n;
}

/*
 * fsk4BitRxInit()
 *
 * Prepares the bit receiver.  The callback gets every frame and the loss of
 * the signal.
 *
 * Parameters:
 *   rx  - The bit receiver.
 *   cfg - The protocol description.
 *   cb  - Called with every frame found, and when the signal is lost.
 *   ctx - Passed to cb.
 *
 * Returns: nothing.
 */
void fsk4BitRxInit(Fsk4BitRx* rx, Fsk4Cfg* cfg, Fsk4FrameFn cb, void* ctx)
{
    fsk4CfgPrepare(cfg);
    memset(rx, 0x00, sizeof(*rx));
    rx->cfg = cfg;
    rx->cb  = cb;
    rx->ctx = ctx;
}

/*
 * fsk4BitRxReset()
 *
 * Back to searching for a sync word.
 *
 * Parameters:
 *   rx - The bit receiver.
 *
 * Returns: nothing.
 */
void fsk4BitRxReset(Fsk4BitRx* rx)
{
    rx->inFrame   = false;
    rx->window    = 0ULL;
    rx->ptr       = 0U;
    rx->lostCount = 0U;
    memset(rx->frame, 0x00, sizeof(rx->frame));
}

/*
 * bitRxBit()
 *
 * Processes one received bit.  While searching, a sync word match starts a
 * frame.  Inside a frame the bits are collected, the next sync word is looked
 * for only where it is expected (plus or minus two bits), and at the end of
 * the frame it is passed to the callback.  When no sync word has been seen
 * for cfg->maxFrames frames the receiver resets and the callback is told that
 * the signal is lost.
 *
 * Parameters:
 *   rx  - The bit receiver.
 *   bit - The received bit.
 *
 * Returns: nothing.
 */
static void bitRxBit(Fsk4BitRx* rx, bool bit)
{
    const Fsk4Cfg* cfg = rx->cfg;

    rx->window = (rx->window << 1) | (bit ? 1ULL : 0ULL);

    if (!rx->inFrame)
    {
        if (bitCount64((rx->window & cfg->syncBitsMask) ^ cfg->syncBitsValue) <= cfg->startErrs)
        {
            memset(rx->frame, 0x00, sizeof(rx->frame));
            memcpy(rx->frame, cfg->syncBytes, (cfg->syncBits + 7U) / 8U);
            rx->ptr       = cfg->syncBits;
            rx->lostCount = cfg->maxFrames;
            rx->inFrame   = true;
        }

        return;
    }

    if (bit)
        rx->frame[rx->ptr >> 3] |= (uint8_t)(0x80U >> (rx->ptr & 7U));
    else
        rx->frame[rx->ptr >> 3] &= (uint8_t)~(0x80U >> (rx->ptr & 7U));

    rx->ptr++;

    if (rx->ptr > cfg->frameBits)
    {
        fsk4BitRxReset(rx);
        return;
    }

    /* only look for the next sync word in the right place, +-2 bits */
    if (rx->ptr >= cfg->syncBits - 2U && rx->ptr <= cfg->syncBits + 2U)
    {
        if (bitCount64((rx->window & cfg->syncBitsMask) ^ cfg->syncBitsValue) <= cfg->runErrs)
        {
            rx->lostCount = cfg->maxFrames;
            rx->ptr       = cfg->syncBits;
        }
    }

    if (rx->ptr == cfg->frameBits)
    {
        rx->lostCount--;

        if (rx->lostCount == 0U)
        {
            rx->statLost++;
            fsk4BitRxReset(rx);

            if (rx->cb != NULL)
                rx->cb(rx->ctx, NULL, true);
        }
        else
        {
            uint8_t out[FSK4_MAX_FRAME_BYTES];

            memcpy(out, rx->frame, cfg->frameBytes);
            rx->statFrames++;

            memset(rx->frame, 0x00, sizeof(rx->frame));
            rx->ptr = 0U;

            if (rx->cb != NULL)
                rx->cb(rx->ctx, out, false);
        }
    }
}

/*
 * fsk4BitRxBytes()
 *
 * Feeds bytes of the bit stream, most significant bit first.
 *
 * Parameters:
 *   rx     - The bit receiver.
 *   bytes  - The received bytes.
 *   length - Number of bytes.
 *
 * Returns: nothing.
 */
void fsk4BitRxBytes(Fsk4BitRx* rx, const uint8_t* bytes, unsigned int length)
{
    for (unsigned int i = 0U; i < length; i++)
        for (int b = 7; b >= 0; b--)
            bitRxBit(rx, ((bytes[i] >> b) & 1U) != 0U);
}

/*
 * fsk4BitTxPreamble()
 *
 * Fills 'bytes' bytes with the 0x77 pattern (dibits 01 11 = +3 -3 symbols),
 * the same alternating outer symbols as the sample preamble.
 *
 * Parameters:
 *   out   - Receives the bytes.
 *   bytes - Number of bytes.
 *
 * Returns: The number of bytes made.
 */
unsigned int fsk4BitTxPreamble(uint8_t* out, unsigned int bytes)
{
    memset(out, 0x77, bytes);
    return bytes;
}
