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
 * hrf_sim.c - a software HackRF, for testing the bridge without hardware.
 *
 *   hrf_open("rx.cs8,tx.cs8")
 *
 * RX: streams rx.cs8 (raw interleaved signed 8-bit IQ, the format
 *     hackrf_transfer -r writes) as if it came off the air, then silence
 *     once the file is used up.  An empty name gives silence throughout.
 * TX: writes everything that would reach the antenna to tx.cs8.
 *
 * It keeps the two properties of the real device that shape the bridge:
 *   - half duplex: starting TX while RX runs (or the reverse) fails
 *   - TX buffers are handed over HRF_TX_QUEUE_BUFFERS deep, so audio the
 *     bridge supplies goes on air a few buffers later, and stopping TX
 *     throws away whatever is still queued
 * Set HRF_SIM_SPEED=N to run N times faster than real time.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hrf_dev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define SIM_RX_BLOCK 16384U

struct hrf_dev
{
    FILE          *rx_fp;
    FILE          *tx_fp;
    uint32_t       rate;
    double         speed;
    pthread_t      rx_thread, tx_thread;
    atomic_bool    rx_run, tx_run;
    bool           rx_started, tx_started;
    hrf_rx_cb      rx_cb;
    void          *rx_ctx;
    hrf_tx_cb      tx_cb;
    void          *tx_ctx;
    pthread_mutex_t lock;
};

static void
sleep_ns(long long ns)
{
    struct timespec ts;

    if (ns <= 0)
    {
        return;
    }
    ts.tv_sec  = ns / 1000000000LL;
    ts.tv_nsec = ns % 1000000000LL;
    nanosleep(&ts, NULL);
}

static long long
now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

hrf_dev_t *
hrf_open(const char *serial)
{
    struct hrf_dev *d = calloc(1, sizeof(*d));
    char spec[1024];
    char *comma;
    const char *env;

    if (d == NULL)
    {
        return NULL;
    }
    pthread_mutex_init(&d->lock, NULL);
    d->speed = 1.0;
    env = getenv("HRF_SIM_SPEED");
    if (env != NULL && atof(env) > 0.0)
    {
        d->speed = atof(env);
    }
    snprintf(spec, sizeof(spec), "%s", serial != NULL ? serial : ",");
    comma = strchr(spec, ',');
    if (comma != NULL)
    {
        *comma++ = '\0';
    }
    if (spec[0] != '\0')
    {
        d->rx_fp = fopen(spec, "rb");
        if (d->rx_fp == NULL)
        {
            fprintf(stderr, "hrf_sim: cannot open RX file %s\n", spec);
            free(d);
            return NULL;
        }
    }
    if (comma != NULL && comma[0] != '\0')
    {
        d->tx_fp = fopen(comma, "wb");
        if (d->tx_fp == NULL)
        {
            fprintf(stderr, "hrf_sim: cannot open TX file %s\n", comma);
            free(d);
            return NULL;
        }
    }
    fprintf(stderr, "hrf_sim: simulated HackRF (speed x%.1f)\n", d->speed);
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
    if (d->rx_fp != NULL)
    {
        fclose(d->rx_fp);
    }
    if (d->tx_fp != NULL)
    {
        fclose(d->tx_fp);
    }
    free(d);
}

bool
hrf_set_sample_rate(hrf_dev_t *d, uint32_t hz)
{
    d->rate = hz;
    fprintf(stderr, "hrf_sim: sample rate %u\n", hz);
    return true;
}

bool
hrf_set_baseband_bw(hrf_dev_t *d, uint32_t hz)
{
    (void)d;
    fprintf(stderr, "hrf_sim: baseband bw %u\n", hz);
    return true;
}

bool
hrf_set_freq(hrf_dev_t *d, uint64_t hz)
{
    (void)d;
    fprintf(stderr, "hrf_sim: freq %llu\n", (unsigned long long)hz);
    return true;
}

bool
hrf_set_rx_gain(hrf_dev_t *d, unsigned lna_db, unsigned vga_db)
{
    (void)d;
    fprintf(stderr, "hrf_sim: rx gain lna=%u vga=%u\n", lna_db, vga_db);
    return true;
}

bool
hrf_set_tx_gain(hrf_dev_t *d, unsigned txvga_db)
{
    (void)d;
    fprintf(stderr, "hrf_sim: tx gain txvga=%u\n", txvga_db);
    return true;
}

bool
hrf_set_amp(hrf_dev_t *d, bool on)
{
    (void)d;
    fprintf(stderr, "hrf_sim: amp %s\n", on ? "on" : "off");
    return true;
}

bool
hrf_set_bias_tee(hrf_dev_t *d, bool on)
{
    (void)d;
    fprintf(stderr, "hrf_sim: bias tee %s\n", on ? "on" : "off");
    return true;
}

static void *
sim_rx_main(void *arg)
{
    struct hrf_dev *d = arg;
    int8_t buf[SIM_RX_BLOCK];
    long long next = now_ns();
    /* bytes/s = rate * 2; one block lasts SIM_RX_BLOCK/(2*rate) seconds */
    long long block_ns = (long long)((double)SIM_RX_BLOCK / (2.0 * (double)d->rate) * 1e9 / d->speed);

    while (atomic_load(&d->rx_run))
    {
        size_t got = 0;

        if (d->rx_fp != NULL)
        {
            got = fread(buf, 1, SIM_RX_BLOCK, d->rx_fp);
        }
        if (got < SIM_RX_BLOCK)
        {
            memset(buf + got, 0, SIM_RX_BLOCK - got);
        }
        if (d->rx_cb(buf, SIM_RX_BLOCK, d->rx_ctx) != 0)
        {
            break;
        }
        next += block_ns;
        sleep_ns(next - now_ns());
    }
    return NULL;
}

static void *
sim_tx_main(void *arg)
{
    struct hrf_dev *d = arg;
    int8_t *q[HRF_TX_QUEUE_BUFFERS];
    unsigned head = 0, i;
    long long next;
    long long block_ns = (long long)((double)HRF_TX_BUFFER_BYTES / (2.0 * (double)d->rate) * 1e9 / d->speed);

    for (i = 0; i < HRF_TX_QUEUE_BUFFERS; i++)
    {
        q[i] = malloc(HRF_TX_BUFFER_BYTES);
        /* like libhackrf, fill every transfer before the first goes out */
        d->tx_cb(q[i], HRF_TX_BUFFER_BYTES, d->tx_ctx);
    }
    next = now_ns();
    while (atomic_load(&d->tx_run))
    {
        next += block_ns;
        sleep_ns(next - now_ns());
        if (!atomic_load(&d->tx_run))
        {
            break;
        }
        /* the oldest buffer has just gone out */
        if (d->tx_fp != NULL)
        {
            fwrite(q[head], 1, HRF_TX_BUFFER_BYTES, d->tx_fp);
            fflush(d->tx_fp);
        }
        if (d->tx_cb(q[head], HRF_TX_BUFFER_BYTES, d->tx_ctx) != 0)
        {
            break;
        }
        head = (head + 1U) % HRF_TX_QUEUE_BUFFERS;
    }
    for (i = 0; i < HRF_TX_QUEUE_BUFFERS; i++)
    {
        free(q[i]);
    }
    return NULL;
}

bool
hrf_start_rx(hrf_dev_t *d, hrf_rx_cb cb, void *ctx)
{
    if (d->tx_started)
    {
        fprintf(stderr, "hrf_sim: start_rx while transmitting: HACKRF_ERROR_BUSY\n");
        return false;
    }
    if (d->rx_started)
    {
        return true;
    }
    d->rx_cb  = cb;
    d->rx_ctx = ctx;
    atomic_store(&d->rx_run, true);
    if (pthread_create(&d->rx_thread, NULL, sim_rx_main, d) != 0)
    {
        return false;
    }
    d->rx_started = true;
    fprintf(stderr, "hrf_sim: RX started\n");
    return true;
}

bool
hrf_stop_rx(hrf_dev_t *d)
{
    if (!d->rx_started)
    {
        return true;
    }
    atomic_store(&d->rx_run, false);
    pthread_join(d->rx_thread, NULL);
    d->rx_started = false;
    fprintf(stderr, "hrf_sim: RX stopped\n");
    return true;
}

bool
hrf_start_tx(hrf_dev_t *d, hrf_tx_cb cb, void *ctx)
{
    if (d->rx_started)
    {
        fprintf(stderr, "hrf_sim: start_tx while receiving: HACKRF_ERROR_BUSY\n");
        return false;
    }
    if (d->tx_started)
    {
        return true;
    }
    d->tx_cb  = cb;
    d->tx_ctx = ctx;
    atomic_store(&d->tx_run, true);
    if (pthread_create(&d->tx_thread, NULL, sim_tx_main, d) != 0)
    {
        return false;
    }
    d->tx_started = true;
    fprintf(stderr, "hrf_sim: TX started\n");
    return true;
}

bool
hrf_stop_tx(hrf_dev_t *d)
{
    if (!d->tx_started)
    {
        return true;
    }
    atomic_store(&d->tx_run, false);
    pthread_join(d->tx_thread, NULL);
    d->tx_started = false;
    fprintf(stderr, "hrf_sim: TX stopped\n");
    return true;
}
