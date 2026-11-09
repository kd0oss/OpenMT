/* Test of the IQ side channel of rf_spectrum: frequency, level, rate, sequence, mirror, alias rejection, and silence without a request. */
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

static uint32_t
get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int16_t
get16s(const uint8_t *p)
{
    return (int16_t)(uint16_t)(p[0] | (p[1] << 8));
}

typedef struct
{
    int frames;
    int bad_header;
    int gaps;
    uint32_t last_seq;
    uint32_t rate;
    unsigned int shift;
    double freq;                /* Hz, from the phase advance */
    double amp;                 /* of the samples, full scale 1.0 */
} result_t;

/* feeds a tone for ms milliseconds, in real time, asking for IQ (or only for the spectrum) */
static void
run(rfspec_t *s, int cfd, const struct sockaddr_in *to, double hz, double amp, int ms, const char *request, result_t *r)
{
    static float complex blk[1250];
    static double ph = 0.0;
    int t;

    memset(r, 0, sizeof(*r));
    for (t = 0; t < ms / 10; t++)
    {
        int i;
        uint8_t f[2048];
        ssize_t n;

        if (t % 20 == 0)
        {
            sendto(cfd, request, 4, 0, (const struct sockaddr *)to, sizeof(*to));
        }
        usleep(10000);
        rfspec_poll(s);
        for (i = 0; i < 1250; i++)
        {
            ph += 2.0 * M_PI * hz / 125000.0;
            blk[i] = (float)(amp * cos(ph)) + I * (float)(amp * sin(ph));
        }
        rfspec_feed(s, blk, 1250);
        while ((n = recv(cfd, f, sizeof(f), MSG_DONTWAIT)) > 0)
        {
            if (memcmp(f, "IQFR", 4) != 0)
            {
                continue;
            }
            if (n != 24 + 4 * 300 || f[4] != 1 || (f[6] | (f[7] << 8)) != 300)
            {
                r->bad_header++;
                continue;
            }
            {
                uint32_t seq = get32(f + 16);
                double scale = 1.0 / (32767.0 * (double)(1U << f[20]));
                double re = 0.0, im = 0.0, pw = 0.0;
                int k;

                if (r->frames > 0 && seq != r->last_seq + 1U)
                {
                    r->gaps++;
                }
                r->last_seq = seq;
                r->rate = get32(f + 12);
                r->shift = f[20];
                for (k = 1; k < 300; k++)
                {
                    double a = get16s(f + 24 + 4 * (k - 1)) * scale, b = get16s(f + 26 + 4 * (k - 1)) * scale;
                    double c = get16s(f + 24 + 4 * k) * scale, d = get16s(f + 26 + 4 * k) * scale;

                    re += c * a + d * b;              /* x[k] * conj(x[k-1]) */
                    im += d * a - c * b;
                    pw += c * c + d * d;
                }
                r->freq = atan2(im, re) * (double)r->rate / (2.0 * M_PI);
                r->amp = sqrt(pw / 299.0);
                r->frames++;
            }
        }
    }
}

int
main(void)
{
    rfspec_t *s = rfspec_create(18077, "127.0.0.1", 125000);
    int cfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to;
    result_t r;

    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(18077);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    check(s != NULL, "server created");
    if (s == NULL)
    {
        return 1;
    }
    rfspec_set_freq(s, 449900000U);

    /* a +3 kHz tone at 0.01 (-40 dBFS) */
    run(s, cfd, &to, 3000.0, 0.01, 1500, "IQST", &r);
    check(r.frames > 100, "frames arrive (about 200 a second)");
    check(r.bad_header == 0, "headers are right");
    check(r.rate == 62500U, "sample rate is span / 2");
    check(r.gaps == 0, "no gaps in the sequence");
    check(fabs(r.freq - 3000.0) < 20.0, "tone at +3000 Hz measured within 20 Hz");
    check(fabs(r.amp - 0.01) < 0.0005, "level kept (the shift is divided out)");
    check(r.shift >= 4U, "a quiet signal is scaled up (shift)");

    /* the tone at the edge: an alias must be rejected.  60 kHz folds to -2.5 kHz at 62.5 kS/s */
    run(s, cfd, &to, 60000.0, 0.5, 1000, "IQST", &r);
    check(r.frames > 50 && r.amp < 0.5 * 0.001, "a tone at 60 kHz is 60 dB down (no alias)");

    /* mirrored: the true frequency is the other sign */
    rfspec_set_mirror(s, true);
    run(s, cfd, &to, 3000.0, 0.01, 1000, "IQST", &r);
    check(fabs(r.freq + 3000.0) < 20.0, "mirror turns the frequency round");
    rfspec_set_mirror(s, false);

    /* a spectrum only request must not give IQ frames */
    sleep(6);                                       /* the IQ subscription expires */
    rfspec_poll(s);
    run(s, cfd, &to, 3000.0, 0.01, 800, "SPEC", &r);
    check(r.frames == 0, "no IQ without a request");

    /* a very strong signal: the shift stays 0 and nothing wraps */
    run(s, cfd, &to, 3000.0, 0.9, 800, "IQST", &r);
    check(r.shift == 0U && fabs(r.amp - 0.9) < 0.01, "full scale signal: shift 0, no wrap");

    rfspec_destroy(s);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
