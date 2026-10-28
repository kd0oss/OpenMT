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
#include "ambe3000.h"

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
        "            [--speech in.wav --ambe /dev/ttyUSB0 [--baud 460800]]   voice from a WAV file through an AMBE3000R\n"
        "nxdn_iq dec [-r rate] [--format cs8|cs16|cf32] [--offset hz | --auto] [--invert] [--shape rrc|rc] [-v] file\n"
        "            gqrx recordings (name ends _fc.raw) are read as cf32 and the rate is taken from the name\n"
        "            [--ambe /dev/ttyUSB0 [--baud 460800] --wav out.wav]     decode the voice to a WAV file\n"
        "nxdn_iq ambe-test /dev/ttyUSB0 [--baud 460800] [in.wav]            check the dongle: speech -> 72 bit blocks -> speech\n"
        "  --ambe-hwreset dtr|rts|both pulses those lines low at open; --ambe-init hex sets the PKT_INIT flags (0 = skip)\n"
        "  --ambe-chunk n --ambe-gap us   write n bytes at a time with a pause (default 16 and 2000); --ambe-flow RTS/CTS\n"
        "  --layout nofec|air|coding   how the chip arranges its 72 bit block (default: detected from the chip)\n"
        "  --dump file writes the chip's 72 bit blocks as hex, one line per frame (ambe-test)\n"
        "  --gain x scales the speech before the chip (0.25 = -12 dB); a hot, clipped WAV distorts in the vocoder\n"
        "  --out-gain x scales the decoded audio (the dongle's output is usually quieter than its input)\n"
        "  --ambe-debug prints every packet to and from the dongle\n"
        "  AMBE rate: --ambe-rate w0,w1,w2,w3,w4,w5 (hex PKT_RATEP words) or --ambe-index n (PKT_RATET index)\n"
        "  default rate 2400000 (must be a multiple of 24000); --level scales deviation (100 = 1.05 kHz)\n");
}

static double catmull(double p0, double p1, double p2, double p3, double t)
{
    return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t);
}

/* ------------------------------------------------------------ AMBE3000R */

static const char* g_ambeDev = NULL;
static const char* g_dump = NULL;   /* write the chip's blocks as hex, one line per 20 ms frame */
static double g_outGain = 1.0;      /* applied to the decoded audio before it is written */
static double g_gain = 1.0;         /* applied to the speech before the chip */
static unsigned int g_ambeBaud = 460800U;
static const char* g_ambeRateArg = NULL;
static int g_ambeIndex = -1;
static int g_layoutArg = -1;        /* -1: detect, else NXDN_CHIP_* */
static unsigned int g_layout = NXDN_CHIP_NOFEC;

/* Handles the --ambe options common to gen, dec and ambe-test. Returns 1 if argv[*i] was one of them. */
static int ambeOption(int argc, char** argv, int* i)
{
    if (!strcmp(argv[*i], "--ambe") && *i + 1 < argc) { g_ambeDev = argv[++(*i)]; return 1; }
    if (!strcmp(argv[*i], "--baud") && *i + 1 < argc) { g_ambeBaud = (unsigned int)atoi(argv[++(*i)]); return 1; }
    if (!strcmp(argv[*i], "--ambe-rate") && *i + 1 < argc) { g_ambeRateArg = argv[++(*i)]; return 1; }
    if (!strcmp(argv[*i], "--ambe-hwreset") && *i + 1 < argc)
    {
        const char* v = argv[++(*i)];
        ambeHwReset = !strcmp(v, "dtr") ? 1 : (!strcmp(v, "rts") ? 2 : (!strcmp(v, "both") ? 3 : 0));
        return 1;
    }
    if (!strcmp(argv[*i], "--ambe-init") && *i + 1 < argc) { ambeInit = (int)strtol(argv[++(*i)], NULL, 16); return 1; }
    if (!strcmp(argv[*i], "--ambe-chunk") && *i + 1 < argc) { ambeChunk = atoi(argv[++(*i)]); return 1; }
    if (!strcmp(argv[*i], "--ambe-gap") && *i + 1 < argc) { ambeGapUs = atoi(argv[++(*i)]); return 1; }
    if (!strcmp(argv[*i], "--ambe-flow")) { ambeFlow = 1; return 1; }
    if (!strcmp(argv[*i], "--layout") && *i + 1 < argc)
    {
        const char* v = argv[++(*i)];
        g_layoutArg = !strcmp(v, "nofec") ? (int)NXDN_CHIP_NOFEC : (!strcmp(v, "air") ? (int)NXDN_CHIP_AIR :
                      (!strcmp(v, "coding") ? (int)NXDN_CHIP_CODING : -1));
        return 1;
    }
    if (!strcmp(argv[*i], "--dump") && *i + 1 < argc) { g_dump = argv[++(*i)]; return 1; }
    if (!strcmp(argv[*i], "--out-gain") && *i + 1 < argc) { g_outGain = atof(argv[++(*i)]); return 1; }
    if (!strcmp(argv[*i], "--gain") && *i + 1 < argc) { g_gain = atof(argv[++(*i)]); return 1; }
    if (!strcmp(argv[*i], "--ambe-debug")) { ambeDebug = 1; return 1; }
    if (!strcmp(argv[*i], "--ambe-index") && *i + 1 < argc) { g_ambeIndex = atoi(argv[++(*i)]); return 1; }
    return 0;
}

static bool ambeStart(Ambe* a, const char* dev)
{
    ambeDefaultRate(a);

    if (g_ambeRateArg != NULL)
    {
        unsigned int w[6];

        if (sscanf(g_ambeRateArg, "%x,%x,%x,%x,%x,%x", &w[0], &w[1], &w[2], &w[3], &w[4], &w[5]) != 6)
        {
            fprintf(stderr, "--ambe-rate needs six comma separated hex words\n");
            return false;
        }
        for (unsigned int k = 0U; k < 6U; k++)
            a->rate[k] = (uint16_t)w[k];
    }
    if (g_ambeIndex >= 0)
        a->rateIndex = g_ambeIndex;

    if (!ambeOpen(a, dev, g_ambeBaud))
        return false;

    fprintf(stderr, "AMBE: %s opened at %u baud%s%s\n", dev, g_ambeBaud, a->product[0] ? ", chip " : "", a->product);
    return true;
}

/*
 * Sends a test signal through the chip and sees which arrangement of the 72 bit block gives valid
 * NXDN FEC words: the block as on the air, coding order, or no parity at all.
 */
static unsigned int detectLayout(Ambe* a)
{
    unsigned long err[3] = { 0UL, 0UL, 0UL };
    unsigned int frames = 0U;
    double ph = 0.0;
    uint32_t rnd = 12345U;

    if (g_layoutArg >= 0)
        return (unsigned int)g_layoutArg;

    for (unsigned int f = 0U; f < 60U; f++)
    {
        int16_t pcm[AMBE_SAMPLES];
        uint8_t chip[AMBE_BLOCK_BYTES], net[7];
        double amp = 600.0 + 150.0 * (f % 20U), freq = 250.0 + 40.0 * f;

        for (unsigned int k = 0U; k < AMBE_SAMPLES; k++)
        {
            rnd = rnd * 1664525U + 1013904223U;
            ph += 2.0 * M_PI * freq / 8000.0;
            pcm[k] = (int16_t)lrint(amp * sin(ph) + ((int)(rnd >> 24) - 128) * 3.0);
        }
        if (!ambeEncode(a, pcm, chip))
            break;
        for (unsigned int l = 0U; l < 3U; l++)
            err[l] += nxdnAudioChipDecode(l, chip, net);
        frames++;
    }

    if (frames == 0U)
        return NXDN_CHIP_NOFEC;

    /* a layout is right if its FEC words are (nearly) always valid; with no parity, if the padding stays zero */
    unsigned int best = NXDN_CHIP_AIR;

    if (err[NXDN_CHIP_CODING] < err[NXDN_CHIP_AIR])
        best = NXDN_CHIP_CODING;
    if (err[best] * 4UL > frames && err[NXDN_CHIP_NOFEC] == 0UL)
        best = NXDN_CHIP_NOFEC;

    fprintf(stderr, "AMBE: block layout detected from %u test frames: %s (corrected bits per frame: air %.2f, coding order %.2f; "
                    "no-parity padding bits set %.2f)\n", frames,
            best == NXDN_CHIP_AIR ? "air" : (best == NXDN_CHIP_CODING ? "coding order" : "no parity"),
            (double)err[NXDN_CHIP_AIR] / frames, (double)err[NXDN_CHIP_CODING] / frames,
            (double)err[NXDN_CHIP_NOFEC] / frames);
    return best;
}

/* 16 bit mono 8 kHz WAV (or raw signed 16 bit little endian if the name ends in .raw or .s16) */
static int16_t* readSpeech(const char* path, size_t* count)
{
    FILE* fp = fopen(path, "rb");
    uint8_t hdr[12];
    size_t len;
    int16_t* pcm;
    bool raw = false;

    if (!fp)
    {
        perror(path);
        return NULL;
    }

    const char* dot = strrchr(path, '.');
    if (dot != NULL && (!strcmp(dot, ".raw") || !strcmp(dot, ".s16")))
        raw = true;

    if (!raw)
    {
        if (fread(hdr, 1, 12, fp) != 12U || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        {
            fprintf(stderr, "%s is not a WAV file\n", path);
            fclose(fp);
            return NULL;
        }

        bool gotFmt = false;
        for (;;)
        {
            uint8_t ck[8];

            if (fread(ck, 1, 8, fp) != 8U)
            {
                fprintf(stderr, "%s has no data chunk\n", path);
                fclose(fp);
                return NULL;
            }
            len = (size_t)ck[4] | ((size_t)ck[5] << 8) | ((size_t)ck[6] << 16) | ((size_t)ck[7] << 24);

            if (!memcmp(ck, "fmt ", 4))
            {
                uint8_t f[16];

                if (len < 16U || fread(f, 1, 16, fp) != 16U)
                {
                    fclose(fp);
                    return NULL;
                }
                unsigned int fmt = f[0] | (f[1] << 8), ch = f[2] | (f[3] << 8);
                unsigned int sr = f[4] | (f[5] << 8) | (f[6] << 16) | ((unsigned int)f[7] << 24);
                unsigned int bps = f[14] | (f[15] << 8);

                if (fmt != 1U || ch != 1U || sr != 8000U || bps != 16U)
                {
                    fprintf(stderr, "%s must be mono, 8000 Hz, 16 bit PCM (it is %u ch, %u Hz, %u bit, format %u)\n",
                            path, ch, sr, bps, fmt);
                    fclose(fp);
                    return NULL;
                }
                fseek(fp, (long)(len - 16U + (len & 1U)), SEEK_CUR);
                gotFmt = true;
            }
            else if (!memcmp(ck, "data", 4))
            {
                if (!gotFmt)
                {
                    fprintf(stderr, "%s: data before fmt\n", path);
                    fclose(fp);
                    return NULL;
                }
                break;
            }
            else
                fseek(fp, (long)(len + (len & 1U)), SEEK_CUR);
        }
    }
    else
    {
        fseek(fp, 0, SEEK_END);
        len = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
    }

    pcm = calloc(len / 2U + AMBE_SAMPLES, sizeof(int16_t));
    size_t n = fread(pcm, 1, len, fp) / 2U;
    fclose(fp);

    for (size_t k = 0U; k < n; k++)                 /* little endian file to host order */
    {
        const uint8_t* p = (const uint8_t*)&pcm[k];
        pcm[k] = (int16_t)(p[0] | (p[1] << 8));
    }

    *count = n;
    return pcm;
}

/* Scales and reports the level of the speech going to the chip */
static void prepSpeech(int16_t* pcm, size_t count)
{
    size_t clipped = 0U;
    double sum = 0.0;

    for (size_t k = 0U; k < count; k++)
    {
        double v = pcm[k] * g_gain;

        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        pcm[k] = (int16_t)lrint(v);
        sum += v * v;
        if (pcm[k] >= 32760 || pcm[k] <= -32760)
            clipped++;
    }

    fprintf(stderr, "speech: %zu samples, rms %.0f, %zu at full scale%s\n", count, count ? sqrt(sum / count) : 0.0,
            clipped, clipped * 100U > count ? " - clipped, try --gain 0.3" : "");
}

static void le16(uint8_t* p, unsigned int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t* p, unsigned int v) { le16(p, v & 0xFFFFU); le16(p + 2, v >> 16); }

static bool writeWav(const char* path, const int16_t* pcm, size_t count)
{
    FILE* fp = fopen(path, "wb");
    uint8_t h[44];

    if (!fp)
    {
        perror(path);
        return false;
    }
    memcpy(h, "RIFF", 4); le32(h + 4, (unsigned int)(36U + count * 2U)); memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16U); le16(h + 20, 1U); le16(h + 22, 1U); le32(h + 24, 8000U); le32(h + 28, 16000U);
    le16(h + 32, 2U); le16(h + 34, 16U); memcpy(h + 36, "data", 4); le32(h + 40, (unsigned int)(count * 2U));
    fwrite(h, 1, sizeof(h), fp);

    for (size_t k = 0U; k < count; k++)
    {
        uint8_t b[2];
        double v = pcm[k] * g_outGain;

        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        le16(b, (uint16_t)(int16_t)lrint(v));
        fwrite(b, 1, 2, fp);
    }
    fclose(fp);
    return true;
}

static unsigned int popcount8(uint8_t v);

static int doAmbeTest(int argc, char** argv)
{
    const char* dev = NULL;
    const char* in = NULL;

    for (int i = 0; i < argc; i++)
    {
        if (ambeOption(argc, argv, &i)) continue;
        if (argv[i][0] != '-' && dev == NULL) dev = argv[i];
        else if (argv[i][0] != '-') in = argv[i];
        else { usage(); return 1; }
    }
    if (dev == NULL)
        dev = g_ambeDev;
    if (dev == NULL) { usage(); return 1; }

    Ambe a;
    if (!ambeStart(&a, dev))
        return 1;

    size_t count = 0U;
    int16_t* pcm;

    if (in != NULL)
    {
        pcm = readSpeech(in, &count);
        if (pcm == NULL)
        {
            ambeClose(&a);
            return 1;
        }
        prepSpeech(pcm, count);
    }
    else
    {
        /* a swept tone, 1.5 s: it is not speech but shows that something sensible comes back */
        count = 12000U;
        pcm = calloc(count + AMBE_SAMPLES, sizeof(int16_t));
        double ph = 0.0;
        for (size_t k = 0U; k < count; k++)
        {
            double f = 300.0 + 1500.0 * k / count;
            ph += 2.0 * M_PI * f / 8000.0;
            pcm[k] = (int16_t)lrint(8000.0 * sin(ph));
        }
    }

    size_t frames = count / AMBE_SAMPLES;
    FILE* dumpFp = g_dump != NULL ? fopen(g_dump, "w") : NULL;
    int16_t* out = calloc(frames * AMBE_SAMPLES + AMBE_SAMPLES, sizeof(int16_t));
    size_t good = 0U;
    double inE = 0.0, outE = 0.0;
    unsigned long layoutErr[3] = { 0UL, 0UL, 0UL };

    for (size_t f = 0U; f < frames; f++)
    {
        uint8_t block[AMBE_BLOCK_BYTES];

        if (!ambeEncode(&a, pcm + f * AMBE_SAMPLES, block))
        {
            fprintf(stderr, "encode failed at frame %zu\n", f);
            break;
        }
        if (dumpFp != NULL)
        {
            double e = 0.0;

            for (unsigned int k = 0U; k < AMBE_SAMPLES; k++)
                e += (double)pcm[f * AMBE_SAMPLES + k] * pcm[f * AMBE_SAMPLES + k];
            for (unsigned int k = 0U; k < AMBE_BLOCK_BYTES; k++)
                fprintf(dumpFp, "%02X", block[k]);
            fprintf(dumpFp, " %.0f\n", sqrt(e / AMBE_SAMPLES));        /* frame rms of the input */
        }
        if (!ambeDecode(&a, block, out + f * AMBE_SAMPLES))
        {
            fprintf(stderr, "decode failed at frame %zu\n", f);
            break;
        }
        {
            uint8_t net[7];

            for (unsigned int l = 0U; l < 3U; l++)
                layoutErr[l] += nxdnAudioChipDecode(l, block, net);
        }
        if (f < 3U)
            printf("frame %zu: %02X %02X %02X %02X %02X %02X %02X %02X %02X\n", f, block[0], block[1], block[2], block[3],
                   block[4], block[5], block[6], block[7], block[8]);
        good++;
    }

    for (size_t k = 0U; k < good * AMBE_SAMPLES; k++)
    {
        inE  += (double)pcm[k] * pcm[k];
        outE += (double)out[k] * out[k];
    }

    printf("%zu of %zu frames went through the chip, input level %.0f rms, output level %.0f rms\n", good, frames,
           good ? sqrt(inE / (good * AMBE_SAMPLES)) : 0.0, good ? sqrt(outE / (good * AMBE_SAMPLES)) : 0.0);
    if (good > 0U)
        printf("layout check over %zu blocks (average per block):\n"
               "  as the air block      %.2f corrected bits\n"
               "  in coding order       %.2f corrected bits\n"
               "  no parity             %.2f padding bits set\n"
               "  (a layout whose average is near 0 is the chip's; 3 or more means it is not)\n",
               good, (double)layoutErr[NXDN_CHIP_AIR] / good, (double)layoutErr[NXDN_CHIP_CODING] / good,
               (double)layoutErr[NXDN_CHIP_NOFEC] / good);
    if (good > 0U)
    {
        writeWav("ambe_test_out.wav", out, good * AMBE_SAMPLES);
        printf("decoded audio written to ambe_test_out.wav\n");
    }

    if (dumpFp != NULL)
    {
        fclose(dumpFp);
        printf("the chip's blocks are in %s\n", g_dump);
    }
    ambeClose(&a);
    free(pcm);
    free(out);
    return good == frames && frames > 0U ? 0 : 1;
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
    const char* speech = NULL;
    uint8_t* ambeBlocks = NULL;

    for (int i = 0; i < argc; i++)
    {
        if (ambeOption(argc, argv, &i)) continue;
        if (!strcmp(argv[i], "--speech") && i + 1 < argc) speech = argv[++i];
        else if (!strcmp(argv[i], "--sweep") && i + 1 < argc) sweepMax = atof(argv[++i]);
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

    if (speech != NULL)
    {
        Ambe a;
        size_t count = 0U;
        int16_t* pcm;

        if (g_ambeDev == NULL)
        {
            fprintf(stderr, "--speech needs --ambe <serial port>\n");
            return 1;
        }
        pcm = readSpeech(speech, &count);
        if (pcm == NULL)
            return 1;
        prepSpeech(pcm, count);
        if (!ambeStart(&a, g_ambeDev))
        {
            free(pcm);
            return 1;
        }

        g_layout = detectLayout(&a);

        size_t blocks = (count + AMBE_SAMPLES - 1U) / AMBE_SAMPLES;
        size_t nFrames = (blocks + 3U) / 4U;

        if (nFrames + 2U > MAX_FRAMES)
        {
            fprintf(stderr, "speech too long: %zu s of audio, the limit is %u s\n", count / 8000U, MAX_FRAMES * 80U / 1000U - 1U);
            free(pcm);
            ambeClose(&a);
            return 1;
        }

        ambeBlocks = calloc(nFrames * 4U * 7U, 1);      /* 49 bit blocks for the frame generator */
        for (size_t b = 0U; b < nFrames * 4U; b++)
        {
            int16_t chunk[AMBE_SAMPLES];

            memset(chunk, 0, sizeof(chunk));
            if (b < blocks)
            {
                size_t n = count - b * AMBE_SAMPLES;
                memcpy(chunk, pcm + b * AMBE_SAMPLES, (n < AMBE_SAMPLES ? n : AMBE_SAMPLES) * sizeof(int16_t));
            }
            uint8_t chip[AMBE_BLOCK_BYTES];

            if (ambeEncode(&a, chunk, chip))
                nxdnAudioChipDecode(g_layout, chip, ambeBlocks + b * 7U);    /* the generator adds the NXDN FEC */
            else
            {
                fprintf(stderr, "AMBE encode failed at block %zu\n", b);
                free(pcm);
                free(ambeBlocks);
                ambeClose(&a);
                return 1;
            }
        }
        ambeClose(&a);
        free(pcm);
        voice = (unsigned int)nFrames;
        data = 0U;
        printf("speech: %zu blocks of 20 ms -> %zu voice frames\n", blocks, nFrames);
    }

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
            nf = nxdnGenVoiceCallAudio((uint8_t)ran, (uint16_t)csrc, (uint16_t)dst, group, voice, ambeBlocks, frames,
                                       MAX_FRAMES, 1U);
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
    free(ambeBlocks);
    return 0;
}

/* ------------------------------------------------------------------ decoder */

static unsigned int g_frames = 0U, g_lost = 0U;
static bool g_verbose = false;

/* Voice goes through an AMBE3000R to this buffer when --ambe and --wav are given */
static Ambe g_ambe;
static bool g_ambeOn = false;
static int16_t* g_pcm = NULL;
static size_t g_pcmCount = 0U, g_pcmCap = 0U;

static void audioBlocks(const uint8_t* net)
{
    for (unsigned int b = 0U; b < 4U; b++)
    {
        uint8_t block[AMBE_BLOCK_BYTES], bits[7];
        const uint8_t* grp = net + (b >> 1) * 14U;
        unsigned int off = (b & 1U) * 49U;

        /* the 49 parameter bits, error corrected by the NXDN FEC, go to the chip as they are */
        memset(bits, 0, sizeof(bits));
        for (unsigned int i = 0U; i < 49U; i++)
            if (grp[(off + i) >> 3] & (0x80U >> ((off + i) & 7U)))
                bits[i >> 3] |= (uint8_t)(0x80U >> (i & 7U));
        nxdnAudioChipEncode(g_layout, bits, block);

        if (g_pcmCount + AMBE_SAMPLES > g_pcmCap)
        {
            g_pcmCap = g_pcmCap ? g_pcmCap * 2U : 80000U;
            g_pcm = realloc(g_pcm, g_pcmCap * sizeof(int16_t));
        }
        if (!ambeDecode(&g_ambe, block, g_pcm + g_pcmCount))
        {
            g_ambeOn = false;
            return;
        }
        g_pcmCount += AMBE_SAMPLES;
    }
}

/*
 * Voice error statistics.  "BER" is counted the way MMDVMHost and nxdn_host count
 * it: Golay-corrected bits over the 188 protected bits of a voice frame.  "raw"
 * compares all 288 received voice bits with a re-encode of the corrected data,
 * which also sees errors in the unprotected bits.
 */
static unsigned long g_callErrs = 0UL, g_callBits = 0UL, g_callRawErrs = 0UL, g_callRawBits = 0UL;
static unsigned long g_totErrs = 0UL, g_totBits = 0UL, g_totRawErrs = 0UL, g_totRawBits = 0UL;

static unsigned int popcount8(uint8_t v)
{
    unsigned int n = 0U;
    for (; v; v >>= 1)
        n += v & 1U;
    return n;
}

static void callSummary(const char* why)
{
    if (g_callBits > 0UL)
        printf("  voice BER %.2f%% (%lu errors / %lu bits), raw channel BER %.2f%% (%lu / %lu) [%s]\n",
               100.0 * g_callErrs / g_callBits, g_callErrs, g_callBits,
               100.0 * g_callRawErrs / g_callRawBits, g_callRawErrs, g_callRawBits, why);
    g_totErrs += g_callErrs;
    g_totBits += g_callBits;
    g_totRawErrs += g_callRawErrs;
    g_totRawBits += g_callRawBits;
    g_callErrs = g_callBits = g_callRawErrs = g_callRawBits = 0UL;
}

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
        callSummary("sync lost");
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

    if (NXDN_LICH_FCT(lich) == NXDN_LICH_USC_SACCH_SS && NXDN_LICH_OPTION(lich) == NXDN_LICH_STEAL_NONE)
    {
        const uint8_t* aud = frame + NXDN_FSW_LICH_SACCH_LENGTH_BYTES;
        uint8_t net[28], again[36];
        unsigned int errors = 0U;

        memset(net, 0, sizeof(net));
        nxdnAudioDecode(aud + 0U, net + 0U, &errors);
        nxdnAudioDecode(aud + 18U, net + 14U, &errors);
        nxdnAudioEncode(net, again + 0U);
        nxdnAudioEncode(net + 14U, again + 18U);

        for (unsigned int i = 0U; i < 36U; i++)
            g_callRawErrs += popcount8((uint8_t)(aud[i] ^ again[i]));

        if (g_ambeOn)
            audioBlocks(net);

        g_callErrs += errors;
        g_callBits += 188UL;
        g_callRawBits += 288UL;
    }

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
        if (NXDN_L3_TYPE(l3) == NXDN_MESSAGE_TYPE_TX_REL)
            callSummary("end of call");
    }
    else if (g_verbose)
        printf("  frame %u: %s RAN %d\n", g_frames, kind, ran);
}


enum { FMT_CS8 = 0, FMT_CS16, FMT_CF32 };

static size_t fmtBytes(int fmt)
{
    return fmt == FMT_CF32 ? 8U : (fmt == FMT_CS16 ? 4U : 2U);
}

/* read up to max IQ pairs, scaled so full scale is about 1.0 */
static size_t readIq(FILE* fp, int fmt, double* iq, size_t max)
{
    uint8_t raw[16384];
    size_t bytes = fmtBytes(fmt);
    size_t want = max * bytes;
    size_t n = 0U;

    if (want > sizeof(raw)) want = sizeof(raw) - (sizeof(raw) % bytes);
    size_t got = fread(raw, 1, want, fp);
    size_t pairs = got / bytes;
    for (size_t k = 0U; k < pairs; k++)
    {
        if (fmt == FMT_CF32)
        {
            float f[2];
            memcpy(f, raw + k * 8U, 8U);
            iq[2U * n] = f[0];
            iq[2U * n + 1U] = f[1];
        }
        else if (fmt == FMT_CS16)
        {
            int16_t v[2];
            memcpy(v, raw + k * 4U, 4U);
            iq[2U * n] = v[0] / 32768.0;
            iq[2U * n + 1U] = v[1] / 32768.0;
        }
        else
        {
            iq[2U * n] = (int8_t)raw[2U * k] / 128.0;
            iq[2U * n + 1U] = (int8_t)raw[2U * k + 1U] / 128.0;
        }
        n++;
    }
    return n;
}

/* strongest carrier in the recording, found with an averaged FFT over blocks spread through the file */
static bool findCarrier(FILE* fp, int fmt, unsigned int rate, double* offset)
{
    enum { N = 65536, BLOCKS = 24 };
    static double power[N];
    static double re[N], im[N];
    double* iq = malloc(sizeof(double) * 2U * N);

    fseek(fp, 0, SEEK_END);
    long total = ftell(fp) / (long)fmtBytes(fmt);
    if (total < N) { free(iq); fseek(fp, 0, SEEK_SET); return false; }

    memset(power, 0, sizeof(power));
    unsigned int used = 0U;
    for (int b = 0; b < BLOCKS; b++)
    {
        long at = (total - N) * b / (BLOCKS - 1);
        fseek(fp, at * (long)fmtBytes(fmt), SEEK_SET);
        size_t have = 0U;
        while (have < N)
        {
            size_t r = readIq(fp, fmt, iq + 2U * have, N - have);
            if (r == 0U) break;
            have += r;
        }
        if (have != N) continue;
        for (int k = 0; k < N; k++)
        {
            double w = 0.5 - 0.5 * cos(2.0 * M_PI * k / N);
            re[k] = iq[2 * k] * w;
            im[k] = iq[2 * k + 1] * w;
        }
        /* in-place radix-2 FFT */
        for (unsigned int i = 1U, j = 0U; i < N; i++)
        {
            unsigned int bit = N >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j)
            {
                double t = re[i]; re[i] = re[j]; re[j] = t;
                t = im[i]; im[i] = im[j]; im[j] = t;
            }
        }
        for (unsigned int len = 2U; len <= N; len <<= 1)
        {
            double ang = -2.0 * M_PI / len;
            double wr = cos(ang), wi = sin(ang);
            for (unsigned int i = 0U; i < N; i += len)
            {
                double cr = 1.0, ci = 0.0;
                for (unsigned int k = 0U; k < len / 2U; k++)
                {
                    unsigned int u = i + k, v = i + k + len / 2U;
                    double xr = re[v] * cr - im[v] * ci, xi = re[v] * ci + im[v] * cr;
                    re[v] = re[u] - xr; im[v] = im[u] - xi;
                    re[u] += xr; im[u] += xi;
                    double nr = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr;
                    cr = nr;
                }
            }
        }
        for (int k = 0; k < N; k++)
            power[k] += re[k] * re[k] + im[k] * im[k];
        used++;
    }
    free(iq);
    fseek(fp, 0, SEEK_SET);
    if (used == 0U) return false;

    /* smooth over ~12.5 kHz (an FM signal is spread over its deviation) and skip the DC spike */
    int half = (int)(6250.0 * N / rate);
    if (half < 1) half = 1;
    int skip = (int)(2000.0 * N / rate) + 1;
    double best = 0.0, sum = 0.0;
    int bestK = 0;
    for (int k = 0; k < N; k++)
        sum += power[k];
    double mean = sum / N;
    double run = 0.0;
    for (int d = -half; d <= half; d++)
        run += power[(d + N) % N];
    for (int k = 0; k < N; k++)
    {
        int kk = k < N / 2 ? k : k - N;
        if (abs(kk) > skip && run > best)
        {
            best = run;
            bestK = kk;
        }
        run += power[(k + half + 1) % N] - power[(k - half + N) % N];
    }
    double ratio = best / ((2 * half + 1) * mean);
    *offset = (double)bestK * rate / N;
    printf("carrier search: strongest signal at %+.0f Hz from the centre, %.1f dB above the average\n", *offset,
           10.0 * log10(ratio));
    return ratio > 3.0;
}

static int doDec(int argc, char** argv)
{
    const char* in = NULL;
    unsigned int rate = 2400000U;
    double offset = 0.0;
    bool invert = false;
    unsigned int shape = NXDN_SHAPE_RRC;
    const char* wav = NULL;
    int fmt = FMT_CS8;
    bool fmtSet = false, rateSet = false, offSet = false, autoOff = false;

    for (int i = 0; i < argc; i++)
    {
        if (ambeOption(argc, argv, &i)) continue;
        if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--format") && i + 1 < argc)
        {
            const char* f = argv[++i];
            fmt = !strcmp(f, "cf32") ? FMT_CF32 : (!strcmp(f, "cs16") ? FMT_CS16 : FMT_CS8);
            fmtSet = true;
        }
        else if (!strcmp(argv[i], "--auto")) autoOff = true;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) { rate = (unsigned int)atoi(argv[++i]); rateSet = true; }
        else if (!strcmp(argv[i], "--offset") && i + 1 < argc) { offset = atof(argv[++i]); offSet = true; }
        else if (!strcmp(argv[i], "--invert")) invert = true;
        else if (!strcmp(argv[i], "--shape") && i + 1 < argc) shape = !strcmp(argv[++i], "rc") ? NXDN_SHAPE_RC : NXDN_SHAPE_RRC;
        else if (!strcmp(argv[i], "-v")) g_verbose = true;
        else if (argv[i][0] != '-') in = argv[i];
        else { usage(); return 1; }
    }
    (void)offSet;
    if (in)
    {
        /* gqrx names its recordings gqrx_DATE_TIME_FREQ_RATE_fc.raw: float32 IQ at the input rate */
        size_t len = strlen(in);
        if (len > 7U && !strcmp(in + len - 7U, "_fc.raw"))
        {
            if (!fmtSet) fmt = FMT_CF32;
            if (!rateSet)
            {
                const char* e = in + len - 7U;
                const char* b = e;
                while (b > in && b[-1] >= '0' && b[-1] <= '9') b--;
                if (e - b >= 5)
                {
                    rate = (unsigned int)atoi(b);
                    printf("gqrx recording: float32 IQ at %u sps (from the file name)\n", rate);
                }
            }
        }
    }
    if (!in || rate < BASE_RATE || (rate % BASE_RATE) != 0U)
    {
        usage();
        return 1;
    }

    if ((g_ambeDev != NULL) != (wav != NULL))
    {
        fprintf(stderr, "--ambe and --wav go together\n");
        return 1;
    }

    FILE* fp = fopen(in, "rb");
    if (!fp) { perror(in); return 1; }

    if (autoOff)
    {
        double found;
        if (findCarrier(fp, fmt, rate, &found))
            offset = found;
        else
            printf("carrier search: no clear signal, using offset %.0f Hz\n", offset);
    }

    if (g_ambeDev != NULL)
    {
        if (!ambeStart(&g_ambe, g_ambeDev))
        {
            fclose(fp);
            return 1;
        }
        g_layout = detectLayout(&g_ambe);
        g_ambeOn = true;
    }

    unsigned int dec = rate / BASE_RATE;
    NxdnRx* rx = calloc(1, sizeof(NxdnRx));
    nxdnRxInit(rx, onFrame, NULL, (uint8_t)shape);

    /* mix to baseband, low-pass and decimate to 24 kHz by averaging, then FM discriminate */
    double mixPh = 0.0, mixStep = -2.0 * M_PI * offset / rate;
    double pi = 1.0, pq = 0.0;
    double ai = 0.0, aq = 0.0, dc = 0.0;
    unsigned int cnt = 0U, nDc = 0U;
    double sgn = invert ? 1.0 : -1.0;
    int16_t out[4096];
    unsigned int on = 0U;
    double* iq = malloc(sizeof(double) * 2U * 4096U);
    size_t got;
    bool first = true;

    while ((got = readIq(fp, fmt, iq, 4096U)) > 0U)
    {
        for (size_t i = 0U; i < got; i++)
        {
            double si = iq[2U * i], sq = iq[2U * i + 1U];
            double c = cos(mixPh), s = sin(mixPh);
            ai += si * c - sq * s;
            aq += si * s + sq * c;
            mixPh += mixStep;
            if (mixPh > M_PI) mixPh -= 2.0 * M_PI;
            if (mixPh < -M_PI) mixPh += 2.0 * M_PI;

            if (++cnt == dec)
            {
                double xi = ai, xq = aq;
                ai = aq = 0.0; cnt = 0U;
                /* arg(x[n] * conj(x[n-1])) at 24 kHz */
                double di = xi * pi + xq * pq, dq = xq * pi - xi * pq;
                pi = xi; pq = xq;
                if (first) { first = false; continue; }

                double hz = atan2(dq, di) * BASE_RATE / (2.0 * M_PI);
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
    free(iq);
    if (on)
        nxdnRxSamples(rx, out, on);
    fclose(fp);

    callSummary("end of file");

    if (wav != NULL)
    {
        if (g_pcmCount > 0U && writeWav(wav, g_pcm, g_pcmCount))
            printf("voice: %zu samples (%.1f s) -> %s\n", g_pcmCount, g_pcmCount / 8000.0, wav);
        else if (g_pcmCount == 0U)
            printf("voice: no audio decoded%s\n", g_ambeOn ? "" : " (the dongle stopped answering)");
        ambeClose(&g_ambe);
        free(g_pcm);
    }
    if (g_totBits > 0UL)
        printf("all voice: BER %.2f%% (%lu / %lu), raw %.2f%% (%lu / %lu)\n", 100.0 * g_totErrs / g_totBits,
               g_totErrs, g_totBits, 100.0 * g_totRawErrs / g_totRawBits, g_totRawErrs, g_totRawBits);
    printf("frames %u  lost %u  candidates %u locks %u dropped %u  carrier offset %.0f Hz\n",
           rx->statFrames, rx->statLost, rx->statCandidates, rx->statLocks, rx->statDropped, dc);
    if (g_frames == 0U && !autoOff)
        printf("nothing decoded: if the signal is not at the centre of the recording, try --auto or --offset hz\n");
    free(rx);
    return g_frames > 0U ? 0 : 2;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && !strcmp(argv[1], "gen"))
        return doGen(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "dec"))
        return doDec(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "ambe-test"))
        return doAmbeTest(argc - 2, argv + 2);
    usage();
    return 1;
}
