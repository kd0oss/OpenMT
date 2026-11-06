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

/*
 * hrf_dev.h - the small slice of a HackRF this bridge uses.
 *
 * Two implementations sit behind it:
 *   hw/hrf_dev.c  - the real thing, on top of libhackrf
 *   hw/hrf_sim.c  - a software HackRF for tests (reads RX IQ from a file,
 *                   writes the TX IQ it is given to a file), selected with
 *                   -DHRF_SIM=ON at configure time
 *
 * Samples are the HackRF's native format: interleaved signed 8-bit I, Q.
 * The HackRF is half duplex: it is either receiving or transmitting, never
 * both, and switching takes tens of milliseconds.
 */
#ifndef HRF_DEV_H
#define HRF_DEV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct hrf_dev hrf_dev_t;

/* Called on the device's USB thread with n bytes (n/2 complex samples) of
 * received IQ.  Must return quickly.  Return 0 to continue. */
typedef int (*hrf_rx_cb)(const int8_t *iq, size_t n, void *ctx);

/* Called on the device's USB thread to fill exactly n bytes of IQ for
 * transmission.  Must return quickly and always fill the whole buffer.
 * Return 0 to continue. */
typedef int (*hrf_tx_cb)(int8_t *iq, size_t n, void *ctx);

/* serial may be NULL (first HackRF found).  For the simulator it is
 * "rxfile,txfile" (rxfile may be empty for silence). */
hrf_dev_t *hrf_open(const char *serial);
void hrf_close(hrf_dev_t *d);

bool hrf_set_sample_rate(hrf_dev_t *d, uint32_t hz);
bool hrf_set_baseband_bw(hrf_dev_t *d, uint32_t hz);
bool hrf_set_freq(hrf_dev_t *d, uint64_t hz);
/* lna 0-40 dB in 8 dB steps, vga 0-62 dB in 2 dB steps (values are
 * rounded down to a valid step by the caller). */
bool hrf_set_rx_gain(hrf_dev_t *d, unsigned lna_db, unsigned vga_db);
/* TX VGA 0-47 dB in 1 dB steps. */
bool hrf_set_tx_gain(hrf_dev_t *d, unsigned txvga_db);
/* The 14 dB RF amplifier, shared by RX and TX. */
bool hrf_set_amp(hrf_dev_t *d, bool on);
/* Antenna port power (bias tee). */
bool hrf_set_bias_tee(hrf_dev_t *d, bool on);

bool hrf_start_rx(hrf_dev_t *d, hrf_rx_cb cb, void *ctx);
bool hrf_stop_rx(hrf_dev_t *d);
bool hrf_start_tx(hrf_dev_t *d, hrf_tx_cb cb, void *ctx);
bool hrf_stop_tx(hrf_dev_t *d);

/* The TX callback is handed buffers of this many bytes, and this many of
 * them are queued in the USB stack ahead of the one being filled: audio the
 * callback hands over reaches the antenna roughly HRF_TX_QUEUE_BUFFERS
 * buffers later.  libhackrf uses 4 transfers of 262144 bytes. */
#define HRF_TX_BUFFER_BYTES   262144U
#define HRF_TX_QUEUE_BUFFERS  4U

#ifdef __cplusplus
}
#endif

#endif /* HRF_DEV_H */
