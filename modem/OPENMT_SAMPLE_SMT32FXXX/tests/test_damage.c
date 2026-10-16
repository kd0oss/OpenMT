/*
 *  test_damage.c - damaged sample frames on the link: checks that the
 *  parser classifies them (FW2 short= / long=) and keeps the timeline.
 */

#include <stdio.h>
#include <string.h>
#include "../config.h"
#include "../hw.h"
#include "../engine.h"
#include "fakehw.h"

static void frame(uint8_t *f, int16_t v)
{
    int i;

    f[0] = 0x61;
    f[1] = 0xC4;
    f[2] = 0x70;
    f[3] = 128;
    for (i = 0; i < 96; i++)
    {
        f[4 + 2 * i] = (uint8_t)(v & 0xFF);
        f[5 + 2 * i] = (uint8_t)((v >> 8) & 0xFF);
    }
}

static void run(int ms)
{
    int i, t;

    for (i = 0; i < ms; i++)
    {
        for (t = 0; t < 24; t++)
        {
            engine_sample_tick(2048);
        }
        fake_ms++;
        engine_poll();
    }
}

int main(void)
{
    uint8_t cfg[196];
    uint8_t f[196];
    uint8_t bad[200];
    const engine_stats_t *st;
    char   *p;
    int     i;

    engine_init();
    memset(cfg, 0, sizeof cfg);
    cfg[0] = 0x61;
    cfg[1] = 0xC4;
    cfg[2] = 0x20;
    fake_feed(cfg, 196);
    run(2);

    /* 40 good frames, one 3 bytes short, 40 good, one 2 bytes long, 40 good */
    for (i = 0; i < 40; i++)
    {
        frame(f, 1000);
        fake_feed(f, 196);
    }
    frame(bad, 1000);
    memmove(bad + 100, bad + 103, 93);              /* lose 3 bytes      */
    fake_feed(bad, 193);
    for (i = 0; i < 40; i++)
    {
        frame(f, 1000);
        fake_feed(f, 196);
    }
    frame(bad, 1000);
    memmove(bad + 102, bad + 100, 96);              /* duplicate 2 bytes */
    fake_feed(bad, 198);
    for (i = 0; i < 40; i++)
    {
        frame(f, 1000);
        fake_feed(f, 196);
    }
    run(1500);
    fake_ms += 5000;
    run(10);

    st = engine_stats();
    printf("txFramesFromHost=%u lostFrames=%u badBytes=%u\n",
           st->txFramesFromHost, st->lostFrames, st->badBytes);
    fake_tx[fake_tx_len] = 0;
    for (i = 0; i < (int)fake_tx_len; i++)
    {
        if (fake_tx[i] < 0x20 || fake_tx[i] > 0x7E)
        {
            fake_tx[i] = '\n';
        }
    }
    p = strstr((char *)fake_tx, "FW2");
    printf("%s\n", p ? strtok(p, "\n") : "no FW2 line");
    return 0;
}
