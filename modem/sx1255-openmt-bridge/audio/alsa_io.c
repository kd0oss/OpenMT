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
 * alsa_io.c - ALSA capture/playback of the SX1255's I/Q baseband.
 *
 * _GNU_SOURCE must be defined before any system header is pulled in here:
 * with strict -std=c11 (no _DEFAULT_SOURCE/_POSIX_C_SOURCE), older
 * alsa-lib packages (notably on Debian/Ubuntu aarch64) end up re-declaring
 * "struct timespec" in <alsa/global.h> because the feature-test macro they
 * probe for isn't set, colliding with glibc's own declaration. Defining
 * _GNU_SOURCE first keeps glibc and alsa-lib's macro probing consistent.
 * Guarded here since the build system (see CMakeLists.txt) also defines
 * this globally for every translation unit -- the #ifndef keeps this
 * file correct and warning-free either way: under the normal CMake
 * build (where it's already set, and re-defining it verbatim would
 * otherwise trigger a harmless but noisy "_GNU_SOURCE redefined"
 * warning) and if this file is ever compiled standalone or by some
 * other build setup that doesn't define it globally.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "alsa_io.h"

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHANNELS 2

/* Power-of-two sizes chosen there specifically for BCM2835 I2S DMA
 * compatibility; kept identical here rather than re-derived. Used for both
 * capture and playback -- the reference uses the same constants for both.
 * Sample rate is passed in by the caller (both currently pass
 * SX1255_SAMPLE_RATE_HZ from hw/sx1255.h) rather than duplicated as a constant
 * here -- see the comment on reapply_base() for why it's requested explicitly
 * at all. */
#define ALSA_BUFFER_FRAMES 16384u
#define ALSA_PERIOD_FRAMES 4096u

struct alsa_capture
{
    snd_pcm_t* handle;
    int32_t* raw; /* scratch buffer, CHANNELS * max_samples */
    int raw_cap;
    unsigned int negotiated_rate;
};

struct alsa_playback
{
    snd_pcm_t* handle;
    int32_t* raw;
    int raw_cap;
    unsigned int negotiated_rate;
};

/*
 * log_negotiated()
 *
 * Logs the rate, buffer size and period size the ALSA driver reports for a
 * configured PCM device.  The rate is only advisory because the SX1255 is
 * the I2S master and sets the real bus rate.
 *
 * Parameters:
 *   kind   - "capture" or "playback", used in the log text.
 *   handle - PCM device whose current hardware parameters are read.
 *
 * Returns: The driver-reported sample rate in Hz, or 0 if the parameters
 *   cannot be read.
 */
static unsigned int log_negotiated(const char* kind, snd_pcm_t* handle)
{
    snd_pcm_hw_params_t* hw_params;
    unsigned int rate     = 0;
    int dir               = 0;
    snd_pcm_uframes_t buf = 0, period = 0;

    snd_pcm_hw_params_alloca(&hw_params);
    if (snd_pcm_hw_params_current(handle, hw_params) != 0)
    {
        return 0;
    }
    snd_pcm_hw_params_get_rate(hw_params, &rate, &dir);
    snd_pcm_hw_params_get_buffer_size(hw_params, &buf);
    snd_pcm_hw_params_get_period_size(hw_params, &period, &dir);

    fprintf(stderr, "alsa_io: %s device: driver-reported rate %u Hz (advisory -- the SX1255 is the I2S\n", kind, rate);
    fprintf(stderr, "alsa_io: master and clocks the bus at whatever rate its SPI registers configured\n");
    fprintf(stderr, "alsa_io: regardless of what this says), buffer=%lu period=%lu frames\n", (unsigned long)buf, (unsigned long)period);
    return rate;
}

/*
 * reapply_base()
 *
 * Resets the hardware parameter set and applies the fixed base settings:
 * interleaved read/write access, stereo, 32-bit little-endian samples and
 * the requested rate.  The exact rate is tried first, then the nearest rate;
 * if both fail the parameters are reset and the driver default rate is used.
 * It is called again whenever a failed buffer or period call may have left
 * the parameters inconsistent.
 *
 * Parameters:
 *   kind        - "capture" or "playback", used in log text.
 *   handle      - PCM device being configured.
 *   hw_params   - Hardware parameter set that is reset and filled in.
 *   sample_rate - Requested sample rate in Hz.
 *
 * Returns: true if the base parameters were applied, false if a mandatory
 *   setting was refused.
 */
static bool reapply_base(const char* kind, snd_pcm_t* handle, snd_pcm_hw_params_t* hw_params, unsigned int sample_rate)
{
    int err;
    unsigned int rate;

    if ((err = snd_pcm_hw_params_any(handle, hw_params)) < 0)
    {
        fprintf(stderr, "alsa_io: %s hw_params_any: %s\n", kind, snd_strerror(err));
        return false;
    }
    if ((err = snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
    {
        fprintf(stderr, "alsa_io: %s set_access: %s\n", kind, snd_strerror(err));
        return false;
    }
    /* Channels before format, matching NegotiateChannels() then
     * NegotiateFormat() in the reference -- refine-based negotiation can
     * be order-sensitive on this driver. */
    if ((err = snd_pcm_hw_params_set_channels(handle, hw_params, CHANNELS)) < 0)
    {
        fprintf(stderr, "alsa_io: %s set_channels: %s\n", kind, snd_strerror(err));
        return false;
    }
    if ((err = snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S32_LE)) < 0)
    {
        fprintf(stderr, "alsa_io: %s set_format S32_LE: %s\n", kind, snd_strerror(err));
        return false;
    }

    rate = sample_rate;
    if (snd_pcm_hw_params_set_rate(handle, hw_params, rate, 0) < 0)
    {
        rate = sample_rate;
        if (snd_pcm_hw_params_set_rate_near(handle, hw_params, &rate, NULL) < 0)
        {
            fprintf(stderr, "alsa_io: %s could not request or negotiate a rate near %u Hz; falling back to\n", kind, sample_rate);
            fprintf(stderr, "alsa_io: whatever the driver defaults to unconstrained -- check the negotiated rate\n");
            fprintf(stderr, "alsa_io: this logs below carefully; if it isn't close to %u Hz, capture/playback\n", sample_rate);
            fprintf(stderr, "alsa_io: timing on this system cannot be trusted.\n");
            /* Reset once more: a failed rate call, like a failed
             * buffer/period call, can leave hw_params inconsistent for
             * the parameters that *did* succeed. */
            if ((err = snd_pcm_hw_params_any(handle, hw_params)) < 0 ||
                (err = snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
                (err = snd_pcm_hw_params_set_channels(handle, hw_params, CHANNELS)) < 0 ||
                (err = snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S32_LE)) < 0)
            {
                fprintf(stderr, "alsa_io: %s failed re-establishing base params after rate fallback: %s\n", kind, snd_strerror(err));
                return false;
            }
        }
        else if (rate != sample_rate)
        {
            fprintf(stderr, "alsa_io: %s note -- nearest available rate to %u Hz was %u Hz\n", kind, sample_rate, rate);
        }
    }

    return true;
}

/*
 * configure_pcm()
 *
 * Configures a PCM device: base parameters, then buffer size and period
 * size, each trying a list of candidate sizes until the driver accepts one.
 * If every candidate fails the driver default is used.  The parameters are
 * committed and the device is prepared.
 *
 * Parameters:
 *   kind        - "capture" or "playback", used in log text.
 *   handle      - PCM device to configure.
 *   sample_rate - Requested sample rate in Hz.
 *   out_rate    - Receives the driver-reported rate after configuration; may
 *                 be NULL.
 *
 * Returns: true on success, false if the device could not be configured.
 */
static bool configure_pcm(const char* kind, snd_pcm_t* handle, unsigned int sample_rate, unsigned int* out_rate)
{
    snd_pcm_hw_params_t* hw_params;
    static const unsigned int buffer_candidates[] = {ALSA_BUFFER_FRAMES, 8192u,
                                                     4096u};
    static const unsigned int period_candidates[] = {ALSA_PERIOD_FRAMES, 2048u,
                                                     1024u};
    unsigned int got_buffer_frames                = 0;
    unsigned int got_period_frames                = 0;
    size_t i;
    int err;

    snd_pcm_hw_params_alloca(&hw_params);

    if (!reapply_base(kind, handle, hw_params, sample_rate))
    {
        return false;
    }

    for (i = 0; i < 3; i++)
    {
        unsigned int v = buffer_candidates[i];

        if (snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, (snd_pcm_uframes_t*)&v) >= 0)
        {
            got_buffer_frames = v;
            break;
        }
        if (!reapply_base(kind, handle, hw_params, sample_rate))
        {
            return false;
        }
    }
    if (got_buffer_frames == 0)
    {
        fprintf(stderr, "alsa_io: %s buffer size negotiation failed for all candidates, using device default\n", kind);
    }

    for (i = 0; i < 3; i++)
    {
        unsigned int v = period_candidates[i];
        int dir        = 0;

        if (snd_pcm_hw_params_set_period_size_near(handle, hw_params, (snd_pcm_uframes_t*)&v, &dir) >= 0)
        {
            got_period_frames = v;
            break;
        }
        /* Reset and reapply the base parameters, plus the buffer size we
         * already secured (if any), before trying the next period
         * candidate on a clean struct. */
        if (!reapply_base(kind, handle, hw_params, sample_rate))
        {
            return false;
        }
        if (got_buffer_frames != 0)
        {
            unsigned int bv = got_buffer_frames;

            if (snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, (snd_pcm_uframes_t*)&bv) < 0)
            {
                got_buffer_frames = 0; /* couldn't restore it; proceed without */
            }
        }
    }
    if (got_period_frames == 0)
    {
        fprintf(stderr, "alsa_io: %s period size negotiation failed for all candidates, using device default\n", kind);
    }

    if ((err = snd_pcm_hw_params(handle, hw_params)) < 0)
    {
        fprintf(stderr, "alsa_io: %s hw_params commit: %s\n", kind, snd_strerror(err));
        return false;
    }
    if ((err = snd_pcm_prepare(handle)) < 0)
    {
        fprintf(stderr, "alsa_io: %s prepare: %s\n", kind, snd_strerror(err));
        return false;
    }

    unsigned int rate = log_negotiated(kind, handle);

    if (out_rate != NULL)
    {
        *out_rate = rate;
    }
    return true;
}

/*
 * alsa_capture_open()
 *
 * Opens an ALSA device for capturing the SX1255 I/Q stream and configures
 * it.
 *
 * Parameters:
 *   device      - ALSA device name.
 *   sample_rate - Requested sample rate in Hz.
 *
 * Returns: A new capture handle, or NULL if the device cannot be opened or
 *   configured.
 */
alsa_capture_t* alsa_capture_open(const char* device, unsigned int sample_rate)
{
    alsa_capture_t* cap = calloc(1, sizeof(*cap));
    int err;

    err = snd_pcm_open(&cap->handle, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0)
    {
        fprintf(stderr, "alsa_io: snd_pcm_open(%s, capture): %s\n", device, snd_strerror(err));
        free(cap);
        return NULL;
    }

    if (!configure_pcm("capture", cap->handle, sample_rate, &cap->negotiated_rate))
    {
        snd_pcm_close(cap->handle);
        free(cap);
        return NULL;
    }

    return cap;
}

/*
 * alsa_capture_get_rate()
 *
 * Gets the sample rate the driver reported when the capture device was
 * configured.
 *
 * Parameters:
 *   cap - Capture handle, may be NULL.
 *
 * Returns: Rate in Hz, or 0 if cap is NULL.
 */
unsigned int alsa_capture_get_rate(alsa_capture_t* cap)
{
    return (cap != NULL) ? cap->negotiated_rate : 0;
}

/*
 * alsa_capture_close()
 *
 * Closes the capture device and frees the handle and its buffer.
 *
 * Parameters:
 *   cap - Capture handle, may be NULL.
 *
 * Returns: nothing.
 */
void alsa_capture_close(alsa_capture_t* cap)
{
    if (cap == NULL)
    {
        return;
    }
    if (cap->handle != NULL)
    {
        snd_pcm_close(cap->handle);
    }
    free(cap->raw);
    free(cap);
}

/*
 * alsa_capture_read()
 *
 * Reads up to max_samples stereo frames from the capture device and converts
 * them to complex floats (left channel is I, right channel is Q) scaled to
 * the range -1 to +1.  An overrun is recovered and reported as zero samples.
 *
 * Parameters:
 *   cap         - Capture handle.
 *   out         - Receives the complex samples; must hold max_samples
 *                 entries.
 *   max_samples - Maximum number of frames to read.
 *
 * Returns: Number of samples written to out, 0 after a recovered overrun, or
 *   -1 on an unrecoverable error.
 */
int alsa_capture_read(alsa_capture_t* cap, alsa_cplx_t* out, int max_samples)
{
    snd_pcm_sframes_t n;
    int i;

    if (cap->raw_cap < max_samples)
    {
        cap->raw = realloc(cap->raw, (size_t)max_samples * CHANNELS * sizeof(int32_t));
        cap->raw_cap = max_samples;
    }

    n = snd_pcm_readi(cap->handle, cap->raw, (snd_pcm_uframes_t)max_samples);
    if (n < 0)
    {
        int err = snd_pcm_recover(cap->handle, (int)n, 1);

        if (err < 0)
        {
            fprintf(stderr, "alsa_io: capture read error: %s\n", snd_strerror((int)n));
            return -1;
        }
        fprintf(stderr, "alsa_io: capture overrun recovered\n");
        return 0;
    }

    for (i = 0; i < n; i++)
    {
        float iv = (float)cap->raw[2 * i] / 2147483648.0f;
        float qv = (float)cap->raw[2 * i + 1] / 2147483648.0f;

        out[i] = iv + qv * I;
    }
    return (int)n;
}

/*
 * alsa_playback_open()
 *
 * Opens an ALSA device for playing the I/Q stream to the SX1255 and
 * configures it.
 *
 * Parameters:
 *   device      - ALSA device name.
 *   sample_rate - Requested sample rate in Hz.
 *
 * Returns: A new playback handle, or NULL if the device cannot be opened or
 *   configured.
 */
alsa_playback_t* alsa_playback_open(const char* device, unsigned int sample_rate)
{
    alsa_playback_t* pb = calloc(1, sizeof(*pb));
    int err;

    err = snd_pcm_open(&pb->handle, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0)
    {
        fprintf(stderr, "alsa_io: snd_pcm_open(%s, playback): %s\n", device, snd_strerror(err));
        free(pb);
        return NULL;
    }

    if (!configure_pcm("playback", pb->handle, sample_rate, &pb->negotiated_rate))
    {
        snd_pcm_close(pb->handle);
        free(pb);
        return NULL;
    }

    return pb;
}

/*
 * alsa_playback_get_rate()
 *
 * Gets the sample rate the driver reported when the playback device was
 * configured.
 *
 * Parameters:
 *   pb - Playback handle, may be NULL.
 *
 * Returns: Rate in Hz, or 0 if pb is NULL.
 */
unsigned int alsa_playback_get_rate(alsa_playback_t* pb)
{
    return (pb != NULL) ? pb->negotiated_rate : 0;
}

/*
 * alsa_playback_close()
 *
 * Closes the playback device and frees the handle and its buffer.
 *
 * Parameters:
 *   pb - Playback handle, may be NULL.
 *
 * Returns: nothing.
 */
void alsa_playback_close(alsa_playback_t* pb)
{
    if (pb == NULL)
    {
        return;
    }
    if (pb->handle != NULL)
    {
        snd_pcm_close(pb->handle);
    }
    free(pb->raw);
    free(pb);
}

/*
 * clampf()
 *
 * Limits a value to a range.
 *
 * Parameters:
 *   v  - Value to limit.
 *   lo - Lower limit.
 *   hi - Upper limit.
 *
 * Returns: v, or the nearest limit if it is outside the range.
 */
static float clampf(float v, float lo, float hi)
{
    if (v < lo)
    {
        return lo;
    }
    if (v > hi)
    {
        return hi;
    }
    return v;
}

/*
 * alsa_playback_write()
 *
 * Converts complex float samples (clamped to -1..+1) to 32-bit stereo frames
 * and writes them all to the playback device, waiting until the device has
 * accepted everything.  An underrun is recovered and the write continues.
 *
 * Parameters:
 *   pb        - Playback handle.
 *   samples   - Complex samples to play (real part is I, imaginary part is
 *               Q).
 *   n_samples - Number of samples.
 *
 * Returns: true when all samples were written, false on an unrecoverable
 *   error.
 */
bool alsa_playback_write(alsa_playback_t* pb, const alsa_cplx_t* samples, int n_samples)
{
    int i;
    snd_pcm_sframes_t remaining = n_samples;
    int32_t* p;

    if (pb->raw_cap < n_samples)
    {
        pb->raw     = realloc(pb->raw, (size_t)n_samples * CHANNELS * sizeof(int32_t));
        pb->raw_cap = n_samples;
    }

    for (i = 0; i < n_samples; i++)
    {
        float iv = clampf(crealf(samples[i]), -1.0f, 1.0f);
        float qv = clampf(cimagf(samples[i]), -1.0f, 1.0f);
        /* Must be done in double precision, not float: the literal
         * 2147483647.0 (INT32_MAX) cannot be exactly represented as a
         * 32-bit float (24-bit mantissa isn't enough at this
         * magnitude) and rounds up to exactly 2147483648.0f instead --
         * so iv=1.0f (a completely ordinary, expected value for
         * constant-envelope FM, where cos/sin legitimately hit
         * near-full-scale on essentially every sample) produces
         * (int32_t)2147483648.0f, which overflows INT32_MAX and is
         * undefined behavior in C -- in practice this wraps around to
         * INT32_MIN, a catastrophic sign-flip glitch in the actual
         * transmitted waveform rather than the intended near-full-scale
         * value. double has 52 bits of mantissa, comfortably enough to
         * represent 2147483647.0 exactly, so this same multiplication
         * done here never rounds past it in the first place. */
        double scaled_i = (double)iv * 2147483647.0;
        double scaled_q = (double)qv * 2147483647.0;

        pb->raw[2 * i]     = (int32_t)scaled_i;
        pb->raw[2 * i + 1] = (int32_t)scaled_q;
    }

    p = pb->raw;
    while (remaining > 0)
    {
        snd_pcm_sframes_t n = snd_pcm_writei(pb->handle, p, (snd_pcm_uframes_t)remaining);

        if (n < 0)
        {
            int err = snd_pcm_recover(pb->handle, (int)n, 1);

            if (err < 0)
            {
                fprintf(stderr, "alsa_io: playback write error: %s\n", snd_strerror((int)n));
                return false;
            }
            fprintf(stderr, "alsa_io: playback underrun recovered\n");
            continue;
        }
        p += n * CHANNELS;
        remaining -= n;
    }
    return true;
}

/*
 * alsa_playback_drain()
 *
 * Waits until everything already written has been played, then prepares the
 * device for the next transmission.  This blocks.
 *
 * Parameters:
 *   pb - Playback handle.
 *
 * Returns: nothing.
 */
void alsa_playback_drain(alsa_playback_t* pb)
{
    snd_pcm_drain(pb->handle);
    snd_pcm_prepare(pb->handle);
}

/*
 * alsa_playback_prepare()
 *
 * Resets the playback device without waiting, clearing position tracking and
 * any underrun left from the idle time between transmissions.
 *
 * Parameters:
 *   pb - Playback handle.
 *
 * Returns: nothing.
 */
void alsa_playback_prepare(alsa_playback_t* pb)
{
    snd_pcm_prepare(pb->handle);
}

/*
 * alsa_playback_delay_frames()
 *
 * Gets the number of frames written but not yet played, so a caller can pace
 * its writes against the hardware.
 *
 * Parameters:
 *   pb - Playback handle.
 *
 * Returns: Number of queued frames, or -1 on error (for example the device
 *   is not running).
 */
long alsa_playback_delay_frames(alsa_playback_t* pb)
{
    snd_pcm_sframes_t delay = 0;

    if (snd_pcm_delay(pb->handle, &delay) < 0)
    {
        return -1;
    }
    return (long)delay;
}
