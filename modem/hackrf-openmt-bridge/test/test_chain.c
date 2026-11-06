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
 * test_chain.c - is the HackRF front end transparent to the shared FM chain?
 *
 * The same synthetic 4FSK-style FM signal is run through dsp.c two ways:
 *
 *   reference:  complex baseband at 125 kS/s straight into the RX pipeline
 *               (what the SX1255 bridge feeds it)
 *   HackRF:     int8 IQ at 2 Msps, 250 kHz off the LO, with noise
 *               -> hrf_frontend -> the same pipeline
 *
 * The two outputs must agree.  How well either matches the ideal baseband
 * is a property of dsp.c on this test signal, not of the front end.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hrf_frontend.h"
#include "dsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>

#define NSYM   9600
#define FS_HRF 2000000
#define OFFSET 250000

/* Correlation of a against b shifted by 'lag' samples (fractional lags use
 * linear interpolation; both signals are band-limited far below 24 kHz, so
 * its error is well under 0.1 %). */
static double
corr_at(const float *a, size_t na, const float *b, size_t nb, double lag, double *slope)
{
    double sab = 0, saa = 0, sbb = 0;
    size_t i, skip = 2400;

    for (i = skip; i < na - skip; i++)
    {
        double j = (double)i + lag, bj;
        long j0 = (long)floor(j);

        if (j0 < (long)skip || j0 + 1 >= (long)nb - (long)skip)
        {
            continue;
        }
        bj = b[j0] * (1.0 - (j - (double)j0)) + b[j0 + 1] * (j - (double)j0);
        sab += (double)a[i] * bj;
        saa += (double)a[i] * a[i];
        sbb += bj * bj;
    }
    *slope = saa > 0 ? sab / saa : 0;
    return (saa > 0 && sbb > 0) ? sab / sqrt(saa * sbb) : -2.0;
}

static double
correlate(const float *a, size_t na, const float *b, size_t nb, double *lag_out, double *slope)
{
    double best = -2.0, bestlag = 0, sl;
    int lag;

    for (lag = -120; lag <= 120; lag++)
    {
        double c = corr_at(a, na, b, nb, lag, &sl);

        if (c > best)
        {
            best    = c;
            bestlag = lag;
        }
    }
    /* refine to a fraction of a sample */
    {
        double l0 = bestlag;

        for (double d = -1.0; d <= 1.0; d += 0.05)
        {
            double c = corr_at(a, na, b, nb, l0 + d, &sl);

            if (c > best)
            {
                best    = c;
                bestlag = l0 + d;
            }
        }
    }
    corr_at(a, na, b, nb, bestlag, slope);
    *lag_out = bestlag;
    return best;
}

int
main(void)
{
    static double dev24[NSYM * 5];
    size_t n24 = NSYM * 5, i, n125, nhrf;
    unsigned int seed = 12345;
    double w[7], wsum = 0.0;
    float complex *ref125;
    int8_t *iq;
    float *out_ref, *out_fe;
    size_t nref = 0, nfe = 0;
    dsp_rx_pipeline_t *p1, *p2;
    hrf_rx_fe_t *fe;
    double phase, corr_fe, corr_ideal_ref, corr_ideal_fe, slope, s_ideal;
    double lag;
    float *ideal;
    int fail = 0;

    /* 4FSK-style deviation: random +-1/+-3 symbols at 4800 baud, smoothed */
    for (i = 0; i < 7; i++)
    {
        w[i] = 0.5 - 0.5 * cos(2.0 * M_PI * (double)(i + 1) / 8.0);
        wsum += w[i];
    }
    {
        double raw[NSYM * 5];

        for (i = 0; i < NSYM; i++)
        {
            static const int lev[4] = {-3, -1, 1, 3};
            int s;

            seed = seed * 1103515245U + 12345U;
            s = lev[(seed >> 16) & 3U];
            for (int k = 0; k < 5; k++)
            {
                raw[i * 5 + k] = s * 950.0;
            }
        }
        for (i = 0; i < n24; i++)
        {
            double acc = 0.0;

            for (int k = 0; k < 7; k++)
            {
                long j = (long)i + k - 3;

                if (j >= 0 && j < (long)n24)
                {
                    acc += raw[j] * w[k];
                }
            }
            dev24[i] = acc / wsum;
        }
    }

    /* reference: 125 kS/s */
    n125 = n24 * 125000 / 24000;
    ref125 = malloc(n125 * sizeof(*ref125));
    phase = 0.0;
    for (i = 0; i < n125; i++)
    {
        double t = (double)i * 24000.0 / 125000.0;
        size_t i0 = (size_t)t;
        double f = dev24[i0 < n24 - 1 ? i0 : n24 - 2] * (1.0 - (t - (double)i0)) +
            dev24[i0 + 1 < n24 ? i0 + 1 : n24 - 1] * (t - (double)i0);

        phase += 2.0 * M_PI * f / 125000.0;
        ref125[i] = (float complex)cexp(I * phase);
    }
    /* HackRF: int8 at 2 Msps, 250 kHz off, noise */
    nhrf = n24 * FS_HRF / 24000;
    iq = malloc(nhrf * 2);
    phase = 0.0;
    seed = 777;
    for (i = 0; i < nhrf; i++)
    {
        double t = (double)i * 24000.0 / FS_HRF;
        size_t i0 = (size_t)t;
        double f = dev24[i0 < n24 - 1 ? i0 : n24 - 2] * (1.0 - (t - (double)i0)) +
            dev24[i0 + 1 < n24 ? i0 + 1 : n24 - 1] * (t - (double)i0);
        double nr = 0, ni = 0;

        phase += 2.0 * M_PI * ((double)OFFSET + f) / FS_HRF;
        for (int k = 0; k < 4; k++)       /* ~ gaussian, sigma 1 */
        {
            seed = seed * 1103515245U + 12345U;
            nr += ((seed >> 16) & 0x7FFF) / 32768.0 - 0.5;
            seed = seed * 1103515245U + 12345U;
            ni += ((seed >> 16) & 0x7FFF) / 32768.0 - 0.5;
        }
        nr *= sqrt(3.0);
        ni *= sqrt(3.0);
        iq[i * 2]     = (int8_t)lrint(100.0 * cos(phase) + nr);
        iq[i * 2 + 1] = (int8_t)lrint(100.0 * sin(phase) + ni);
    }

    p1 = dsp_rx_pipeline_create(125000);
    p2 = dsp_rx_pipeline_create(125000);
    out_ref = malloc(n24 * 2 * sizeof(float));
    out_fe  = malloc(n24 * 2 * sizeof(float));
    for (i = 0; i < n125; i++)
    {
        float bb[16];
        int n = dsp_rx_pipeline_process_baseband(p1, ref125[i], bb, 16);

        for (int k = 0; k < n; k++)
        {
            out_ref[nref++] = bb[k];
        }
    }
    fe = hrf_rx_fe_create(FS_HRF, OFFSET);
    {
        float complex *bbc = malloc((nhrf / 16 + 64) * sizeof(*bbc));
        /* feed in odd-sized pieces, as the USB thread would */
        size_t pos = 0, piece = 12345, got = 0;

        while (pos < nhrf)
        {
            size_t n = nhrf - pos < piece ? nhrf - pos : piece;

            got = hrf_rx_fe_process(fe, iq + pos * 2, n, bbc, nhrf / 16 + 64);
            for (size_t j = 0; j < got; j++)
            {
                float bb[16];
                int m = dsp_rx_pipeline_process_baseband(p2, bbc[j], bb, 16);

                for (int k = 0; k < m; k++)
                {
                    out_fe[nfe++] = bb[k];
                }
            }
            pos += n;
        }
        free(bbc);
    }
    printf("samples out: reference %zu, HackRF path %zu (expected about %zu)\n", nref, nfe, n24);

    corr_fe = correlate(out_ref, nref, out_fe, nfe, &lag, &slope);
    printf("HackRF path vs reference: correlation %.5f, slope %.4f, lag %.2f samples\n", corr_fe, slope, lag);
    /* (the slope reads a little under 1 even for a perfect match: aligning
     * to a fraction of a sample by linear interpolation attenuates the
     * 1-2 kHz content a percent or so) */
    if (corr_fe < 0.999 || fabs(slope - 1.0) > 0.02)
    {
        printf("  FAIL: the front end changes the signal\n");
        fail = 1;
    }

    ideal = malloc(n24 * sizeof(float));
    for (i = 0; i < n24; i++)
    {
        ideal[i] = (float)(dev24[i] * 1.24 * 2.0 * M_PI / 12500.0);
    }
    corr_ideal_ref = correlate(ideal, n24, out_ref, nref, &lag, &s_ideal);
    printf("reference vs ideal baseband: correlation %.4f, slope %.3f, lag %.2f  (dsp.c on this test signal)\n", corr_ideal_ref, s_ideal, lag);
    corr_ideal_fe = correlate(ideal, n24, out_fe, nfe, &lag, &s_ideal);
    printf("HackRF path vs ideal:        correlation %.4f, slope %.3f, lag %.2f\n", corr_ideal_fe, s_ideal, lag);
    printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
