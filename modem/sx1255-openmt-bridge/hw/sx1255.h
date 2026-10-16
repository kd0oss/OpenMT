/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 *                                                                         *
 ***************************************************************************/

#ifndef SX1255_H
#define SX1255_H

#include <stdint.h>
#include <stdbool.h>
#include "gpio_line.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SX1255_CLK_FREQ_HZ        32000000.0
#define SX1255_SAMPLE_RATE_HZ     125000
#define SX1255_EXPECTED_VERSION   0x11

typedef struct
{
    int spi_fd;
    gpio_line_t reset_line;
    bool have_reset_line;
} sx1255_t;

/* Open the spidev device (e.g. "/dev/spidev0.0") and, if reset_chip_path is
 * non-NULL, request reset_pin as an output on that GPIO chip
 * (e.g. "/dev/gpiochip0"). Returns true on success. */
bool sx1255_open(sx1255_t *dev, const char *spi_path, const char *reset_chip_path, unsigned int reset_pin);

/* Hardware reset + register bring-up sequence (mirrors sx1255Init()).
 * Verifies the chip version register reads SX1255_EXPECTED_VERSION. */
bool sx1255_init(sx1255_t *dev);

bool sx1255_reset(sx1255_t *dev);

/* Frequency in Hz -> 24-bit PLL register value, and vice versa. */
uint32_t sx1255_freq_to_reg(uint32_t hz);

bool sx1255_set_rx_freq(sx1255_t *dev, uint32_t hz);
bool sx1255_set_tx_freq(sx1255_t *dev, uint32_t hz);

/* Configure the I2S interface for 125 kSa/s, Mode B2. */
bool sx1255_set_rate(sx1255_t *dev);

/* Gain controls. db values are clamped/snapped to the nearest supported
 * setting, mirroring the reference implementation. */
bool sx1255_set_lna_gain(sx1255_t *dev, uint8_t db);   /* 0,12,24,36,42,48 dB */
bool sx1255_set_pga_gain(sx1255_t *dev, uint8_t db);   /* 0-30 dB, 2 dB steps */
bool sx1255_set_dac_gain(sx1255_t *dev, int8_t db);    /* 0,-3,-6,-9 dB */
bool sx1255_set_mixer_gain(sx1255_t *dev, float db);   /* -37.5..-7.5 dB, 2 dB steps */

bool sx1255_set_rx_pll_bw(sx1255_t *dev, uint16_t bw_khz); /* 75/150/225/300 */
bool sx1255_set_tx_pll_bw(sx1255_t *dev, uint16_t bw_khz);

bool sx1255_enable_rx(sx1255_t *dev, bool enable);
bool sx1255_enable_tx(sx1255_t *dev, bool enable);
bool sx1255_enable_rf_loopback(sx1255_t *dev, bool enable);

/* Returns false (with pll_locked left unmodified) if the status register
 * could not be read. */
bool sx1255_get_pll_status(sx1255_t *dev, bool *tx_locked, bool *rx_locked);

/* Polls the PLL status register (like sx1255_get_pll_status, but
 * repeatedly, roughly every 10ms since register reads already pace
 * themselves at about that rate) until the requested PLL reports lock or
 * timeout_ms elapses. Matches the reference implementation's
 * sx1255WaitPLLLock() -- a single immediate check is often too early: on
 * a marginal power supply, the PA switching on can brown out the rail
 * enough to delay PLL lock well past when a one-shot check would give up. */
bool sx1255_wait_pll_lock(sx1255_t *dev, bool tx, int timeout_ms);

/* Reads chip version register (0x07), returns 0 on read failure. */
uint8_t sx1255_get_chip_version(sx1255_t *dev);

void sx1255_close(sx1255_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* SX1255_H */
