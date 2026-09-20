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
 * This bridge's generic, mode-agnostic front end (RX) and back end (TX)
 * only -- the RRC matched filter (RX) and RRC pulse shaping (TX) steps
 * this file's reference implementation also has are each protocol
 * service's own responsibility upstream/downstream of this code, not
 * this pipeline's, so they aren't included here.
 *
 * RX pipeline (125 kSa/s complex IQ from the SX1255 -> real baseband):
 *   DC removal -> polyphase decimating FIR (125k -> 12.5k, 12.5 kHz channel
 *   filter) -> FM demodulator -> baseband DC removal (software AFC) ->
 *   rational resampler (12.5k -> 24k) -> real-valued baseband at 24 kSa/s.
 *
 * TX pipeline (real baseband at 24 kSa/s -> 125 kSa/s complex IQ for the
 * SX1255): rational resampler (24k -> 125k) -> FM modulator -> complex IQ.
 */
#ifndef DSP_H
#define DSP_H

#include <stdint.h>
#include <stdbool.h>
#include <complex.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef float complex dsp_cplx_t;

/* ---- RX pipeline --------------------------------------------------------*/

typedef struct dsp_rx_pipeline dsp_rx_pipeline_t;

/*
 * Pass 2000 and 1.24f unless you have reason to override them (see
 * dsp_rx_pipeline_create_tuned()). actual_sample_rate_hz is the *true*
 * negotiated ALSA capture rate (see alsa_capture_get_rate()), which is
 * not guaranteed to be exactly 125000 -- on real hardware this codebase
 * has been run against, the nearest rate the driver would negotiate to
 * was 128000. Every decimation/resampling ratio and FM demod frequency
 * scaling factor in this pipeline is derived from this actual rate
 * rather than assuming it landed on exactly 125000; a mismatch here
 * (even a small one) systematically miscalibrates the channel filter
 * cutoff, the FM demodulator's Hz-per-radian scaling, and the resampler's
 * target rate on every single frame, which a purely-software TX->RX
 * loopback test (self-consistent by construction, since both ends use
 * the same assumption) can never reveal. */
dsp_rx_pipeline_t *dsp_rx_pipeline_create(unsigned int actual_sample_rate_hz);
dsp_rx_pipeline_t *dsp_rx_pipeline_create_tuned(unsigned int actual_sample_rate_hz,
    int baseband_dc_avg_cnt, float scaling_coeff);
void dsp_rx_pipeline_destroy(dsp_rx_pipeline_t *p);

/* Diagnostic accessor: the baseband DC filter's current moving-average
 * value (the estimated frequency-offset bias, in the same units as the
 * demodulated signal feeding it). Track this over a long capture to see
 * whether it converges and stays stable, or drifts/diverges -- see the
 * comment on dc_filter_r_process() in dsp.c for why this specifically
 * matters for sync distance. Returns 0 if the filter is disabled
 * (baseband_dc_avg_cnt <= 1 at creation). */
double dsp_rx_pipeline_get_dc_bias(const dsp_rx_pipeline_t *p);

/*
 * Feed one 125 kSa/s complex IQ sample in. Writes up to "max_out"
 * produced real-valued baseband samples (at 24 kSa/s) directly to "out"
 * -- DC removal, decimator, FM demod, baseband DC/AFC filter, resampler,
 * scaled by scalingCoeff (a generic hardware/demodulator gain
 * calibration, not specific to any one mode's own matched filter).
 * Returns the number of samples written (0, 1, or on rare rounding
 * boundaries slightly more).
 *
 * The RRC matched filter, symbol slicing, sync, and FEC for whichever
 * digital mode is running are each protocol service's own responsibility
 * downstream of this call, not this pipeline's -- every digital mode
 * built on 4FSK/GMSK-family modulation needs this same generic front
 * end, but each has its own, different matched filter and decode logic
 * after it.
 */
int dsp_rx_pipeline_process_baseband(dsp_rx_pipeline_t *p, dsp_cplx_t iq_sample, float *out, int max_out);

/* ---- TX pipeline ---------------------------------------------------------*/

typedef struct dsp_tx_pipeline dsp_tx_pipeline_t;

/* actual_sample_rate_hz: see the note on dsp_rx_pipeline_create() above --
 * the true negotiated ALSA playback rate, used to derive the TX
 * resampler's ratio and the FM modulator's Hz-per-radian scaling, rather
 * than assuming exactly 125000. */
dsp_tx_pipeline_t *dsp_tx_pipeline_create(unsigned int actual_sample_rate_hz);
void dsp_tx_pipeline_destroy(dsp_tx_pipeline_t *p);
void dsp_tx_pipeline_reset(dsp_tx_pipeline_t *p);

/*
 * Convert "n_samples" of an already pulse-shaped, real-valued baseband
 * signal at 24 kSa/s directly to complex IQ at 125 kSa/s -- resampler,
 * FM modulator only. Writes to *out, which this function allocates/
 * reallocates (caller frees); returns the number of IQ samples written.
 * deviation_hz is the FM deviation for a baseband sample value of 1.0.
 *
 * Pulse shaping is each protocol service's own responsibility upstream
 * of this call, not this pipeline's, for exactly the same reason the
 * RRC matched filter is on the RX side -- see
 * dsp_rx_pipeline_process_baseband()'s own comment.
 */
int dsp_tx_pipeline_process_baseband(dsp_tx_pipeline_t *p, const float *baseband_samples,
    int n_samples, double deviation_hz, dsp_cplx_t **out, int *out_cap);

#ifdef __cplusplus
}
#endif

#endif /* DSP_H */
