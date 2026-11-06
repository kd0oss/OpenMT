/*
 * COMPILE-CHECK STUB -- not the real libhackrf header.
 *
 * Declares the subset of the libhackrf API that hw/hrf_dev.c calls, as
 * documented upstream, so the file can be syntax-checked on a machine
 * without libhackrf installed:
 *
 *   gcc -fsyntax-only -Wall -Wextra -Itest/stubs -Ihw hw/hrf_dev.c
 *
 * It proves nothing about behaviour.  Real builds use the installed
 * libhackrf (pkg-config libhackrf).
 */
#ifndef STUB_HACKRF_H
#define STUB_HACKRF_H
#include <stdint.h>

enum hackrf_error
{
    HACKRF_SUCCESS = 0,
    HACKRF_TRUE = 1,
    HACKRF_ERROR_INVALID_PARAM = -2,
    HACKRF_ERROR_NOT_FOUND = -5,
    HACKRF_ERROR_BUSY = -6,
    HACKRF_ERROR_OTHER = -9999
};

typedef struct hackrf_device hackrf_device;

typedef struct
{
    hackrf_device *device;
    uint8_t *buffer;
    int buffer_length;
    int valid_length;
    void *rx_ctx;
    void *tx_ctx;
} hackrf_transfer;

typedef int (*hackrf_sample_block_cb_fn)(hackrf_transfer *transfer);

int hackrf_init(void);
int hackrf_exit(void);
int hackrf_open(hackrf_device **device);
int hackrf_open_by_serial(const char *desired_serial_number, hackrf_device **device);
int hackrf_close(hackrf_device *device);
int hackrf_start_rx(hackrf_device *device, hackrf_sample_block_cb_fn callback, void *rx_ctx);
int hackrf_stop_rx(hackrf_device *device);
int hackrf_start_tx(hackrf_device *device, hackrf_sample_block_cb_fn callback, void *tx_ctx);
int hackrf_stop_tx(hackrf_device *device);
int hackrf_is_streaming(hackrf_device *device);
int hackrf_set_baseband_filter_bandwidth(hackrf_device *device, const uint32_t bandwidth_hz);
uint32_t hackrf_compute_baseband_filter_bw(const uint32_t bandwidth_hz);
int hackrf_set_freq(hackrf_device *device, const uint64_t freq_hz);
int hackrf_set_sample_rate(hackrf_device *device, const double freq_hz);
int hackrf_set_amp_enable(hackrf_device *device, const uint8_t value);
int hackrf_set_lna_gain(hackrf_device *device, uint32_t value);
int hackrf_set_vga_gain(hackrf_device *device, uint32_t value);
int hackrf_set_txvga_gain(hackrf_device *device, uint32_t value);
int hackrf_set_antenna_enable(hackrf_device *device, const uint8_t value);
const char *hackrf_error_name(enum hackrf_error errcode);
#endif
