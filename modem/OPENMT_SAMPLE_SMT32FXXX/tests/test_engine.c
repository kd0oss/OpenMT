/*
 *  test_engine.c - exercises the OpenMT protocol and sample engine on a
 *  PC with a simulated 24 kHz sample interrupt.
 *
 *  Build and run:  make -C tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../config.h"
#include "../hw.h"
#include "../engine.h"
#include "../clockcalc.h"
#include "fakehw.h"

static int failures;

#define CHECK(cond, ...) \
    do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* run the sample interrupt for n ticks, a conversion of v at every tick */
static uint16_t lastDac;
static void ticks(uint32_t n, uint16_t (*adc)(uint32_t), uint32_t *counter)
{
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        lastDac = engine_sample_tick(adc((*counter)++));
        if ((*counter % 24) == 0)
        {
            fake_ms++;                  /* 24 samples = 1 ms           */
            engine_poll();
        }
    }
}

static uint32_t sampleCounter;
static uint16_t adcSine(uint32_t n)
{
    return (uint16_t)(2048 + 1000.0 * sin(2.0 * M_PI * 1000.0 * n / 24000.0));
}
static uint16_t adcMid(uint32_t n)
{
    (void)n;
    return 2048;
}

static void sendConfig(uint8_t flags, uint8_t rxLevel)
{
    uint8_t f[196];

    memset(f, 0, sizeof(f));
    f[0] = 0x61;
    f[1] = 0xC4;
    f[2] = 0x20;
    f[3] = flags;
    f[4] = rxLevel;
    fake_feed(f, 196);
}

static void sendSamples(uint8_t txLevel, int16_t value, int count)
{
    uint8_t f[256];
    int i;

    f[0] = 0x61;
    f[1] = (uint8_t)(4 + count * 2);
    f[2] = 0x70;
    f[3] = txLevel;
    for (i = 0; i < count; i++)
    {
        f[4 + 2 * i] = (uint8_t)(value & 0xFF);
        f[5 + 2 * i] = (uint8_t)((value >> 8) & 0xFF);
    }
    fake_feed(f, f[1]);
}

static void sendEot(void)
{
    uint8_t f[3] = { 0x61, 0x03, 0x00 };

    fake_feed(f, 3);
}

/* Count RX sample frames in the captured UART output; returns frames */
static int scanOutput(int *acks, int *status, int *naks, int *badFormat)
{
    uint32_t p = 0;
    int frames = 0;

    *acks = *status = *naks = *badFormat = 0;
    while (p < fake_tx_len)
    {
        uint8_t len;
        uint8_t type;

        if (fake_tx[p] != 0x61)
        {
            (*badFormat)++;
            p++;
            continue;
        }
        len = fake_tx[p + 1];
        type = fake_tx[p + 2];
        if ((type & 0xF0) == 0x70)
        {
            frames++;
            if (len != 195 || (type & 0x0F) != 0)
            {
                (*badFormat)++;
            }
        }
        else if ((type & 0xF0) == 0xE0)
        {
            (*acks)++;
        }
        else if ((type & 0xF0) == 0x10)
        {
            (*status)++;
        }
        else if ((type & 0xF0) == 0xF0)
        {
            (*naks)++;
        }
        p += len;
    }
    return frames;
}

static void testProtocol(void)
{
    int acks, status, naks, bad, frames;
    uint32_t i;
    int16_t first;

    printf("protocol / RX stream\n");
    engine_init();
    fake_tx_len = 0;
    sampleCounter = 0;

    /* before SET_CONFIG nothing is streamed */
    ticks(24 * 100, adcSine, &sampleCounter);
    frames = scanOutput(&acks, &status, &naks, &bad);
    CHECK(frames == 0, "streamed %d frames before config", frames);

    sendConfig(0x01, 50);               /* duplex, rxLevel 50 = unity   */
    ticks(24, adcSine, &sampleCounter);
    frames = scanOutput(&acks, &status, &naks, &bad);
    CHECK(acks == 1, "SET_CONFIG not ACKed (%d)", acks);
    CHECK(engine_configured(), "not configured");

    fake_tx_len = 0;
    ticks(24 * 1000, adcSine, &sampleCounter);     /* 1 second */
    frames = scanOutput(&acks, &status, &naks, &bad);
    CHECK(frames >= 248 && frames <= 252, "frames in 1 s: %d (want ~250)", frames);
    CHECK(bad == 0, "bad frame format %d", bad);

    /* check the first frame's samples against the input */
    for (i = 0; i < fake_tx_len; i++)
    {
        if (fake_tx[i] == 0x61 && fake_tx[i + 1] == 195)
        {
            break;
        }
    }
    memcpy(&first, fake_tx + i + 3 + 2 * 50, 2);
    CHECK(abs(first) > 1000 || first == 0 || 1, "sample read");
    {
        /* amplitude: +-1000 counts * 16 = about 16000 q15 */
        int32_t peak = 0;
        uint32_t k;

        for (k = 0; k < 96; k++)
        {
            int16_t v;

            memcpy(&v, fake_tx + i + 3 + 2 * k, 2);
            if (abs(v) > peak)
            {
                peak = abs(v);
            }
        }
        CHECK(peak > 14000 && peak < 18000, "RX amplitude %d (want ~16000)", (int)peak);
    }

    /* GET_STATUS */
    fake_tx_len = 0;
    {
        uint8_t f[3] = { 0x61, 0x03, 0x10 };

        fake_feed(f, 3);
    }
    ticks(48, adcMid, &sampleCounter);
    {
        uint32_t p;
        int found = 0;

        for (p = 0; p + 2 < fake_tx_len; p++)
        {
            if (fake_tx[p] == 0x61 && fake_tx[p + 1] == 3 && (fake_tx[p + 2] & 0xF0) == 0x10)
            {
                found = 1;
                CHECK((fake_tx[p + 2] & 0x0F) > 0, "status says no space");
            }
        }
        CHECK(found, "no GET_STATUS reply");
    }
}

static void testTx(void)
{
    uint32_t t;
    int i;
    int played = 0;
    uint16_t maxDac = 0;

    printf("TX path / PTT / EOT drain\n");
    engine_init();
    fake_tx_len = 0;
    sampleCounter = 0;
    sendConfig(0x01, 50);
    ticks(48, adcMid, &sampleCounter);

    CHECK(!fake_ptt, "PTT on before TX");

    /* host sends 20 frames of 96 samples of constant +8000, at txLevel 100 */
    for (i = 0; i < 20; i++)
    {
        sendSamples(100, 8000, 96);
    }
    sendEot();
    ticks(24, adcMid, &sampleCounter);
    CHECK(fake_ptt, "PTT not keyed after data");
    CHECK(engine_stats()->txFramesFromHost == 20, "frames %u", engine_stats()->txFramesFromHost);

    /* all 1920 samples must come out, then PTT drops */
    for (t = 0; t < 24 * 200 && fake_ptt; t++)
    {
        ticks(1, adcMid, &sampleCounter);
        if (lastDac > 2048 + 400)
        {
            played++;
        }
        if (lastDac > maxDac)
        {
            maxDac = lastDac;
        }
    }
    CHECK(!fake_ptt, "PTT still on");
    CHECK(played == 1920, "played %d samples (want 1920)", played);
    CHECK(maxDac == 2048 + (8000 >> 4), "DAC level %u want %u", maxDac, 2048 + (8000 >> 4));
    CHECK(engine_stats()->txOverflow == 0, "overflow");
    CHECK(engine_stats()->txUnderrun == 0, "underrun %u", engine_stats()->txUnderrun);

    /* the DAC returns to mid scale */
    ticks(10, adcMid, &sampleCounter);
    CHECK(lastDac == DAC_MID, "DAC idle %u", lastDac);
}

static void testLevelsAndInvert(void)
{
    int i;
    uint16_t d = 0;
    uint32_t t;

    printf("tx level / invert\n");
    engine_init();
    sampleCounter = 0;
    sendConfig(0x01 | 0x04, 50);           /* duplex, txInvert */
    ticks(48, adcMid, &sampleCounter);
    for (i = 0; i < 4; i++)
    {
        sendSamples(50, 16000, 96);        /* txLevel 50 -> x0.5 */
    }
    sendEot();
    for (t = 0; t < 24 * 40; t++)
    {
        ticks(1, adcMid, &sampleCounter);
        if (lastDac != DAC_MID)
        {
            d = lastDac;
            break;
        }
    }
    /* 16000 * 0.5 = 8000 -> inverted -8000 -> -500 counts */
    CHECK(d == 2048 - 500, "inverted half level DAC %u want %u", d, 2048 - 500);
    for (t = 0; t < 24 * 100; t++)
    {
        ticks(1, adcMid, &sampleCounter);
    }
    CHECK(!fake_ptt, "ptt after drain");
}

static void testOverflowAndTimeout(void)
{
    int i;
    int acks, status, naks, bad;

    printf("overflow NAK / host-loss timeout\n");
    engine_init();
    fake_tx_len = 0;
    sampleCounter = 0;
    sendConfig(0x01, 50);
    ticks(48, adcMid, &sampleCounter);
    fake_tx_len = 0;

    /* dump 100 frames at once = 9600 samples > TX_BUF_SAMPLES-1 (8191) */
    for (i = 0; i < 100; i++)
    {
        sendSamples(100, 1000, 96);
    }
    ticks(24 * 5, adcMid, &sampleCounter);
    CHECK(engine_stats()->txOverflow > 0, "no overflow detected");
    scanOutput(&acks, &status, &naks, &bad);
    CHECK(naks >= 1, "no NAK sent");
    {
        uint32_t p;
        int ok = 0;

        for (p = 0; p + 4 < fake_tx_len; p++)
        {
            if (fake_tx[p] == 0x61 && fake_tx[p + 2] == 0xF0 && fake_tx[p + 4] == 6)
            {
                ok = 1;
            }
        }
        CHECK(ok, "NAK is not Err 6");
    }

    /* host stops without EOT: PTT must drop after the timeout */
    ticks(24 * (uint32_t)(TX_BUF_SAMPLES / 24 + 100), adcMid, &sampleCounter);
    {
        uint32_t t;

        for (t = 0; t < 24 * (TX_UNDERRUN_TIMEOUT_MS + 500) && fake_ptt; t++)
        {
            ticks(1, adcMid, &sampleCounter);
        }
    }
    CHECK(!fake_ptt, "PTT stuck on without EOT");
}

static void testTimingReport(void)
{
    int i;
    uint32_t t;
    uint32_t firstPlay = 0;
    int found = 0;
    uint32_t p;
    int framesBefore = 0;

    printf("TX/RX timing report\n");
    engine_init();
    fake_tx_len = 0;
    sampleCounter = 0;
    sendConfig(0x01, 50);
    ticks(24 * 50, adcMid, &sampleCounter);
    fake_tx_len = 0;

    for (i = 0; i < 10; i++)
    {
        sendSamples(100, 8000, 96);
    }
    sendEot();
    for (t = 0; t < 24 * 60; t++)
    {
        ticks(1, adcMid, &sampleCounter);
        if (!firstPlay && lastDac != DAC_MID)
        {
            firstPlay = sampleCounter - 1;      /* index of that tick */
        }
    }
    CHECK(firstPlay != 0, "no audio played");

    /* find the report and count the RX frames before it */
    for (p = 0; p < fake_tx_len; )
    {
        if (fake_tx[p] != 0x61)
        {
            p++;
            continue;
        }
        if ((fake_tx[p + 2] & 0xF0) == 0x70)
        {
            framesBefore++;
        }
        if ((fake_tx[p + 2] & 0xF0) == 0xB0)
        {
            int32_t off = (int32_t)(((uint32_t)fake_tx[p + 3] << 24) | ((uint32_t)fake_tx[p + 4] << 16) |
                ((uint32_t)fake_tx[p + 5] << 8) | fake_tx[p + 6]);
            uint32_t k = ((uint32_t)fake_tx[p + 7] << 24) | ((uint32_t)fake_tx[p + 8] << 16) |
                ((uint32_t)fake_tx[p + 9] << 8) | fake_tx[p + 10];
            /* RX frames sent before the report, plus the 50 ms of
             * frames discarded/sent before fake_tx_len was cleared */
            uint32_t rdBase = (uint32_t)(24 * 50 / 96) + (uint32_t)framesBefore;
            uint32_t r = (uint32_t)((int32_t)(rdBase * 96) + off);

            found++;
            CHECK(fake_tx[p + 1] == 11, "report length %u", fake_tx[p + 1]);
            CHECK(k == 0, "k = %u (want 0)", k);
            CHECK(r == firstPlay, "reported RX sample %u, first TX sample played at %u", r, firstPlay);
            printf("  report: k=%u offset=%d -> RX sample %u (first TX sample on air at tick %u)\n",
                k, off, r, firstPlay);
        }
        p += fake_tx[p + 1];
    }
    CHECK(found == 1, "%d timing reports (want 1)", found);
}

/* count DEBUG text frames whose text begins with FW_NAME */
static int countVersionFrames(char *textOut, size_t outSize)
{
    uint32_t p;
    int found = 0;

    for (p = 0; p + 3 <= fake_tx_len; p++)
    {
        if (fake_tx[p] == 0x61 && fake_tx[p + 2] == 0xC0 && fake_tx[p + 1] > 3 &&
            p + fake_tx[p + 1] <= fake_tx_len &&
            memcmp(fake_tx + p + 3, FW_NAME, strlen(FW_NAME)) == 0)
        {
            uint8_t n = (uint8_t)(fake_tx[p + 1] - 3);

            if (found == 0 && textOut && n < outSize)
            {
                memcpy(textOut, fake_tx + p + 3, n);
                textOut[n] = '\0';
            }
            found++;
            p += fake_tx[p + 1] - 1;
        }
    }
    return found;
}

static void testVersion(void)
{
    uint8_t q[3] = {0x61, 0x03, 0xD0};
    char text[80] = "";
    char want[80];

    engine_init();
    fake_tx_len = 0;
    sampleCounter = 0;

    /* announced once when the host first configures, after the ACK */
    sendConfig(0x01, 50);
    ticks(24, adcMid, &sampleCounter);
    CHECK(countVersionFrames(text, sizeof(text)) == 1, "version not announced once at SET_CONFIG");
    snprintf(want, sizeof(want), "%s v%s %s id=10000418", FW_NAME, FW_VERSION, __DATE__);
    CHECK(strcmp(text, want) == 0, "version text '%s' want '%s'", text, want);

    /* a second SET_CONFIG on the same link does not repeat it */
    fake_tx_len = 0;
    sendConfig(0x01, 50);
    ticks(24, adcMid, &sampleCounter);
    CHECK(countVersionFrames(NULL, 0) == 0, "version repeated on re-config");

    /* explicit query always answers */
    fake_tx_len = 0;
    fake_feed(q, 3);
    ticks(24, adcMid, &sampleCounter);
    CHECK(countVersionFrames(NULL, 0) == 1, "no reply to version query");
}

static void testBootloaderCommand(void)
{
    static const uint8_t good[7] = {0x61, 0x07, 0xD1, 'B', 'O', 'O', 'T'};
    static const uint8_t bad[7] = {0x61, 0x07, 0xD1, 'B', 'O', 'O', 'X'};
    uint32_t p;
    int acks = 0;
    int naks = 0;

    engine_init();
    fake_tx_len = 0;
    fake_boot_calls = 0;
    sampleCounter = 0;
    sendConfig(0x01, 50);
    ticks(24, adcMid, &sampleCounter);

    fake_tx_len = 0;
    fake_feed(bad, 7);
    ticks(24, adcMid, &sampleCounter);
    CHECK(fake_boot_calls == 0, "bootloader entered with a wrong key");
    for (p = 0; p + 5 <= fake_tx_len; p++)
    {
        if (fake_tx[p] == 0x61 && fake_tx[p + 1] == 5 && fake_tx[p + 2] == 0xF0 && fake_tx[p + 3] == 0xD1)
        {
            naks++;
        }
    }
    CHECK(naks == 1, "wrong key not NAKed (%d)", naks);

    fake_tx_len = 0;
    fake_feed(good, 7);
    ticks(24, adcMid, &sampleCounter);
#if BOOTLOADER_COMMAND
    CHECK(fake_boot_calls == 1, "bootloader command ignored (%d)", fake_boot_calls);
    for (p = 0; p + 3 <= fake_tx_len; p++)
    {
        if (fake_tx[p] == 0x61 && fake_tx[p + 1] == 3 && fake_tx[p + 2] == 0xE0)
        {
            acks++;
        }
    }
    CHECK(acks == 1, "no ACK before the reset (%d)", acks);
#else
    CHECK(fake_boot_calls == 0, "bootloader entered although disabled");
    (void)acks;
#endif
}

static void testResync(void)
{
    uint8_t junk[] = { 0x00, 0xFF, 0x13, 0x61, 0x02, 0x55 };
    int acks, status, naks, bad;

    printf("resync after line noise\n");
    engine_init();
    fake_tx_len = 0;
    sampleCounter = 0;
    fake_feed(junk, sizeof(junk));
    sendConfig(0x01, 50);
    ticks(48, adcMid, &sampleCounter);
    scanOutput(&acks, &status, &naks, &bad);
    CHECK(engine_configured(), "did not resync");
    CHECK(engine_stats()->badBytes >= 4, "bad bytes %u", engine_stats()->badBytes);
}

static void testSimplex(void)
{
    uint8_t d[1];
    int i;
    int nonzero = 0;
    uint32_t t;

    printf("simplex mutes RX while keyed\n");
    engine_init();
    fake_tx_len = 0;
    sampleCounter = 0;
    sendConfig(0x00, 50);                  /* simplex */
    (void)d;
    ticks(24 * 20, adcSine, &sampleCounter);
    for (i = 0; i < 10; i++)
    {
        sendSamples(100, 100, 96);
    }
    sendEot();
    fake_tx_len = 0;
    for (t = 0; t < 24 * 30; t++)
    {
        ticks(1, adcSine, &sampleCounter);
    }
    /* frames sent while keyed (first ~40 ms) must be zeros */
    {
        uint32_t p = 0;
        int f = 0;

        while (p + 195 <= fake_tx_len && f < 7)
        {
            if (fake_tx[p] == 0x61 && fake_tx[p + 1] == 195 && (fake_tx[p + 2] & 0xF0) == 0x70)
            {
                int k;

                /* skip the first two frames: they were filled while the
                 * PTT was still being keyed */
                for (k = 0; f >= 2 && k < 192; k++)
                {
                    if (fake_tx[p + 3 + k] != 0)
                    {
                        nonzero++;
                    }
                }
                f++;
                p += 195;
            }
            else
            {
                p++;
            }
        }
    }
    CHECK(nonzero == 0, "%d nonzero bytes while keyed in simplex", nonzero);
}

static void testClockPlans(void)
{
    static const uint32_t xtals[] = { 8000000, 12000000, 16000000, 25000000, 24000000,
        26000000, 14745600, 19200000, 12288000, 10000000, 4000000, 3686400, 20000000 };
    uint32_t k;

    printf("clock plans (baud %lu, UART1)\n", (unsigned long)UART_BAUD);
    for (k = 0; k < sizeof(xtals) / sizeof(xtals[0]); k++)
    {
        clk_plan_t p;
        tim_plan_t t;
        bool ok = clk_plan_hse(xtals[k], 921600, 1, 24000, true, &p);

        CHECK(ok, "no plan for %lu", (unsigned long)xtals[k]);
        if (ok)
        {
            uint32_t timclk = p.sysHz;

            tim_plan_internal(timclk, 24000, &t);
            printf("  HSE %8lu -> /%2lu x%4.1f = SYSCLK %8lu  sample %lu Hz  cost %lu ppm\n",
                (unsigned long)xtals[k], (unsigned long)p.prediv1, p.mulX2 / 2.0,
                (unsigned long)p.sysHz, (unsigned long)t.actualHz, (unsigned long)p.costPpm);
            CHECK(p.sysHz <= 72000000UL, "over 72 MHz");
            CHECK(p.costPpm < 5000, "poor accuracy for %lu: %lu ppm",
                (unsigned long)xtals[k], (unsigned long)p.costPpm);
        }
    }
}

static void checkPllPlan(const char *chip, const clk_pll_limits_t *lim, uint32_t src, uint8_t uart,
    bool timApb2, bool internal)
{
    clk_pll_plan_t p;
    tim_plan_t t;
    uint64_t vco;
    uint32_t uclk, brr;
    bool ok = clk_plan_pll(lim, src, 921600, uart, 24000, internal, timApb2, &p);

    CHECK(ok, "%s: no plan for %lu", chip, (unsigned long)src);
    if (!ok)
    {
        return;
    }
    vco = ((uint64_t)src * p.n) / p.m;
    CHECK(src / p.m >= 1000000UL && src / p.m <= 2000000UL, "%s %lu: VCO input %lu", chip,
        (unsigned long)src, (unsigned long)(src / p.m));
    CHECK(p.n >= 50 && p.n <= 432, "%s: N %lu", chip, (unsigned long)p.n);
    CHECK(vco >= lim->minVcoHz && vco <= lim->maxVcoHz, "%s %lu: VCO %lu", chip,
        (unsigned long)src, (unsigned long)vco);
    CHECK(p.p == 2 || p.p == 4 || p.p == 6 || p.p == 8, "%s: P %lu", chip, (unsigned long)p.p);
    CHECK(p.sysHz == (uint32_t)(vco / p.p), "%s: sys mismatch", chip);
    CHECK(p.sysHz <= lim->maxSysHz, "%s %lu: SYSCLK %lu over limit", chip, (unsigned long)src,
        (unsigned long)p.sysHz);
    {
        bool od = p.sysHz > lim->odAboveHz;

        CHECK(p.pclk1Hz <= (od ? lim->maxApb1OdHz : lim->maxApb1Hz) &&
              p.pclk2Hz <= (od ? lim->maxApb2OdHz : lim->maxApb2Hz), "%s %lu: APB over limit", chip,
              (unsigned long)p.sysHz);
    }
    CHECK(p.q >= 2 && p.q <= 15 && vco / p.q <= 48000000UL, "%s: Q %lu", chip, (unsigned long)p.q);
    CHECK(p.timClk1Hz == (p.ppre1 == 1 ? p.pclk1Hz : p.pclk1Hz * 2U), "%s: TIM1 clk", chip);

    uclk = clk_uart_on_apb2(uart) ? p.pclk2Hz : p.pclk1Hz;
    brr = (uclk + 921600 / 2) / 921600;
    tim_plan_internal(timApb2 ? p.timClk2Hz : p.timClk1Hz, 24000, &t);
    printf("  %s src %8lu: M%2lu N%3lu P%lu = %3lu MHz  APB/%lu,/%lu  UART%u baud %lu  sample %lu Hz  cost %lu\n",
        chip, (unsigned long)src, (unsigned long)p.m, (unsigned long)p.n, (unsigned long)p.p,
        (unsigned long)(p.sysHz / 1000000UL), (unsigned long)p.ppre1, (unsigned long)p.ppre2,
        uart, (unsigned long)(uclk / brr), (unsigned long)t.actualHz, (unsigned long)p.costPpm);
    CHECK(p.costPpm < 5000, "%s %lu: poor accuracy %lu ppm", chip, (unsigned long)src,
        (unsigned long)p.costPpm);
}

static void testPllPlans(void)
{
    static const clk_pll_limits_t f446 = { 180000000UL, 42000000UL, 84000000UL, 45000000UL, 90000000UL,
        168000000UL, 100000000UL, 432000000UL };
    static const clk_pll_limits_t f722 = { 216000000UL, 45000000UL, 90000000UL, 54000000UL, 108000000UL,
        180000000UL, 192000000UL, 432000000UL };
    static const uint32_t xtals[] = { 8000000, 12000000, 16000000, 25000000, 24000000,
        26000000, 14745600, 19200000, 12288000, 10000000, 4000000, 3686400, 20000000 };
    uint32_t k;

    printf("F4 / F7 PLL plans (baud 921600)\n");
    for (k = 0; k < sizeof(xtals) / sizeof(xtals[0]); k++)
    {
        checkPllPlan("F446", &f446, xtals[k], 1, false, true);
        checkPllPlan("F722", &f722, xtals[k], 2, false, true);
    }
    /* HSI, external sample clock (timer accuracy not part of the cost), UART6 and TIM1 */
    checkPllPlan("F446", &f446, 16000000UL, 1, false, true);
    checkPllPlan("F722", &f722, 16000000UL, 6, true, true);
    checkPllPlan("F446", &f446, 8000000UL, 3, false, false);
    checkPllPlan("F722", &f722, 8000000UL, 8, false, false);

    /* a plain 8 MHz crystal gets close to the top speed (a few ppm of baud error beats +-1 MHz) */
    {
        clk_pll_plan_t p;

        CHECK(clk_plan_pll(&f446, 8000000UL, 921600, 1, 24000, true, false, &p) && p.sysHz >= 170000000UL,
            "F446 8 MHz only reached %lu Hz", (unsigned long)p.sysHz);
        CHECK(clk_plan_pll(&f722, 8000000UL, 921600, 1, 24000, true, false, &p) && p.sysHz >= 205000000UL,
            "F722 8 MHz only reached %lu Hz", (unsigned long)p.sysHz);
        CHECK(clk_plan_pll(&f446, 26000000UL, 921600, 1, 24000, true, false, &p) && p.m == 13,
            "F446 26 MHz: PLL input not the 2 MHz choice (M%lu)", (unsigned long)p.m);
    }
}

static void testExtClock(void)
{
    static const uint32_t ext[] = { 12288000, 6144000, 12000000, 24000000, 9600000,
        19200000, 24576000, 3072000, 26000000, 48000000, 1536000, 24000 };
    uint32_t k;

    printf("external clock dividers (TIMCLK 72 MHz)\n");
    for (k = 0; k < sizeof(ext) / sizeof(ext[0]); k++)
    {
        tim_plan_t t;
        double err;

        tim_plan_external(ext[k], 72000000UL, 24000, &t);
        err = ((double)t.actualHz - 24000.0) / 24000.0 * 1e6;
        printf("  ext %9lu Hz: ETPS %lu  ARR+1 %5lu  -> %lu Hz (%+.0f ppm)\n",
            (unsigned long)ext[k], (unsigned long)t.etps, (unsigned long)t.arr + 1,
            (unsigned long)t.actualHz, err);
        CHECK(fabs(err) < 25000, "ext %lu poor accuracy", (unsigned long)ext[k]);
        CHECK(t.arr <= 65535, "ARR too big");
    }
}


/* After an underrun the audio must restart on a whole TDMA cycle (1440). */
static void testStallAlign(void)
{
    uint32_t n = 0, lastPlayed = 0, firstAfter = 0;
    int i, fed = 0;
    int played = 0;

    printf("stall alignment\n");
    for (i = 0; i < 6; i++)
    {
        sendSamples(100, 8000, 96);
    }
    for (n = 1; n < 24 * 400; n++)
    {
        ticks(1, adcMid, &sampleCounter);
        if (lastDac > 2048 + 400)
        {
            if (fed && firstAfter == 0)
            {
                firstAfter = n;
            }
            if (!fed)
            {
                lastPlayed = n;
            }
            played++;
        }
        if (!fed && n > lastPlayed + 150 && lastPlayed != 0)
        {
            for (i = 0; i < 40; i++)
            {
                sendSamples(100, 8000, 96);
            }
            fed = 1;
        }
    }
    CHECK(engine_stats()->txUnderrun >= 1, "no underrun happened");
    CHECK(firstAfter != 0, "audio never resumed");
    CHECK(((firstAfter - lastPlayed - 1) % 1440U) == 0U,
          "silent gap %u not a multiple of 1440", (unsigned)(firstAfter - lastPlayed - 1));
    printf("  gap %u samples\n", (unsigned)(firstAfter - lastPlayed - 1));
    sendEot();
    ticks(24 * 300, adcMid, &sampleCounter);
}

/* Frames arriving one at a time (the next header not yet in the buffer):
 * a frame that lost bytes must still be detected and replaced by silence. */
static void testDripLoss(void)
{
    int i;
    uint32_t before = engine_stats()->lostFrames;
    uint32_t badBefore = engine_stats()->badBytes;
    uint32_t framesBefore = engine_stats()->txFramesFromHost;
    uint8_t f[256];

    printf("drip-fed frame loss\n");
    for (i = 0; i < 14; i++)
    {
        int j;

        f[0] = 0x61; f[1] = 196; f[2] = 0x70; f[3] = 100;
        for (j = 0; j < 96; j++)
        {
            f[4 + 2 * j] = (uint8_t)(1000 & 0xFF);
            f[5 + 2 * j] = (uint8_t)(1000 >> 8);
        }
        if (i == 3 || i == 8)
        {
            f[40] = 0x61; f[41] = 0xB0; f[42] = 0x20;   /* false header in the payload */
        }
        {
            int n = (i == 3 || i == 8 || i == 9) ? 196 - 5 : ((i == 11) ? 53 : 196);   /* bytes vanished */
            int p;

            for (p = 0; p < n; p += 1)              /* trickle in, polled */
            {
                fake_feed(f + p, 1);
                ticks(24, adcMid, &sampleCounter);
            }
        }
    }
    sendEot();
    ticks(24 * 300, adcMid, &sampleCounter);
    CHECK(engine_stats()->lostFrames - before == 4, "lostFrames +%u (want 4)",
          (unsigned)(engine_stats()->lostFrames - before));
    CHECK(engine_stats()->txFramesFromHost - framesBefore == 14, "frames +%u (want 14)",
          (unsigned)(engine_stats()->txFramesFromHost - framesBefore));
    CHECK(engine_stats()->badBytes - badBefore < 700, "badBytes +%u",
          (unsigned)(engine_stats()->badBytes - badBefore));
}

/* sample clock trim: the dithered period averages to the requested ppm */
static void testSampleTrim(void)
{
    static const int32_t trims[] = { 0, 198, 1000, -150, 20000 };
    static const uint32_t arrs[] = { 3728U, 4499U, 2999U };
    unsigned a;
    unsigned k;

    printf("sample clock trim\n");
    for (a = 0; a < sizeof(arrs) / sizeof(arrs[0]); a++)
    {
        for (k = 0; k < sizeof(trims) / sizeof(trims[0]); k++)
        {
            tim_trim_t t;
            uint32_t acc = 0;
            uint64_t ticks = 0;
            uint32_t n;
            uint32_t N = 2000000U;
            double want;
            double got;

            tim_plan_trim(arrs[a], trims[k], &t);
            /* ARR written is (period or period-1); the period in ticks is ARR + 1 */
            for (n = 0; n < N; n++)
            {
                uint32_t arr;

                acc += t.frac;
                arr = (acc < t.frac) ? t.period : (t.period - 1U);
                ticks += (uint64_t)arr + 1U;
            }
            want = (double)(arrs[a] + 1U) * (1.0 + trims[k] * 1e-6);
            got = (double)ticks / N;
            CHECK(fabs(got / want - 1.0) < 1e-7, "arr %u trim %d: average %.6f want %.6f", (unsigned)arrs[a], (int)trims[k], got, want);
        }
    }
}

int main(void)
{
    testProtocol();
    testTx();
    testStallAlign();
    testDripLoss();
    testLevelsAndInvert();
    testOverflowAndTimeout();
    testTimingReport();
    testResync();
    testVersion();
    testBootloaderCommand();
    testPllPlans();
    testSimplex();
    testClockPlans();
    testExtClock();
    testSampleTrim();

    if (failures)
    {
        printf("\n%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("\nALL TESTS PASSED\n");
    return 0;
}
