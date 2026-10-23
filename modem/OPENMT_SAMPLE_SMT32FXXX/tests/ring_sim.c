/* Created by Rick KD0OSS */
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

/*
 * hw_init()
 *
 * Test stand-in for hardware initialisation; does nothing.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void     hw_init(void) {}
/*
 * hw_start_sampling()
 *
 * Test stand-in for starting the sample interrupt; does nothing.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void     hw_start_sampling(void) {}
/*
 * hw_ext_clock_missing()
 *
 * Test stand-in: the external clock is never missing.
 *
 * Parameters: none.
 *
 * Returns: false.
 */
bool     hw_ext_clock_missing(void) { return false; }
/*
 * hw_watchdog_kick()
 *
 * Test stand-in for feeding the watchdog; does nothing.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void     hw_watchdog_kick(void) {}
/*
 * hw_poll()
 *
 * Test stand-in for hardware housekeeping; does nothing.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void     hw_poll(void) {}
/*
 * hw_enter_bootloader()
 *
 * Test stand-in for starting the boot loader; does nothing.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void     hw_enter_bootloader(void) {}
/*
 * hw_millis()
 *
 * Gets the fake clock, which the test advances by hand.
 *
 * Parameters: none.
 *
 * Returns: Fake time in milliseconds.
 */
uint32_t hw_millis(void) { return ms; }
/*
 * hw_chip_id()
 *
 * Test stand-in for the chip ID.
 *
 * Parameters: none.
 *
 * Returns: 1.
 */
uint32_t hw_chip_id(void) { return 1; }
/*
 * hw_ptt()
 *
 * Test stand-in for PTT; does nothing.
 *
 * Parameters:
 *   on - Wanted PTT state (not used).
 *
 * Returns: nothing.
 */
void     hw_ptt(bool on) { (void)on; }
/*
 * hw_set_ptt_invert()
 *
 * Test stand-in for the PTT invert flag; does nothing.
 *
 * Parameters:
 *   i - PTT invert flag (not used).
 *
 * Returns: nothing.
 */
void     hw_set_ptt_invert(bool i) { (void)i; }
/*
 * hw_led()
 *
 * Test stand-in for an LED; does nothing.
 *
 * Parameters:
 *   n  - LED number (not used).
 *   on - true if lit (not used).
 *
 * Returns: nothing.
 */
void     hw_led(uint8_t n, bool on) { (void)n; (void)on; }
/*
 * hw_uart_rx_available()
 *
 * Gets the number of bytes in the simulated 2048-byte receive ring.
 *
 * Parameters: none.
 *
 * Returns: Number of bytes.
 */
uint16_t hw_uart_rx_available(void) { return (uint16_t)((head - tail) & (RS - 1)); }
/*
 * hw_uart_rx_peek()
 *
 * Reads a byte of the simulated receive ring without removing it.
 *
 * Parameters:
 *   o - Position counted from the oldest unread byte.
 *
 * Returns: The byte.
 */
uint8_t  hw_uart_rx_peek(uint16_t o) { return ring[(tail + o) & (RS - 1)]; }
/*
 * hw_uart_rx_consume()
 *
 * Removes bytes from the front of the simulated receive ring.
 *
 * Parameters:
 *   c - Number of bytes to remove.
 *
 * Returns: nothing.
 */
void     hw_uart_rx_consume(uint16_t c) { tail = (uint16_t)((tail + c) & (RS - 1)); }
/*
 * hw_uart_tx()
 *
 * Test stand-in for sending to the host; discards the bytes.
 *
 * Parameters:
 *   d - Bytes to send (not used).
 *   l - Number of bytes (not used).
 *
 * Returns: true.
 */
bool     hw_uart_tx(const uint8_t *d, uint16_t l) { (void)d; (void)l; return true; }
/*
 * hw_uart_poll()
 *
 * Test stand-in for servicing the UART; does nothing.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void     hw_uart_poll(void) {}
/*
 * hw_uart_errors()
 *
 * Test stand-in for the UART error count.
 *
 * Parameters: none.
 *
 * Returns: 0.
 */
uint32_t hw_uart_errors(void) { return 0; }

/*
 * put()
 *
 * Writes bytes into the simulated receive ring in random chunks of 1 to 40
 * bytes, advancing the clock and running engine_poll() after each chunk, to
 * imitate DMA arrival.
 *
 * Parameters:
 *   d - Bytes to write.
 *   n - Number of bytes.
 *
 * Returns: nothing.
 */
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

/*
 * main()
 *
 * Sends a SET_CONFIG frame, then 20000 rounds of a status request and a
 * random 196-byte sample frame, each through the ring in random chunks with
 * a pause between, and prints the number of bad bytes the parser skipped (it
 * should be 0).
 *
 * Parameters: none.
 *
 * Returns: 0.
 */
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
