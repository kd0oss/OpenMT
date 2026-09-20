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
 * alsa_io.h - ALSA capture/playback of the SX1255's I/Q baseband over I2S.
 * The SX1255 HAT presents itself as a stereo S32_LE ALSA device at 125 kSa/s:
 * left channel = I, right = Q, * MSB-aligned in the 32-bit word
 * (i.e. treat the raw value as Q1.31 and * normalize by 2^31).
 */
#ifndef ALSA_IO_H
#define ALSA_IO_H

#include <stdbool.h>
#include <complex.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef float complex alsa_cplx_t;

typedef struct alsa_capture alsa_capture_t;
typedef struct alsa_playback alsa_playback_t;

/* device is an ALSA PCM device name, e.g. "hw:sx1255" or "plughw:1,0". */
alsa_capture_t *alsa_capture_open(const char *device, unsigned int sample_rate);
void alsa_capture_close(alsa_capture_t *cap);

/* The actual negotiated rate (which may differ from what was requested --
 * see the comment on reapply_base() in alsa_io.c). Callers should use
 * this, not the value they originally requested, for anything rate-
 * sensitive downstream (in particular, the DSP pipeline's decimation/
 * resampling ratios, which are computed from the real rate rather than
 * assuming it landed on exactly what was asked for). Returns 0 if the
 * rate couldn't be queried. */
unsigned int alsa_capture_get_rate(alsa_capture_t *cap);
unsigned int alsa_playback_get_rate(alsa_playback_t *pb);

/* Blocking read of up to "max_samples" complex IQ samples. Returns the
 * number read, or -1 on error (including buffer over-run, which is logged
 * and treated as recoverable by the caller retrying). */
int alsa_capture_read(alsa_capture_t *cap, alsa_cplx_t *out, int max_samples);

alsa_playback_t *alsa_playback_open(const char *device, unsigned int sample_rate);
void alsa_playback_close(alsa_playback_t *pb);

/* Blocking write of "n_samples" complex IQ samples. Returns true on
 * success. */
bool alsa_playback_write(alsa_playback_t *pb, const alsa_cplx_t *samples, int n_samples);

/* Drop any pending pipeline for pb (called after a TX-off, so the next
 * transmission starts from a clean buffer). */
void alsa_playback_drain(alsa_playback_t *pb);

/* Non-blocking reset of the PCM device's internal state (buffer
 * position, any pending xrun condition) -- safe to call on every
 * key-up, unlike alsa_playback_drain() which blocks. See the comment
 * at its definition for why this exists as a separate call. */
void alsa_playback_prepare(alsa_playback_t *pb);

/* Frames currently buffered in ALSA's hardware playback queue -- i.e.
 * how much "lookahead" of already-written audio is still waiting to
 * actually reach the DAC. Returns -1 on error (e.g. device not
 * running yet). Exists so a caller can pace writes against the
 * hardware's own true consumption rate directly, rather than against
 * an independent software clock assumption about what that rate
 * should be -- the two are never exactly identical oscillators, and
 * pacing against an assumed rate that's even slightly off from the
 * real one accumulates drift over time. */
long alsa_playback_delay_frames(alsa_playback_t *pb);

#ifdef __cplusplus
}
#endif

#endif /* ALSA_IO_H */
