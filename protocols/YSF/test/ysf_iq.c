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
 * ysf_iq - YSF (C4FM, 9600 bit/s) test signal generator and decoder for SDR hardware.
 *
 *   ysf_iq gen [options] -o call.cs8      write 8-bit signed IQ for hackrf_transfer -t
 *   ysf_iq dec [options] capture          decode a recording (hackrf_transfer -r, gqrx, ...)
 *
 * The IQ file is centred on the carrier.  The outer symbols deviate 2.7 kHz
 * (1.35 kHz with --low), the same levels as the host.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ysf_func.h"
#include "ysf_dsp.h"
#include "ysf_gen.h"
#include "ambe3000.h"
#include "nxdn_func.h"        /* the chip block <-> 49 bit conversion */
#include "ambe_util.h"

#define BASE_RATE       24000U
#define HZ_PER_COUNT    (2700.0 / 1890.0)   /* YSF_LEVEL_OUTER_H counts = 2.7 kHz */
#define MAX_FRAMES      1200U               /* two minutes */
#define FL              YSF_FRAME_LENGTH_BYTES

static void usage(void)
{
    fprintf(stderr,
        "ysf_iq gen [-o file.cs8] [-r rate] [--src call] [--dst call] [--dgid n] [--voice frames | --data frames]\n"
        "           [--repeat n] [--gap ms] [--offset hz] [--level pct] [--low] [--shape rrc|rc] [--invert]\n"
        "           [--raw24 file.s16] [--sweep maxhz [--step hz]]   one call per offset and polarity, source SWEEPnnn\n"
        "           [--speech in.wav --ambe /dev/ttyUSB0 [--baud 460800]]   voice from a WAV file (mono, 8000 Hz, 16 bit) through an AMBE3000R\n"
        "ysf_iq dec [-r rate] [--format cs8|cs16|cf32] [--offset hz | --auto] [--invert] [--shape rrc|rc] [--dump-vch file] [-v] file\n"
        "           gqrx recordings (name ends _fc.raw) are read as cf32 and the rate is taken from the name\n"
        "           [--ambe /dev/ttyUSB0 [--baud 460800] --wav out.wav]     decode the voice to a WAV file\n"
        "  AMBE options as for nxdn_iq: --layout nofec|air|coding, --ambe-rate w0,..,w5, --ambe-index n, --gain x, --out-gain x,\n"
        "  --ambe-hwreset, --ambe-init, --ambe-chunk, --ambe-gap, --ambe-flow, --ambe-debug (check the dongle with nxdn_iq ambe-test)\n"
        "  default rate 2400000 (must be a multiple of 24000); --level scales the deviation (100 = 2.7 kHz)\n"
        "  --low selects the low deviation setting (1.35 kHz); frames are V/D mode 2 voice (random bits) or data FR mode\n");
}

static double catmull(double p0, double p1, double p2, double p3, double t)
{
    return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t);
}

/* ------------------------------------------------------------------ generator */

static int doGen(int argc, char** argv)
{
    const char* out = "ysf_test.cs8";
    const char* raw24 = NULL;
    const char* src = "N0CALL";
    const char* dst = "ALL";
    unsigned int rate = 2400000U;
    unsigned int dgid = 0U;
    bool invert = false, low = false;
    unsigned int voice = 100U, data = 0U, repeat = 1U, gapMs = 500U;
    double offset = 0.0, level = 100.0;
    double sweepMax = 0.0, sweepStep = 1000.0;
    unsigned int shape = YSF_SHAPE_RRC;

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
        else if (!strcmp(argv[i], "--src") && i + 1 < argc) src = argv[++i];
        else if (!strcmp(argv[i], "--dst") && i + 1 < argc) dst = argv[++i];
        else if (!strcmp(argv[i], "--dgid") && i + 1 < argc) dgid = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--voice") && i + 1 < argc) { voice = (unsigned int)atoi(argv[++i]); data = 0U; }
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gap") && i + 1 < argc) gapMs = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--offset") && i + 1 < argc) offset = atof(argv[++i]);
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) level = atof(argv[++i]);
        else if (!strcmp(argv[i], "--low")) low = true;
        else if (!strcmp(argv[i], "--shape") && i + 1 < argc) shape = !strcmp(argv[++i], "rc") ? YSF_SHAPE_RC : YSF_SHAPE_RRC;
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
        size_t nFrames = (blocks + 4U) / 5U;

        if (nFrames + 2U > MAX_FRAMES)
        {
            fprintf(stderr, "speech too long: %zu s of audio, the limit is %u s\n", count / 8000U, (MAX_FRAMES - 2U) / 10U);
            free(pcm);
            ambeClose(&a);
            return 1;
        }

        ambeBlocks = calloc(nFrames * 5U * 7U, 1);      /* 49 bit blocks, 7 bytes each */
        for (size_t b = 0U; b < nFrames * 5U; b++)
        {
            int16_t chunk[AMBE_SAMPLES];
            uint8_t chip[AMBE_BLOCK_BYTES];

            memset(chunk, 0, sizeof(chunk));
            if (b < blocks)
            {
                size_t n = count - b * AMBE_SAMPLES;
                memcpy(chunk, pcm + b * AMBE_SAMPLES, (n < AMBE_SAMPLES ? n : AMBE_SAMPLES) * sizeof(int16_t));
            }
            if (ambeEncode(&a, chunk, chip))
                nxdnAudioChipDecode(g_layout, chip, ambeBlocks + b * 7U);
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

    static uint8_t frames[MAX_FRAMES][FL];
    unsigned int nf = 0U;
    unsigned int nSeg = 0U;
    double segOff[512];
    bool segInv[512];
    size_t segEnd[512];

    if (sweepMax > 0.0)
    {
        if (sweepStep < 100.0) sweepStep = 100.0;
        if (voice > 40U) voice = 20U;
        data = 0U;
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

    size_t cap = (size_t)repeat * ((data ? data : voice) + 6U) * YSF_SAMPLES_PER_FRAME + (size_t)repeat * (gapMs * 24U + 2000U) + 8000U;
    int16_t* base = calloc(cap, sizeof(int16_t));
    size_t n = 0U;
    YsfTx tx;

    ysfTxInit(&tx, (low ? YSF_LEVEL_OUTER_L : YSF_LEVEL_OUTER_H) * (float)(level / 100.0), (uint8_t)shape);

    for (unsigned int r = 0U; r < repeat; r++)
    {
        char csrc[16];

        if (sweepMax > 0.0)
            snprintf(csrc, sizeof(csrc), "SWEEP%03u", r);
        else
            snprintf(csrc, sizeof(csrc), "%s", src);

        if (data > 0U)
            nf = ysfGenDataCall(dst, csrc, (uint8_t)dgid, data, frames, MAX_FRAMES);
        else
            nf = ysfGenVoiceCallAudio(dst, csrc, (uint8_t)dgid, voice, ambeBlocks, frames, MAX_FRAMES);
        if (nf == 0U)
        {
            fprintf(stderr, "call too long (limit %u frames)\n", MAX_FRAMES - 2U);
            free(base);
            free(ambeBlocks);
            return 1;
        }

        n += ysfTxPreamble(&tx, 80U, base + n);
        for (unsigned int f = 0U; f < nf; f++)
            n += ysfTxFrame(&tx, frames[f], base + n);
        n += ysfTxFlush(&tx, base + n);
        n += (size_t)gapMs * 24U;       /* unmodulated carrier between calls */

        if (sweepMax > 0.0)
            printf("source %s: offset %+.0f Hz, %s polarity\n", csrc, segOff[r], segInv[r] ? "inverted" : "normal");
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

    FILE* fp = fopen(out, "wb");
    if (!fp) { perror(out); free(base); return 1; }

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
    free(ambeBlocks);
    printf("Transmit (low power into a dummy load or your own receiver, licensed frequency only):\n"
           "  hackrf_transfer -t %s -f <Hz> -s %u -x <0-47> -R\n", out, rate);
    free(base);
    return 0;
}

/* ------------------------------------------------------------------ decoder */

static unsigned int g_frames = 0U, g_lost = 0U, g_calls = 0U, g_badFich = 0U;
static bool g_verbose = false;
static YsfCtrl g_ctrl;
static FILE* g_dump = NULL;     /* --dump-vch: the voice channels of V/D mode 2 frames, one hex line per 20 ms */

static const uint8_t WHITE[] = {0x93U, 0xD7U, 0x51U, 0x21U, 0x9CU, 0x2FU, 0x6CU, 0xD0U, 0xEFU, 0x0FU, 0xF8U, 0x3DU, 0xF1U};

/*
 * One 20 ms voice channel (104 bits, de-whitened, the repeated bits voted) to the chip: the first 27 bits
 * and the next 22 are the 49 bits of the vocoder; the last bit is not used.
 */
static bool g_ambeOn = false;
static Ambe g_ambe;
static int16_t* g_pcm = NULL;
static size_t g_pcmCount = 0U, g_pcmCap = 0U;

static void audioBlock(const uint8_t* vch)
{
    uint8_t net[7], block[AMBE_BLOCK_BYTES];

    memset(net, 0, sizeof(net));
    for (unsigned int i = 0U; i < 27U; i++)
        if (vch[(3U * i) >> 3] & (0x80U >> ((3U * i) & 7U)))
            net[i >> 3] |= (uint8_t)(0x80U >> (i & 7U));
    for (unsigned int i = 0U; i < 22U; i++)
        if (vch[(81U + i) >> 3] & (0x80U >> ((81U + i) & 7U)))
            net[(27U + i) >> 3] |= (uint8_t)(0x80U >> ((27U + i) & 7U));

    nxdnAudioChipEncode(g_layout, net, block);

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

/*
 * The five voice channels of a V/D mode 2 frame: de-interleaved, de-whitened, the 27 repeated bits
 * voted.  Each line is 13 bytes (104 bits) as hex and the number of corrected triplets.
 */
static void voiceChannels(const uint8_t* frame)
{
    const uint8_t* data = frame + 30;

    for (unsigned int j = 0U; j < 5U; j++)
    {
        unsigned int offset = 40U + 144U * j;
        uint8_t vch[13] = {0};
        unsigned int fixed = 0U;

        for (unsigned int i = 0U; i < 104U; i++)
        {
            unsigned int p = offset + (i / 26U) + 4U * (i % 26U);

            if ((data[p >> 3] >> (7 - (p & 7U))) & 1U)
                vch[i >> 3] |= (uint8_t)(0x80U >> (i & 7U));
        }
        for (unsigned int i = 0U; i < 13U; i++)
            vch[i] ^= WHITE[i];
        for (unsigned int i = 0U; i < 81U; i += 3U)
        {
            unsigned int v = 0U;

            for (unsigned int k = 0U; k < 3U; k++)
                v += (vch[(i + k) >> 3] >> (7 - ((i + k) & 7U))) & 1U;
            if (v == 1U || v == 2U)
            {
                fixed++;
                for (unsigned int k = 0U; k < 3U; k++)
                {
                    if (v == 2U) vch[(i + k) >> 3] |= (uint8_t)(0x80U >> ((i + k) & 7U));
                    else vch[(i + k) >> 3] &= (uint8_t)~(0x80U >> ((i + k) & 7U));
                }
            }
        }
        if (g_dump != NULL)
        {
            for (unsigned int i = 0U; i < 13U; i++)
                fprintf(g_dump, "%02X", vch[i]);
            fprintf(g_dump, " %u\n", fixed);
        }
        if (g_ambeOn)
            audioBlock(vch);
    }
}

static const char* fiName(unsigned int fi)
{
    static const char* n[] = { "header", "communications", "terminator", "test" };
    return n[fi & 3U];
}

static const char* dtName(unsigned int dt)
{
    static const char* n[] = { "V/D mode 1", "data FR", "V/D mode 2", "voice FR" };
    return n[dt & 3U];
}

static void onEvent(void* ctx, const YsfCall* c)
{
    (void)ctx;
    switch (c->event)
    {
        case YSF_EV_RF_START:
        case YSF_EV_RF_LATE:
            g_calls++;
            printf("  call %s: %.10s -> %.10s  DG-ID %u  %s%s\n", c->event == YSF_EV_RF_LATE ? "(late entry)" : "start",
                   c->source, c->dest, c->dgid, dtName(c->dt), c->data ? " (data)" : "");
            break;
        case YSF_EV_RF_END:
            printf("  call end: %.10s -> %.10s, %.1f s, BER %.2f%%\n", c->source, c->dest, c->seconds, c->ber);
            break;
        case YSF_EV_RF_LOST:
            printf("  call lost: %.10s -> %.10s, %.1f s, BER %.2f%%\n", c->source, c->dest, c->seconds, c->ber);
            break;
        case YSF_EV_RF_REJECTED:
            printf("  call rejected: %.10s\n", c->source);
            break;
        default:
            break;
    }
}

static void onNet(void* ctx, const uint8_t* body, bool end)
{
    (void)ctx; (void)body; (void)end;
}

static void onFrame(void* ctx, const uint8_t* frame, bool lost)
{
    (void)ctx;

    if (lost)
    {
        g_lost++;
        printf("  [lost sync]\n");
        ysfCtrlRf(&g_ctrl, frame, true);
        return;
    }

    YsfFich f;
    if (!ysfFichDecode(&f, frame))
    {
        g_badFich++;
        if (g_verbose)
            printf("  (FICH not valid)\n");
    }
    else
    {
        g_frames++;
        if ((g_dump != NULL || g_ambeOn) && ysfFichGetFI(&f) == YSF_FI_COMMUNICATIONS && ysfFichGetDT(&f) == YSF_DT_VD_MODE2)
            voiceChannels(frame);
        if (g_verbose)
            printf("  frame %u: %s DT %u (%s) FN %u/%u DG-ID %u\n", g_frames, fiName(ysfFichGetFI(&f)), ysfFichGetDT(&f),
                   dtName(ysfFichGetDT(&f)), ysfFichGetFN(&f), ysfFichGetFT(&f), ysfFichGetDGId(&f));
    }

    ysfCtrlRf(&g_ctrl, frame, false);
    ysfCtrlClock(&g_ctrl, YSF_FRAME_TIME_MS);
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

    /* smooth over ~12.5 kHz (the C4FM signal is spread over its deviation) and skip the DC spike */
    int half = (int)(6250.0 * N / rate);
    if (half < 1) half = 1;
    int skip = (int)(300.0 * N / rate) + 1;
    double best = 0.0, sum = 0.0;
    int bestK = 0;

    /* the SDR's own DC spike: replace the bins around DC by the level just outside them, so that a signal
     * that really is at the centre of the recording (gqrx tuned on it) is still found */
    {
        double side = 0.0;
        for (int d = 1; d <= 8; d++)
            side += power[(skip + d) % N] + power[(N - skip - d) % N];
        side /= 16.0;
        for (int d = -skip; d <= skip; d++)
            power[(d + N) % N] = side;
    }
    for (int k = 0; k < N; k++)
        sum += power[k];
    double mean = sum / N;
    double run = 0.0;
    for (int d = -half; d <= half; d++)
        run += power[(d + N) % N];
    for (int k = 0; k < N; k++)
    {
        int kk = k < N / 2 ? k : k - N;
        if (run > best)
        {
            best = run;
            bestK = kk;
        }
        run += power[(k + half + 1) % N] - power[(k - half + N) % N];
    }
    double ratio = best / ((2 * half + 1) * mean);

    /* the smoothed peak is only good to a few kHz: refine with the centre of gravity of the signal around it */
    {
        int span = (int)(4000.0 * N / rate) + 1;
        double wsum = 0.0, psum = 0.0;

        for (int d = -span; d <= span; d++)
        {
            double pw = power[(bestK + d + N) % N];

            if (pw > mean * 2.0)
            {
                wsum += pw * d;
                psum += pw;
            }
        }
        if (psum > 0.0)
            bestK += (int)lrint(wsum / psum);
    }
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
    unsigned int shape = YSF_SHAPE_RRC;
    int fmt = FMT_CS8;
    bool fmtSet = false, rateSet = false, autoOff = false;

    const char* wav = NULL;

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
        else if (!strcmp(argv[i], "--offset") && i + 1 < argc) offset = atof(argv[++i]);
        else if (!strcmp(argv[i], "--invert")) invert = true;
        else if (!strcmp(argv[i], "--shape") && i + 1 < argc) shape = !strcmp(argv[++i], "rc") ? YSF_SHAPE_RC : YSF_SHAPE_RRC;
        else if (!strcmp(argv[i], "--dump-vch") && i + 1 < argc) { g_dump = fopen(argv[++i], "w"); if (!g_dump) { perror(argv[i]); return 1; } }
        else if (!strcmp(argv[i], "-v")) g_verbose = true;
        else if (argv[i][0] != '-') in = argv[i];
        else { usage(); return 1; }
    }
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

    if (autoOff)
    {
        double found;
        if (findCarrier(fp, fmt, rate, &found))
            offset = found;
        else
            printf("carrier search: no clear signal, using offset %.0f Hz\n", offset);
    }

    ysfCtrlInit(&g_ctrl);
    ysfCtrlSetCallsign(&g_ctrl, "N0CALL");
    g_ctrl.sendNet = onNet;
    g_ctrl.event = onEvent;

    unsigned int dec = rate / BASE_RATE;
    YsfRx* rx = calloc(1, sizeof(YsfRx));
    ysfRxInit(rx, onFrame, NULL, (uint8_t)shape);

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
                    ysfRxSamples(rx, out, on);
                    on = 0U;
                }
            }
        }
    }
    free(iq);
    if (on)
        ysfRxSamples(rx, out, on);
    fclose(fp);

    printf("frames %u (bad FICH %u)  lost %u  calls %u  candidates %u locks %u dropped %u  carrier offset %.0f Hz\n",
           g_frames, g_badFich, rx->statLost, g_calls, rx->statCandidates, rx->statLocks, rx->statDropped, dc);
    if (rx->statAmp > 0.0F)
        printf("outer symbol level %.0f counts = %.2f kHz deviation (nominal 2.7 kHz, 1.35 kHz low; reads about 3%% low)\n",
               rx->statAmp, rx->statAmp * HZ_PER_COUNT / 1000.0);
    if (g_frames == 0U && !autoOff)
        printf("nothing decoded: if the signal is not at the centre of the recording, try --auto or --offset hz\n");
    free(rx);
    if (wav != NULL)
    {
        if (g_pcmCount > 0U && writeWav(wav, g_pcm, g_pcmCount))
            printf("voice: %zu samples (%.1f s) -> %s\n", g_pcmCount, g_pcmCount / 8000.0, wav);
        else if (g_pcmCount == 0U)
            printf("voice: no audio decoded%s\n", g_ambeOn ? "" : " (the dongle stopped answering)");
        ambeClose(&g_ambe);
        free(g_pcm);
    }
    if (g_dump != NULL)
        fclose(g_dump);
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
