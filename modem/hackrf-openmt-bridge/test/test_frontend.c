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
 * test_frontend.c - checks hrf_frontend.c against what it promises:
 *   - unity gain across the 12.5 kHz channel, RX and TX
 *   - aliases / images of the 125 kS/s signal rejected
 *   - the frequency shift goes the right way (RX down, TX up)
 *   - TX followed by RX returns the input
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hrf_frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>

static int failures;

#define CHECK(cond, ...)                                  \
    do                                                    \
    {                                                     \
        if (!(cond))                                      \
        {                                                 \
            printf("  FAIL: " __VA_ARGS__);               \
            printf("\n");                                 \
            failures++;                                   \
        }                                                 \
    } while (0)

/* magnitude of the component of x at frequency f (Hz) of rate fs, skipping
 * the first 'skip' samples (filter start-up) */
static double
tone_mag(const float complex *x, size_t n, size_t skip, double f, double fs)
{
    double complex acc = 0.0;
    size_t i;

    for (i = skip; i < n; i++)
    {
        acc += (double complex)x[i] * cexp(-I * 2.0 * M_PI * f * (double)i / fs);
    }
    return cabs(acc) / (double)(n - skip);
}

static double
db(double v)
{
    return 20.0 * log10(v > 1e-12 ? v : 1e-12);
}

static void
test_rx(uint32_t fs, int32_t offset)
{
    const double amp = 100.0;           /* int8 counts */
    const double tones[] = {0.0, 3000.0, -3000.0, 8000.0, -8000.0, 12000.0, -12000.0};
    const double rej[] = {125000.0 + 3000.0, -125000.0 + 3000.0, 250000.0, -190000.0, 500000.0 + 3000.0, 1000000.0 + 3000.0};
    size_t n = 200000U, i, t;
    hrf_rx_fe_t *fe = hrf_rx_fe_create(fs, offset);
    int8_t *iq = malloc(n * 2U);
    float complex *out = malloc((n / 16U + 64U) * sizeof(*out));

    printf("RX fs=%u offset=%d\n", fs, offset);
    CHECK(fe != NULL, "create");
    if (fe == NULL)
    {
        return;
    }
    for (t = 0; t < sizeof(tones) / sizeof(tones[0]); t++)
    {
        double f = tones[t] + offset;
        size_t no;
        double m, expect = amp / 128.0;

        for (i = 0; i < n; i++)
        {
            double complex v = amp * cexp(I * 2.0 * M_PI * f * (double)i / (double)fs);

            iq[i * 2U]      = (int8_t)lrint(creal(v));
            iq[i * 2U + 1U] = (int8_t)lrint(cimag(v));
        }
        hrf_rx_fe_reset(fe);
        no = hrf_rx_fe_process(fe, iq, n, out, n / 16U + 64U);
        m  = tone_mag(out, no, 200, tones[t], 125000.0);
        printf("  passband %+8.0f Hz: gain %+.2f dB\n", tones[t], db(m / expect));
        CHECK(fabs(db(m / expect)) < 0.5, "passband gain at %.0f Hz", tones[t]);
    }
    for (t = 0; t < sizeof(rej) / sizeof(rej[0]); t++)
    {
        double f = rej[t] + offset;
        size_t no;
        double worst = 0.0;

        /* a tone that, unfiltered, would alias to about +3 kHz (or, for
         * the 250/-190 kHz ones, somewhere inside the channel) */
        for (i = 0; i < n; i++)
        {
            double complex v = amp * cexp(I * 2.0 * M_PI * f * (double)i / (double)fs);

            iq[i * 2U]      = (int8_t)lrint(creal(v));
            iq[i * 2U + 1U] = (int8_t)lrint(cimag(v));
        }
        hrf_rx_fe_reset(fe);
        no = hrf_rx_fe_process(fe, iq, n, out, n / 16U + 64U);
        /* worst component anywhere in the +-25 kHz the DSP chain looks at */
        for (double g = -25000.0; g <= 25000.0; g += 500.0)
        {
            double mm = tone_mag(out, no, 200, g, 125000.0);

            if (mm > worst)
            {
                worst = mm;
            }
        }
        printf("  reject %+9.0f Hz: worst in-channel %+.1f dBc\n", rej[t], db(worst / (amp / 128.0)));
        CHECK(db(worst / (amp / 128.0)) < -60.0, "rejection at %.0f Hz", rej[t]);
    }
    free(iq);
    free(out);
    hrf_rx_fe_destroy(fe);
}

static void
test_tx(uint32_t fs, int32_t offset)
{
    const float amplitude = 0.8f;
    size_t n = 20000U, i, nb, k;
    hrf_tx_fe_t *fe = hrf_tx_fe_create(fs, offset, amplitude);
    float complex *in = malloc(n * sizeof(*in));
    int8_t *out;
    float complex *o;
    double tones[] = {0.0, 5000.0, -9000.0};

    printf("TX fs=%u offset=%d\n", fs, offset);
    CHECK(fe != NULL, "create");
    if (fe == NULL)
    {
        return;
    }
    out = malloc(hrf_tx_fe_bytes(fe, n));
    o   = malloc(hrf_tx_fe_bytes(fe, n) / 2U * sizeof(*o));
    for (k = 0; k < sizeof(tones) / sizeof(tones[0]); k++)
    {
        double want = tones[k] + offset, mw, worst_img = 0.0;
        size_t nout;

        for (i = 0; i < n; i++)
        {
            in[i] = (float complex)cexp(I * 2.0 * M_PI * tones[k] * (double)i / 125000.0);
        }
        hrf_tx_fe_reset(fe);
        nb   = hrf_tx_fe_process(fe, in, n, out);
        nout = nb / 2U;
        for (i = 0; i < nout; i++)
        {
            o[i] = (float)out[i * 2U] + (float)out[i * 2U + 1U] * I;
        }
        mw = tone_mag(o, nout, 4000, want, (double)fs);
        printf("  tone %+7.0f Hz -> level %.1f (want %.1f)\n", tones[k], mw, amplitude * 127.0);
        CHECK(fabs(mw / (amplitude * 127.0) - 1.0) < 0.03, "TX level at %.0f Hz", tones[k]);
        /* images of the 125 kS/s signal: every multiple of 125k and 500k
         * around the wanted frequency */
        for (int m = -3; m <= 3; m++)
        {
            double fi;

            if (m == 0)
            {
                continue;
            }
            fi = want + m * 125000.0;
            if (fabs(fi) >= fs / 2.0)
            {
                continue;
            }
            double mi = tone_mag(o, nout, 4000, fi, (double)fs);

            if (mi > worst_img)
            {
                worst_img = mi;
            }
        }
        printf("            worst image %+.1f dBc\n", db(worst_img / mw));
        CHECK(db(worst_img / mw) < -55.0, "image rejection for %.0f Hz tone", tones[k]);
    }
    free(in);
    free(out);
    free(o);
    hrf_tx_fe_destroy(fe);
}

static void
test_loopback(uint32_t fs, int32_t offset)
{
    size_t n = 30000U, i, nb, nr, best_lag = 0;
    hrf_tx_fe_t *tx = hrf_tx_fe_create(fs, offset, 0.8f);
    hrf_rx_fe_t *rx = hrf_rx_fe_create(fs, offset);
    float complex *in = malloc(n * sizeof(*in)), *out;
    int8_t *iq = malloc(hrf_tx_fe_bytes(tx, n));
    double best = 0.0, phase = 0.0;

    printf("loopback fs=%u offset=%d\n", fs, offset);
    /* an FM signal like the real thing: random +-1 symbols, 950 Hz/unit */
    srand(7);
    for (i = 0; i < n; i++)
    {
        double sym = ((i / 25U) % 2U == 0U) ? 1.0 : -1.0;
        sym = (rand() & 1) ? sym : -sym;
        phase += 2.0 * M_PI * 950.0 * sym * 3.0 / 125000.0;
        in[i] = (float complex)cexp(I * phase);
    }
    nb  = hrf_tx_fe_process(tx, in, n, iq);
    out = malloc((nb / 2U / 16U + 64U) * sizeof(*out));
    nr  = hrf_rx_fe_process(rx, iq, nb / 2U, out, nb / 2U / 16U + 64U);
    /* find the lag with the best correlation (group delay of the filters) */
    for (size_t lag = 0; lag < 60; lag++)
    {
        double complex acc = 0.0;

        for (i = 1000; i + lag < nr && i < n - 1000; i++)
        {
            acc += (double complex)out[i + lag] * conj((double complex)in[i]);
        }
        if (cabs(acc) > best)
        {
            best     = cabs(acc);
            best_lag = lag;
        }
    }
    best /= (double)(nr - 2000 - best_lag);
    printf("  group delay %zu samples, correlation %.3f (ideal 0.8)\n", best_lag, best);
    CHECK(fabs(best - 0.8 * 127.0 / 128.0) < 0.03, "loopback amplitude/phase");
    free(in);
    free(out);
    free(iq);
    hrf_tx_fe_destroy(tx);
    hrf_rx_fe_destroy(rx);
}

static void
test_shift_direction(void)
{
    /* DC in at 125 kS/s, offset +250 kHz: the TX output must sit at +250k.
     * RX: a tone at +250k must come out at DC.  (Both are covered by the
     * tone tests above; this one fails loudly if the sign is wrong.) */
    hrf_tx_fe_t *tx = hrf_tx_fe_create(2000000U, 250000, 0.5f);
    float complex in[2000];
    int8_t out[2000 * 16 * 2];
    float complex *o = malloc(2000 * 16 * sizeof(*o));
    size_t i, nb;
    double plus, minus;

    for (i = 0; i < 2000; i++)
    {
        in[i] = 1.0f;
    }
    nb = hrf_tx_fe_process(tx, in, 2000, out);
    for (i = 0; i < nb / 2U; i++)
    {
        o[i] = (float)out[i * 2U] + (float)out[i * 2U + 1U] * I;
    }
    plus  = tone_mag(o, nb / 2U, 4000, 250000.0, 2e6);
    minus = tone_mag(o, nb / 2U, 4000, -250000.0, 2e6);
    printf("shift direction: +250k %.1f, -250k %.1f\n", plus, minus);
    CHECK(plus > 50.0 && minus < 1.0, "TX offset sign");
    free(o);
    hrf_tx_fe_destroy(tx);
}

int
main(void)
{
    test_rx(2000000U, 250000);
    test_rx(2000000U, 0);
    test_rx(2500000U, -300000);
    test_tx(2000000U, 0);
    test_tx(2000000U, 250000);
    test_loopback(2000000U, 250000);
    test_loopback(2000000U, 0);
    test_shift_direction();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures != 0;
}
