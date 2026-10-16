/*
 *  fakehw.c - PC implementation of hw.h for unit tests.
 */

#include <string.h>
#include "../config.h"
#include "../hw.h"
#include "fakehw.h"

uint32_t fake_ms;
bool     fake_ptt;
int      fake_boot_calls;
bool     fake_ptt_invert;
bool     fake_led[NUM_LEDS];
uint8_t  fake_rx[65536];
uint32_t fake_rx_len;
uint32_t fake_rx_pos;
uint8_t  fake_tx[1 << 20];
uint32_t fake_tx_len;
uint32_t fake_tx_cap = 1 << 20;         /* simulated UART TX capacity   */

void     hw_init(void) {}
void     hw_start_sampling(void) {}
bool     hw_ext_clock_missing(void) { return false; }
void     hw_watchdog_kick(void) {}
void     hw_poll(void) {}
void     hw_enter_bootloader(void) { fake_boot_calls++; }
uint32_t hw_millis(void) { return fake_ms; }
uint32_t hw_chip_id(void) { return 0x10000418UL; }
void     hw_ptt(bool on) { fake_ptt = on; }
void     hw_set_ptt_invert(bool invert) { fake_ptt_invert = invert; }
uint8_t  hw_ptt_state(void) { return fake_ptt ? 1U : 0U; }
void     hw_led(uint8_t n, bool on) { fake_led[n] = on; }

uint16_t hw_uart_rx_available(void) { return (uint16_t)(fake_rx_len - fake_rx_pos); }
uint8_t  hw_uart_rx_peek(uint16_t offset) { return fake_rx[fake_rx_pos + offset]; }
void     hw_uart_rx_consume(uint16_t count) { fake_rx_pos += count; }

bool hw_uart_tx(const uint8_t *data, uint16_t len)
{
    if (fake_tx_len + len > fake_tx_cap)
    {
        return false;
    }
    memcpy(fake_tx + fake_tx_len, data, len);
    fake_tx_len += len;
    return true;
}

void     hw_uart_poll(void) {}
uint32_t hw_uart_errors(void) { return 0; }

void fake_feed(const uint8_t *data, uint32_t len)
{
    memcpy(fake_rx + fake_rx_len, data, len);
    fake_rx_len += len;
}
