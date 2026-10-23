/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS                                     *
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
 ***************************************************************************/

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "fm_dsp.h"

#define FM_TWO_PI 6.28318530717958647692f

static const char* const MORSE[] = {
    ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
    "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.."};

static const char* const MORSE_DIGITS[] = {
    "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};

/*
 * fmBiquadLowPass()
 *
 * Sets b up as a second order low pass with cutoff fc (Hz) and quality q at
 * the FM sample rate and clears its state.
 *
 * Parameters:
 *   b  - The filter.
 *   fc - Cutoff frequency, in Hz.
 *   q  - Quality factor.
 *
 * Returns: nothing.
 */
void fmBiquadLowPass(FmBiquad* b, float fc, float q)
{
    float w0    = FM_TWO_PI * fc / (float)FM_FS;
    float cs    = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0    = 1.0f + alpha;

    b->b0 = ((1.0f - cs) / 2.0f) / a0;
    b->b1 = (1.0f - cs) / a0;
    b->b2 = ((1.0f - cs) / 2.0f) / a0;
    b->a1 = (-2.0f * cs) / a0;
    b->a2 = (1.0f - alpha) / a0;
    b->z1 = 0.0f;
    b->z2 = 0.0f;
}

/*
 * fmBiquadHighPass()
 *
 * Sets b up as a second order high pass with cutoff fc (Hz) and quality q at
 * the FM sample rate and clears its state.
 *
 * Parameters:
 *   b  - The filter.
 *   fc - Cutoff frequency, in Hz.
 *   q  - Quality factor.
 *
 * Returns: nothing.
 */
void fmBiquadHighPass(FmBiquad* b, float fc, float q)
{
    float w0    = FM_TWO_PI * fc / (float)FM_FS;
    float cs    = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0    = 1.0f + alpha;

    b->b0 = ((1.0f + cs) / 2.0f) / a0;
    b->b1 = (-(1.0f + cs)) / a0;
    b->b2 = ((1.0f + cs) / 2.0f) / a0;
    b->a1 = (-2.0f * cs) / a0;
    b->a2 = (1.0f - alpha) / a0;
    b->z1 = 0.0f;
    b->z2 = 0.0f;
}

/*
 * fmBiquadRun()
 *
 * Filters one sample through b and returns the result.
 *
 * Parameters:
 *   b - The filter.
 *   x - The input sample.
 *
 * Returns: The filtered sample.
 */
float fmBiquadRun(FmBiquad* b, float x)
{
    float y = b->b0 * x + b->z1;

    b->z1 = b->b1 * x - b->a1 * y + b->z2;
    b->z2 = b->b2 * x - b->a2 * y;
    return y;
}

/*
 * fmGoertzel()
 *
 * Returns the signal power at frequency f (Hz) over the n samples of the
 * circular buffer buf, read oldest first starting at index start. The buffer
 * holds samples taken at FM_CT_FS.
 *
 * Parameters:
 *   buf   - The circular buffer of samples.
 *   n     - Number of samples in buf.
 *   start - Index of the oldest sample.
 *   f     - The frequency to measure, in Hz.
 *
 * Returns: The power at frequency f.
 */
static float fmGoertzel(const float* buf, uint16_t n, uint16_t start, float f)
{
    float coeff = 2.0f * cosf(FM_TWO_PI * f / (float)FM_CT_FS);
    float s1    = 0.0f;
    float s2    = 0.0f;
    uint16_t idx = start;

    for (uint16_t i = 0; i < n; i++)
    {
        float s0 = buf[idx] + coeff * s1 - s2;

        s2 = s1;
        s1 = s0;
        idx++;
        if (idx >= n)
            idx = 0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/*
 * fmCtcssInit()
 *
 * Prepares the CTCSS detector for tone freq (Hz, 0 disables it). The tone
 * must make up at least thresholdPercent of the energy below 300 Hz to be
 * accepted, and is held after it disappears for holdMs.
 *
 * Parameters:
 *   d                - The detector.
 *   freq             - The tone frequency in Hz, or 0 to turn the detector
 *                      off.
 *   thresholdPercent - Share of the energy below 300 Hz that the tone must
 *                      have.
 *   holdMs           - How long the detection is held after the tone
 *                      disappears, in milliseconds.
 *
 * Returns: nothing.
 */
void fmCtcssInit(FmCtcssDet* d, float freq, float thresholdPercent, uint32_t holdMs)
{
    memset(d, 0, sizeof(*d));
    fmBiquadLowPass(&d->lp[0], 300.0f, 0.5412f);
    fmBiquadLowPass(&d->lp[1], 300.0f, 1.3066f);
    d->freq      = freq;
    d->offset    = 9.0f;
    d->threshold = thresholdPercent / 100.0f;
    d->openHops  = 2;
    d->closeHops = (uint8_t)((holdMs + FM_CT_HOP_MS - 1) / FM_CT_HOP_MS);
    if (d->closeHops < 1)
        d->closeHops = 1;
    if (d->closeHops > 250)
        d->closeHops = 250;
}

/*
 * fmCtcssReset()
 *
 * Forgets the audio history and the detected state, keeping the settings.
 *
 * Parameters:
 *   d - The detector.
 *
 * Returns: nothing.
 */
void fmCtcssReset(FmCtcssDet* d)
{
    FmCtcssDet keep = *d;

    fmCtcssInit(d, keep.freq, keep.threshold * 100.0f, (uint32_t)keep.closeHops * FM_CT_HOP_MS);
}

/*
 * fmCtcssEvaluate()
 *
 * Runs the tone test on the current window. It measures the power at the
 * wanted tone and at tones offset above and below it, and sets raw (clearly
 * present), weak (still present) and the run counters; detected follows the
 * counters.
 *
 * Parameters:
 *   d - The detector.
 *
 * Returns: nothing.
 */
static void fmCtcssEvaluate(FmCtcssDet* d)
{
    uint16_t start = d->pos;
    float    energy = 0.0f;
    float    low = d->freq - d->offset;
    float    high = d->freq + d->offset;

    for (uint16_t i = 0; i < FM_CT_N; i++)
        energy += d->buf[i] * d->buf[i];

    if (low < 40.0f)
        low = 40.0f;

    float pt = fmGoertzel(d->buf, FM_CT_N, start, d->freq);
    float pl = fmGoertzel(d->buf, FM_CT_N, start, low);
    float ph = fmGoertzel(d->buf, FM_CT_N, start, high);
    float pn = pl > ph ? pl : ph;

    if (energy < 1.0f)
    {
        d->lastFraction = 0.0f;
        d->lastRatio    = 0.0f;
    }
    else
    {
        d->lastFraction = 2.0f * pt / ((float)FM_CT_N * energy);
        d->lastRatio    = pt / (pn + 1e-9f);
    }

    bool strong = d->lastFraction >= d->threshold && d->lastRatio >= 3.0f;
    bool weak   = d->lastFraction >= d->threshold * 0.5f && d->lastRatio >= 2.0f;

    d->raw  = strong;
    d->weak = weak;

    if (!d->detected)
    {
        d->posRun = strong ? (uint8_t)(d->posRun + 1) : 0;
        if (d->posRun >= d->openHops)
        {
            d->detected = true;
            d->negRun   = 0;
        }
    }
    else
    {
        d->negRun = weak ? 0 : (uint8_t)(d->negRun + 1);
        if (d->negRun >= d->closeHops)
        {
            d->detected = false;
            d->posRun   = 0;
        }
    }
}

/*
 * fmCtcssProcess()
 *
 * Feeds one input sample (24 kHz) to the detector and returns whether the
 * CTCSS tone is currently detected. Always false when no tone is set.
 *
 * Parameters:
 *   d - The detector.
 *   x - The input sample.
 *
 * Returns: true if the CTCSS tone is detected, false if it is not or no tone
 *   is set.
 */
bool fmCtcssProcess(FmCtcssDet* d, float x)
{
    if (d->freq <= 0.0f)
        return false;

    float y = fmBiquadRun(&d->lp[1], fmBiquadRun(&d->lp[0], x));

    d->decimCount++;
    if (d->decimCount < FM_CT_DECIM)
        return d->detected;
    d->decimCount = 0;

    d->buf[d->pos] = y;
    d->pos++;
    if (d->pos >= FM_CT_N)
        d->pos = 0;
    if (d->filled < FM_CT_N)
        d->filled++;

    d->hopCount++;
    if (d->hopCount >= FM_CT_HOP && d->filled >= FM_CT_N)
    {
        d->hopCount = 0;
        fmCtcssEvaluate(d);
    }
    return d->detected;
}

/*
 * fmCtcssScan()
 *
 * Diagnostic. Tests the current window against the 50 standard CTCSS tones
 * and returns the strongest one, with the fraction of the low band energy
 * that it holds.
 *
 * Parameters:
 *   d            - The detector.
 *   bestFreq     - Receives the frequency of the strongest tone, in Hz.
 *   bestFraction - Receives the fraction of the low band energy that it
 *                  holds.
 *
 * Returns: nothing.
 */
void fmCtcssScan(const FmCtcssDet* d, float* bestFreq, float* bestFraction)
{
    static const float tones[] =
    {
        67.0f, 69.3f, 71.9f, 74.4f, 77.0f, 79.7f, 82.5f, 85.4f, 88.5f, 91.5f,
        94.8f, 97.4f, 100.0f, 103.5f, 107.2f, 110.9f, 114.8f, 118.8f, 123.0f, 127.3f,
        131.8f, 136.5f, 141.3f, 146.2f, 151.4f, 156.7f, 162.2f, 167.9f, 173.8f, 179.9f,
        186.2f, 192.8f, 203.5f, 210.7f, 218.1f, 225.7f, 233.6f, 241.8f, 250.3f
    };
    float energy = 0.0f;

    *bestFreq     = 0.0f;
    *bestFraction = 0.0f;
    if (d->filled < FM_CT_N)
        return;
    for (uint16_t i = 0; i < FM_CT_N; i++)
        energy += d->buf[i] * d->buf[i];
    if (energy < 1.0f)
        return;
    for (unsigned int k = 0; k < sizeof(tones) / sizeof(tones[0]); k++)
    {
        float f = 2.0f * fmGoertzel(d->buf, FM_CT_N, d->pos, tones[k]) / ((float)FM_CT_N * energy);

        if (f > *bestFraction)
        {
            *bestFraction = f;
            *bestFreq     = tones[k];
        }
    }
}

/*
 * fmNoiseInit()
 *
 * Prepares the noise squelch. The squelch opens when the smoothed high band
 * noise falls to openPercent of full scale or below and closes again when it
 * reaches closePercent.
 *
 * Parameters:
 *   n            - The squelch.
 *   openPercent  - Noise level at or below which the squelch opens, as a
 *                  percentage of full scale.
 *   closePercent - Noise level at which the squelch closes, as a percentage
 *                  of full scale.
 *
 * Returns: nothing.
 */
void fmNoiseInit(FmNoiseSq* n, float openPercent, float closePercent)
{
    memset(n, 0, sizeof(*n));
    fmBiquadHighPass(&n->hp[0], 6000.0f, 0.5412f);
    fmBiquadHighPass(&n->hp[1], 6000.0f, 1.3066f);
    n->alpha      = 1.0f - expf(-1.0f / (0.020f * (float)FM_FS));
    n->level      = 100.0f;
    n->openLevel  = openPercent;
    n->closeLevel = closePercent > openPercent ? closePercent : openPercent;
    n->open       = false;
}

/*
 * fmNoiseReset()
 *
 * Closes the squelch and clears the filters, keeping the thresholds.
 *
 * Parameters:
 *   n - The squelch.
 *
 * Returns: nothing.
 */
void fmNoiseReset(FmNoiseSq* n)
{
    FmNoiseSq keep = *n;

    fmNoiseInit(n, keep.openLevel, keep.closeLevel);
}

/*
 * fmNoiseProcess()
 *
 * Feeds one input sample (24 kHz) to the noise squelch and returns whether
 * it is open, meaning a carrier is quieting the receiver noise.
 *
 * Parameters:
 *   n - The squelch.
 *   x - The input sample.
 *
 * Returns: true if the squelch is open (a carrier is quieting the noise).
 */
bool fmNoiseProcess(FmNoiseSq* n, float x)
{
    float y = fmBiquadRun(&n->hp[1], fmBiquadRun(&n->hp[0], x));

    n->level += n->alpha * (fabsf(y) * 100.0f / 32768.0f - n->level);

    if (n->open)
    {
        if (n->level >= n->closeLevel)
            n->open = false;
    }
    else
    {
        if (n->level <= n->openLevel)
            n->open = true;
    }
    return n->open;
}

/*
 * fmAudioInit()
 *
 * Prepares the audio chain: DC blocker, 300 Hz high pass (removes the CTCSS
 * tone and anything below it) and 3 kHz low pass, both fourth order.
 *
 * Parameters:
 *   a - The audio chain.
 *
 * Returns: nothing.
 */
void fmAudioInit(FmAudio* a)
{
    memset(a, 0, sizeof(*a));
    fmBiquadHighPass(&a->hp[0], 300.0f, 0.5412f);
    fmBiquadHighPass(&a->hp[1], 300.0f, 1.3066f);
    fmBiquadLowPass(&a->lp[0], 3000.0f, 0.5412f);
    fmBiquadLowPass(&a->lp[1], 3000.0f, 1.3066f);
}

/*
 * fmAudioDc()
 *
 * Removes the DC offset of the discriminator output from one sample.
 *
 * Parameters:
 *   a - The audio chain.
 *   x - The input sample.
 *
 * Returns: The sample without the DC offset.
 */
float fmAudioDc(FmAudio* a, float x)
{
    float y = x - a->dcX + 0.995f * a->dcY;

    a->dcX = x;
    a->dcY = y;
    return y;
}

/*
 * fmAudioShape()
 *
 * Band limits one DC free sample to the voice band, 300 Hz to 3 kHz.
 *
 * Parameters:
 *   a - The audio chain.
 *   x - The DC free sample.
 *
 * Returns: The band limited sample.
 */
float fmAudioShape(FmAudio* a, float x)
{
    float y = fmBiquadRun(&a->hp[1], fmBiquadRun(&a->hp[0], x));

    return fmBiquadRun(&a->lp[1], fmBiquadRun(&a->lp[0], y));
}

/*
 * fmToneInit()
 *
 * Prepares the sine generator for freq (Hz); a frequency of 0 makes it
 * output silence.
 *
 * Parameters:
 *   t    - The generator.
 *   freq - The frequency in Hz, or 0 for silence.
 *
 * Returns: nothing.
 */
void fmToneInit(FmTone* t, float freq)
{
    t->phase = 0.0f;
    t->step  = FM_TWO_PI * freq / (float)FM_FS;
}

/*
 * fmToneNext()
 *
 * Returns the next generator sample, amplitude 1.0.
 *
 * Parameters:
 *   t - The generator.
 *
 * Returns: The next sample.
 */
float fmToneNext(FmTone* t)
{
    float v = sinf(t->phase);

    t->phase += t->step;
    if (t->phase >= FM_TWO_PI)
        t->phase -= FM_TWO_PI;
    return t->step > 0.0f ? v : 0.0f;
}

/*
 * fmCwAdd()
 *
 * Appends one keying event, on or off for units dit lengths, to the CW
 * timeline of c.
 *
 * Parameters:
 *   c           - The CW generator.
 *   on          - true for a key down (tone) period, false for silence.
 *   units       - Length of the period, in dit lengths.
 *   unitSamples - Number of samples in a dit.
 *
 * Returns: nothing.
 */
static void fmCwAdd(FmCw* c, bool on, uint32_t units, uint32_t unitSamples)
{
    if (c->count >= FM_CW_MAX_EVENTS)
        return;
    c->on[c->count]  = on ? 1 : 0;
    c->len[c->count] = units * unitSamples;
    c->count++;
}

/*
 * fmCwStart()
 *
 * Builds the Morse timeline for text (letters, digits, '/' and spaces) at
 * wpm words per minute with a sidetone of freq (Hz) and starts it.
 *
 * Parameters:
 *   c    - The CW generator.
 *   text - The message.
 *   wpm  - Speed in words per minute (20 if 0).
 *   freq - The sidetone frequency, in Hz.
 *
 * Returns: nothing.
 */
void fmCwStart(FmCw* c, const char* text, uint8_t wpm, float freq)
{
    uint32_t unit = (uint32_t)((float)FM_FS * 1.2f / (float)(wpm > 0 ? wpm : 20));

    memset(c, 0, sizeof(*c));
    c->step = FM_TWO_PI * freq / (float)FM_FS;
    fmCwAdd(c, false, 2, unit);

    for (const char* p = text; *p; p++)
    {
        const char* code = NULL;
        char        ch   = (char)toupper((unsigned char)*p);

        if (ch >= 'A' && ch <= 'Z')
            code = MORSE[ch - 'A'];
        else if (ch >= '0' && ch <= '9')
            code = MORSE_DIGITS[ch - '0'];
        else if (ch == '/')
            code = "-..-.";
        else if (ch == ' ')
        {
            fmCwAdd(c, false, 4, unit);
            continue;
        }

        if (code == NULL)
            continue;

        for (const char* e = code; *e; e++)
        {
            fmCwAdd(c, true, *e == '-' ? 3 : 1, unit);
            fmCwAdd(c, false, 1, unit);
        }
        fmCwAdd(c, false, 2, unit);
    }

    c->index  = 0;
    c->left   = c->count > 0 ? c->len[0] : 0;
    c->active = c->count > 0;
}

/*
 * fmCwNext()
 *
 * Produces the next CW sample into out (amplitude 1.0 with softened key
 * edges). Returns false once the message has finished.
 *
 * Parameters:
 *   c   - The CW generator.
 *   out - Receives the sample.
 *
 * Returns: true if a sample was made, false once the message has finished.
 */
bool fmCwNext(FmCw* c, float* out)
{
    if (!c->active)
    {
        *out = 0.0f;
        return false;
    }

    while (c->left == 0)
    {
        c->index++;
        if (c->index >= c->count)
        {
            c->active = false;
            *out      = 0.0f;
            return false;
        }
        c->left = c->len[c->index];
    }

    float target = c->on[c->index] ? 1.0f : 0.0f;

    c->env += 0.0104f * (target - c->env);
    c->phase += c->step;
    if (c->phase >= FM_TWO_PI)
        c->phase -= FM_TWO_PI;
    *out = c->env * sinf(c->phase);
    c->left--;
    return true;
}

/*
 * fmDecimInit()
 *
 * Prepares the converter from the 24 kSa/s audio to the 8 kSa/s of EchoLink.
 * The anti-alias filter is a fourth order 3.4 kHz low pass.
 *
 * Parameters:
 *   d - The converter.
 *
 * Returns: nothing.
 */
void fmDecimInit(FmDecim* d)
{
    memset(d, 0, sizeof(*d));
    fmBiquadLowPass(&d->lp[0], 3400.0f, 0.5412f);
    fmBiquadLowPass(&d->lp[1], 3400.0f, 1.3066f);
}

/*
 * fmDecimPush()
 *
 * Feeds one 24 kSa/s sample to the converter.
 *
 * Parameters:
 *   d   - The converter.
 *   x   - The input sample.
 *   out - Receives the 8 kSa/s sample when one is produced.
 *
 * Returns: true when out was written (every third input sample), false
 *   otherwise.
 */
bool fmDecimPush(FmDecim* d, float x, float* out)
{
    float y = fmBiquadRun(&d->lp[1], fmBiquadRun(&d->lp[0], x));

    if (++d->phase < FM_LINK_DECIM)
        return false;
    d->phase = 0;
    *out     = y;
    return true;
}

/*
 * fmInterpInit()
 *
 * Prepares the converter from the 8 kSa/s of EchoLink to the 24 kSa/s
 * transmit rate. The image filter is a fourth order 3.4 kHz low pass.
 *
 * Parameters:
 *   i - The converter.
 *
 * Returns: nothing.
 */
void fmInterpInit(FmInterp* i)
{
    memset(i, 0, sizeof(*i));
    fmBiquadLowPass(&i->lp[0], 3400.0f, 0.5412f);
    fmBiquadLowPass(&i->lp[1], 3400.0f, 1.3066f);
}

/*
 * fmInterpRun()
 *
 * Converts one 8 kSa/s sample to three 24 kSa/s samples: the sample followed
 * by two zeros goes through the low pass, with a gain of 3 to keep the level.
 *
 * Parameters:
 *   i   - The converter.
 *   x   - The 8 kSa/s sample.
 *   out - Receives FM_LINK_DECIM samples at 24 kSa/s.
 *
 * Returns: nothing.
 */
void fmInterpRun(FmInterp* i, float x, float out[FM_LINK_DECIM])
{
    for (int k = 0; k < FM_LINK_DECIM; k++)
    {
        float v = (k == 0) ? x * (float)FM_LINK_DECIM : 0.0f;

        out[k] = fmBiquadRun(&i->lp[1], fmBiquadRun(&i->lp[0], v));
    }
}

/*
 * fmDtmfDecide()
 *
 * Decides which DTMF digit, if any, a finished block of Goertzel results
 * holds. A digit needs the strongest low group tone and the strongest high
 * group tone to hold most of the block energy, to stand clearly above the
 * other tones of their group, and to be within the allowed twist.
 *
 * Parameters:
 *   p      - Power of the eight tones, 697 Hz to 1633 Hz.
 *   energy - Sum of the squares of the block samples.
 *
 * Returns: The digit ('0'-'9', '*', '#', 'A'-'D') or 0 if there is none.
 */
static char fmDtmfDecide(const float p[8], float energy)
{
    static const char DIGITS[4][4] = {
        {'1', '2', '3', 'A'}, {'4', '5', '6', 'B'}, {'7', '8', '9', 'C'}, {'*', '0', '#', 'D'}};
    int   lo = 0;
    int   hi = 4;
    float loSecond = 0.0f;
    float hiSecond = 0.0f;

    if (energy < 2500.0f * (float)FM_DTMF_N)
        return 0;

    for (int k = 1; k < 4; k++)
    {
        if (p[k] > p[lo])
            lo = k;
        if (p[4 + k] > p[hi])
            hi = 4 + k;
    }
    for (int k = 0; k < 4; k++)
    {
        if (k != lo && p[k] > loSecond)
            loSecond = p[k];
        if (4 + k != hi && p[4 + k] > hiSecond)
            hiSecond = p[4 + k];
    }

    float total = energy * (float)FM_DTMF_N / 2.0f;

    if (p[lo] + p[hi] < 0.7f * total)
        return 0;
    if (loSecond > 0.1f * p[lo] || hiSecond > 0.1f * p[hi])
        return 0;
    if (p[hi] > 8.0f * p[lo] || p[lo] > 8.0f * p[hi])
        return 0;

    return DIGITS[lo][hi - 4];
}

/*
 * fmDtmfInit()
 *
 * Clears the DTMF decoder.
 *
 * Parameters:
 *   d - The decoder.
 *
 * Returns: nothing.
 */
void fmDtmfInit(FmDtmf* d)
{
    memset(d, 0, sizeof(*d));
}

/*
 * fmDtmfPush()
 *
 * Feeds one 8 kSa/s sample to the DTMF decoder. A digit is reported once,
 * after two blocks in a row (about 51 ms) hold the same digit, and again
 * only after a block without that digit. The active flag is set from the
 * first block that holds a digit until a block without one, so the caller
 * can mute the tones.
 *
 * Parameters:
 *   d - The decoder.
 *   x - The 8 kSa/s sample.
 *
 * Returns: The digit when a new one has been detected, otherwise 0.
 */
char fmDtmfPush(FmDtmf* d, float x)
{
    static const float FREQ[8] = {697.0f, 770.0f, 852.0f, 941.0f, 1209.0f, 1336.0f, 1477.0f, 1633.0f};
    char result = 0;

    for (int k = 0; k < 8; k++)
    {
        float coeff = 2.0f * cosf(FM_TWO_PI * FREQ[k] / (float)FM_LINK_FS);
        float s0    = x + coeff * d->s1[k] - d->s2[k];

        d->s2[k] = d->s1[k];
        d->s1[k] = s0;
    }
    d->energy += x * x;

    if (++d->count < FM_DTMF_N)
        return 0;

    float p[8];

    for (int k = 0; k < 8; k++)
    {
        float coeff = 2.0f * cosf(FM_TWO_PI * FREQ[k] / (float)FM_LINK_FS);

        p[k]     = d->s1[k] * d->s1[k] + d->s2[k] * d->s2[k] - coeff * d->s1[k] * d->s2[k];
        d->s1[k] = 0.0f;
        d->s2[k] = 0.0f;
    }

    char digit = fmDtmfDecide(p, d->energy);

    d->energy = 0.0f;
    d->count  = 0;

    if (digit != 0 && digit == d->block)
    {
        if (d->run < 255)
            d->run++;
        if (d->run >= 2 && !d->reported)
        {
            d->reported = true;
            result      = digit;
        }
    }
    else
    {
        d->run = (digit != 0) ? 1 : 0;
        if (digit == 0 || digit != d->block)
            d->reported = false;
    }
    d->block  = digit;
    d->active = (digit != 0);
    return result;
}
