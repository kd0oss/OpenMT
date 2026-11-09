/* Test of rf_spectrum: a client asks for frames, a tone and noise are fed, the frames are checked. */
#include "rf_spectrum.h"

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;

static void
check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
    {
        failures++;
    }
}

static int
client_open(uint16_t port, struct sockaddr_in *to)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = {0, 300000};

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(to, 0, sizeof(*to));
    to->sin_family = AF_INET;
    to->sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &to->sin_addr);
    return fd;
}

/* feeds a tone (hz, amplitude) plus noise for ms milliseconds in real time, polling and asking as a client would */
static void
run(rfspec_t *s, int cfd, const struct sockaddr_in *to, double hz, double amp, double noise, int ms, int ask)
{
    static float complex blk[1250];
    static double ph = 0.0;
    int t;

    for (t = 0; t < ms / 10; t++)
    {
        int i;

        if (ask && t % 20 == 0)
        {
            sendto(cfd, "SPEC", 4, 0, (const struct sockaddr *)to, sizeof(*to));
        }
        usleep(10000);
        rfspec_poll(s);
        for (i = 0; i < 1250; i++)
        {
            double nr = ((rand() / (double)RAND_MAX) - 0.5) * noise * 3.4641;
            double ni = ((rand() / (double)RAND_MAX) - 0.5) * noise * 3.4641;

            ph += 2.0 * M_PI * hz / 125000.0;
            blk[i] = (float)(amp * cos(ph) + nr) + I * (float)(amp * sin(ph) + ni);
        }
        rfspec_feed(s, blk, 1250);
    }
}

/* the latest frame waiting at the client, 0 when none came */
static int
latest(int cfd, uint8_t *frame, size_t cap)
{
    int got = 0;
    ssize_t r;

    while ((r = recv(cfd, frame, cap, got ? MSG_DONTWAIT : 0)) > 0)
    {
        got = (int)r;
    }
    return got;
}

int
main(void)
{
    const uint16_t port = 28051;
    rfspec_t *s = rfspec_create(port, "127.0.0.1", 125000);
    struct sockaddr_in to;
    int cfd;
    uint8_t f[2048];
    int n, k, peak;

    check(s != NULL, "the socket opens");
    check(rfspec_create(0, NULL, 125000) == NULL, "port 0 means off");
    if (s == NULL)
    {
        return 1;
    }
    rfspec_set_freq(s, 449900000U);
    cfd = client_open(port, &to);

    /* nobody has asked: nothing is sent */
    run(s, cfd, &to, 10000.0, 0.5, 0.001, 400, 0);
    check(latest(cfd, f, sizeof(f)) == 0, "no frames before a request");

    /* a request, a -6 dBFS tone at +10 kHz */
    run(s, cfd, &to, 10000.0, 0.5, 0.001, 700, 1);
    n = latest(cfd, f, sizeof(f));
    check(n == (int)(20 + RFSPEC_BINS), "a frame of 20 + 1024 bytes arrives");
    check(n > 0 && memcmp(f, "SPEC", 4) == 0 && f[4] == 1, "magic and version");
    check(n > 0 && (f[8] | (f[9] << 8) | (f[10] << 16) | ((unsigned)f[11] << 24)) == 449900000U, "centre frequency");
    check(n > 0 && (f[12] | (f[13] << 8) | (f[14] << 16)) == 125000, "span");
    peak = 0;
    for (k = 0; k < (int)RFSPEC_BINS; k++)
    {
        if (f[20 + k] < f[20 + peak])
        {
            peak = k;
        }
    }
    printf("   peak bin %d (%.0f Hz), level %.1f dBFS\n", peak, (peak - 512) * 125000.0 / 1024.0, -0.5 * f[20 + peak]);
    check(abs(peak - (512 + (int)lround(10000.0 / (125000.0 / 1024.0)))) <= 1, "the tone is at +10 kHz");
    check(fabs(-0.5 * f[20 + peak] - (-6.0)) < 2.0, "the tone reads about -6 dBFS");
    check((f[5] & 1) == 0, "no overload flag");

    /* the ADC samples at full scale */
    {
        int8_t raw[2000];

        for (k = 0; k < 2000; k++)
        {
            raw[k] = (k % 4 == 0) ? 127 : 20;
        }
        rfspec_adc(s, raw, sizeof(raw));
        run(s, cfd, &to, 10000.0, 0.5, 0.001, 400, 1);
        {
            int flagged = 0, share = 0, peakv = 0;
            ssize_t r;

            while ((r = recv(cfd, f, sizeof(f), MSG_DONTWAIT)) > 0)
            {
                if (f[5] & 1)
                {
                    flagged++;
                    peakv = f[16];
                    share = f[18] | (f[19] << 8);
                }
            }
            check(flagged == 1 && peakv == 127, "samples at full scale set the overload flag and the peak, in one frame");
            printf("   share at full scale %.2f %% (a quarter of the samples were set to 127)\n", share / 100.0);
            check(share > 2000 && share < 3000, "the share at full scale is about 25 %");
        }
    }

    /* the bridge's RX invert: the frequency axis is turned round */
    rfspec_set_mirror(s, true);
    run(s, cfd, &to, 10000.0, 0.5, 0.001, 700, 1);
    while (recv(cfd, f, sizeof(f), MSG_DONTWAIT) > 0)
    {
        /* drain: the frames made before the switch */
    }
    run(s, cfd, &to, 10000.0, 0.5, 0.001, 700, 1);
    n = latest(cfd, f, sizeof(f));
    peak = 0;
    for (k = 0; k < (int)RFSPEC_BINS; k++)
    {
        if (f[20 + k] < f[20 + peak])
        {
            peak = k;
        }
    }
    printf("   mirrored: peak bin %d (%.0f Hz)\n", peak, (peak - 512) * 125000.0 / 1024.0);
    check(n > 0 && abs(peak - (512 - (int)lround(10000.0 / (125000.0 / 1024.0)))) <= 1, "mirrored: the +10 kHz tone shows at -10 kHz");
    rfspec_set_mirror(s, false);

    /* floating point I/Q (the SX1255's): a quarter of the samples at full scale */
    {
        static float complex q[1000];

        for (k = 0; k < 1000; k++)
        {
            q[k] = (k % 4 == 0) ? (1.0f + 0.0f * I) : (0.1f + 0.1f * I);
        }
        run(s, cfd, &to, 10000.0, 0.5, 0.001, 300, 1);
        rfspec_level(s, q, 1000);
        run(s, cfd, &to, 10000.0, 0.5, 0.001, 400, 1);
        {
            int flagged = 0, peakv = 0, share = 0;
            ssize_t r;

            while ((r = recv(cfd, f, sizeof(f), MSG_DONTWAIT)) > 0)
            {
                if (f[5] & 1)
                {
                    flagged++;
                    peakv = f[16];
                    share = f[18] | (f[19] << 8);
                }
            }
            check(flagged == 1 && peakv == 127, "float I/Q at full scale sets the overload flag and the peak");
            printf("   float share at full scale %.2f %% (one real component of eight)\n", share / 100.0);
            check(share > 1000 && share < 1500, "the share at full scale is about 12.5 %");
        }
    }

    /* the client goes away: frames stop after about 5 s */
    run(s, cfd, &to, 10000.0, 0.5, 0.001, 6000, 0);
    latest(cfd, f, sizeof(f));
    run(s, cfd, &to, 10000.0, 0.5, 0.001, 600, 0);
    check(latest(cfd, f, sizeof(f)) == 0, "no frames after the client stopped asking");

    rfspec_destroy(s);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
