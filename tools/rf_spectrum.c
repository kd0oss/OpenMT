#include "rf_spectrum.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_SUBS      4
#define SUB_TIMEOUT_MS 5000LL
#define FRAME_EVERY_MS 125LL
#define HEADER_BYTES  20U

typedef struct
{
    struct sockaddr_in addr;
    long long          last_ms;
    bool               used;
} sub_t;

struct hrf_spec
{
    int           fd;
    sub_t         sub[MAX_SUBS];
    uint32_t      span_hz;
    uint32_t      centre_hz;

    float         window[RFSPEC_BINS];
    float         window_sum;
    float complex buf[RFSPEC_BINS];
    size_t        fill;
    float         power[RFSPEC_BINS];     /* sum of |X|^2 of the frames since the last datagram */
    unsigned int  frames;
    long long     last_send_ms;
    bool          active;

    bool          mirror;
    int           adc_peak;
    unsigned long adc_n;
    unsigned long adc_clip;
};

static long long
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

/* in place radix 2 FFT of RFSPEC_BINS points */
static void
fft(float complex *a)
{
    const size_t n = RFSPEC_BINS;
    size_t i, j, len, k;

    for (i = 1, j = 0; i < n; i++)
    {
        size_t bit = n >> 1;

        for (; j & bit; bit >>= 1)
        {
            j ^= bit;
        }
        j ^= bit;
        if (i < j)
        {
            float complex t = a[i];

            a[i] = a[j];
            a[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1)
    {
        float ang = -2.0f * (float)M_PI / (float)len;
        float complex wl = cosf(ang) + I * sinf(ang);

        for (i = 0; i < n; i += len)
        {
            float complex w = 1.0f;

            for (k = 0; k < len / 2; k++)
            {
                float complex u = a[i + k];
                float complex v = a[i + k + len / 2] * w;

                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

rfspec_t *
rfspec_create(uint16_t port, const char *bind_addr, uint32_t span_hz)
{
    rfspec_t *s;
    struct sockaddr_in a;
    size_t i;
    int fl;

    if (port == 0)
    {
        return NULL;
    }
    s = calloc(1, sizeof(*s));
    if (s == NULL)
    {
        return NULL;
    }
    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd < 0)
    {
        free(s);
        return NULL;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (bind_addr == NULL || inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1)
    {
        a.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (bind(s->fd, (struct sockaddr *)&a, sizeof(a)) < 0)
    {
        close(s->fd);
        free(s);
        return NULL;
    }
    fl = fcntl(s->fd, F_GETFL, 0);
    fcntl(s->fd, F_SETFL, fl | O_NONBLOCK);

    s->span_hz = span_hz;
    s->window_sum = 0.0f;
    for (i = 0; i < RFSPEC_BINS; i++)
    {
        s->window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)(RFSPEC_BINS - 1U));
        s->window_sum += s->window[i];
    }
    return s;
}

void
rfspec_destroy(rfspec_t *s)
{
    if (s == NULL)
    {
        return;
    }
    close(s->fd);
    free(s);
}

void
rfspec_set_freq(rfspec_t *s, uint32_t centre_hz)
{
    if (s != NULL)
    {
        s->centre_hz = centre_hz;
    }
}

bool
rfspec_poll(rfspec_t *s)
{
    int n;
    long long now = now_ms();
    bool any = false;
    int i;

    if (s == NULL)
    {
        return false;
    }
    for (n = 0; n < 16; n++)
    {
        uint8_t req[64];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t r = recvfrom(s->fd, req, sizeof(req), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);

        if (r < 0)
        {
            break;
        }
        if (r >= 4 && memcmp(req, "SPEC", 4) == 0)
        {
            int slot = -1;

            for (i = 0; i < MAX_SUBS; i++)
            {
                if (s->sub[i].used && s->sub[i].addr.sin_addr.s_addr == from.sin_addr.s_addr && s->sub[i].addr.sin_port == from.sin_port)
                {
                    slot = i;
                    break;
                }
            }
            for (i = 0; slot < 0 && i < MAX_SUBS; i++)
            {
                if (!s->sub[i].used)
                {
                    slot = i;
                }
            }
            if (slot >= 0)
            {
                s->sub[slot].addr = from;
                s->sub[slot].last_ms = now;
                s->sub[slot].used = true;
            }
        }
    }
    for (i = 0; i < MAX_SUBS; i++)
    {
        if (s->sub[i].used && now - s->sub[i].last_ms > SUB_TIMEOUT_MS)
        {
            s->sub[i].used = false;
        }
        if (s->sub[i].used)
        {
            any = true;
        }
    }
    if (!any && s->active)
    {
        /* the last monitor has gone: drop what was collected */
        s->fill = 0;
        s->frames = 0;
        memset(s->power, 0, sizeof(s->power));
        s->adc_peak = 0;
        s->adc_n = 0;
        s->adc_clip = 0;
    }
    s->active = any;
    return any;
}

void
rfspec_adc(rfspec_t *s, const int8_t *iq, size_t n_bytes)
{
    size_t i;

    if (s == NULL || !s->active)
    {
        return;
    }
    for (i = 0; i < n_bytes; i++)
    {
        int v = iq[i];
        int av = v < 0 ? -v : v;

        if (av > s->adc_peak)
        {
            s->adc_peak = av;
        }
        if (av >= 127)
        {
            s->adc_clip++;
        }
    }
    s->adc_n += n_bytes;
}

void
rfspec_level(rfspec_t *s, const float complex *iq, size_t n)
{
    size_t i;

    if (s == NULL || !s->active)
    {
        return;
    }
    for (i = 0; i < n; i++)
    {
        float a = fabsf(crealf(iq[i]));
        float b = fabsf(cimagf(iq[i]));
        int v;

        if (b > a)
        {
            a = b;
        }
        v = (int)(a * 127.0f + 0.5f);
        if (v > s->adc_peak)
        {
            s->adc_peak = v > 127 ? 127 : v;
        }
        if (a >= 0.99f)
        {
            s->adc_clip++;
        }
    }
    s->adc_n += 2 * n;
}

void
rfspec_set_mirror(rfspec_t *s, bool mirror)
{
    if (s != NULL)
    {
        s->mirror = mirror;
    }
}

static void
put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void
put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)(v & 0xFFFF));
    put16(p + 2, (uint16_t)(v >> 16));
}

static void
send_frame(rfspec_t *s)
{
    uint8_t out[HEADER_BYTES + RFSPEC_BINS];
    size_t k;
    int i;
    float norm = 1.0f / ((float)s->frames * s->window_sum * s->window_sum);
    unsigned int clip_h = 0;

    memcpy(out, "SPEC", 4);
    out[4] = 1;
    out[5] = s->adc_clip > 0 ? 1 : 0;
    put16(out + 6, (uint16_t)RFSPEC_BINS);
    put32(out + 8, s->centre_hz);
    put32(out + 12, s->span_hz);
    out[16] = (uint8_t)(s->adc_peak > 127 ? 127 : s->adc_peak);
    out[17] = 0;
    if (s->adc_n > 0)
    {
        clip_h = (unsigned int)(10000.0 * (double)s->adc_clip / (double)s->adc_n + 0.5);
    }
    put16(out + 18, (uint16_t)(clip_h > 10000 ? 10000 : clip_h));

    for (k = 0; k < RFSPEC_BINS; k++)
    {
        /* the FFT has DC first: put -span/2 first */
        size_t src = (k + RFSPEC_BINS / 2) % RFSPEC_BINS;
        float p;

        if (s->mirror)
        {
            src = (RFSPEC_BINS - src) % RFSPEC_BINS;       /* f -> -f */
        }
        p = s->power[src] * norm;
        float db = 10.0f * log10f(p + 1e-13f);
        int q = (int)lroundf(-db * 2.0f);

        out[HEADER_BYTES + k] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
    }
    for (i = 0; i < MAX_SUBS; i++)
    {
        if (s->sub[i].used)
        {
            (void)sendto(s->fd, out, sizeof(out), MSG_DONTWAIT, (const struct sockaddr *)&s->sub[i].addr, sizeof(s->sub[i].addr));
        }
    }
    memset(s->power, 0, sizeof(s->power));
    s->frames = 0;
    s->adc_peak = 0;
    s->adc_n = 0;
    s->adc_clip = 0;
}

void
rfspec_feed(rfspec_t *s, const float complex *bb, size_t n)
{
    size_t i;

    if (s == NULL || !s->active)
    {
        return;
    }
    for (i = 0; i < n; i++)
    {
        s->buf[s->fill] = bb[i] * s->window[s->fill];
        s->fill++;
        if (s->fill == RFSPEC_BINS)
        {
            size_t k;

            fft(s->buf);
            for (k = 0; k < RFSPEC_BINS; k++)
            {
                float re = crealf(s->buf[k]);
                float im = cimagf(s->buf[k]);

                s->power[k] += re * re + im * im;
            }
            s->frames++;
            s->fill = 0;
        }
    }
    if (s->frames > 0)
    {
        long long t = now_ms();

        if (t - s->last_send_ms >= FRAME_EVERY_MS)
        {
            send_frame(s);
            s->last_send_ms = t;
        }
    }
}
