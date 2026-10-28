/* Created by Rick KD0OSS */
#ifndef FAKEHW_H
#define FAKEHW_H
#include <stdint.h>
#include <stdbool.h>
extern uint32_t fake_ms;
extern int      fake_boot_calls;
extern bool     fake_ptt, fake_ptt_invert, fake_led[8];
extern uint8_t  fake_rx[65536];
extern uint32_t fake_rx_len, fake_rx_pos;
extern uint8_t  fake_tx[1 << 20];
extern uint32_t fake_tx_len, fake_tx_cap;
void fake_feed(const uint8_t *data, uint32_t len);
#endif
