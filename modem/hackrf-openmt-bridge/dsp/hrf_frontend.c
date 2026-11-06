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
/*
 * hrf_frontend.c - see hrf_frontend.h.
 *
 * Filter design (all windowed-sinc, Blackman, unity DC gain):
 *
 *   stage A  fs -> 500 kS/s   decimate by M = N/4, 8*M taps,
 *                             cutoff 240 kHz
 *   stage B  500k -> 125 kS/s decimate by 4, 48 taps, cutoff 62.5 kHz
 *
 * What the filters have to do is keep everything that aliases into the
 * wanted 125 kS/s channel (the DSP chain's own 12.5 kHz channel filter
 * follows) out of it: stage A leaves nothing from 475 kHz upwards, stage B
 * nothing from about 90 kHz, i.e. aliases of +-25 kHz around the wanted
 * signal are down by 70 dB or more.  The TX interpolators are the same
 * filters run backwards, so the images of the 125 kS/s signal are
 * suppressed to the same degree.
 *
 * Data inside is interleaved float (re, im), real filter taps.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hrf_frontend.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MID_RATE   500000U
#define BLOCK      8192U            /* input samples handled per inner pass */
#define STAGE_B_M  4U
#define STAGE_B_T  48U

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static unsigned int
gcd_u(unsigned int a, unsigned int b)
{
    while (b != 0U)
    {
        unsigned int t = b;

        b = a % b;
        a = t;
    }
    return a;
}

bool
hrf_fe_rate_ok(uint32_t fs)
{
    return fs >= 1000000U && fs <= 8000000U && (fs % MID_RATE) == 0U;
}

unsigned int
hrf_fe_ratio(uint32_t fs)
{
    return fs / HRF_FE_BASEBAND_RATE;
}

/* Windowed-sinc low pass.  cutoff_norm is the cutoff as a fraction of the
 * rate the filter runs at (0..0.5).  Taps sum to 'gain'. */
static float *
design_lp(unsigned int taps, double cutoff_norm, double gain)
{
    float *h = malloc(taps * sizeof(float));
    double *d = malloc(taps * sizeof(double));
    double center = (double)(taps - 1U) / 2.0, sum = 0.0;
    unsigned int i;

    for (i = 0; i < taps; i++)
    {
        double n = (double)i - center;
        double s = (fabs(n) < 1e-9) ? 2.0 * cutoff_norm : sin(2.0 * M_PI * cutoff_norm * n) / (M_PI * n);
        double w = 0.42 - 0.5 * cos(2.0 * M_PI * (double)i / (double)(taps - 1U)) +
            0.08 * cos(4.0 * M_PI * (double)i / (double)(taps - 1U));

        d[i] = s * w;
        sum += d[i];
    }
    for (i = 0; i < taps; i++)
    {
        h[i] = (float)(d[i] / sum * gain);
    }
    free(d);
    return h;
}

/* ---- decimating FIR ---------------------------------------------------*/
typedef struct
{
    unsigned int m, t;
    float       *h;
    float       *z;        /* (t - 1 + BLOCK) complex samples */
    unsigned int phase;    /* inputs to skip before the next output */
} decim_t;

static void
decim_init(decim_t *d, unsigned int m, unsigned int taps, double cutoff_norm)
{
    d->m     = m;
    d->t     = taps;
    d->h     = design_lp(taps, cutoff_norm, 1.0);
    d->z     = calloc((size_t)(taps - 1U + BLOCK) * 2U, sizeof(float));
    d->phase = 0U;
}

static void
decim_free(decim_t *d)
{
    free(d->h);
    free(d->z);
}

static void
decim_reset(decim_t *d)
{
    memset(d->z, 0, (size_t)(d->t - 1U + BLOCK) * 2U * sizeof(float));
    d->phase = 0U;
}

/* x: n complex (n <= BLOCK).  Returns outputs written to y. */
static size_t
decim_run(decim_t *d, const float *x, size_t n, float *y)
{
    unsigned int t = d->t, m = d->m;
    size_t p, count = 0U;
    unsigned int k;

    memcpy(d->z + (size_t)(t - 1U) * 2U, x, n * 2U * sizeof(float));
    for (p = d->phase; p < n; p += m)
    {
        const float *zp = d->z + (p + t - 1U) * 2U;
        float re = 0.0f, im = 0.0f;

        for (k = 0U; k < t; k++)
        {
            re += d->h[k] * zp[-(ptrdiff_t)k * 2];
            im += d->h[k] * zp[-(ptrdiff_t)k * 2 + 1];
        }
        y[count * 2U]      = re;
        y[count * 2U + 1U] = im;
        count++;
    }
    d->phase = (unsigned int)(p - n);
    memmove(d->z, d->z + n * 2U, (size_t)(t - 1U) * 2U * sizeof(float));
    return count;
}

/* ---- interpolating FIR ------------------------------------------------*/
typedef struct
{
    unsigned int l, q;      /* interpolation factor, taps per phase */
    float       *h;         /* l*q taps */
    float       *z;         /* (q - 1 + BLOCK) complex samples */
} interp_t;

static void
interp_init(interp_t *u, unsigned int l, unsigned int taps, double cutoff_norm)
{
    u->l = l;
    u->q = taps / l;
    u->h = design_lp(u->l * u->q, cutoff_norm, (double)l);
    u->z = calloc((size_t)(u->q - 1U + BLOCK) * 2U, sizeof(float));
}

static void
interp_free(interp_t *u)
{
    free(u->h);
    free(u->z);
}

static void
interp_reset(interp_t *u)
{
    memset(u->z, 0, (size_t)(u->q - 1U + BLOCK) * 2U * sizeof(float));
}

/* x: n complex (n <= BLOCK) -> y: n*l complex */
static void
interp_run(interp_t *u, const float *x, size_t n, float *y)
{
    unsigned int l = u->l, q = u->q, ph, k;
    size_t i;

    memcpy(u->z + (size_t)(q - 1U) * 2U, x, n * 2U * sizeof(float));
    for (i = 0U; i < n; i++)
    {
        const float *zp = u->z + (i + q - 1U) * 2U;

        for (ph = 0U; ph < l; ph++)
        {
            float re = 0.0f, im = 0.0f;

            for (k = 0U; k < q; k++)
            {
                float c = u->h[k * l + ph];

                re += c * zp[-(ptrdiff_t)k * 2];
                im += c * zp[-(ptrdiff_t)k * 2 + 1];
            }
            y[(i * l + ph) * 2U]      = re;
            y[(i * l + ph) * 2U + 1U] = im;
        }
    }
    memmove(u->z, u->z + n * 2U, (size_t)(q - 1U) * 2U * sizeof(float));
}

/* ---- frequency shifter ------------------------------------------------*/
typedef struct
{
    unsigned int n;         /* table length (0: no shift) */
    unsigned int step;
    unsigned int idx;
    float       *cs;        /* n * (cos, sin) */
} nco_t;

static bool
nco_init(nco_t *o, uint32_t fs, int32_t offset_hz)
{
    unsigned int g, i;
    int64_t step;

    memset(o, 0, sizeof(*o));
    if (offset_hz == 0)
    {
        return true;
    }
    g = gcd_u(fs, (unsigned int)abs(offset_hz));
    o->n = fs / g;
    if (o->n > 65536U)
    {
        return false;
    }
    step = (int64_t)(offset_hz / (int)g) % (int64_t)o->n;
    if (step < 0)
    {
        step += o->n;
    }
    o->step = (unsigned int)step;
    o->cs   = malloc((size_t)o->n * 2U * sizeof(float));
    for (i = 0U; i < o->n; i++)
    {
        o->cs[i * 2U]      = (float)cos(2.0 * M_PI * (double)i / (double)o->n);
        o->cs[i * 2U + 1U] = (float)sin(2.0 * M_PI * (double)i / (double)o->n);
    }
    return true;
}

/* ---- RX ---------------------------------------------------------------*/
struct hrf_rx_fe
{
    unsigned int m_a;
    nco_t        nco;
    decim_t      a, b;
    float       *x;      /* BLOCK complex */
    float       *mid;    /* BLOCK/m_a complex + slack */
};

hrf_rx_fe_t *
hrf_rx_fe_create(uint32_t fs, int32_t offset_hz)
{
    hrf_rx_fe_t *fe;

    if (!hrf_fe_rate_ok(fs))
    {
        return NULL;
    }
    fe = calloc(1, sizeof(*fe));
    fe->m_a = hrf_fe_ratio(fs) / STAGE_B_M;
    if (!nco_init(&fe->nco, fs, offset_hz))
    {
        free(fe);
        return NULL;
    }
    decim_init(&fe->a, fe->m_a, 8U * fe->m_a < 16U ? 16U : 8U * fe->m_a, 240000.0 / (double)fs);
    decim_init(&fe->b, STAGE_B_M, STAGE_B_T, 62500.0 / (double)MID_RATE);
    fe->x   = malloc((size_t)BLOCK * 2U * sizeof(float));
    fe->mid = malloc((size_t)(BLOCK / fe->m_a + 2U) * 2U * sizeof(float));
    return fe;
}

void
hrf_rx_fe_destroy(hrf_rx_fe_t *fe)
{
    if (fe == NULL)
    {
        return;
    }
    decim_free(&fe->a);
    decim_free(&fe->b);
    free(fe->nco.cs);
    free(fe->x);
    free(fe->mid);
    free(fe);
}

void
hrf_rx_fe_reset(hrf_rx_fe_t *fe)
{
    decim_reset(&fe->a);
    decim_reset(&fe->b);
    fe->nco.idx = 0U;
}

size_t
hrf_rx_fe_process(hrf_rx_fe_t *fe, const int8_t *iq, size_t n, float complex *out, size_t out_cap)
{
    size_t total = 0U;
    float *o = (float *)out;

    while (n > 0U)
    {
        size_t blk = n < BLOCK ? n : BLOCK, i, nm, no;
        float yb[(BLOCK / 4U + 2U) * 2U];

        /* int8 -> float, shifted down by the offset */
        if (fe->nco.n == 0U)
        {
            for (i = 0U; i < blk * 2U; i++)
            {
                fe->x[i] = (float)iq[i] * (1.0f / 128.0f);
            }
        }
        else
        {
            unsigned int idx = fe->nco.idx;

            for (i = 0U; i < blk; i++)
            {
                float re = (float)iq[i * 2U] * (1.0f / 128.0f);
                float im = (float)iq[i * 2U + 1U] * (1.0f / 128.0f);
                float c = fe->nco.cs[idx * 2U], s = fe->nco.cs[idx * 2U + 1U];

                /* multiply by e^{-j theta} = c - j s */
                fe->x[i * 2U]      = re * c + im * s;
                fe->x[i * 2U + 1U] = im * c - re * s;
                idx += fe->nco.step;
                if (idx >= fe->nco.n)
                {
                    idx -= fe->nco.n;
                }
            }
            fe->nco.idx = idx;
        }
        nm = decim_run(&fe->a, fe->x, blk, fe->mid);
        no = decim_run(&fe->b, fe->mid, nm, yb);
        if (total + no > out_cap)
        {
            no = out_cap - total;
        }
        memcpy(o + total * 2U, yb, no * 2U * sizeof(float));
        total += no;
        iq += blk * 2U;
        n  -= blk;
    }
    return total;
}

/* ---- TX ---------------------------------------------------------------*/
struct hrf_tx_fe
{
    unsigned int l_a;
    float        scale;
    nco_t        nco;
    interp_t     b, a;
    float       *mid;   /* 4 * in */
    float       *hi;    /* n * ratio */
};

#define TX_BLOCK (BLOCK / 16U)       /* baseband samples per pass */

hrf_tx_fe_t *
hrf_tx_fe_create(uint32_t fs, int32_t offset_hz, float amplitude)
{
    hrf_tx_fe_t *fe;

    if (!hrf_fe_rate_ok(fs))
    {
        return NULL;
    }
    fe = calloc(1, sizeof(*fe));
    fe->l_a = hrf_fe_ratio(fs) / STAGE_B_M;
    if (!nco_init(&fe->nco, fs, offset_hz))
    {
        free(fe);
        return NULL;
    }
    fe->scale = amplitude * 127.0f;
    /* 125k -> 500k (x4), then 500k -> fs (x l_a); cutoffs are relative to
     * the rate each filter runs at (its output rate) */
    interp_init(&fe->b, STAGE_B_M, STAGE_B_T, 62500.0 / (double)MID_RATE);
    interp_init(&fe->a, fe->l_a, 8U * fe->l_a < 16U ? 16U : 8U * fe->l_a, 240000.0 / (double)fs);
    fe->mid = malloc((size_t)TX_BLOCK * STAGE_B_M * 2U * sizeof(float));
    fe->hi  = malloc((size_t)TX_BLOCK * STAGE_B_M * fe->l_a * 2U * sizeof(float));
    return fe;
}

void
hrf_tx_fe_destroy(hrf_tx_fe_t *fe)
{
    if (fe == NULL)
    {
        return;
    }
    interp_free(&fe->a);
    interp_free(&fe->b);
    free(fe->nco.cs);
    free(fe->mid);
    free(fe->hi);
    free(fe);
}

void
hrf_tx_fe_reset(hrf_tx_fe_t *fe)
{
    interp_reset(&fe->a);
    interp_reset(&fe->b);
    fe->nco.idx = 0U;
}

size_t
hrf_tx_fe_bytes(const hrf_tx_fe_t *fe, size_t n)
{
    return n * STAGE_B_M * fe->l_a * 2U;
}

static int8_t
q8(float v)
{
    long r = lrintf(v);

    return (int8_t)(r > 127 ? 127 : (r < -127 ? -127 : r));
}

size_t
hrf_tx_fe_process(hrf_tx_fe_t *fe, const float complex *in, size_t n, int8_t *out)
{
    const float *x = (const float *)in;
    size_t written = 0U;

    while (n > 0U)
    {
        size_t blk = n < TX_BLOCK ? n : TX_BLOCK, nhi = blk * STAGE_B_M * fe->l_a, i;

        interp_run(&fe->b, x, blk, fe->mid);
        interp_run(&fe->a, fe->mid, blk * STAGE_B_M, fe->hi);
        if (fe->nco.n == 0U)
        {
            for (i = 0U; i < nhi; i++)
            {
                out[i * 2U]      = q8(fe->hi[i * 2U] * fe->scale);
                out[i * 2U + 1U] = q8(fe->hi[i * 2U + 1U] * fe->scale);
            }
        }
        else
        {
            unsigned int idx = fe->nco.idx;

            for (i = 0U; i < nhi; i++)
            {
                float re = fe->hi[i * 2U], im = fe->hi[i * 2U + 1U];
                float c = fe->nco.cs[idx * 2U], s = fe->nco.cs[idx * 2U + 1U];

                /* multiply by e^{+j theta} */
                out[i * 2U]      = q8((re * c - im * s) * fe->scale);
                out[i * 2U + 1U] = q8((re * s + im * c) * fe->scale);
                idx += fe->nco.step;
                if (idx >= fe->nco.n)
                {
                    idx -= fe->nco.n;
                }
            }
            fe->nco.idx = idx;
        }
        x       += blk * 2U;
        out     += nhi * 2U;
        written += nhi * 2U;
        n       -= blk;
    }
    return written;
}
