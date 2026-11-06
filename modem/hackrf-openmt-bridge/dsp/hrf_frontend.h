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
 * hrf_frontend.h - HackRF sample-rate front end.
 *
 * The HackRF runs at a few Msps; the bridge's tuned FM DSP (dsp.c, shared
 * with sx1255-openmt-bridge) wants 125 kS/s complex baseband.  This module
 * is the translation in both directions:
 *
 *   RX:  int8 IQ @ fs -> shift offset_hz down to DC -> /(N/4) -> /4
 *        -> float complex @ 125 kS/s
 *   TX:  float complex @ 125 kS/s -> x4 -> x(N/4) -> shift offset_hz up
 *        -> int8 IQ @ fs
 *
 * with N = fs / 125000.  At the default 2 Msps that is 16 = 4 x 4.
 *
 * Why offset tuning: the HackRF's ADC/DC offset and LO leakage put a
 * spike at the centre of the spectrum.  Tuning the LO offset_hz away from
 * the wanted frequency and shifting digitally moves that spike out of the
 * channel (the decimation filters then remove it).
 */
#ifndef HRF_FRONTEND_H
#define HRF_FRONTEND_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <complex.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HRF_FE_BASEBAND_RATE 125000U    /* rate the DSP chain runs at */

/* A sample rate is usable when it is a multiple of 500 kHz between
 * 1 and 8 Msps (N = fs/125000 a multiple of 4, 8..64). */
bool hrf_fe_rate_ok(uint32_t fs);

/* fs / 125000 */
unsigned int hrf_fe_ratio(uint32_t fs);

/* ---- RX ---- */
typedef struct hrf_rx_fe hrf_rx_fe_t;

/* offset_hz: the wanted signal sits this far ABOVE the LO (LO = f - offset). */
hrf_rx_fe_t *hrf_rx_fe_create(uint32_t fs, int32_t offset_hz);
void hrf_rx_fe_destroy(hrf_rx_fe_t *fe);
void hrf_rx_fe_reset(hrf_rx_fe_t *fe);

/* Consume n complex int8 samples, write baseband to out (capacity out_cap
 * complex samples).  Returns the number written; at most
 * n / ratio + 1.  Any n is fine. */
size_t hrf_rx_fe_process(hrf_rx_fe_t *fe, const int8_t *iq, size_t n, float complex *out, size_t out_cap);

/* ---- TX ---- */
typedef struct hrf_tx_fe hrf_tx_fe_t;

/* amplitude: 0..1, fraction of int8 full scale for a unit-magnitude input.
 * offset_hz: the signal is generated this far ABOVE the LO. */
hrf_tx_fe_t *hrf_tx_fe_create(uint32_t fs, int32_t offset_hz, float amplitude);
void hrf_tx_fe_destroy(hrf_tx_fe_t *fe);
void hrf_tx_fe_reset(hrf_tx_fe_t *fe);

/* Bytes of int8 IQ produced by n baseband samples. */
size_t hrf_tx_fe_bytes(const hrf_tx_fe_t *fe, size_t n);

/* Convert n baseband samples; out must hold hrf_tx_fe_bytes(fe, n) bytes.
 * Returns the number of bytes written. */
size_t hrf_tx_fe_process(hrf_tx_fe_t *fe, const float complex *in, size_t n, int8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HRF_FRONTEND_H */
