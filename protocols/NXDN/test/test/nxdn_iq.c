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
 * nxdn_iq - NXDN 4800 test signal generator and decoder for SDR hardware.
 *
 *   nxdn_iq gen [options] -o call.cs8     write 8-bit signed IQ for hackrf_transfer -t
 *   nxdn_iq dec [options] capture.cs8     decode a hackrf_transfer -r capture
 *
 * The IQ file is centred on the carrier (use hackrf_transfer -f for the
 * frequency).  The frequency deviation is 1.05 kHz for the outer symbols.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nxdn_func.h"
#include "nxdn_dsp.h"
#include "nxdn_gen.h"

#define BASE_RATE       24000U
#define HZ_PER_COUNT    (1050.0 / 735.0)    /* NXDN_LEVEL_OUTER counts = 1.05 kHz */
#define MAX_FRAMES      4096U

/*
 * Host sample polarity: a negative sample is the +3 symbol.  On the air the
 * +3 symbol is +deviation, so the generator and decoder negate by default.
 * --invert flips it for a radio or decoder with the opposite sense.
 */
static void usage(void)
{
    fprintf(stderr,
        "nxdn_iq gen [-o file.cs8] [-r rate] [--ran n] [--src id] [--dst id] [--individual]\n"
        "            [--data blocks | --voice frames] [--repeat n] [--gap ms] [--offset hz]\n"
        "            [--level pct] [--shape rrc|rc] [--invert] [--raw24 file.s16]\n"
        "            [--sweep maxhz [--step hz]]   one call per offset and polarity, source ID 1000+n\n"
        "nxdn_iq dec [-r rate] [--offset hz] [--invert] [--shape rrc|rc] [-v] file.cs8\n"
        "  default rate 2400000 (must be a multiple of 24000); --level scales deviation (100 = 1.05 kHz)\n");
}

static double catmull(double p0, double p1, double p2, double p3, double t)
{
    return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t);
}

/* ---------------------------------------------------------------- generator */

static int doGen(int argc, char** argv)
{
    const char* out = "nxdn_test.cs8";
    const char* raw24 = NULL;
    unsigned int rate = 2400000U;
    unsigned int ran = 1U, src = 1234U, dst = 9U;
    bool group = true, invert = false;
    unsigned int voice = 50U, data = 0U, repeat = 1U, gapMs = 500U;
    double offset = 0.0, level = 100.0;
    double sweepMax = 0.0, sweepStep = 1000.0;
    unsigned int shape = NXDN_SHAPE_RRC;

    for (int i = 0; i < argc; i++)
    {
        if (!strcmp(argv[i], "--sweep") && i + 1 < argc) sweepMax = atof(argv[++i]);
        else if (!strcmp(argv[i], "--step") && i + 1 < argc) sweepStep = atof(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) rate = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ran") && i + 1 < argc) ran = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--src") && i + 1 < argc) src = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dst") && i + 1 < argc) dst = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--individual")) group = false;
        else if (!strcmp(argv[i], "--voice") && i + 1 < argc) { voice = (unsigned int)atoi(argv[++i]); data = 0U; }
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gap") && i + 1 < argc) gapMs = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--offset") && i + 1 < argc) offset = atof(argv[++i]);
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) level = atof(argv[++i]);
        else if (!strcmp(argv[i], "--shape") && i + 1 < argc) shape = !strcmp(argv[++i], "rc") ? NXDN_SHAPE_RC : NXDN_SHAPE_RRC;
        else if (!strcmp(argv[i], "--invert")) invert = true;
        else if (!strcmp(argv[i], "--raw24") && i + 1 < argc) raw24 = argv[++i];
        else { usage(); return 1; }
    }

    if (rate < BASE_RATE || (rate % BASE_RATE) != 0U)
    {
        fprintf(stderr, "rate must be a multiple of 24000\n");
        return 1;
    }
    unsigned int up = rate / BASE_RATE;

    static uint8_t frames[MAX_FRAMES][48];
    unsigned int nf = 0U;
    unsigned int nSeg = 0U;
    double segOff[512];
    bool segInv[512];
    size_t segEnd[512];

    if (sweepMax > 0.0)
    {
        if (sweepStep < 100.0) sweepStep = 100.0;
        if (voice > 20U) voice = 10U;
        repeat = 0U;
        for (int pol = 0; pol < 2; pol++)
            for (double o = -sweepMax; o <= sweepMax + 1.0 && repeat < 512U; o += sweepStep)
            {
                segOff[repeat] = o;
                segInv[repeat] = pol != 0;
                repeat++;
            }
    }
    else if (repeat > 512U)
        repeat = 512U;

    /* build the 24 kHz deviation signal, in host counts */
    size_t cap = (size_t)repeat * ((data ? data : voice) + 6U) * NXDN_SAMPLES_PER_FRAME + (size_t)repeat * (gapMs * 24U + 2000U) + 8000U;
    int16_t* base = calloc(cap, sizeof(int16_t));
    size_t n = 0U;
    NxdnTx tx;

    nxdnTxInit(&tx, NXDN_LEVEL_OUTER * (float)(level / 100.0), (uint8_t)shape);

    for (unsigned int r = 0U; r < repeat; r++)
    {
        unsigned int csrc = sweepMax > 0.0 ? 1000U + r : src;

        if (data > 0U)
            nf = nxdnGenDataCall((uint8_t)ran, (uint16_t)csrc, (uint16_t)dst, group, data, frames, MAX_FRAMES, 1U);
        else
            nf = nxdnGenVoiceCall((uint8_t)ran, (uint16_t)csrc, (uint16_t)dst, group, voice, frames, MAX_FRAMES, 1U);
        if (nf == 0U)
        {
            fprintf(stderr, "call too long\n");
            return 1;
        }

        n += nxdnTxPreamble(&tx, 40U, base + n);
        for (unsigned int f = 0U; f < nf; f++)
            n += nxdnTxFrame(&tx, frames[f], base + n);
        n += nxdnTxFlush(&tx, base + n);
        n += (size_t)gapMs * 24U;       /* unmodulated carrier between calls */

        if (sweepMax > 0.0)
            printf("source %u: offset %+.0f Hz, %s polarity\n", csrc, segOff[r], segInv[r] ? "inverted" : "normal");
        else
        {
            segOff[r] = offset;
            segInv[r] = invert;
        }
        segEnd[r] = n;
        nSeg = r + 1U;
    }

    if (raw24)
    {
        FILE* fr = fopen(raw24, "wb");
        if (fr) { fwrite(base, sizeof(int16_t), n, fr); fclose(fr); }
    }

    /* FM modulate: upsample the deviation, integrate phase */
    FILE* fp = fopen(out, "wb");
    if (!fp) { perror(out); return 1; }

    double phase = 0.0;
    size_t written = 0U;
    unsigned int seg = 0U;
    for (size_t i = 0U; i + 1U < n; i++)
    {
        while (seg + 1U < nSeg && i >= segEnd[seg])
            seg++;
        double sgn = segInv[seg] ? 1.0 : -1.0;
        double segOffset = segOff[seg];
        double p0 = base[i ? i - 1U : 0U], p1 = base[i], p2 = base[i + 1U];
        double p3 = base[(i + 2U < n) ? i + 2U : i + 1U];
        for (unsigned int k = 0U; k < up; k++)
        {
            double v = catmull(p0, p1, p2, p3, (double)k / up);
            double hz = sgn * v * HZ_PER_COUNT + segOffset;
            phase += 2.0 * M_PI * hz / (double)rate;
            if (phase > M_PI) phase -= 2.0 * M_PI;
            if (phase < -M_PI) phase += 2.0 * M_PI;
            int8_t iq[2] = { (int8_t)lrint(cos(phase) * 100.0), (int8_t)lrint(sin(phase) * 100.0) };
            fwrite(iq, 1, 2, fp);
            written++;
        }
    }
    fclose(fp);

    printf("%u frames/call x %u, %.2f s, %zu IQ samples at %u sps -> %s\n", nf, repeat,
           (double)written / rate, written, rate, out);
    printf("Transmit (low power into a dummy load or your own receiver, licensed frequency only):\n"
           "  hackrf_transfer -t %s -f <Hz> -s %u -x <0-47> -R\n", out, rate);
    free(base);
    return 0;
}

/* ------------------------------------------------------------------ decoder */

static unsigned int g_frames = 0U, g_lost = 0U;
static bool g_verbose = false;

static void onFrame(void* ctx, const uint8_t* raw, bool lost)
{
    uint8_t frame[48];
    (void)ctx;
    if (!lost)
    {
        memcpy(frame, raw, sizeof(frame));
        nxdnScramble(frame);    /* frames from the receiver are as on the air */
    }
    uint8_t lich = 0U, sacch[5], facch[12], udch[25], l3[22];

    if (lost)
    {
        g_lost++;
        printf("  [lost sync]\n");
        return;
    }

    if (!nxdnLichDecode(frame, &lich))
    {
        /* trailing frames after the transmitter has dropped, until sync is declared lost */
        if (g_verbose)
            printf("  (no valid LICH)\n");
        return;
    }
    g_frames++;

    unsigned int usc = NXDN_LICH_FCT(lich);
    const char* kind = usc == NXDN_LICH_USC_UDCH ? "data" : (usc == NXDN_LICH_USC_SACCH_SS ? "voice" : "facch/idle");
    int ran = -1;
    if (usc != NXDN_LICH_USC_UDCH && nxdnSacchDecode(frame, sacch))
        ran = NXDN_SACCH_RAN(sacch);

    if (usc == NXDN_LICH_USC_UDCH && nxdnUdchDecode(frame, udch))
    {
        ran = udch[0] & 0x3FU;
        nxdnL3Decode(l3, udch + 1U, 176U, 0U);
        printf("  frame %u: %s RAN %d type 0x%02X src %u dst %u\n", g_frames, kind, ran,
               NXDN_L3_TYPE(l3), NXDN_L3_SRC(l3), NXDN_L3_DST(l3));
        return;
    }

    if (nxdnFacch1Decode(frame, NXDN_FACCH1_OFFSET_1, facch))
    {
        nxdnL3Decode(l3, facch, 80U, 0U);
        printf("  frame %u: %s RAN %d FACCH1 type 0x%02X src %u dst %u %s\n", g_frames, kind, ran,
               NXDN_L3_TYPE(l3), NXDN_L3_SRC(l3), NXDN_L3_DST(l3), NXDN_L3_GROUP(l3) ? "group" : "individual");
    }
    else if (g_verbose)
        printf("  frame %u: %s RAN %d\n", g_frames, kind, ran);
}

static int doDec(int argc, char** argv)
{
    const char* in = NULL;
    unsigned int rate = 2400000U;
    double offset = 0.0;
    bool invert = false;
    unsigned int shape = NXDN_SHAPE_RRC;

    for (int i = 0; i < argc; i++)
    {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) rate = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--offset") && i + 1 < argc) offset = atof(argv[++i]);
        else if (!strcmp(argv[i], "--invert")) invert = true;
        else if (!strcmp(argv[i], "--shape") && i + 1 < argc) shape = !strcmp(argv[++i], "rc") ? NXDN_SHAPE_RC : NXDN_SHAPE_RRC;
        else if (!strcmp(argv[i], "-v")) g_verbose = true;
        else if (argv[i][0] != '-') in = argv[i];
        else { usage(); return 1; }
    }
    if (!in || rate < BASE_RATE || (rate % BASE_RATE) != 0U)
    {
        usage();
        return 1;
    }

    FILE* fp = fopen(in, "rb");
    if (!fp) { perror(in); return 1; }

    unsigned int dec = rate / BASE_RATE;
    NxdnRx* rx = calloc(1, sizeof(NxdnRx));
    nxdnRxInit(rx, onFrame, NULL, (uint8_t)shape);

    /* mix to baseband, FM discriminate, then average down to 24 kHz */
    double mixPh = 0.0, mixStep = -2.0 * M_PI * offset / rate;
    double pi = 1.0, pq = 0.0;
    double acc = 0.0, dc = 0.0;
    unsigned int cnt = 0U, nDc = 0U;
    double sgn = invert ? 1.0 : -1.0;
    int16_t out[4096];
    unsigned int on = 0U;
    uint8_t buf[8192];
    size_t got;
    bool first = true;

    while ((got = fread(buf, 1, sizeof(buf) & ~1U, fp)) > 1U)
    {
        for (size_t i = 0U; i + 1U < got; i += 2U)
        {
            double si = (int8_t)buf[i], sq = (int8_t)buf[i + 1U];
            double c = cos(mixPh), s = sin(mixPh);
            double xi = si * c - sq * s, xq = si * s + sq * c;
            mixPh += mixStep;
            if (mixPh > M_PI) mixPh -= 2.0 * M_PI;
            if (mixPh < -M_PI) mixPh += 2.0 * M_PI;

            /* arg(x[n] * conj(x[n-1])) */
            double di = xi * pi + xq * pq, dq = xq * pi - xi * pq;
            pi = xi; pq = xq;
            if (first) { first = false; continue; }

            acc += atan2(dq, di);
            if (++cnt == dec)
            {
                double hz = (acc / dec) * rate / (2.0 * M_PI);
                acc = 0.0; cnt = 0U;
                dc += (hz - dc) * (nDc < 1200U ? 0.01 : 0.0005);   /* carrier offset tracking, fast at first */
                nDc++;
                double v = sgn * (hz - dc) / HZ_PER_COUNT;
                if (v > 32000.0) v = 32000.0;
                if (v < -32000.0) v = -32000.0;
                out[on++] = (int16_t)lrint(v);
                if (on == sizeof(out) / sizeof(out[0]))
                {
                    nxdnRxSamples(rx, out, on);
                    on = 0U;
                }
            }
        }
    }
    if (on)
        nxdnRxSamples(rx, out, on);
    fclose(fp);

    printf("frames %u  lost %u  candidates %u locks %u dropped %u  carrier offset %.0f Hz\n",
           rx->statFrames, rx->statLost, rx->statCandidates, rx->statLocks, rx->statDropped, dc);
    free(rx);
    return g_frames > 0U ? 0 : 2;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && !strcmp(argv[1], "gen"))
        return doGen(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "dec"))
        return doDec(argc - 2, argv + 2);
    usage();
    return 1;
}
