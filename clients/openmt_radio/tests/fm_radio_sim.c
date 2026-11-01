/* Plays the modem host for the repo's fm_host: the radio's transmission (samples.raw, from fm_interop_tool capture) is what
 * the receiver hears, between stretches of noise as a real discriminator gives when no carrier is there, and what fm_host
 * transmits back (the repeater) is recorded to <out.raw> and checked: the speech repeated, the CTCSS tone, the end.
 *
 * Usage: fm_radio_sim <samples.raw> <out.raw> */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sim_common.h"

static double goertzel(const int16_t* x, long n, double freq)
{
    double w = 2.0 * M_PI * freq / 24000.0, c = 2.0 * cos(w), s1 = 0.0, s2 = 0.0;

    for (long i = 0; i < n; i++)
    {
        double s0 = x[i] + c * s1 - s2;

        s2 = s1;
        s1 = s0;
    }
    double p = s1 * s1 + s2 * s2 - c * s1 * s2;

    return n == 0 ? 0.0 : 2.0 * sqrt(p > 0.0 ? p : 0.0) / (double)n;
}

/* The modem host's active_mode status, which fm_host reads from the (stubbed) database */
static void writeMode(const char* name)
{
    FILE* f = fopen("/tmp/fm_stub_mode", "w");

    if (f != NULL)
    {
        fprintf(f, "%s\n", name);
        fclose(f);
    }
}

static unsigned int seed = 4242U;

static int16_t noiseSample(void)
{
    seed = seed * 1103515245U + 12345U;
    return (int16_t)(((int)((seed >> 16) & 0xFFFF) - 32768) * 9 / 10);
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: fm_radio_sim <samples.raw> <out.raw>\n");
        return 2;
    }

    FILE* f = fopen(argv[1], "rb");

    if (!f)
    {
        printf("fm_radio_sim: cannot read the radio's transmission\n");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    long nsig = bytes / 2;
    int16_t* sig = (int16_t*)calloc((size_t)nsig + 96, 2);
    if (fread(sig, 1, (size_t)bytes, f) != (size_t)bytes)
        return 2;
    fclose(f);

    writeMode("idle");
    int lfd = simListen(18000);
    printf("fm_radio_sim: waiting for fm_host on port 18000...\n");
    int mfd = simAccept(lfd, 30000);
    if (mfd < 0)
    {
        printf("fm_radio_sim: host did not connect\n");
        return 2;
    }

    SimConn modemC;
    simConnInit(&modemC, mfd);

    SimPacket reg;
    if (simRead(&modemC, &reg, 10000) != 1 || memcmp(reg.data + 8, "FM", 2) != 0)
    {
        printf("fm_radio_sim: no registration for FM\n");
        return 2;
    }

    /* the receiver: noise for 3 s, the radio's transmission, noise until the host has finished its transmission */
    int16_t* hostTx = (int16_t*)calloc(24000L * 60, 2);
    long hostN = 0;
    int eots = 0, setmodes = 0, setidles = 0, txPackets = 0;
    long total = 0, sent = 0;
    const long lead = 3 * 24000L;
    long endAt = -1;
    struct timespec t0, now;

    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;)
    {
        uint8_t pkt[200] = {0x61, 0x00, 0xC8, 0x04, 'S', 'A', 'M', 'P'};

        for (int i = 0; i < 96; i++)
        {
            long pos = total + i - lead;
            int16_t v = (pos >= 0 && pos < nsig) ? sig[pos] : noiseSample();

            pkt[8 + 2 * i] = (uint8_t)(v & 0xFF);
            pkt[9 + 2 * i] = (uint8_t)((v >> 8) & 0xFF);
        }
        if (!simSendRaw(&modemC, pkt, 200))
            break;
        total += 96;
        sent++;

        /* what the host sends back */
        SimPacket p;

        while (simRead(&modemC, &p, 0) == 1)
        {
            if (memcmp(p.data + 4, "SAMP", 4) == 0 && p.len > 8)
            {
                long n = (p.len - 8) / 2;

                for (long i = 0; i < n && hostN < 24000L * 60; i++)
                    hostTx[hostN++] = (int16_t)(p.data[8 + 2 * i] | (p.data[9 + 2 * i] << 8));
                txPackets++;
            }
            else if (memcmp(p.data + 4, "SEOT", 4) == 0)
            {
                eots++;
                if (endAt < 0)
                    endAt = total + 24000L;
            }
            else if (p.len == 5 && p.data[4] == 2)
            {
                setmodes++;
                writeMode("FM");
            }
            else if (p.len == 5 && p.data[4] == 3)
            {
                setidles++;
                writeMode("idle");
            }
        }

        if (endAt >= 0 && total > endAt)
            break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - t0.tv_sec) > 60)
            break;

        /* real time: 96 samples every 4 ms */
        long due = (long)(sent * 4);
        long el = (long)((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000);

        if (due > el)
            usleep((useconds_t)((due - el) * 1000));
    }

    simDrain(&modemC, 500, 2000, NULL, NULL);
    unlink("/tmp/fm_stub_mode");

    f = fopen(argv[2], "wb");
    for (long i = 0; f && i < hostN; i++)
    {
        uint8_t b[2] = {(uint8_t)(hostTx[i] & 0xFF), (uint8_t)((hostTx[i] >> 8) & 0xFF)};

        fwrite(b, 1, 2, f);
    }
    if (f)
        fclose(f);

    printf("host transmitted %ld samples (%.2f s) in %d packets, %d end of transmission, %d SETMODE, %d SETIDLE\n", hostN,
           hostN / 24000.0, txPackets, eots, setmodes, setidles);

    double a1k = 0.0, a88 = 0.0;

    if (hostN > 24000)
    {
        /* the middle of the repeated speech */
        long from = hostN / 3, n = 12000;

        if (from + n > hostN)
            n = hostN - from;
        a1k = goertzel(hostTx + from, n, 1000.0);
        a88 = goertzel(hostTx + from, n, 88.5);
    }
    printf("repeated audio: 1 kHz %.0f, 88.5 Hz %.0f\n", a1k, a88);

    /* how much of the 1.5 s of speech came back: blocks of 100 ms that hold the 1 kHz tone */
    int speechBlocks = 0;

    for (long at = 0; at + 2400 <= hostN; at += 2400)
        if (goertzel(hostTx + at, 2400, 1000.0) > 4000.0)
            speechBlocks++;
    printf("the repeater sent %d blocks of 100 ms with the speech (15 were sent)\n", speechBlocks);

    simCheck(setmodes >= 1, "the host asked the modem host for the mode (%d)", setmodes);
    simCheck(hostN > 36000, "the host repeated the call: %ld samples sent", hostN);
    simCheck(a1k > 1500.0, "the 1 kHz speech was repeated at %.0f", a1k);
    simCheck(a88 > 400.0 && a88 < 2000.0, "the repeater's CTCSS tone is on its transmission: %.0f", a88);
    simCheck(speechBlocks >= 13, "at least 1.3 s of the 1.5 s of speech was repeated: %d blocks of 100 ms", speechBlocks);
    simCheck(eots >= 1, "the transmission ended with an end of transmission (%d)", eots);

    printf("\n%s (%d checks, %d failed)\n", simFails == 0 ? "PASS" : "FAIL", simChecks, simFails);
    return simFails == 0 ? 0 : 1;
}
