/* ring_sim.c - feeds the parser through a 2048-byte DMA-style ring with
 * random arrival chunks, to see whether frames straddling the wrap or
 * arriving in pieces upset it.  Build: see Makefile target ring_sim. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../config.h"
#include "../hw.h"
#include "../engine.h"

#define RS 2048U
static uint8_t  ring[RS];
static uint16_t head, tail;
static uint32_t ms;

void     hw_init(void) {}
void     hw_start_sampling(void) {}
bool     hw_ext_clock_missing(void) { return false; }
void     hw_watchdog_kick(void) {}
void     hw_poll(void) {}
void     hw_enter_bootloader(void) {}
uint32_t hw_millis(void) { return ms; }
uint32_t hw_chip_id(void) { return 1; }
void     hw_ptt(bool on) { (void)on; }
void     hw_set_ptt_invert(bool i) { (void)i; }
void     hw_led(uint8_t n, bool on) { (void)n; (void)on; }
uint16_t hw_uart_rx_available(void) { return (uint16_t)((head - tail) & (RS - 1)); }
uint8_t  hw_uart_rx_peek(uint16_t o) { return ring[(tail + o) & (RS - 1)]; }
void     hw_uart_rx_consume(uint16_t c) { tail = (uint16_t)((tail + c) & (RS - 1)); }
bool     hw_uart_tx(const uint8_t *d, uint16_t l) { (void)d; (void)l; return true; }
void     hw_uart_poll(void) {}
uint32_t hw_uart_errors(void) { return 0; }

static void put(const uint8_t *d, unsigned n)
{
    while (n)
    {
        unsigned c = 1 + (unsigned)(rand() % 40);
        if (c > n) c = n;
        for (unsigned i = 0; i < c; i++) { ring[head] = *d++; head = (head + 1) & (RS - 1); }
        n -= c;
        ms++;
        engine_poll();
    }
}

int main(void)
{
    uint8_t cfg[196], st[3] = {0x61, 0x03, 0x10}, fr[196];
    int i, k;

    engine_init();
    memset(cfg, 0, sizeof cfg); cfg[0] = 0x61; cfg[1] = 0xC4; cfg[2] = 0x20;
    put(cfg, 196);
    for (k = 0; k < 20000; k++)
    {
        if (k % 3 == 0) put(st, 3);
        fr[0] = 0x61; fr[1] = 0xC4; fr[2] = 0x70; fr[3] = 140;
        for (i = 4; i < 196; i++) fr[i] = (uint8_t)(rand() & 0x7F);
        put(fr, 196);
        for (i = 0; i < 50; i++) { ms++; engine_poll(); }
    }
    printf("bad=%u\n", engine_stats()->badBytes);
    return 0;
}
