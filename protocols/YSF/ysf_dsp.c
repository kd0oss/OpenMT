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

#include <math.h>
#include <string.h>

#include "ysf_dsp.h"
#include "ysf_func.h"

#define ALPHA               0.2F
#define SPS                 YSF_SAMPLES_PER_SYMBOL
#define SYNC_SYMBOLS        20U
#define SYNC_SPAN           ((SYNC_SYMBOLS - 1U) * SPS)     /* samples from the first to the last sync symbol */
#define FRAME_LAST          ((YSF_FRAME_LENGTH_SYMBOLS - SYNC_SYMBOLS) * SPS)   /* samples from the last sync symbol to the last frame symbol */
#define FRAME_FIRST         SYNC_SPAN       /* samples from the first frame symbol to the last sync symbol */

#define HUNT_THRESHOLD      0.80F
#define TRACK_THRESHOLD     0.55F

/*
 * The sync word in the sample domain: q = -(symbol), made from the sync bytes
 * the first time it is needed.  Dibits 01 -> +3, 00 -> +1, 10 -> -1, 11 -> -3.
 */
static float SYNC_Q[SYNC_SYMBOLS];
static float SYNC_ENERGY;
static unsigned int SYNC_HI;            /* number of +-3 symbols of each sign, for the levels */
static unsigned int SYNC_LO;
static bool syncReady = false;

static void makeSync(void)
{
    uint8_t sync[YSF_SYNC_LENGTH_BYTES];

    if (syncReady)
        return;

    ysfAddSync(sync);

    SYNC_ENERGY = 0.0F;
    SYNC_HI = 0U;
    SYNC_LO = 0U;

    for (unsigned int i = 0U; i < SYNC_SYMBOLS; i++)
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
        SYNC_ENERGY += q * q;

        if (q > 2.0F)
            SYNC_HI++;
        else if (q < -2.0F)
            SYNC_LO++;
    }

    syncReady = true;
}

/*
 * makeTaps()
 *   Root raised cosine (or raised cosine) pulse, alpha 0.2, scaled so that the
 *   taps sum to the number of samples per symbol.
 */
static void makeTaps(float* taps, uint8_t shape)
{
    const float pi = 3.14159265358979F;
    int mid = (int)(YSF_FILTER_TAPS / 2U);
    float sum = 0.0F;

    for (int i = 0; i < (int)YSF_FILTER_TAPS; i++)
    {
        float t = (float)(i - mid) / (float)SPS;
        float h;

        if (shape == YSF_SHAPE_RC)
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

    for (unsigned int i = 0U; i < YSF_FILTER_TAPS; i++)
        taps[i] *= (float)SPS / sum;
}

/* ------------------------------------------------------------------------ */
/* Transmit                                                                  */
/* ------------------------------------------------------------------------ */

void ysfTxInit(YsfTx* tx, float level, uint8_t shape)
{
    memset(tx->sym, 0x00, sizeof(tx->sym));
    tx->level = level;
    tx->shape = shape;
    makeTaps(tx->taps, shape);
}

void ysfTxSetLevel(YsfTx* tx, float level)
{
    tx->level = level;
}

static int16_t clip16(float v)
{
    if (v > 32767.0F)
        return 32767;
    if (v < -32768.0F)
        return -32768;

    return (int16_t)lrintf(v);
}

/* Pushes one symbol in the sample domain (q = +-3, +-1) and makes 10 samples */
static void txSymbol(YsfTx* tx, float q, int16_t* out)
{
    const unsigned int count = YSF_FILTER_SPAN * 2U + 1U;

    memmove(&tx->sym[1], &tx->sym[0], (count - 1U) * sizeof(float));
    tx->sym[0] = q * (tx->level / 3.0F);

    for (unsigned int j = 0U; j < SPS; j++)
    {
        float y = 0.0F;

        for (unsigned int m = 0U; m < count; m++)
        {
            unsigned int idx = j + SPS * m;

            if (idx < YSF_FILTER_TAPS)
                y += tx->sym[m] * tx->taps[idx];
        }

        out[j] = clip16(y);
    }
}

/*
 * ysfTxPreamble()
 *   Alternating +3 -3 symbols (0x77 bytes) so the receiver can settle.
 */
unsigned int ysfTxPreamble(YsfTx* tx, unsigned int symbols, int16_t* out)
{
    for (unsigned int i = 0U; i < symbols; i++)
        txSymbol(tx, (i & 1U) ? 3.0F : -3.0F, out + i * SPS);

    return symbols * SPS;
}

/*
 * ysfTxFrame()
 *   480 symbols from the 120 bytes of a frame (sync and all), 2400
 *   samples.  The dibit 01 is symbol +3, which is a negative sample.
 */
unsigned int ysfTxFrame(YsfTx* tx, const uint8_t* frame, int16_t* out)
{
    for (unsigned int i = 0U; i < YSF_FRAME_LENGTH_SYMBOLS; i++)
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

        txSymbol(tx, q, out + i * SPS);
    }

    return YSF_SAMPLES_PER_FRAME;
}

/*
 * ysfTxFlush()
 *   Pushes zero symbols so the last real symbol leaves the filter.
 */
unsigned int ysfTxFlush(YsfTx* tx, int16_t* out)
{
    const unsigned int symbols = YSF_FILTER_SPAN + 1U;

    for (unsigned int i = 0U; i < symbols; i++)
        txSymbol(tx, 0.0F, out + i * SPS);

    return symbols * SPS;
}

/* ------------------------------------------------------------------------ */
/* Receive                                                                   */
/* ------------------------------------------------------------------------ */

void ysfRxInit(YsfRx* rx, YsfRxFrameFn cb, void* ctx, uint8_t shape)
{
    makeSync();
    memset(rx, 0x00, sizeof(*rx));
    makeTaps(rx->taps, shape);
    rx->minAmp = 30.0F;
    rx->cb = cb;
    rx->ctx = ctx;
}

void ysfRxReset(YsfRx* rx)
{
    rx->candCount = 0U;
    rx->tracking = false;
    rx->prov = false;
    rx->dueValid = false;
    rx->windowDone = false;
    rx->held = false;
    rx->miss = 0U;
}

static inline float ringY(const YsfRx* rx, uint64_t idx)
{
    return rx->y[idx & (YSF_RX_RING - 1U)];
}

static inline float ringR(const YsfRx* rx, uint64_t idx)
{
    return rx->r[idx & (YSF_RX_RING - 1U)];
}

/* Correlation of the last 96 filter outputs against the frame sync word */
static float correlate(const YsfRx* rx, uint64_t n)
{
    float sx = 0.0F, sxx = 0.0F, sqx = 0.0F;

    for (unsigned int k = 0U; k < SYNC_SYMBOLS; k++)
    {
        float x = ringY(rx, n - SYNC_SPAN + k * SPS);
        sx += x;
        sxx += x * x;
        sqx += SYNC_Q[k] * x;
    }

    float var = sxx - sx * sx / (float)SYNC_SYMBOLS;

    if (var < 1.0F)
        return 0.0F;

    return sqx / sqrtf(SYNC_ENERGY * var);
}

/* High and low levels measured on the sync word ending at nb */
static bool syncLevels(const YsfRx* rx, uint64_t nb, float* centre, float* amp)
{
    float hi = 0.0F, lo = 0.0F;
    float x[SYNC_SYMBOLS];

    for (unsigned int k = 0U; k < SYNC_SYMBOLS; k++)
    {
        x[k] = ringY(rx, nb - SYNC_SPAN + k * SPS);

        if (SYNC_Q[k] > 2.0F)
            hi += x[k];
        else if (SYNC_Q[k] < -2.0F)
            lo += x[k];
    }

    hi /= (float)SYNC_HI;
    lo /= (float)SYNC_LO;

    float c = (hi + lo) / 2.0F;
    float a = (hi - lo) / 2.0F;

    *centre = c;
    *amp = a;

    if (a < rx->minAmp)
        return false;

    /* Every sync symbol has to be on the right side of its slicing level */
    for (unsigned int k = 0U; k < SYNC_SYMBOLS; k++)
    {
        float v = x[k] - c;

        if (SYNC_Q[k] > 2.0F && v < a / 3.0F)
            return false;
        if (SYNC_Q[k] < -2.0F && v > -a / 3.0F)
            return false;
        if (SYNC_Q[k] > 0.0F && SYNC_Q[k] < 2.0F && v < -a / 3.0F)
            return false;
        if (SYNC_Q[k] < 0.0F && SYNC_Q[k] > -2.0F && v > a / 3.0F)
            return false;
    }

    return true;
}

/* Slices the 480 symbols of the frame whose last sync symbol is at nb */
static void sliceFrame(const YsfRx* rx, uint64_t nb, float centre, float amp, uint8_t* frame)
{
    float th = amp * 2.0F / 3.0F;
    uint64_t first = nb - FRAME_FIRST;

    memset(frame, 0x00, YSF_FRAME_LENGTH_BYTES);

    for (unsigned int i = 0U; i < YSF_FRAME_LENGTH_SYMBOLS; i++)
    {
        float v = ringY(rx, first + (uint64_t)i * SPS) - centre;
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

/* A hunting candidate is accepted only if the FICH (Golay and CRC) agrees */
static bool frameLooksValid(const uint8_t* raw)
{
    YsfFich fich;

    ysfFichInit(&fich);

    return ysfFichDecode(&fich, raw);
}

static void addCandidate(YsfRx* rx, uint64_t nb, float r)
{
    unsigned int slot = rx->candCount;

    rx->statCandidates++;

    if (rx->candCount >= YSF_RX_MAX_CAND)
    {
        unsigned int worst = 0U;

        for (unsigned int i = 1U; i < rx->candCount; i++)
            if (rx->cand[i].r < rx->cand[worst].r)
                worst = i;

        if (rx->cand[worst].r >= r)
            return;

        memmove(&rx->cand[worst], &rx->cand[worst + 1U], (rx->candCount - worst - 1U) * sizeof(YsfCand));
        slot = rx->candCount - 1U;
    }
    else
    {
        rx->candCount++;
    }

    rx->cand[slot].nb = nb;
    rx->cand[slot].due = nb + FRAME_LAST;
    rx->cand[slot].r = r;
}

static void emitFrame(YsfRx* rx, uint64_t nb)
{
    uint8_t frame[YSF_FRAME_LENGTH_BYTES];

    sliceFrame(rx, nb, rx->centre, rx->amp, frame);

    if (rx->prov)
    {
        if (!frameLooksValid(frame))
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

    if (rx->miss > 0U)
    {
        /* No sync word was found for this frame: only pass it on once the next one is there */
        memcpy(rx->heldFrame, frame, YSF_FRAME_LENGTH_BYTES);
        rx->held = true;
        return;
    }

    rx->statFrames++;

    if (rx->cb != NULL)
        rx->cb(rx->ctx, frame, false);
}

static void processCandidates(YsfRx* rx, uint64_t n)
{
    while (rx->candCount > 0U && rx->cand[0].due <= n)
    {
        YsfCand c = rx->cand[0];
        float centre, amp;

        memmove(&rx->cand[0], &rx->cand[1], (rx->candCount - 1U) * sizeof(YsfCand));
        rx->candCount--;

        if (!syncLevels(rx, c.nb, &centre, &amp))
            continue;

        uint8_t frame[YSF_FRAME_LENGTH_BYTES];

        sliceFrame(rx, c.nb, centre, amp, frame);

        if (!frameLooksValid(frame))
            continue;

        /*
         * Provisional lock.  The frame is held back until the next frame also
         * shows up where it should be and decodes, so noise that happens to
         * look like a frame cannot reach the control logic.
         */
        rx->tracking = true;
        rx->prov = true;
        memcpy(rx->provFrame, frame, YSF_FRAME_LENGTH_BYTES);
        rx->miss = 0U;
        rx->centre = centre;
        rx->amp = amp;
        rx->exp = c.nb + YSF_SAMPLES_PER_FRAME;
        rx->windowDone = false;
        rx->dueValid = false;
        rx->candCount = 0U;
        rx->statAmp = amp;
    }
}

static void track(YsfRx* rx, uint64_t n)
{
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

        if (rx->miss > YSF_RX_MAX_MISS)
        {
            rx->tracking = false;
            rx->dueValid = false;
            rx->held = false;
            rx->statLost++;

            if (rx->cb != NULL)
                rx->cb(rx->ctx, NULL, true);

            return;
        }

        rx->due = rx->nb + FRAME_LAST;
        rx->dueValid = true;
        rx->exp = rx->nb + YSF_SAMPLES_PER_FRAME;
    }
}

static void hunt(YsfRx* rx, uint64_t n)
{
    if (n < SYNC_SPAN + 8U)
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
 * ysfRxSamples()
 *   Feeds demodulated samples to the receiver.  Every complete frame found is
 *   passed to the callback as the 120 bytes that came off the air (still
 *   scrambled); lost = true reports that the signal has gone.
 */
void ysfRxSamples(YsfRx* rx, const int16_t* samples, unsigned int length)
{
    for (unsigned int i = 0U; i < length; i++)
    {
        /* matched filter */
        rx->inbuf[rx->inpos] = (float)samples[i];
        rx->inbuf[rx->inpos + YSF_FILTER_TAPS] = (float)samples[i];

        const float* w = &rx->inbuf[rx->inpos + 1U];
        float y = 0.0F;

        for (unsigned int k = 0U; k < YSF_FILTER_TAPS; k++)
            y += w[k] * rx->taps[k];

        y /= (float)SPS;

        rx->inpos++;
        if (rx->inpos >= YSF_FILTER_TAPS)
            rx->inpos = 0U;

        uint64_t n = rx->n++;

        rx->y[n & (YSF_RX_RING - 1U)] = y;
        rx->r[n & (YSF_RX_RING - 1U)] = (n >= SYNC_SPAN) ? correlate(rx, n) : 0.0F;

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

#define YSF_SYNC_BITS_VALUE         0x000000D471C9634DULL
#define YSF_SYNC_BITS_MASK_VALUE    0x000000FFFFFFFFFFULL
#define YSF_SYNC_START_ERRS         2U      /* allowed to start a lock       */
#define YSF_SYNC_RUN_ERRS           4U      /* allowed once locked           */

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
 * ysfBitRxInit()
 *   Prepares the bit receiver. cb gets every frame (and the loss of the signal).
 */
void ysfBitRxInit(YsfBitRx* rx, YsfRxFrameFn cb, void* ctx)
{
    memset(rx, 0x00, sizeof(*rx));
    rx->cb  = cb;
    rx->ctx = ctx;
}

/*
 * ysfBitRxReset()
 *   Back to searching for a sync word.
 */
void ysfBitRxReset(YsfBitRx* rx)
{
    rx->inFrame   = false;
    rx->window    = 0ULL;
    rx->ptr       = 0U;
    rx->lostCount = 0U;
    memset(rx->frame, 0x00, sizeof(rx->frame));
}

static void bitRxBit(YsfBitRx* rx, bool bit)
{
    rx->window = (rx->window << 1) | (bit ? 1ULL : 0ULL);

    if (!rx->inFrame)
    {
        if (bitCount64((rx->window & YSF_SYNC_BITS_MASK_VALUE) ^ YSF_SYNC_BITS_VALUE) <= YSF_SYNC_START_ERRS)
        {
            memset(rx->frame, 0x00, sizeof(rx->frame));
            ysfAddSync(rx->frame);
            rx->ptr       = YSF_SYNC_LENGTH_BITS;
            rx->lostCount = YSF_BIT_MAX_FRAMES;
            rx->inFrame   = true;
        }

        return;
    }

    if (bit)
        rx->frame[rx->ptr >> 3] |= (uint8_t)(0x80U >> (rx->ptr & 7U));
    else
        rx->frame[rx->ptr >> 3] &= (uint8_t)~(0x80U >> (rx->ptr & 7U));

    rx->ptr++;

    if (rx->ptr > YSF_FRAME_LENGTH_BITS)
    {
        ysfBitRxReset(rx);
        return;
    }

    /* only look for the next sync word in the right place, +-2 bits */
    if (rx->ptr >= YSF_SYNC_LENGTH_BITS - 2U && rx->ptr <= YSF_SYNC_LENGTH_BITS + 2U)
    {
        if (bitCount64((rx->window & YSF_SYNC_BITS_MASK_VALUE) ^ YSF_SYNC_BITS_VALUE) <= YSF_SYNC_RUN_ERRS)
        {
            rx->lostCount = YSF_BIT_MAX_FRAMES;
            rx->ptr       = YSF_SYNC_LENGTH_BITS;
        }
    }

    if (rx->ptr == YSF_FRAME_LENGTH_BITS)
    {
        rx->lostCount--;

        if (rx->lostCount == 0U)
        {
            rx->statLost++;
            ysfBitRxReset(rx);

            if (rx->cb != NULL)
                rx->cb(rx->ctx, NULL, true);
        }
        else
        {
            uint8_t out[YSF_FRAME_LENGTH_BYTES];

            memcpy(out, rx->frame, sizeof(out));
            rx->statFrames++;

            memset(rx->frame, 0x00, sizeof(rx->frame));
            rx->ptr = 0U;

            if (rx->cb != NULL)
                rx->cb(rx->ctx, out, false);
        }
    }
}

/*
 * ysfBitRxBytes()
 *   Feeds bytes of the bit stream, most significant bit first.
 */
void ysfBitRxBytes(YsfBitRx* rx, const uint8_t* bytes, unsigned int length)
{
    for (unsigned int i = 0U; i < length; i++)
        for (int b = 7; b >= 0; b--)
            bitRxBit(rx, ((bytes[i] >> b) & 1U) != 0U);
}

/*
 * ysfBitTxPreamble()
 *   Fills 'bytes' bytes with the 0x77 pattern (dibits 01 11 = +3 -3 symbols),
 *   the same alternating outer symbols as the sample preamble.
 */
unsigned int ysfBitTxPreamble(uint8_t* out, unsigned int bytes)
{
    memset(out, 0x77, bytes);
    return bytes;
}
