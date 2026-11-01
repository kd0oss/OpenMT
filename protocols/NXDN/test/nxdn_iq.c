/* Created by Rick KD0OSS */
/*
 * nxdn_iq: writes an IQ file of a synthetic NXDN 4800 call, to be transmitted with a HackRF (or any SDR).
 *
 * The call is made by nxdn_gen.c (header, voice frames with test audio and call information, trailer), modulated
 * by nxdn_dsp.c (the same 4FSK modulator the host uses: 2400 baud, 1.05 kHz outer deviation, 24 kHz audio rate) and
 * turned into complex baseband by integrating the frequency at the output sample rate.  The carrier is at 0 Hz
 * (or at --offset); tune the SDR to the channel frequency.
 *
 * Usage: nxdn_iq -o file [options]
 *   -o file        output file (required)
 *   -r rate        output sample rate in Hz, 2000000..20000000 for a HackRF (default 2000000)
 *   -f format      int8 (HackRF, hackrf_transfer -t; default) or cf32 (GNU Radio, SDR++, hackrf_transfer is not for this)
 *   -a amplitude   0.0..1.0 of full scale (default 0.8)
 *   --offset hz    carrier offset from the tuned frequency (default 0); use e.g. 100000 to keep the call off the
 *                  HackRF's centre spike, and tune that much lower
 *   --ran n        radio access number 0..63 (default 1)
 *   --src id       source unit id (default 1234)
 *   --dst id       destination id / talk group (default 9)
 *   --private      private call (default is a group call)
 *   --voice n      voice frames of 80 ms each (default 25 = 2 s)
 *   --data n       send a data call of n blocks instead of voice
 *   --level pct    deviation as a percentage of nominal (default 100)
 *   --rc           raised cosine shaping instead of root raised cosine
 *   --invert       swap the sign of the frequency (if your decoder hears an inverted signal)
 *   --verify       demodulate the file again and compare the frames with the ones sent
 *   --decode file  do not make a file: decode an int8 IQ file (-r rate, --offset hz as above) with the NXDN
 *                  receiver, in both polarities, and print how many frames each finds
 *
 * Example (HackRF, 449.0 MHz channel, call placed 100 kHz up):
 *   ./nxdn_iq -o call.iq --offset 100000 --voice 40
 *   hackrf_transfer -t call.iq -f 448900000 -s 2000000 -x 20 -a 1
 *
 * Only transmit on frequencies and with power you are licensed for, and use a dummy load or a very low power
 * level for tests.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nxdn_dsp.h"
#include "nxdn_func.h"
#include "nxdn_gen.h"

#define MAXF 1024U
#define AUDIO_RATE 24000.0
#define NOMINAL_DEV_HZ 1050.0           /* outer symbol deviation of NXDN 4800 */

typedef struct
{
    uint8_t  frames[MAXF][48];
    unsigned count;
} Frames;

/*
 * rxCb()
 *
 * Receiver callback of --verify: stores each frame received.
 *
 * Parameters:
 *   ctx  - A Frames structure.
 *   f    - The frame.
 *   lost - true if the signal was lost (ignored).
 *
 * Returns: nothing.
 */
static void rxCb(void* ctx, const uint8_t* f, bool lost)
{
    Frames* r = ctx;

    if (!lost && r->count < MAXF)
        memcpy(r->frames[r->count++], f, 48);
}

/*
 * usage()
 *
 * Prints the command line.
 *
 * Parameters:
 *   prog - Program name.
 *
 * Returns: nothing.
 */
static void usage(const char* prog)
{
    fprintf(stderr,
            "Usage: %s -o file [-r rate] [-f int8|cf32] [-a amp] [--offset hz] [--ran n] [--src id] [--dst id]\n"
            "          [--private] [--voice n | --data n] [--level pct] [--rc] [--invert] [--verify]\n"
            "See the comment at the top of nxdn_iq.c.\n", prog);
}

/*
 * decodeFile()
 *
 * Decodes an int8 IQ file with the NXDN receiver, once in each polarity, and
 * reports what it found.
 *
 * Parameters:
 *   name   - The file.
 *   rate   - Its sample rate.
 *   offset - Carrier offset in Hz to remove.
 *
 * Returns: 0.
 */
static int decodeFile(const char* name, double rate, double offset)
{
    FILE* f = fopen(name, "rb");
    long  size;
    int8_t* b;
    size_t n;
    double ratio = rate / AUDIO_RATE;
    static int16_t back[MAXF * NXDN_SAMPLES_PER_FRAME];

    if (f == NULL) { perror(name); return 1; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)size);
    if (b == NULL || fread(b, 1, (size_t)size, f) != (size_t)size) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);
    n = (size_t)size / 2;

    for (int pol = 0; pol < 2; pol++)
    {
        static Frames r;
        NxdnRx rx;
        double sign = pol ? 1.0 : -1.0, acc = 0.0, outPos = 0.0, lastI = b[0], lastQ = b[1];
        size_t nb = 0, cnt = 0;

        r.count = 0;
        nxdnRxInit(&rx, rxCb, &r, NXDN_SHAPE_RRC);
        for (size_t i = 1; i < n && nb + 1 < sizeof(back) / sizeof(back[0]); i++)
        {
            double I = b[2 * i], Q = b[2 * i + 1];
            double d = atan2(Q * lastI - I * lastQ, I * lastI + Q * lastQ);

            if (I * I + Q * Q < 25.0) { lastI = I; lastQ = Q; continue; }      /* silence */
            lastI = I; lastQ = Q;
            acc += d * rate / (2.0 * M_PI) - offset; cnt++;
            outPos += 1.0;
            if (outPos >= ratio)
            {
                outPos -= ratio;
                back[nb++] = (int16_t)lrint(sign * (acc / (double)cnt) / NOMINAL_DEV_HZ * NXDN_LEVEL_OUTER);
                acc = 0.0; cnt = 0;
            }
        }
        nxdnRxSamples(&rx, back, (unsigned)nb);
        printf("%s polarity: %zu symbols' worth of samples, %u valid NXDN frames decoded\n",
               pol ? "inverted" : "normal  ", nb / 10, r.count);
    }
    free(b);
    return 0;
}

int main(int argc, char** argv)
{
    const char* outName = NULL;
    double   rate = 2000000.0, amp = 0.8, offset = 0.0, levelPct = 100.0;
    bool     cf32 = false, group = true, invert = false, verify = false, rc = false;
    unsigned ran = 1, src = 1234, dst = 9, voice = 25, data = 0;
    static Frames sent, got;
    static int16_t audio[MAXF * NXDN_SAMPLES_PER_FRAME + 4096];
    unsigned nFrames, nAudio = 0;

    const char* decodeName = NULL;

    for (int i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        bool more = i + 1 < argc;

        if (!strcmp(a, "--decode") && more) { decodeName = argv[++i]; continue; }

        if (!strcmp(a, "-o") && more)              outName = argv[++i];
        else if (!strcmp(a, "-r") && more)         rate = atof(argv[++i]);
        else if (!strcmp(a, "-f") && more)         cf32 = !strcmp(argv[++i], "cf32");
        else if (!strcmp(a, "-a") && more)         amp = atof(argv[++i]);
        else if (!strcmp(a, "--offset") && more)   offset = atof(argv[++i]);
        else if (!strcmp(a, "--ran") && more)      ran = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--src") && more)      src = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--dst") && more)      dst = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--voice") && more)    voice = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--data") && more)     data = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--level") && more)    levelPct = atof(argv[++i]);
        else if (!strcmp(a, "--private"))          group = false;
        else if (!strcmp(a, "--rc"))               rc = true;
        else if (!strcmp(a, "--invert"))           invert = true;
        else if (!strcmp(a, "--verify"))           verify = true;
        else
        {
            usage(argv[0]);
            return 1;
        }
    }
    if (decodeName != NULL)
        return decodeFile(decodeName, rate, offset);
    if (outName == NULL || rate < 48000.0 || amp <= 0.0 || amp > 1.0 || voice + 2 > MAXF || data + 2 > MAXF || ran > 63)
    {
        usage(argv[0]);
        return 1;
    }

    /* the frames of the call */
    nFrames = data ? nxdnGenDataCall((uint8_t)ran, (uint16_t)src, (uint16_t)dst, group, data, sent.frames, MAXF, 3) :
                     nxdnGenVoiceCall((uint8_t)ran, (uint16_t)src, (uint16_t)dst, group, voice, sent.frames, MAXF, 3);
    if (nFrames == 0)
    {
        fprintf(stderr, "nxdn_iq: could not make the call\n");
        return 1;
    }
    sent.count = nFrames;

    /* 4FSK samples at 24 kHz: a short preamble, the frames, the tail of the filter */
    NxdnTx tx;
    uint8_t shape = rc ? NXDN_SHAPE_RC : NXDN_SHAPE_RRC;

    nxdnTxInit(&tx, NXDN_LEVEL_OUTER * (float)(levelPct / 100.0), shape);
    nAudio += nxdnTxPreamble(&tx, 48, audio + nAudio);
    for (unsigned i = 0; i < nFrames; i++)
        nAudio += nxdnTxFrame(&tx, sent.frames[i], audio + nAudio);
    nAudio += nxdnTxFlush(&tx, audio + nAudio);

    /* complex baseband: integrate the frequency.  A negative sample is the +3 symbol in the host's polarity, so
     * the frequency is the sample with its sign changed. */
    double   ratio  = rate / AUDIO_RATE;
    size_t   total  = (size_t)((double)nAudio * ratio) + (size_t)(rate * 0.2);      /* 0.1 s of silence each side */
    size_t   lead   = (size_t)(rate * 0.1);
    FILE*    out    = fopen(outName, "wb");
    double   phase  = 0.0;
    double   sign   = invert ? 1.0 : -1.0;
    float*   iq     = malloc(sizeof(float) * 2 * 65536);
    size_t   written = 0;
    double   peak   = 0.0;

    if (out == NULL || iq == NULL)
    {
        perror("nxdn_iq");
        return 1;
    }

    /* keep the whole IQ stream only for --verify */
    float* all = verify ? malloc(sizeof(float) * 2 * total) : NULL;

    for (size_t n = 0; n < total;)
    {
        size_t chunk = total - n < 65536 ? total - n : 65536;

        for (size_t k = 0; k < chunk; k++, n++)
        {
            double I = 0.0, Q = 0.0;

            if (n >= lead && n < lead + (size_t)((double)nAudio * ratio))
            {
                double pos = (double)(n - lead) / ratio;
                size_t i0  = (size_t)pos;
                double fr  = pos - (double)i0;
                double s0  = audio[i0];
                double s1  = i0 + 1 < nAudio ? audio[i0 + 1] : s0;
                double hz  = sign * ((s0 + (s1 - s0) * fr) / NXDN_LEVEL_OUTER) * NOMINAL_DEV_HZ;

                phase += 2.0 * M_PI * (hz + offset) / rate;
                if (phase > M_PI)  phase -= 2.0 * M_PI;
                if (phase < -M_PI) phase += 2.0 * M_PI;
                I = amp * cos(phase);
                Q = amp * sin(phase);
            }
            iq[2 * k]     = (float)I;
            iq[2 * k + 1] = (float)Q;
            if (fabs(I) > peak) peak = fabs(I);
            if (all) { all[2 * n] = (float)I; all[2 * n + 1] = (float)Q; }
        }
        if (cf32)
        {
            written += fwrite(iq, sizeof(float) * 2, chunk, out);
        }
        else
        {
            static int8_t b[2 * 65536];

            for (size_t k = 0; k < 2 * chunk; k++)
                b[k] = (int8_t)lrintf(iq[k] * 127.0f);
            written += fwrite(b, 2, chunk, out);
        }
    }
    fclose(out);
    free(iq);

    printf("nxdn_iq: %s call, %u frames (%.2f s on the air), %zu IQ samples at %.0f Hz, %s, carrier at %+.0f Hz\n",
           data ? "data" : "voice", nFrames, nFrames * 0.08, written, rate, cf32 ? "cf32" : "int8", offset);

    if (verify)
    {
        /* FM demodulate: phase step per sample, undo the offset, average down to 24 kHz, undo the sign */
        NxdnRx rx;
        static int16_t back[MAXF * NXDN_SAMPLES_PER_FRAME + 8192];
        size_t nb = 0;
        size_t cnt = 0;
        double lastI = all[0], lastQ = all[1];

        nxdnRxInit(&rx, rxCb, &got, shape);
        double acc = 0.0, outPos = 0.0;
        for (size_t n = 1; n < total && nb + 1 < sizeof(back) / sizeof(back[0]); n++)
        {
            double I = all[2 * n], Q = all[2 * n + 1];
            double d = atan2(Q * lastI - I * lastQ, I * lastI + Q * lastQ);        /* phase step */
            double hz = d * rate / (2.0 * M_PI) - offset;

            lastI = I; lastQ = Q;
            acc += hz; cnt++;
            outPos += 1.0;
            if (outPos >= ratio)
            {
                outPos -= ratio;
                back[nb++] = (int16_t)lrint(sign * (acc / (double)cnt) / NOMINAL_DEV_HZ * NXDN_LEVEL_OUTER);
                acc = 0.0; cnt = 0;
            }
        }
        nxdnRxSamples(&rx, back, (unsigned)nb);

        unsigned match = 0;
        for (unsigned i = 0; i < got.count && i < sent.count; i++)
            match += memcmp(got.frames[i], sent.frames[i], 48) == 0;
        printf("verify: %u of %u frames came back from the IQ file (%u received)\n", match, sent.count, got.count);
        free(all);
        return match == sent.count ? 0 : 2;
    }
    return 0;
}
