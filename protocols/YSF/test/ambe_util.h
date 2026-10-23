/* Created by Rick KD0OSS */
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
 * Shared with nxdn_iq: the AMBE3000R dongle options, start-up, block layout detection and the WAV
 * input/output.  The chip runs the same settings as for NXDN (AMBE+2 2450 + 1150 bit/s, 72 bit blocks);
 * the 49 vocoder bits are taken out of and put into the block with nxdnAudioChipDecode()/Encode().
 * Included once, by ysf_iq.c, after ambe3000.h and nxdn_func.h.
 */

static const char* g_ambeDev = NULL;
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

