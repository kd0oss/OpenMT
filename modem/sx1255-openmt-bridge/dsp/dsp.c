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

#include "dsp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* No fixed SX1255 sample-rate constant here anymore: every rate-dependent
 * calculation in this file (decimation factor, resampler ratios, FM
 * demod/mod frequency scaling) is derived from the *actual* negotiated
 * ALSA rate passed into dsp_rx_pipeline_create()/dsp_tx_pipeline_create(),
 * not assumed to be exactly 125000 -- see the comments there for why. */

/*
 * design_lowpass_fir()
 *
 * Designs a windowed-sinc low-pass FIR filter with a Blackman window.  The
 * tap count is rounded up to a multiple of interpFactor and the taps are
 * scaled by interpFactor so an interpolator keeps its gain.
 *
 * Parameters:
 *   numTaps      - Requested number of taps.
 *   cutoffNorm   - Cutoff as a fraction of the Nyquist frequency.
 *   interpFactor - Interpolation factor the filter will be used with (1 for
 *                  none).
 *   out_len      - Receives the actual number of taps (numTaps rounded up
 *                  to a multiple of interpFactor).
 *
 * Returns: Newly allocated tap array (caller frees).
 */
static double *design_lowpass_fir(int numTaps, double cutoffNorm, int interpFactor, int *out_len)
{
    double *taps;
    double center, wc;
    int i;

    if (numTaps % interpFactor != 0)
    {
        numTaps += interpFactor - numTaps % interpFactor;
    }

    taps = malloc((size_t)numTaps * sizeof(double));
    center = (double)(numTaps - 1) / 2.0;
    wc = cutoffNorm * M_PI;

    for (i = 0; i < numTaps; i++)
    {
        double n = (double)i - center;
        double h, w;

        if (fabs(n) < 1e-10)
        {
            h = wc / M_PI;
        }
        else
        {
            h = sin(wc * n) / (M_PI * n);
        }
        w = 0.42 - 0.5 * cos(2 * M_PI * (double)i / (double)(numTaps - 1)) +
            0.08 * cos(4 * M_PI * (double)i / (double)(numTaps - 1));
        taps[i] = h * w * (double)interpFactor;
    }

    *out_len = numTaps;
    return taps;
}

/*
 * design_channel_filter()
 *
 * Designs the receive channel filter: a low-pass filter whose cutoff is the
 * channel bandwidth.
 *
 * Parameters:
 *   channelBWHz - Channel bandwidth in Hz, used as the cutoff.
 *   sampleRate  - Sample rate of the data to be filtered, in Hz.
 *   numTaps     - Requested number of taps.
 *   out_len     - Receives the actual number of taps.
 *
 * Returns: Newly allocated tap array (caller frees).
 */
static double *design_channel_filter(double channelBWHz, double sampleRate, int numTaps, int *out_len)
{
    return design_lowpass_fir(numTaps, channelBWHz / sampleRate, 1, out_len);
}

/* ---- Complex DC removal (EMA high-pass) --------------------------------*/
typedef struct
{
    double avgI, avgQ, alpha;
} dc_removal_c_t;

/*
 * dc_removal_c_process()
 *
 * Removes the DC offset from one complex sample using a slow running average
 * on each of I and Q.
 *
 * Parameters:
 *   f      - Filter state.
 *   sample - Input sample.
 *
 * Returns: The sample with the running average subtracted.
 */
static dsp_cplx_t dc_removal_c_process(dc_removal_c_t *f, dsp_cplx_t sample)
{
    double i = crealf(sample), q = cimagf(sample);

    f->avgI = f->alpha * f->avgI + (1.0 - f->alpha) * i;
    f->avgQ = f->alpha * f->avgQ + (1.0 - f->alpha) * q;
    return (float)(i - f->avgI) + (float)(q - f->avgQ) * I;
}

/* ---- FM demodulator ------------------------------------------------------*/
typedef struct
{
    dsp_cplx_t prev;
} fm_demod_t;

/*
 * fm_demod_process()
 *
 * FM-demodulates one complex sample: the phase difference between this
 * sample and the previous one.
 *
 * Parameters:
 *   f      - Demodulator state holding the previous sample.
 *   sample - Next complex sample.
 *
 * Returns: Phase change in radians (-pi to +pi).
 */
static double fm_demod_process(fm_demod_t *f, dsp_cplx_t sample)
{
    dsp_cplx_t product = sample * conjf(f->prev);

    f->prev = sample;
    return atan2((double)cimagf(product), (double)crealf(product));
}

/* ---- Real moving-average DC filter (software AFC) ------------------------*/
typedef struct
{
    double avgCnt, movingAvg;
} dc_filter_r_t;

/*
 * dc_filter_r_process()
 *
 * Removes the DC offset from one real sample using a moving average.
 *
 * Parameters:
 *   f      - Filter state.
 *   sample - Input sample.
 *
 * Returns: The sample minus the current average.
 */
static double dc_filter_r_process(dc_filter_r_t *f, double sample)
{
    double ret = sample - f->movingAvg;

    f->movingAvg = (f->movingAvg * (f->avgCnt - 1.0) + sample) / f->avgCnt;
    return ret;
}

/* ---- Polyphase decimating FIR (complex) ---------------------------------*/
typedef struct
{
    int decimFactor;
    int tapsPerPhase;
    double **phases; /* [decimFactor][tapsPerPhase] */
    dsp_cplx_t *buffer;
    int bufLen;
    int bufIdx;
    int count;
} poly_decim_c_t;

/*
 * poly_decim_c_init()
 *
 * Sets up a polyphase decimating FIR filter: splits the taps into
 * decimFactor phases (padding the taps with zeros to a multiple of
 * decimFactor) and allocates the sample history.
 *
 * Parameters:
 *   d           - Decimator to initialise.
 *   taps        - Filter taps.
 *   taps_len    - Number of taps.
 *   decimFactor - Decimation factor.
 *
 * Returns: nothing.
 */
static void poly_decim_c_init(poly_decim_c_t *d, const double *taps, int taps_len, int decimFactor)
{
    int padded_len = taps_len;
    double *padded;
    int p, t;

    if (padded_len % decimFactor != 0)
    {
        padded_len += decimFactor - padded_len % decimFactor;
    }
    padded = calloc((size_t)padded_len, sizeof(double));
    memcpy(padded, taps, (size_t)taps_len * sizeof(double));

    d->decimFactor = decimFactor;
    d->tapsPerPhase = padded_len / decimFactor;
    d->phases = malloc((size_t)decimFactor * sizeof(double *));
    for (p = 0; p < decimFactor; p++)
    {
        d->phases[p] = malloc((size_t)d->tapsPerPhase * sizeof(double));
        for (t = 0; t < d->tapsPerPhase; t++)
        {
            d->phases[p][t] = padded[t * decimFactor + p];
        }
    }
    free(padded);

    d->bufLen = padded_len;
    d->buffer = calloc((size_t)d->bufLen, sizeof(dsp_cplx_t));
    d->bufIdx = 0;
    d->count = 0;
}

/*
 * poly_decim_c_free()
 *
 * Frees the memory held by a decimator.
 *
 * Parameters:
 *   d - Decimator to free.
 *
 * Returns: nothing.
 */
static void poly_decim_c_free(poly_decim_c_t *d)
{
    int p;

    for (p = 0; p < d->decimFactor; p++)
    {
        free(d->phases[p]);
    }
    free(d->phases);
    free(d->buffer);
}

/*
 * poly_decim_c_process()
 *
 * Feeds one complex sample into the decimator.  Every decimFactor-th input
 * produces one filtered output.
 *
 * Parameters:
 *   d      - Decimator state.
 *   sample - Next input sample.
 *   out    - Receives the output sample when one is produced.
 *
 * Returns: true if an output sample was produced, false otherwise.
 */
static bool poly_decim_c_process(poly_decim_c_t *d, dsp_cplx_t sample, dsp_cplx_t *out)
{
    int M = d->decimFactor;
    int p, t;
    dsp_cplx_t acc = 0.0f;

    d->buffer[d->bufIdx] = sample;
    d->bufIdx = (d->bufIdx + 1) % d->bufLen;
    d->count++;

    if (d->count % M != 0)
    {
        return false;
    }

    for (p = 0; p < M; p++)
    {
        int startIdx = d->bufIdx - M + p;

        if (startIdx < 0)
        {
            startIdx += d->bufLen;
        }
        for (t = 0; t < d->tapsPerPhase; t++)
        {
            int idx = startIdx - t * M;

            while (idx < 0)
            {
                idx += d->bufLen;
            }
            acc += d->buffer[idx] * (float)d->phases[p][t];
        }
    }

    *out = acc;
    return true;
}

/* ---- Polyphase rational resampler (real), used for both the RX
 * 12.5k->24k stage (streaming) and the TX 24k->125k stage (batch). -------*/
typedef struct
{
    int interpFactor;
    int decimFactor;
    int tapsPerPhase;
    double **phases; /* [interpFactor][tapsPerPhase] */
    double *buffer;
    int bufIdx;
    int outPhase;
} poly_resampler_t;

/*
 * poly_resampler_init()
 *
 * Sets up a polyphase rational resampler (interpolate by interpFactor,
 * decimate by decimFactor).  The low-pass filter is designed for the larger
 * of the two factors with 24 taps per phase.
 *
 * Parameters:
 *   r            - Resampler to initialise.
 *   interpFactor - Interpolation factor.
 *   decimFactor  - Decimation factor.
 *
 * Returns: nothing.
 */
static void poly_resampler_init(poly_resampler_t *r, int interpFactor, int decimFactor)
{
    int max_id = interpFactor > decimFactor ? interpFactor : decimFactor;
    double cutoffNorm = 1.0 / (double)max_id;
    int design_taps = interpFactor * 24;
    int taps_len;
    double *taps = design_lowpass_fir(design_taps, cutoffNorm, interpFactor, &taps_len);
    int p, t;

    r->interpFactor = interpFactor;
    r->decimFactor = decimFactor;
    r->tapsPerPhase = taps_len / interpFactor;
    r->phases = malloc((size_t)interpFactor * sizeof(double *));
    for (p = 0; p < interpFactor; p++)
    {
        r->phases[p] = malloc((size_t)r->tapsPerPhase * sizeof(double));
        for (t = 0; t < r->tapsPerPhase; t++)
        {
            r->phases[p][t] = taps[t * interpFactor + p];
        }
    }
    free(taps);

    r->buffer = calloc((size_t)r->tapsPerPhase, sizeof(double));
    r->bufIdx = 0;
    r->outPhase = 0;
}

/*
 * poly_resampler_free()
 *
 * Frees the memory held by a resampler.
 *
 * Parameters:
 *   r - Resampler to free.
 *
 * Returns: nothing.
 */
static void poly_resampler_free(poly_resampler_t *r)
{
    int p;

    for (p = 0; p < r->interpFactor; p++)
    {
        free(r->phases[p]);
    }
    free(r->phases);
    free(r->buffer);
}

/*
 * poly_resampler_reset()
 *
 * Clears the resampler history and output phase so the next stream starts
 * clean.
 *
 * Parameters:
 *   r - Resampler to reset.
 *
 * Returns: nothing.
 */
static void poly_resampler_reset(poly_resampler_t *r)
{
    memset(r->buffer, 0, (size_t)r->tapsPerPhase * sizeof(double));
    r->bufIdx = 0;
    r->outPhase = 0;
}

/*
 * poly_resampler_process()
 *
 * Feeds one real sample into the resampler and produces the output samples
 * that fall due.  If more outputs fall due than max_out allows, the extra
 * ones are dropped and a message is logged.
 *
 * Parameters:
 *   r       - Resampler state.
 *   sample  - Next input sample.
 *   out     - Receives the output samples.
 *   max_out - Size of out in samples.
 *
 * Returns: Number of output samples produced (counts dropped ones too).
 */
static int poly_resampler_process(poly_resampler_t *r, double sample, double *out, int max_out)
{
    int n = 0;
    int len = r->tapsPerPhase;

    r->buffer[r->bufIdx] = sample;
    r->bufIdx = (r->bufIdx + 1) % len;

    while (r->outPhase < r->interpFactor)
    {
        int phase = r->outPhase;
        double acc = 0.0;
        int idx = r->bufIdx;
        int t;

        for (t = 0; t < len; t++)
        {
            idx--;
            if (idx < 0)
            {
                idx = len - 1;
            }
            acc += r->buffer[idx] * r->phases[phase][t];
        }
        if (n < max_out)
        {
            out[n] = acc;
        }
        else
        {
            fprintf(stderr, "dsp: poly_resampler_process: output buffer (%d) too small for "
                "this rate's interp/decim ratio -- dropping samples, audio will be "
                "corrupted. Increase the caller's buffer.\n", max_out);
        }
        n++;
        r->outPhase += r->decimFactor;
    }
    r->outPhase -= r->interpFactor;

    return n < max_out ? n : max_out;
}

/* ---- RX pipeline ----------------------------------------------------------*/
/*
 * gcd_ll()
 *
 * Calculates the greatest common divisor, used to reduce a sample rate ratio
 * to lowest terms.
 *
 * Parameters:
 *   a - First value.
 *   b - Second value.
 *
 * Returns: The greatest common divisor, never negative.
 */
static long gcd_ll(long a, long b)
{
    while (b != 0)
    {
        long t = b;

        b = a % b;
        a = t;
    }
    return a < 0 ? -a : a;
}

struct dsp_rx_pipeline
{
    dc_removal_c_t dcr;
    poly_decim_c_t decim;
    fm_demod_t fmdemod;
    bool use_baseband_dc;
    dc_filter_r_t basebandDC;
    poly_resampler_t resampler;
    float scalingCoeff;
    double demod_gain_correction; /* see dsp_rx_pipeline_create_tuned() */
};

/*
 * Real hardware applies its own analog gain on top of this via the
 * SX1255's LNA/PGA registers (--lna-gain/--pga-gain), same as any radio's
 * front end -- that's the first knob to reach for if your receive symbol
 * amplitude sits far from the expected ~1-3 range. This constant is the
 * second one, via dsp_rx_pipeline_create_tuned().
 */
#define DSP_DEFAULT_SCALING_COEFF 1.24f

/*
 * dsp_rx_pipeline_create()
 *
 * Creates the receive pipeline with the default DC averaging count (2000)
 * and the default scaling coefficient.
 *
 * Parameters:
 *   actual_sample_rate_hz - Real sample rate of the I/Q input in Hz; 0 means
 *                           assume 125000.
 *
 * Returns: New pipeline, to be freed with dsp_rx_pipeline_destroy().
 */
dsp_rx_pipeline_t *dsp_rx_pipeline_create(unsigned int actual_sample_rate_hz)
{
    return dsp_rx_pipeline_create_tuned(actual_sample_rate_hz, 2000, DSP_DEFAULT_SCALING_COEFF);
}

/*
 * dsp_rx_pipeline_create_tuned()
 *
 * Creates the receive pipeline: DC removal, decimation with channel filter
 * to about 12.5 kHz, FM demodulation, optional baseband DC removal and
 * resampling to 24 kHz.  The decimation factor, demodulator gain correction
 * and resampler ratio are derived from the actual input rate.
 *
 * Parameters:
 *   actual_sample_rate_hz - Real sample rate of the I/Q input in Hz; 0 means
 *                           assume 125000.
 *   baseband_dc_avg_cnt   - Length of the baseband DC moving average; 1 or
 *                           less turns baseband DC removal off.
 *   scaling_coeff         - Gain applied to the final audio.
 *
 * Returns: New pipeline, to be freed with dsp_rx_pipeline_destroy().
 */
dsp_rx_pipeline_t *dsp_rx_pipeline_create_tuned(unsigned int actual_sample_rate_hz, int baseband_dc_avg_cnt, float scaling_coeff)
{
    dsp_rx_pipeline_t *p = calloc(1, sizeof(*p));
    int taps_len;
    double *channelTaps;
    double actual_rate = (double)actual_sample_rate_hz;
    /* Nominal design targets, matching the reference's hardcoded
     * assumption of an exact 125000 Hz input: a 12500 Hz intermediate
     * (post-decimation) rate, and a 24000 Hz baseband rate feeding the
     * RRC matched filter (5 samples/symbol at 4800 baud). Both are kept
     * as fixed *targets*; what varies with the actual hardware rate is
     * how we get there (decimation factor, resampler ratio, and a small
     * demod-gain correction), not these targets themselves. */
    const double nominal_intermediate_rate = 12500.0;
    const double target_baseband_rate = 24000.0;
    int decimFactor;
    double actual_intermediate_rate;
    long resampler_interp, resampler_decim, g;

    if (actual_rate <= 0.0)
    {
        actual_rate = 125000.0; /* caller couldn't determine the real rate; fall
                                  * back to the nominal assumption rather than
                                  * divide by zero below. */
    }

    p->dcr.alpha = 0.9999;

    /* Decimation factor: nearest integer that lands closest to the
     * nominal 12500 Hz intermediate rate for whatever the actual input
     * rate is (e.g. 128000/10 = 12800, still decimFactor=10 -- the
     * *ratio* barely changes, but see demod_gain_correction below for why
     * even a small residual mismatch matters). */
    decimFactor = (int)lround(actual_rate / nominal_intermediate_rate);
    if (decimFactor < 1)
    {
        decimFactor = 1;
    }
    actual_intermediate_rate = actual_rate / (double)decimFactor;

    channelTaps = design_channel_filter(12500.0, actual_rate, 200, &taps_len);
    poly_decim_c_init(&p->decim, channelTaps, taps_len, decimFactor);
    free(channelTaps);

    p->fmdemod.prev = 1.0f + 0.0f * I;

    /* The FM demodulator's raw output (radians of phase difference
     * between consecutive *decimated-rate* samples) is inversely
     * proportional to that decimated rate for a given physical frequency
     * deviation: fewer samples per second means more phase accumulates
     * between them, i.e. raw_output(rate) = 2*pi*f_dev/rate. scalingCoeff
     * =1.24 was validated against the nominal 12500 Hz intermediate rate;
     * if the actual rate is higher (e.g. 12800 Hz from a 128000 Hz true
     * input), the raw output comes out *smaller* than what that
     * calibration expects by a factor of nominal/actual, so it needs to
     * be scaled back *up* by the inverse (actual/nominal) to match. */
    p->demod_gain_correction = actual_intermediate_rate / nominal_intermediate_rate;

    p->use_baseband_dc = (baseband_dc_avg_cnt > 1);
    if (p->use_baseband_dc)
    {
        p->basebandDC.avgCnt = (double)baseband_dc_avg_cnt;
    }

    /* Resampler ratio from whatever the actual intermediate rate is to
     * the fixed 24000 Hz target, reduced to lowest terms rather than
     * assuming the textbook 48:25 (which is only exactly 12500:24000 in
     * lowest terms when the input truly is 12500 Hz). */
    g = gcd_ll(lround(target_baseband_rate), lround(actual_intermediate_rate));
    resampler_interp = lround(target_baseband_rate) / g;
    resampler_decim = lround(actual_intermediate_rate) / g;
    poly_resampler_init(&p->resampler, (int)resampler_interp, (int)resampler_decim);

    p->scalingCoeff = scaling_coeff;

    fprintf(stderr, "dsp: RX pipeline configured for actual_rate=%.0f Hz: decimFactor=%d, "
        "intermediate_rate=%.2f Hz,\n", actual_rate, decimFactor, actual_intermediate_rate);
    fprintf(stderr, "dsp: demod_gain_correction=%.4f, resampler=%ld:%ld, scalingCoeff=%.4f\n",
        p->demod_gain_correction, resampler_interp, resampler_decim, (double)p->scalingCoeff);

    return p;
}

/*
 * dsp_rx_pipeline_destroy()
 *
 * Frees a receive pipeline.
 *
 * Parameters:
 *   p - Pipeline, may be NULL.
 *
 * Returns: nothing.
 */
void dsp_rx_pipeline_destroy(dsp_rx_pipeline_t *p)
{
    if (p == NULL)
    {
        return;
    }
    poly_decim_c_free(&p->decim);
    poly_resampler_free(&p->resampler);
    free(p);
}

/*
 * dsp_rx_pipeline_get_dc_bias()
 *
 * Gets the DC offset the baseband filter has measured.
 *
 * Parameters:
 *   p - Pipeline, may be NULL.
 *
 * Returns: Current average, or 0 if p is NULL or baseband DC removal is off.
 */
double dsp_rx_pipeline_get_dc_bias(const dsp_rx_pipeline_t *p)
{
    if (p == NULL || !p->use_baseband_dc)
    {
        return 0.0;
    }
    return p->basebandDC.movingAvg;
}

/*
 * dsp_rx_pipeline_process_baseband()
 *
 * Runs one I/Q sample through the receive pipeline.  Most calls produce
 * nothing because of the decimation; some produce one or two 24 kHz audio
 * samples.
 *
 * Parameters:
 *   p         - Pipeline.
 *   iq_sample - Next I/Q sample from the SX1255.
 *   out       - Receives the audio samples.
 *   max_out   - Size of out in samples.
 *
 * Returns: Number of audio samples written to out.
 */
int dsp_rx_pipeline_process_baseband(dsp_rx_pipeline_t *p, dsp_cplx_t iq_sample, float *out, int max_out)
{
    dsp_cplx_t dc_out, decim_out;
    double demod, baseband, resamp_out[32];
    int n_resamp, i, n_out = 0;

    dc_out = dc_removal_c_process(&p->dcr, iq_sample);

    if (!poly_decim_c_process(&p->decim, dc_out, &decim_out))
    {
        return 0;
    }

    demod = fm_demod_process(&p->fmdemod, decim_out);
    demod *= p->demod_gain_correction;

    baseband = p->use_baseband_dc ? dc_filter_r_process(&p->basebandDC, demod) : demod;

    n_resamp = poly_resampler_process(&p->resampler, baseband, resamp_out, 32);
    for (i = 0; i < n_resamp && n_out < max_out; i++)
    {
        out[n_out++] = (float)(resamp_out[i] * p->scalingCoeff);
    }

    return n_out;
}

/* ---- TX pipeline ----------------------------------------------------------*/
struct dsp_tx_pipeline
{
    poly_resampler_t resampler;
    double phase; /* FM modulator phase accumulator */
    double actual_sample_rate_hz;
    /* Pre-allocated scratch buffer for the resampler's output, sized
     * generously up front and reused across every call rather than
     * malloc'd/freed per frame. A real-time (SCHED_FIFO) RX thread and
     * a normal-priority TX thread both allocating frequently through
     * the same glibc malloc arena is a textbook priority-inversion
     * setup: the high-priority thread can block on malloc's internal
     * lock for as long as the low-priority thread holds it, for an
     * unpredictable duration -- entirely independent of ALSA, I2S, or
     * DMA, and invisible to every one of those layers' own
     * diagnostics. */
    double *resampled_buf;
    int resampled_cap;
};

#define DSP_TX_INITIAL_SAMPLE_CAP 2048 /* generous initial sizing hint for
                                          * the resampler output buffer
                                          * above -- comfortably larger
                                          * than any typical per-call
                                          * batch size, and grows
                                          * automatically (see
                                          * dsp_tx_pipeline_process_
                                          * baseband()) if a caller ever
                                          * passes more samples in one
                                          * call than this. */

/*
 * dsp_tx_pipeline_create()
 *
 * Creates the transmit pipeline: a resampler from 24 kHz baseband audio to
 * the actual I/Q rate and the output buffer for it.
 *
 * Parameters:
 *   actual_sample_rate_hz - Real sample rate of the I/Q output in Hz; 0
 *                           means assume 125000.
 *
 * Returns: New pipeline, to be freed with dsp_tx_pipeline_destroy().
 */
dsp_tx_pipeline_t *dsp_tx_pipeline_create(unsigned int actual_sample_rate_hz)
{
    dsp_tx_pipeline_t *p = calloc(1, sizeof(*p));
    const double baseband_rate = 24000.0; /* fixed: 5 samples/symbol at 4800 baud,
                                            * same target dsp_rx_pipeline_create_tuned()
                                            * resamples down to on the RX side. */
    double actual_rate = (double)actual_sample_rate_hz;
    long resampler_interp, resampler_decim, g;

    if (actual_rate <= 0.0)
    {
        actual_rate = 125000.0; /* couldn't determine the real rate; fall back to
                                  * the nominal assumption rather than divide by
                                  * zero below. */
    }
    p->actual_sample_rate_hz = actual_rate;

    /* Resampler ratio from the fixed 24000 Hz baseband rate to whatever
     * the actual playback rate is, reduced to lowest terms -- see the
     * matching comment in dsp_rx_pipeline_create_tuned(). */
    g = gcd_ll(lround(actual_rate), lround(baseband_rate));
    resampler_interp = lround(actual_rate) / g;
    resampler_decim = lround(baseband_rate) / g;
    poly_resampler_init(&p->resampler, (int)resampler_interp, (int)resampler_decim);

    p->resampled_cap = DSP_TX_INITIAL_SAMPLE_CAP * resampler_interp / resampler_decim +
        (int)resampler_interp + 64;
    p->resampled_buf = malloc((size_t)p->resampled_cap * sizeof(double));

    fprintf(stderr, "dsp: TX pipeline configured for actual_rate=%.0f Hz: resampler=%ld:%ld\n",
        actual_rate, resampler_interp, resampler_decim);

    return p;
}

/*
 * dsp_tx_pipeline_destroy()
 *
 * Frees a transmit pipeline.
 *
 * Parameters:
 *   p - Pipeline, may be NULL.
 *
 * Returns: nothing.
 */
void dsp_tx_pipeline_destroy(dsp_tx_pipeline_t *p)
{
    if (p == NULL)
    {
        return;
    }
    poly_resampler_free(&p->resampler);
    free(p->resampled_buf);
    free(p);
}

/*
 * dsp_tx_pipeline_reset()
 *
 * Clears the resampler history and the FM phase so a new transmission starts
 * clean.
 *
 * Parameters:
 *   p - Pipeline.
 *
 * Returns: nothing.
 */
void dsp_tx_pipeline_reset(dsp_tx_pipeline_t *p)
{
    poly_resampler_reset(&p->resampler);
    p->phase = 0.0;
}

/*
 * dsp_tx_pipeline_process_baseband()
 *
 * Turns 24 kHz baseband samples into FM-modulated I/Q at the actual sample
 * rate: resample, integrate to a phase scaled by the deviation, and output
 * cos and sin of the phase.  The output buffer is grown if needed.
 *
 * Parameters:
 *   p                - Pipeline.
 *   baseband_samples - Input audio samples.
 *   n_samples        - Number of input samples.
 *   deviation_hz     - FM deviation, in Hz, for an input value of 1.0.
 *   out              - Pointer to the caller's output buffer; reallocated if
 *                      too small.
 *   out_cap          - Capacity of *out in samples; updated if the buffer
 *                      grows.
 *
 * Returns: Number of I/Q samples written to *out.
 */
int dsp_tx_pipeline_process_baseband(dsp_tx_pipeline_t *p, const float *baseband_samples,
    int n_samples, double deviation_hz, dsp_cplx_t **out, int *out_cap)
{
    int est_resampled = n_samples * p->resampler.interpFactor / p->resampler.decimFactor + p->resampler.interpFactor + 8;
    double *resampled;
    int n_resampled = 0, i;
    double phaseScale = 2.0 * M_PI * deviation_hz / p->actual_sample_rate_hz;

    /* Pre-allocated scratch buffer for the resampler's own output --
     * see the comment on dsp_tx_pipeline_t in this file for why
     * avoiding hot-path allocation matters here. */
    if (est_resampled > p->resampled_cap)
    {
        fprintf(stderr, "dsp: TX pipeline (baseband) resampled buffer (%d) too small for this "
            "call (needs ~%d), growing (this should be rare)\n", p->resampled_cap, est_resampled);
        p->resampled_cap = est_resampled;
        p->resampled_buf = realloc(p->resampled_buf, (size_t)p->resampled_cap * sizeof(double));
    }
    resampled = p->resampled_buf;

    for (i = 0; i < n_samples; i++)
    {
        double batch_out[32];
        int n = poly_resampler_process(&p->resampler, (double)baseband_samples[i], batch_out, 32);
        int j;

        for (j = 0; j < n && n_resampled < est_resampled; j++)
        {
            resampled[n_resampled++] = batch_out[j];
        }
    }

    if (*out_cap < n_resampled)
    {
        *out = realloc(*out, (size_t)n_resampled * sizeof(dsp_cplx_t));
        *out_cap = n_resampled;
    }

    for (i = 0; i < n_resampled; i++)
    {
        p->phase += resampled[i] * phaseScale;
        if (p->phase > M_PI)
        {
            p->phase -= 2 * M_PI;
        }
        else if (p->phase < -M_PI)
        {
            p->phase += 2 * M_PI;
        }
        (*out)[i] = (float)cos(p->phase) + (float)sin(p->phase) * I;
    }

    return n_resampled;
}
