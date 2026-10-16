/* Checks the "raw counts" scaling used to match older firmware.
 * Build:  make -C tests legacy */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../config.h"
#include "../hw.h"
#include "../engine.h"
#include "fakehw.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    uint8_t cfg[196] = { 0x61, 0xC4, 0x20, 0x01, 128 };
    uint8_t tx[196] = { 0x61, 0xC4, 0x70, 150 };
    uint32_t i;
    int16_t v;
    uint16_t dac = 0;

    engine_init();
    fake_feed(cfg, 196);
    for (i = 0; i < 24; i++) { engine_sample_tick(2048); fake_ms++; engine_poll(); }

    /* RX: 1000 counts above mid at rxLevel 128 -> 500 (old: x rxLevel/256) */
    fake_tx_len = 0;
    for (i = 0; i < 96 * 2; i++) { engine_sample_tick(3048); }
    engine_poll();
    memcpy(&v, fake_tx + 3 + 2 * 50, 2);
    printf("RX 1000 counts, rxLevel 128 -> %d (old firmware: 500)\n", v);
    CHECK(v == 500, "rx %d", v);

    /* TX: sample 2000 at txLevel 150 -> 2000*150/256 = 1171 counts above mid */
    for (i = 0; i < 96; i++) { tx[4 + 2 * i] = 2000 & 0xFF; tx[5 + 2 * i] = 2000 >> 8; }
    fake_feed(tx, 196);
    fake_feed(tx, 196);
    for (i = 0; i < 24; i++) { engine_sample_tick(2048); fake_ms++; engine_poll(); }
    for (i = 0; i < 24 * 20; i++) { uint16_t d = engine_sample_tick(2048); if (d != DAC_MID) { dac = d; break; } }
    printf("TX sample 2000, txLevel 150 -> DAC %u (old firmware: %d)\n", dac, 2048 + 2000 * 150 / 256);
    CHECK(dac == 2048 + 2000 * 150 / 256, "tx dac %u", dac);
    printf(fails ? "FAILED\n" : "LEGACY SCALING OK\n");
    return fails != 0;
}
