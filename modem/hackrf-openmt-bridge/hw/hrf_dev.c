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
 * hrf_dev.c - hrf_dev.h on top of libhackrf.
 *
 * Threading: libhackrf runs the RX/TX callbacks on its own USB thread.
 * Control calls (frequency, gains, start/stop) may come from any bridge
 * thread; they are serialised with one mutex because several of them are
 * multi-step USB conversations inside libhackrf.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hrf_dev.h"

#include <libhackrf/hackrf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

struct hrf_dev
{
    hackrf_device  *dev;
    pthread_mutex_t lock;
    hrf_rx_cb       rx_cb;
    void           *rx_ctx;
    hrf_tx_cb       tx_cb;
    void           *tx_ctx;
    uint32_t        sample_rate;
};

static void
hrf_err(const char *what, int rc)
{
    fprintf(stderr, "hackrf: %s failed: %s (%d)\n", what, hackrf_error_name((enum hackrf_error)rc), rc);
}

#define CHECK(what, call)                \
    do                                   \
    {                                    \
        int rc_ = (call);                \
        if (rc_ != HACKRF_SUCCESS)       \
        {                                \
            hrf_err(what, rc_);          \
            pthread_mutex_unlock(&d->lock); \
            return false;                \
        }                                \
    } while (0)

static int
rx_trampoline(hackrf_transfer *t)
{
    struct hrf_dev *d = t->rx_ctx;

    return d->rx_cb((const int8_t *)t->buffer, (size_t)t->valid_length, d->rx_ctx);
}

static int
tx_trampoline(hackrf_transfer *t)
{
    struct hrf_dev *d = t->tx_ctx;

    t->valid_length = t->buffer_length;
    return d->tx_cb((int8_t *)t->buffer, (size_t)t->buffer_length, d->tx_ctx);
}

hrf_dev_t *
hrf_open(const char *serial)
{
    struct hrf_dev *d = calloc(1, sizeof(*d));
    int rc;

    if (d == NULL)
    {
        return NULL;
    }
    pthread_mutex_init(&d->lock, NULL);
    rc = hackrf_init();
    if (rc != HACKRF_SUCCESS)
    {
        hrf_err("hackrf_init", rc);
        free(d);
        return NULL;
    }
    if (serial != NULL && serial[0] != '\0')
    {
        rc = hackrf_open_by_serial(serial, &d->dev);
    }
    else
    {
        rc = hackrf_open(&d->dev);
    }
    if (rc != HACKRF_SUCCESS)
    {
        hrf_err("open (is a HackRF plugged in, and do you have USB permission? see README)", rc);
        hackrf_exit();
        free(d);
        return NULL;
    }
    return d;
}

void
hrf_close(hrf_dev_t *d)
{
    if (d == NULL)
    {
        return;
    }
    hrf_stop_rx(d);
    hrf_stop_tx(d);
    pthread_mutex_lock(&d->lock);
    hackrf_close(d->dev);
    pthread_mutex_unlock(&d->lock);
    hackrf_exit();
    pthread_mutex_destroy(&d->lock);
    free(d);
}

bool
hrf_set_sample_rate(hrf_dev_t *d, uint32_t hz)
{
    pthread_mutex_lock(&d->lock);
    CHECK("set_sample_rate", hackrf_set_sample_rate(d->dev, (double)hz));
    d->sample_rate = hz;
    pthread_mutex_unlock(&d->lock);
    return true;
}

bool
hrf_set_baseband_bw(hrf_dev_t *d, uint32_t hz)
{
    pthread_mutex_lock(&d->lock);
    /* the MAX2837 filter only has fixed steps (1.75 MHz up); pick the
     * nearest one at or above the request */
    CHECK("set_baseband_filter_bandwidth",
        hackrf_set_baseband_filter_bandwidth(d->dev, hackrf_compute_baseband_filter_bw(hz)));
    pthread_mutex_unlock(&d->lock);
    return true;
}

bool
hrf_set_freq(hrf_dev_t *d, uint64_t hz)
{
    pthread_mutex_lock(&d->lock);
    CHECK("set_freq", hackrf_set_freq(d->dev, hz));
    pthread_mutex_unlock(&d->lock);
    return true;
}

bool
hrf_set_rx_gain(hrf_dev_t *d, unsigned lna_db, unsigned vga_db)
{
    pthread_mutex_lock(&d->lock);
    CHECK("set_lna_gain", hackrf_set_lna_gain(d->dev, lna_db));
    CHECK("set_vga_gain", hackrf_set_vga_gain(d->dev, vga_db));
    pthread_mutex_unlock(&d->lock);
    return true;
}

bool
hrf_set_tx_gain(hrf_dev_t *d, unsigned txvga_db)
{
    pthread_mutex_lock(&d->lock);
    CHECK("set_txvga_gain", hackrf_set_txvga_gain(d->dev, txvga_db));
    pthread_mutex_unlock(&d->lock);
    return true;
}

bool
hrf_set_amp(hrf_dev_t *d, bool on)
{
    pthread_mutex_lock(&d->lock);
    CHECK("set_amp_enable", hackrf_set_amp_enable(d->dev, on ? 1 : 0));
    pthread_mutex_unlock(&d->lock);
    return true;
}

bool
hrf_set_bias_tee(hrf_dev_t *d, bool on)
{
    pthread_mutex_lock(&d->lock);
    CHECK("set_antenna_enable", hackrf_set_antenna_enable(d->dev, on ? 1 : 0));
    pthread_mutex_unlock(&d->lock);
    return true;
}

bool
hrf_start_rx(hrf_dev_t *d, hrf_rx_cb cb, void *ctx)
{
    pthread_mutex_lock(&d->lock);
    d->rx_cb  = cb;
    d->rx_ctx = ctx;
    CHECK("start_rx", hackrf_start_rx(d->dev, rx_trampoline, d));
    pthread_mutex_unlock(&d->lock);
    return true;
}

bool
hrf_stop_rx(hrf_dev_t *d)
{
    pthread_mutex_lock(&d->lock);
    if (hackrf_is_streaming(d->dev) == HACKRF_TRUE && d->rx_cb != NULL)
    {
        CHECK("stop_rx", hackrf_stop_rx(d->dev));
    }
    d->rx_cb = NULL;
    pthread_mutex_unlock(&d->lock);
    return true;
}

bool
hrf_start_tx(hrf_dev_t *d, hrf_tx_cb cb, void *ctx)
{
    pthread_mutex_lock(&d->lock);
    d->tx_cb  = cb;
    d->tx_ctx = ctx;
    CHECK("start_tx", hackrf_start_tx(d->dev, tx_trampoline, d));
    pthread_mutex_unlock(&d->lock);
    return true;
}

bool
hrf_stop_tx(hrf_dev_t *d)
{
    pthread_mutex_lock(&d->lock);
    if (hackrf_is_streaming(d->dev) == HACKRF_TRUE && d->tx_cb != NULL)
    {
        CHECK("stop_tx", hackrf_stop_tx(d->dev));
    }
    d->tx_cb = NULL;
    pthread_mutex_unlock(&d->lock);
    return true;
}
