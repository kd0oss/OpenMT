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
 * Loopback tests of the YSF modulator and demodulators.
 *
 *   gcc -I.. -I../.. ysf_dsp_test.c ../ysf_dsp.c ../ysf_func.c ../../dsp_tools.c -lm -o ysf_dsp_test
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ysf_dsp.h"
#include "ysf_func.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MAX_FRAMES 64

typedef struct
{
    uint8_t frames[MAX_FRAMES][YSF_FRAME_LENGTH_BYTES];
    int count;
    int lost;
} Got;

static void onFrame(void* ctx, const uint8_t* frame, bool lost)
{
    Got* g = (Got*)ctx;

    if (lost)
        g->lost++;
    else if (g->count < MAX_FRAMES)
        memcpy(g->frames[g->count++], frame, YSF_FRAME_LENGTH_BYTES);
}

static float gauss(void)
{
    float u = ((float)rand() + 1.0F) / ((float)RAND_MAX + 2.0F);
    float v = ((float)rand() + 1.0F) / ((float)RAND_MAX + 2.0F);

    return sqrtf(-2.0F * logf(u)) * cosf(6.2831853F * v);
}

/* A frame with a good FICH and random contents behind it */
static void makeFrame(uint8_t* f, int n)
{
    YsfFich h;

    for (int i = 0; i < 120; i++)
        f[i] = (uint8_t)rand();

    ysfAddSync(f);
    ysfFichInit(&h);
    ysfFichSetFI(&h, YSF_FI_COMMUNICATIONS);
    ysfFichSetFN(&h, (uint8_t)(n % 7));
    ysfFichSetFT(&h, 6);
    ysfFichSetDT(&h, YSF_DT_VD_MODE2);
    ysfFichEncode(&h, f);
}

static int runSamples(int nFrames, float gain, float dc, float noise, uint8_t txShape, uint8_t rxShape, int lead, Got* got)
{
    static uint8_t sent[MAX_FRAMES][YSF_FRAME_LENGTH_BYTES];
    YsfTx tx;
    YsfRx* rx = (YsfRx*)calloc(1, sizeof(YsfRx));
    int16_t buf[YSF_SAMPLES_PER_FRAME];
    int16_t pad[5 * 100];
    int match = 0;

    memset(got, 0, sizeof(*got));
    ysfTxInit(&tx, YSF_LEVEL_OUTER_H, txShape);
    ysfRxInit(rx, onFrame, got, rxShape);

    /* some silence with noise in front */
    for (int i = 0; i < lead; i++)
    {
        int16_t s = (int16_t)(noise * gauss() + dc);
        ysfRxSamples(rx, &s, 1);
    }

    unsigned int n = ysfTxPreamble(&tx, 40, pad);
    for (unsigned int i = 0; i < n; i++) pad[i] = (int16_t)(pad[i] * gain + dc + noise * gauss());
    ysfRxSamples(rx, pad, n);

    for (int k = 0; k < nFrames; k++)
    {
        makeFrame(sent[k], k);
        n = ysfTxFrame(&tx, sent[k], buf);
        for (unsigned int i = 0; i < n; i++) buf[i] = (int16_t)(buf[i] * gain + dc + noise * gauss());
        ysfRxSamples(rx, buf, n);
    }

    n = ysfTxFlush(&tx, pad);
    for (unsigned int i = 0; i < n; i++) pad[i] = (int16_t)(pad[i] * gain + dc + noise * gauss());
    ysfRxSamples(rx, pad, n);

    /* then silence until the receiver declares the signal lost */
    for (int i = 0; i < 6 * 2400; i++)
    {
        int16_t s = (int16_t)(noise * gauss() + dc);
        ysfRxSamples(rx, &s, 1);
    }

    for (int k = 0; k < got->count && k < nFrames; k++)
        if (memcmp(got->frames[k], sent[k], YSF_FRAME_LENGTH_BYTES) == 0)
            match++;

    free(rx);
    return match;
}

static void testSamples(void)
{
    Got g;
    int m;

    m = runSamples(20, 1.0F, 0.0F, 0.0F, YSF_SHAPE_RRC, YSF_SHAPE_RRC, 100, &g);
    CHECK(g.count == 20 && m == 20, "clean: got %d frames, %d identical", g.count, m);
    CHECK(g.lost == 1, "clean: lost reported %d times", g.lost);

    m = runSamples(20, 0.5F, 300.0F, 0.0F, YSF_SHAPE_RRC, YSF_SHAPE_RRC, 333, &g);
    CHECK(g.count == 20 && m == 20, "gain/offset: got %d, %d identical", g.count, m);

    m = runSamples(20, 1.0F, 0.0F, 250.0F, YSF_SHAPE_RRC, YSF_SHAPE_RRC, 777, &g);
    CHECK(g.count == 20 && m >= 18, "noise: got %d, %d identical", g.count, m);

    /* a raised cosine transmitter into the matched receiver is a bit worse, but must still work */
    m = runSamples(20, 1.0F, 0.0F, 0.0F, YSF_SHAPE_RC, YSF_SHAPE_RRC, 55, &g);
    CHECK(g.count == 20 && m == 20, "RC tx: got %d, %d identical", g.count, m);

    /* pure noise must not produce a frame */
    {
        YsfRx* rx = (YsfRx*)calloc(1, sizeof(YsfRx));
        Got gn;
        memset(&gn, 0, sizeof(gn));
        ysfRxInit(rx, onFrame, &gn, YSF_SHAPE_RRC);
        for (int i = 0; i < 2400 * 40; i++)
        {
            int16_t s = (int16_t)(800.0F * gauss());
            ysfRxSamples(rx, &s, 1);
        }
        CHECK(gn.count == 0, "noise produced %d frames", gn.count);
        free(rx);
    }
}

/* Bits of a transmission: random lead-in, preamble, frames, junk */
static void testBits(void)
{
    static uint8_t sent[MAX_FRAMES][YSF_FRAME_LENGTH_BYTES];
    YsfBitRx rx;
    Got g;
    uint8_t pre[40];

    memset(&g, 0, sizeof(g));
    ysfBitRxInit(&rx, onFrame, &g);

    for (int i = 0; i < 17; i++)
    {
        uint8_t b = (uint8_t)rand();
        ysfBitRxBytes(&rx, &b, 1);
    }

    ysfBitTxPreamble(pre, sizeof(pre));
    ysfBitRxBytes(&rx, pre, sizeof(pre));

    for (int k = 0; k < 12; k++)
    {
        makeFrame(sent[k], k);

        /* two bit errors in the sync of every other frame, a bit error in the data */
        uint8_t tmp[YSF_FRAME_LENGTH_BYTES];
        memcpy(tmp, sent[k], sizeof(tmp));
        if (k % 2) { tmp[1] ^= 0x04; tmp[3] ^= 0x10; }
        ysfBitRxBytes(&rx, tmp, sizeof(tmp));
    }

    for (int i = 0; i < 3 * 120; i++)
    {
        uint8_t b = 0x00;
        ysfBitRxBytes(&rx, &b, 1);
    }

    CHECK(g.count == 12, "bits: got %d frames", g.count);
    CHECK(g.lost == 1, "bits: lost %d", g.lost);

    int bad = 0;
    for (int k = 0; k < g.count && k < 12; k++)
    {
        uint8_t ref[YSF_FRAME_LENGTH_BYTES];
        memcpy(ref, sent[k], sizeof(ref));
        if (memcmp(g.frames[k] + 5, ref + 5, 115) != 0) bad++;
        if (!ysfHasSync(g.frames[k], 4)) bad++;
    }
    CHECK(bad == 0, "bits: %d frames differ", bad);

    /* a start that is one bit out of step must resynchronise */
    ysfBitRxReset(&rx);
    memset(&g, 0, sizeof(g));
    {
        uint8_t shifted[YSF_FRAME_LENGTH_BYTES + 1];
        makeFrame(sent[0], 0);
        memset(shifted, 0, sizeof(shifted));
        for (int i = 0; i < 960; i++)
        {
            int bit = (sent[0][i >> 3] >> (7 - (i & 7))) & 1;
            int p = i + 1;
            if (bit) shifted[p >> 3] |= (uint8_t)(0x80 >> (p & 7));
        }
        ysfBitRxBytes(&rx, shifted, sizeof(shifted));
        ysfBitRxBytes(&rx, shifted, sizeof(shifted));
    }
    CHECK(g.count >= 1, "bits: shifted frames %d", g.count);
}

int main(void)
{
    srand(3);
    testSamples();
    testBits();

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "all dsp tests passed", failures);
    return failures != 0;
}
