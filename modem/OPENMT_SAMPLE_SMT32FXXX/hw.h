/*
 *  hw.h - hardware abstraction used by the portable engine.
 *
 *  hw_f1.c (F105) and hw_f4f7.c (F446, F722) implement this.  tests/fakehw.c implements
 *  it on a PC so the protocol and sample engine can be unit tested.
 */

#ifndef HW_H
#define HW_H

#include <stdint.h>
#include <stdbool.h>

/* Number of LEDs the engine drives */
#define NUM_LEDS    8

void     hw_init(void);                 /* clocks, pins, UART, ADC, DAC  */
void     hw_start_sampling(void);       /* enable the 24 kHz interrupt   */
bool     hw_ext_clock_missing(void);    /* true if fallback was used     */
void     hw_watchdog_kick(void);
void     hw_enter_bootloader(void);     /* does not return               */
void     hw_poll(void);                 /* slow housekeeping, main loop  */

uint32_t hw_millis(void);
uint32_t hw_chip_id(void);              /* DBGMCU_IDCODE: 0x..418 = F105 */

void     hw_ptt(bool on);               /* physical level handled in hw  */
void     hw_set_ptt_invert(bool invert);/* host "pttInvert" flag         */
/* Diagnostic: what the PTT pin really is.  Bit 0 PTT wanted, 1 output latch
 * high, 2 pin reads high, 3 inhibit active, 4 host pttInvert.             */
uint8_t  hw_ptt_state(void);
void     hw_led(uint8_t n, bool on);

/* UART byte stream to / from the Raspberry Pi */
uint16_t hw_uart_rx_available(void);
uint8_t  hw_uart_rx_peek(uint16_t offset);
void     hw_uart_rx_consume(uint16_t count);
bool     hw_uart_tx(const uint8_t *data, uint16_t len);   /* false = full */
void     hw_uart_poll(void);            /* keep the TX DMA moving        */
uint32_t hw_uart_errors(void);          /* overrun/framing counter       */

/* Raw sample hooks: called from the sample interrupt */
uint16_t engine_sample_tick(uint16_t adc_raw);

#endif /* HW_H */
