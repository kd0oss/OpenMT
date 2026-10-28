/* Created by Rick KD0OSS */
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
 * Test stand-in for starting the boot loader: counts the calls in
 * fake_boot_calls.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void     hw_enter_bootloader(void) { fake_boot_calls++; }
/*
 * hw_millis()
 *
 * Gets the fake clock, which the tests advance by hand.
 *
 * Parameters: none.
 *
 * Returns: fake_ms.
 */
uint32_t hw_millis(void) { return fake_ms; }
/*
 * hw_chip_id()
 *
 * Test stand-in for the chip ID.
 *
 * Parameters: none.
 *
 * Returns: 0x10000418.
 */
uint32_t hw_chip_id(void) { return 0x10000418UL; }
/*
 * hw_ptt()
 *
 * Test stand-in for PTT: records the state in fake_ptt.
 *
 * Parameters:
 *   on - Wanted PTT state.
 *
 * Returns: nothing.
 */
void     hw_ptt(bool on) { fake_ptt = on; }
/*
 * hw_set_ptt_invert()
 *
 * Test stand-in: records the PTT invert flag in fake_ptt_invert.
 *
 * Parameters:
 *   invert - PTT invert flag.
 *
 * Returns: nothing.
 */
void     hw_set_ptt_invert(bool invert) { fake_ptt_invert = invert; }
/*
 * hw_ptt_state()
 *
 * Test stand-in for the PTT diagnostic value.
 *
 * Parameters: none.
 *
 * Returns: 1 if PTT is on, otherwise 0.
 */
uint8_t  hw_ptt_state(void) { return fake_ptt ? 1U : 0U; }
/*
 * hw_led()
 *
 * Test stand-in for an LED: records the state in fake_led.
 *
 * Parameters:
 *   n  - LED number.
 *   on - true if lit.
 *
 * Returns: nothing.
 */
void     hw_led(uint8_t n, bool on) { fake_led[n] = on; }

/*
 * hw_uart_rx_available()
 *
 * Gets the number of bytes the test has fed that the engine has not yet
 * consumed.
 *
 * Parameters: none.
 *
 * Returns: Number of bytes.
 */
uint16_t hw_uart_rx_available(void) { return (uint16_t)(fake_rx_len - fake_rx_pos); }
/*
 * hw_uart_rx_peek()
 *
 * Reads a byte of the fed input without removing it.
 *
 * Parameters:
 *   offset - Position counted from the oldest unread byte.
 *
 * Returns: The byte.
 */
uint8_t  hw_uart_rx_peek(uint16_t offset) { return fake_rx[(uint16_t)(fake_rx_pos + offset)]; }
/*
 * hw_uart_rx_consume()
 *
 * Removes bytes from the front of the fed input.
 *
 * Parameters:
 *   count - Number of bytes to remove.
 *
 * Returns: nothing.
 */
void     hw_uart_rx_consume(uint16_t count) { fake_rx_pos += count; }

/*
 * hw_uart_tx()
 *
 * Collects the bytes the engine sends in fake_tx, up to the simulated
 * capacity fake_tx_cap.
 *
 * Parameters:
 *   data - Bytes to send.
 *   len  - Number of bytes.
 *
 * Returns: true if the bytes were kept, false if they would not fit.
 */
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
 * hw_uart_error_detail()
 *
 * Test stand-in for the detailed UART error counts; all are zero.
 *
 * Parameters:
 *   ore - Receives the overrun count.
 *   fe  - Receives the framing error count.
 *   ne  - Receives the noise error count.
 *
 * Returns: nothing.
 */
void hw_uart_error_detail(uint32_t *ore, uint32_t *fe, uint32_t *ne) { *ore = 0; *fe = 0; *ne = 0; }
/*
 * hw_uart_rx_total()
 *
 * Gets the total number of input bytes consumed so far.
 *
 * Parameters: none.
 *
 * Returns: Number of bytes.
 */
uint32_t hw_uart_rx_total(void) { return fake_rx_pos; }

/*
 * fake_feed()
 *
 * Adds bytes to the input the engine will read, as if they had arrived on
 * the UART.
 *
 * Parameters:
 *   data - Bytes to add.
 *   len  - Number of bytes.
 *
 * Returns: nothing.
 */
void fake_feed(const uint8_t *data, uint32_t len)
{
    memcpy(fake_rx + fake_rx_len, data, len);
    fake_rx_len += len;
}
