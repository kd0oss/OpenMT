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
 * bridge.c - HackRF <-> OpenMT bridge.
 *
 * Presents a HackRF to OpenMT's Modem_Host.c as a virtual "OPENMTHS" /
 * "OPENMTSX" modem over a PTY, exactly the way sx1255-openmt-bridge does
 * for the SX1255 HAT: this program does the radio and the generic FM
 * front-end DSP (FM demodulation, baseband AFC, resampling to 24 kSa/s)
 * and exchanges Q15 baseband samples with Modem_Host.c using the OpenMT
 * serial framing.  Every protocol service (D-Star, DMR, YSF, P25, NXDN,
 * M17) does its own symbol filtering, sync and FEC from there, unchanged.
 *
 * What differs from the SX1255 bridge is the radio:
 *
 *   - The HackRF streams 8-bit IQ over USB at a few Msps instead of
 *     125 kSa/s over I2S.  dsp/hrf_frontend.c brings that to the 125 kSa/s
 *     the (shared, already tuned) dsp.c chain expects, and back.
 *   - The HackRF is HALF DUPLEX.  Keying up stops RX; unkeying restarts
 *     it.  While transmitting, nothing is received.
 *   - The TX path is paced by the USB stack: the TX callback is pulled
 *     every ~65 ms for a 262144-byte buffer and four buffers are in
 *     flight, so audio reaches the antenna ~0.2-0.3 s after it is handed
 *     over, and the end of a transmission has to be flushed through that
 *     queue before the radio is switched back to RX.
 *   - There is no common sample clock shared by RX and TX, so the
 *     OPENMT_TX_TIMING report the SX1255 bridge sends for DMR slot
 *     alignment is not available.
 *
 * Wire protocol, framing and the key-on-samples / key-off-on-timeout
 * logic are unchanged from sx1255-openmt-bridge; see its README for how
 * they were traced against Modem_Host.c.
 */
#define _DEFAULT_SOURCE
#include "hrf_dev.h"
#include "hrf_db.h"
#include "bridge_common.h"
#include "hrf_frontend.h"
#include "rf_spectrum.h"
#include "dsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <math.h>
#include <complex.h>
#include <stdatomic.h>

#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>
#else
#include <pty.h>
#endif

/* ---- Timestamped logging ---------------------------------------------- */

/* Prepends a Unix timestamp (seconds.milliseconds) to every log line,
 * replacing scattered log_msg( ...) calls throughout this file.
 * Added specifically to let timing be read directly off the log --
 * e.g. comparing how long TX stayed keyed, or how long the gap was
 * between a key-down and the next key-up, across a working case
 * against a failing one -- without needing to guess at timing from
 * message order alone. */
static void
log_msg(const char *fmt, ...)
{
    struct timespec ts;
    va_list args;

    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(stderr, "[%lld.%03ld] ", (long long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

/* ---- OpenMT wire protocol constants --------------------------------- */
#define OPENMT_START        0x61U
#define OPENMT_SAMP_EOT     0x00U
#define OPENMT_GET_STATUS   0x10U
#define OPENMT_SET_CONFIG   0x20U
#define OPENMT_SET_MODE     0x30U
#define OPENMT_ADD_MODE     0x40U
#define OPENMT_DEL_MODE     0x50U
#define OPENMT_BIT_DATA     0x60U
#define OPENMT_SAMP_DATA    0x70U
#define OPENMT_TO_MODEM     0x80U
#define OPENMT_ACK          0xE0U
#define OPENMT_NAK          0xF0U
#define OPENMT_MODE_IDLE    0x0FU /* SET_MODE | this value means "go idle" */

/* Max samples per SAMP_DATA frame: the single-byte length field caps a
 * whole frame at 255 bytes; header is 3 bytes (start, len, type), each
 * sample is 2 bytes (q15_t). 100 samples/frame stays comfortably clear
 * of that limit and divides the 24 kSa/s rate into ~4.17ms/frame. */
#define SAMPLES_PER_FRAME 100
#define RX_LEVEL_UNITY 50U      /* rxLevel for gain x1, the same as the SAMPLE modem firmware */
#define MAX_FRAME_BYTES 255

#define BB_RATE HRF_FE_BASEBAND_RATE            /* 125000: what dsp.c runs at */

/* How much transmit audio is kept queued ahead of the USB callback, in
 * milliseconds.  The callback takes 65 ms buffers (at 2 Msps); keeping at
 * least one buffer's worth queued means it is never caught empty. */
#define TX_LOOKAHEAD_MS 100U

/* Byte capacity of the RX and TX IQ rings: 0.5 s at 2 Msps. */
#define IQ_RING_BYTES (2U * 1024U * 1024U)

/* ---- Configuration ---------------------------------------------------*/
typedef struct
{
    const char *serial;          /* HackRF serial (or "rx.cs8,tx.cs8" for the simulator) */
    uint32_t sample_rate;        /* HackRF rate: multiple of 500 kHz */
    uint32_t rx_freq_hz;
    uint32_t tx_freq_hz;
    int32_t rx_offset_hz;        /* RX LO is tuned this far BELOW the wanted frequency */
    int32_t tx_offset_hz;        /* likewise for TX */
    double ppm;                  /* the HackRF tunes to f*(1+ppm/1e6) when asked for f */
    unsigned int lna_gain_db;    /* RX IF gain, 0-40 in 8 dB steps */
    unsigned int vga_gain_db;    /* RX baseband gain, 0-62 in 2 dB steps */
    unsigned int tx_vga_db;      /* TX VGA gain, 0-47 */
    bool rx_amp;                 /* 14 dB RF amp while receiving */
    bool tx_amp;                 /* 14 dB RF amp while transmitting */
    bool bias_tee;               /* antenna port power */
    bool rx_stats;               /* log the level of the HackRF samples every 2 s (--rx-stats) */
    unsigned int spectrum_port;  /* UDP port that serves the RF spectrum to monitors (OpenMT Radio's Monitor tab), 0 = off */
    const char *spectrum_bind;   /* address it listens on, NULL = every interface */
    float tx_amplitude;          /* fraction of int8 full scale for the (constant envelope) TX signal */
    double deviation_hz;
    atomic_bool rx_invert;       /* negate the baseband sign before Q15 conversion */
    float rx_gain;               /* scale before Q15 conversion at rx_level 50 (RX_LEVEL_UNITY) */
    volatile uint32_t rx_level;  /* rxLevel of the Modem tab; the gain is rx_gain * rx_level / RX_LEVEL_UNITY */
    const char *pty_symlink;
    float tx_symbol_scale;       /* divisor for incoming Q15 TX samples */
    atomic_bool tx_invert;       /* negate incoming baseband before the FM modulator */
    unsigned int tx_prime_ms;    /* audio queued before the first sample is played, and again after a gap */
    unsigned int tx_key_timeout_ms; /* de-key after this long with no TX samples */
    unsigned int tx_key_settle_ms;  /* wait after starting the transmitter before audio is allowed through */
    bool key_on_set_mode;        /* SET_MODE|conn keys the transmitter (off: see handle_frame) */
    const char *modem_name;      /* name of the modem in the config database */
} bridge_config_t;

static volatile sig_atomic_t g_stop = 0;

static void
handle_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void
config_set_defaults(bridge_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->serial = NULL;
    cfg->sample_rate = 2000000U;
    cfg->rx_freq_hz = 434000000U;
    cfg->tx_freq_hz = 434000000U;
    cfg->modem_name = "modem1";
    /* RX: the HackRF's DC spike and LO leakage sit at the centre of its
     * spectrum, so tune a quarter of a megahertz away and shift digitally.
     * 250 kHz at 2 Msps is exactly fs/8 (an 8-entry shift table).  TX: a
     * constant-envelope FM signal is not hurt by LO leakage landing on its
     * own carrier, and shifting it would put the mirror image (the HackRF's
     * I/Q imbalance, typically -30 to -40 dBc) 500 kHz away; so transmit
     * on the LO by default. */
    cfg->rx_offset_hz = 250000;
    cfg->tx_offset_hz = 0;
    cfg->ppm = 0.0;
    cfg->lna_gain_db = 24U;
    cfg->vga_gain_db = 20U;
    cfg->tx_vga_db = 20U;
    cfg->rx_amp = false;
    cfg->tx_amp = false;
    cfg->bias_tee = false;
    cfg->spectrum_port = RFSPEC_DEFAULT_PORT;
    cfg->spectrum_bind = NULL;
    cfg->tx_amplitude = 0.9f;
    /* The FM deviation, TX symbol scale, RX gain and Q15 conventions are
     * properties of the 125 kSa/s DSP chain and of what the protocol
     * services expect, not of the radio, so they carry over unchanged from
     * sx1255-openmt-bridge's tuned values. */
    cfg->deviation_hz = 950.0;
    cfg->tx_symbol_scale = 494.0f;
    cfg->rx_gain = 0.41f;
    cfg->rx_level = RX_LEVEL_UNITY;
    /* Polarity: the SX1255 bridge defaults to inverting both (its I/Q
     * wiring on the HAT is reversed).  The HackRF uses the ordinary
     * convention (I + jQ, higher RF = positive frequency) on both RX and
     * TX, so no inversion is expected.  NOT verified on air: if a decoder
     * finds sync but every frame fails FEC, or a known-good radio decodes
     * your transmission with swapped symbols, flip --rx-invert / --tx-invert. */
    atomic_store(&cfg->rx_invert, false);
    atomic_store(&cfg->tx_invert, false);
    cfg->pty_symlink = "/dev/hackrf-modem";
    cfg->tx_prime_ms = 250U;
    cfg->tx_key_timeout_ms = 1000U;
    cfg->tx_key_settle_ms = 20U;
    cfg->key_on_set_mode = false;
}

static void
print_usage(const char *argv0)
{
    log_msg(
        "Usage: %s [options]\n"
        "  --serial S            HackRF serial number (default: first one found)\n"
        "  --sample-rate HZ      HackRF sample rate, a multiple of 500000 from\n"
        "                        1000000 to 8000000 (default 2000000)\n"
        "  --modem-name NAME     modem name in the config database (default modem1);\n"
        "                        the frequencies, gains and invert flags are read from\n"
        "                        there and the options below only give the defaults of\n"
        "                        settings that are not saved yet\n"
        "  --rx-freq HZ          RX frequency (default 434000000)\n"
        "  --tx-freq HZ          TX frequency (default 434000000)\n"
        "  --rx-offset HZ        tune the RX LO this far below the wanted\n"
        "                        frequency and shift digitally, to keep the\n"
        "                        HackRF's centre spike out of the channel\n"
        "                        (default 250000; 0 = tune straight to it)\n"
        "  --tx-offset HZ        same for TX (default 0)\n"
        "  --ppm P               the HackRF's clock error: it actually tunes to\n"
        "                        f*(1+P/1e6) when asked for f.  P = (f_true -\n"
        "                        f_shown)/f_tuned * 1e6, from a signal of known\n"
        "                        frequency.  Matters: 10 ppm at 435 MHz is 4 kHz.\n"
        "  --lna-gain DB         RX IF gain 0-40 in 8 dB steps (default 24)\n"
        "  --vga-gain DB         RX baseband gain 0-62 in 2 dB steps (default 20)\n"
        "  --tx-vga-gain DB      TX VGA gain 0-47 (default 20)\n"
        "  --rx-amp / --tx-amp   switch the 14 dB RF amplifier on while\n"
        "                        receiving / transmitting (default off)\n"
        "  --bias-tee            power the antenna port\n"
        "  --tx-amplitude X      TX signal level, 0-1 of int8 full scale\n"
        "                        (default 0.9).  Set output power with the\n"
        "                        gains, not this.\n"
        "  --pty-symlink PATH    stable symlink to the PTY slave device\n"
        "                        (default /dev/hackrf-modem)\n"
        "  --rx-invert / --no-rx-invert   negate RX baseband (default off)\n"
        "  --tx-invert / --no-tx-invert   negate TX baseband (default off)\n"
        "  --rx-stats            log the level of the HackRF's 8-bit samples every 2 s,\n"
        "                        to set the LNA/VGA gains\n"
        "  --spectrum-port N     UDP port that serves the RF spectrum of the received channel\n"
        "                        to monitors such as OpenMT Radio's Monitor tab (default 18050,\n"
        "                        0 = off).  Frames are only computed and sent while a monitor\n"
        "                        asks for them.  See tools/rf_spectrum.h for the protocol\n"
        "  --spectrum-bind ADDR  IPv4 address it listens on (default: every interface)\n"
        "  --rx-gain MULT        scale on baseband before Q15 at rxLevel 50 (default 0.41);\n"
        "                        the Modem tab's rxLevel multiplies it by rxLevel/50\n"
        "  --tx-symbol-scale N   divisor for incoming Q15 TX samples (default 494)\n"
        "  --deviation-hz N      FM deviation per baseband unit (default 950)\n"
        "  --tx-prime-ms N       audio to queue before playing (default 250)\n"
        "  --tx-key-timeout-ms N de-key after this long without TX samples\n"
        "                        (default 1000)\n"
        "  --tx-key-settle-ms N  wait after starting the transmitter before\n"
        "                        audio is allowed through (default 20)\n"
        "  --key-on-set-mode     also key up on SET_MODE.  Off by default: hosts\n"
        "                        send SET_MODE when they RECEIVE a call too, and on\n"
        "                        a half-duplex radio keying up would cut it off.\n"
        "  -h, --help            show this help\n"
        "\n"
        "With OpenMT configured as modem=OPENMTSX a SET_CONFIG packet at startup\n"
        "overrides the frequencies, gains and invert flags above; see README.\n",
        argv0);
}

#define NEXT_ARG() (i + 1 < argc ? argv[++i] : NULL)

static bool
parse_args(int argc, char **argv, bridge_config_t *cfg)
{
    int i;
    const char *val;

    config_set_defaults(cfg);

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            print_usage(argv[0]);
            exit(0);
        }
        else if (strcmp(argv[i], "--serial") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->serial = val;
        }
        else if (strcmp(argv[i], "--sample-rate") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->sample_rate = (uint32_t)strtoul(val, NULL, 10);
        }
        else if (strcmp(argv[i], "--modem-name") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->modem_name = val;
        }
        else if (strcmp(argv[i], "--rx-freq") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->rx_freq_hz = (uint32_t)strtoul(val, NULL, 10);
        }
        else if (strcmp(argv[i], "--tx-freq") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->tx_freq_hz = (uint32_t)strtoul(val, NULL, 10);
        }
        else if (strcmp(argv[i], "--rx-offset") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->rx_offset_hz = (int32_t)strtol(val, NULL, 10);
        }
        else if (strcmp(argv[i], "--tx-offset") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->tx_offset_hz = (int32_t)strtol(val, NULL, 10);
        }
        else if (strcmp(argv[i], "--ppm") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->ppm = atof(val);
        }
        else if (strcmp(argv[i], "--lna-gain") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->lna_gain_db = (unsigned int)atoi(val);
        }
        else if (strcmp(argv[i], "--vga-gain") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->vga_gain_db = (unsigned int)atoi(val);
        }
        else if (strcmp(argv[i], "--tx-vga-gain") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->tx_vga_db = (unsigned int)atoi(val);
        }
        else if (strcmp(argv[i], "--rx-amp") == 0)
        {
            cfg->rx_amp = true;
        }
        else if (strcmp(argv[i], "--tx-amp") == 0)
        {
            cfg->tx_amp = true;
        }
        else if (strcmp(argv[i], "--spectrum-port") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->spectrum_port = (unsigned int)strtoul(val, NULL, 10);
        }
        else if (strcmp(argv[i], "--spectrum-bind") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->spectrum_bind = val;
        }
        else if (strcmp(argv[i], "--rx-stats") == 0)
        {
            cfg->rx_stats = true;
        }
        else if (strcmp(argv[i], "--bias-tee") == 0)
        {
            cfg->bias_tee = true;
        }
        else if (strcmp(argv[i], "--tx-amplitude") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->tx_amplitude = (float)atof(val);
        }
        else if (strcmp(argv[i], "--pty-symlink") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->pty_symlink = val;
        }
        else if (strcmp(argv[i], "--rx-invert") == 0)
        {
            atomic_store(&cfg->rx_invert, true);
        }
        else if (strcmp(argv[i], "--no-rx-invert") == 0)
        {
            atomic_store(&cfg->rx_invert, false);
        }
        else if (strcmp(argv[i], "--tx-invert") == 0)
        {
            atomic_store(&cfg->tx_invert, true);
        }
        else if (strcmp(argv[i], "--no-tx-invert") == 0)
        {
            atomic_store(&cfg->tx_invert, false);
        }
        else if (strcmp(argv[i], "--rx-gain") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->rx_gain = (float)atof(val);
        }
        else if (strcmp(argv[i], "--tx-symbol-scale") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->tx_symbol_scale = (float)atof(val);
        }
        else if (strcmp(argv[i], "--deviation-hz") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->deviation_hz = atof(val);
        }
        else if (strcmp(argv[i], "--tx-prime-ms") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->tx_prime_ms = (unsigned int)atoi(val);
        }
        else if (strcmp(argv[i], "--tx-key-timeout-ms") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->tx_key_timeout_ms = (unsigned int)atoi(val);
        }
        else if (strcmp(argv[i], "--tx-key-settle-ms") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->tx_key_settle_ms = (unsigned int)atoi(val);
        }
        else if (strcmp(argv[i], "--key-on-set-mode") == 0)
        {
            cfg->key_on_set_mode = true;
        }
        else if (strcmp(argv[i], "--no-key-on-set-mode") == 0)
        {
            cfg->key_on_set_mode = false;
        }
        else
        {
            log_msg("Unknown or incomplete option: %s\n", argv[i]);
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

/* Validate and round the settings; returns false (after saying why) if
 * they can't work. */
static bool
check_config(bridge_config_t *cfg)
{
    if (!hrf_fe_rate_ok(cfg->sample_rate))
    {
        log_msg("bridge: --sample-rate %u is not usable: it must be a multiple of 500000 "
            "between 1000000 and 8000000\n", cfg->sample_rate);
        return false;
    }
    if (cfg->tx_amplitude <= 0.0f || cfg->tx_amplitude > 1.0f)
    {
        log_msg("bridge: --tx-amplitude must be above 0 and at most 1\n");
        return false;
    }
    if (abs(cfg->rx_offset_hz) >= (int32_t)(cfg->sample_rate / 4U) ||
        abs(cfg->tx_offset_hz) >= (int32_t)(cfg->sample_rate / 4U))
    {
        log_msg("bridge: the RX/TX offsets must be within a quarter of the sample rate\n");
        return false;
    }
    cfg->lna_gain_db = (cfg->lna_gain_db > 40U ? 40U : cfg->lna_gain_db) / 8U * 8U;
    cfg->vga_gain_db = (cfg->vga_gain_db > 62U ? 62U : cfg->vga_gain_db) / 2U * 2U;
    if (cfg->tx_vga_db > 47U)
    {
        cfg->tx_vga_db = 47U;
    }
    return true;
}

/* ---- PTY setup --------------------------------------------------------*/

/* Returns the master fd (used for all subsequent read()/write() calls in
 * this program), and fills slave_path with the slave device path (e.g.
 * "/dev/pts/4") -- point OpenMT's modemtty config at that path, or at
 * --pty-symlink's path if one was requested. */
static int
open_bridge_pty(char *slave_path, size_t slave_path_cap)
{
    int master_fd, slave_fd;
    char path_buf[256];
    struct termios tty;

    if (openpty(&master_fd, &slave_fd, path_buf, NULL, NULL) != 0)
    {
        log_msg( "bridge: openpty failed: %s\n", strerror(errno));
        return -1;
    }
    /* Keep the slave fd open for the life of the process -- some libc/
     * kernel combinations reset PTY state or signal EOF to the master
     * side once every slave-side fd has been closed, since the pty
     * layer thinks nothing is connected to that end. We never read/write
     * this fd ourselves; Modem_Host.c opens the same slave device path
     * independently (its own fd, its own open() call). */

    /* Raw mode: this link carries a binary framed protocol, not a
     * human terminal session -- line discipline features (canonical
     * mode, echo, signal-generating characters) must not interfere. */
    if (tcgetattr(slave_fd, &tty) == 0)
    {
        cfmakeraw(&tty);
        tcsetattr(slave_fd, TCSANOW, &tty);
    }
    if (tcgetattr(master_fd, &tty) == 0)
    {
        cfmakeraw(&tty);
        tcsetattr(master_fd, TCSANOW, &tty);
    }

    snprintf(slave_path, slave_path_cap, "%s", path_buf);
    return master_fd;
}

/* ---- OpenMT frame I/O --------------------------------------------------
 * Framing: [0x61][len][type_hi|conn_lo][payload...], len counts every
 * byte in the frame including the 0x61 itself (matches processSerial()'s
 * "respLen = buffer[1]" then reading up to that many total bytes). */

static bool
write_frame(int fd, uint8_t type_and_conn, const uint8_t *payload, uint8_t payload_len)
{
    uint8_t buf[MAX_FRAME_BYTES];
    uint8_t total_len = (uint8_t)(3 + payload_len);
    ssize_t written;
    struct pollfd pfd;

    if (3 + (int)payload_len > MAX_FRAME_BYTES)
    {
        log_msg( "bridge: write_frame: payload too large (%d bytes)\n", payload_len);
        return false;
    }

    /* Real-time streaming data (SAMP_DATA frames, mostly): if nothing is
     * currently reading the other end of the PTY (Modem_Host.c not
     * running/connected yet, or momentarily behind), a PTY's kernel
     * buffer is limited and fills up within a few seconds of the RX
     * thread's continuous ~4ms-interval writes -- and a plain, unbounded
     * write() then blocks indefinitely. That's fatal for shutdown
     * specifically: this function is called from the RX thread, and
     * main()'s shutdown sequence does pthread_join() on that thread,
     * which would then never return either. Bound this with poll()
     * (same pattern as read_frame()'s own timeout) and drop the frame
     * rather than block if there's nowhere for it to go right now --
     * stale real-time samples that couldn't be delivered promptly
     * aren't useful to deliver late anyway. */
    pfd.fd = fd;
    pfd.events = POLLOUT;
    if (poll(&pfd, 1, 100) <= 0)
    {
        static time_t last_warn = 0;
        time_t now = time(NULL);

        if (now != last_warn)
        {
            log_msg( "bridge: write_frame: PTY not accepting data (nothing reading the "
                "other end?) -- dropping frame(s). This message is rate-limited.\n");
            last_warn = now;
        }
        return false;
    }

    buf[0] = OPENMT_START;
    buf[1] = total_len;
    buf[2] = type_and_conn;
    if (payload_len > 0)
    {
        memcpy(buf + 3, payload, payload_len);
    }
    written = write(fd, buf, total_len);
    return written == total_len;
}

/* A control frame read by read_frame(): carried out by the main loop. */
static bool g_ctl_pending = false;
static uint8_t g_ctl_type = 0;

/* Blocking read of exactly one frame, but never blocks indefinitely
 * waiting for a *new* frame to start (a short poll() with a timeout
 * gates the search for the initial start byte) -- once a start byte has
 * actually been seen, the rest of the (small, <=255 byte) frame is read
 * with ordinary blocking reads, since it should arrive essentially
 * immediately and there is no correctness benefit to timing out
 * mid-frame (and real risk of desyncing the stream if we did: a
 * mid-frame read timeout would abandon a partially-read frame while its
 * remaining bytes are still sitting in the fd's buffer, corrupting the
 * next read_frame() call's framing). Returns true on success, filling
 * type_and_conn and payload (payload_len in/out: caller's buffer size in,
 * actual payload length out). Returns false if no frame is currently
 * available (timeout) or on read error/EOF -- caller should treat both
 * the same way (nothing to do right now, try again shortly), which is
 * also what makes this safe for the caller's main loop to re-check its
 * own exit condition frequently regardless of host-side traffic. */
static bool
read_frame(int fd, uint8_t *type_and_conn, uint8_t *payload, uint8_t *payload_len)
{
    uint8_t hdr[3];
    ssize_t n;
    size_t got = 0;
    uint8_t total_len, actual_payload_len;
    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 500) <= 0)
    {
        return false; /* timeout, or poll itself interrupted/errored --
                        * either way, no frame available right now. */
    }

    while (got < 1)
    {
        n = read(fd, hdr, 1);
        if (n <= 0)
        {
            return false;
        }
        if (hdr[0] == BRIDGE_CTL_START)
        {
            /* a control frame of Modem_Host: 0xFE <total length> <type> [payload] */
            uint8_t clen;
            uint8_t cbuf[BRIDGE_CTL_MAX];
            size_t have;

            if (read(fd, &clen, 1) != 1 || clen < BRIDGE_CTL_LENGTH || clen > BRIDGE_CTL_MAX)
            {
                continue;
            }
            cbuf[0] = BRIDGE_CTL_START;
            cbuf[1] = clen;
            have = 2;
            while (have < clen)
            {
                n = read(fd, cbuf + have, clen - have);
                if (n <= 0)
                {
                    return false;
                }
                have += (size_t)n;
            }
            g_ctl_type = cbuf[2];
            g_ctl_pending = true;
            *type_and_conn = 0;
            *payload_len = 0;
            return true;
        }
        if (hdr[0] != OPENMT_START)
        {
            log_msg( "bridge: unexpected byte from host, 0x%02X\n", hdr[0]);
            continue; /* resync: keep looking for a start byte */
        }
        got = 1;
    }

    got = 0;
    while (got < 2)
    {
        n = read(fd, hdr + 1 + got, 2 - got);
        if (n <= 0)
        {
            return false;
        }
        got += (size_t)n;
    }

    total_len = hdr[1];
    if (total_len < 3)
    {
        log_msg( "bridge: invalid frame length %u\n", total_len);
        return false;
    }
    *type_and_conn = hdr[2];
    actual_payload_len = (uint8_t)(total_len - 3);

    if (actual_payload_len > *payload_len)
    {
        log_msg( "bridge: frame payload (%u) exceeds caller buffer (%u), truncating\n",
            actual_payload_len, *payload_len);
        actual_payload_len = *payload_len;
    }

    got = 0;
    while (got < (size_t)actual_payload_len)
    {
        n = read(fd, payload + got, (size_t)actual_payload_len - got);
        if (n <= 0)
        {
            return false;
        }
        got += (size_t)n;
    }
    *payload_len = actual_payload_len;
    return true;
}

/* ---- TX sample queue: decouples the main command-processing loop
 * (which reads incoming PTY frames) from the TX playback thread, which
 * does the DSP and feeds the radio.  Without it, anything slow on the
 * playback side -- and keying up, which stops RX and starts TX on the
 * HackRF, is slow -- would delay reading the *next* incoming frame,
 * letting data pile up unread in the PTY's kernel buffer and then arrive
 * in a burst.  Sized for ~2 seconds of audio at 24kSa/s, 2 bytes/sample:
 * generous headroom against bursts at the cost of up to that much
 * latency. */
#define TX_QUEUE_CAP (24000 * 2 * 2)

typedef struct
{
    uint8_t buf[TX_QUEUE_CAP];
    size_t head; /* next byte to read */
    size_t tail; /* next byte to write */
    size_t count;
    pthread_mutex_t mutex;
} tx_queue_t;

static void
tx_queue_init(tx_queue_t *q)
{
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
}

/* Drops any data currently queued, without touching the mutex itself
 * (already initialized, stays that way for the life of the program).
 * Must be called on every de-key: SAMP_EOT (the common case) can
 * arrive while the queue still has a real backlog, and without this,
 * that leftover data from the just-ended transmission would sit in
 * the FIFO until the next transmission's new samples get appended
 * after it, meaning every subsequent transmission would start by
 * playing out stale data from the end of the previous one before ever
 * reaching its own real content. */
static void
tx_queue_clear(tx_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_unlock(&q->mutex);
}

/* Non-blocking: returns false (and drops the data) if there isn't
 * enough space, rather than ever blocking the caller -- a full queue
 * means the playback thread is falling behind for some other reason,
 * and blocking the frame-reading loop here would reintroduce the exact
 * problem this queue exists to avoid. */
static bool
tx_queue_push(tx_queue_t *q, const uint8_t *data, size_t len)
{
    size_t i;

    pthread_mutex_lock(&q->mutex);
    if (TX_QUEUE_CAP - q->count < len)
    {
        pthread_mutex_unlock(&q->mutex);
        return false;
    }
    for (i = 0; i < len; i++)
    {
        q->buf[q->tail] = data[i];
        q->tail = (q->tail + 1) % TX_QUEUE_CAP;
    }
    q->count += len;
    pthread_mutex_unlock(&q->mutex);
    return true;
}

/* Pops up to max_len bytes, returning the actual number popped (0 if
 * the queue is currently empty). */
static size_t
tx_queue_pop(tx_queue_t *q, uint8_t *out, size_t max_len)
{
    size_t n, i;

    pthread_mutex_lock(&q->mutex);
    n = q->count < max_len ? q->count : max_len;
    for (i = 0; i < n; i++)
    {
        out[i] = q->buf[q->head];
        q->head = (q->head + 1) % TX_QUEUE_CAP;
    }
    q->count -= n;
    pthread_mutex_unlock(&q->mutex);
    return n;
}

/* Current queue depth in bytes, without popping anything -- used by
 * the playback thread's prime/drain state machine below. */
static size_t
tx_queue_depth(tx_queue_t *q)
{
    size_t n;

    pthread_mutex_lock(&q->mutex);
    n = q->count;
    pthread_mutex_unlock(&q->mutex);
    return n;
}

static long long monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- IQ ring buffer ----------------------------------------------------
 * Byte FIFO between the HackRF's USB callback thread (which must never
 * block) and the DSP threads.  Writes are all-or-nothing and drop data
 * when full rather than wait. */
typedef struct
{
    uint8_t *buf;
    size_t cap, head, count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} ring_t;

static void
ring_init(ring_t *r, size_t cap)
{
    r->buf = malloc(cap);
    r->cap = cap;
    r->head = 0;
    r->count = 0;
    pthread_mutex_init(&r->mutex, NULL);
    pthread_cond_init(&r->cond, NULL);
}

static void
ring_free(ring_t *r)
{
    free(r->buf);
    pthread_mutex_destroy(&r->mutex);
    pthread_cond_destroy(&r->cond);
}

static bool
ring_push(ring_t *r, const uint8_t *data, size_t n)
{
    size_t tail, first;

    pthread_mutex_lock(&r->mutex);
    if (r->cap - r->count < n)
    {
        pthread_mutex_unlock(&r->mutex);
        return false;
    }
    tail = (r->head + r->count) % r->cap;
    first = r->cap - tail < n ? r->cap - tail : n;
    memcpy(r->buf + tail, data, first);
    memcpy(r->buf, data + first, n - first);
    r->count += n;
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->mutex);
    return true;
}

/* Called with the mutex held. */
static size_t
ring_take(ring_t *r, uint8_t *out, size_t max)
{
    size_t n = r->count < max ? r->count : max, first;

    first = r->cap - r->head < n ? r->cap - r->head : n;
    memcpy(out, r->buf + r->head, first);
    memcpy(out + first, r->buf, n - first);
    r->head = (r->head + n) % r->cap;
    r->count -= n;
    return n;
}

/* Non-blocking. */
static size_t
ring_pop(ring_t *r, uint8_t *out, size_t max)
{
    size_t n;

    pthread_mutex_lock(&r->mutex);
    n = ring_take(r, out, max);
    pthread_mutex_unlock(&r->mutex);
    return n;
}

/* Waits up to timeout_ms for data. */
static size_t
ring_wait_pop(ring_t *r, uint8_t *out, size_t max, unsigned int timeout_ms)
{
    struct timespec ts;
    size_t n;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000U;
    ts.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (ts.tv_nsec >= 1000000000L)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&r->mutex);
    while (r->count == 0)
    {
        if (pthread_cond_timedwait(&r->cond, &r->mutex, &ts) != 0)
        {
            break;
        }
    }
    n = ring_take(r, out, max);
    pthread_mutex_unlock(&r->mutex);
    return n;
}

static size_t
ring_count(ring_t *r)
{
    size_t n;

    pthread_mutex_lock(&r->mutex);
    n = r->count;
    pthread_mutex_unlock(&r->mutex);
    return n;
}

static void
ring_clear(ring_t *r)
{
    pthread_mutex_lock(&r->mutex);
    r->head = 0;
    r->count = 0;
    pthread_mutex_unlock(&r->mutex);
}

/* ---- Shared state ------------------------------------------------------*/
typedef struct
{
    hrf_dev_t *dev;
    bridge_config_t *cfg; /* mutable: a live SET_CONFIG packet can override
                           * fields after startup */
    dsp_tx_pipeline_t *tx_dsp;
    hrf_tx_fe_t *tx_fe;
    unsigned int ratio;   /* sample_rate / 125000 */

    /* key_up / key_down / retune are mutually exclusive */
    pthread_mutex_t radio_mutex;
    /* protects tx_dsp, tx_fe and pushes to tx_ring against key_down */
    pthread_mutex_t pb_mutex;

    atomic_bool keyed;
    atomic_llong last_tx_sample_ms;
    tx_queue_t queue;                  /* Q15 samples from the host */

    ring_t rx_ring;                    /* int8 IQ from the HackRF */
    atomic_ulong rx_dropped;           /* bytes the RX callback had to drop */
    atomic_bool rx_reset;              /* RX thread: restart the DSP chain */

    ring_t tx_ring;                    /* int8 IQ for the HackRF */
    atomic_bool tx_go;                 /* audio may flow; before this the radio is fed zeros */
    atomic_bool tx_ending;             /* draining for key-down: feed zeros after the data */
    atomic_bool flush;                 /* key-down wants the queued audio played out first */
    atomic_uint tx_zero_cbs;           /* callbacks that carried no data since tx_ending */
    atomic_ulong tx_underruns;
    int8_t hold_i, hold_q;             /* last sample, repeated on underrun (callback thread only) */
} bridge_t;

/* ---- HackRF callbacks ---------------------------------------------------*/
static int
rx_callback(const int8_t *iq, size_t n, void *ctx)
{
    bridge_t *b = ctx;

    if (!ring_push(&b->rx_ring, (const uint8_t *)iq, n))
    {
        atomic_fetch_add(&b->rx_dropped, n);
    }
    return 0;
}

/* Fills the whole buffer, every time.
 *   before tx_go:      zeros (no modulation; the radio idles on its LO)
 *   data available:    the data
 *   underrun:          the last sample repeated -- an unmodulated carrier
 *                      for as long as the gap lasts, which is the least
 *                      harmful thing to put on the air mid-transmission
 *   ending (key-down): zeros after the last data */
static int
tx_callback(int8_t *iq, size_t n, void *ctx)
{
    bridge_t *b = ctx;
    size_t got = 0, j;

    if (atomic_load(&b->tx_go))
    {
        got = ring_pop(&b->tx_ring, (uint8_t *)iq, n) & ~(size_t)1;
        if (got >= 2)
        {
            b->hold_i = iq[got - 2];
            b->hold_q = iq[got - 1];
        }
    }
    if (got < n)
    {
        if (!atomic_load(&b->tx_go))
        {
            memset(iq + got, 0, n - got);
        }
        else if (atomic_load(&b->tx_ending))
        {
            memset(iq + got, 0, n - got);
            if (got == 0)
            {
                atomic_fetch_add(&b->tx_zero_cbs, 1U);
            }
        }
        else
        {
            for (j = got; j + 1 < n; j += 2)
            {
                iq[j]     = b->hold_i;
                iq[j + 1] = b->hold_q;
            }
            atomic_fetch_add(&b->tx_underruns, 1UL);
        }
    }
    return 0;
}

/* ---- Radio control -------------------------------------------------------
 * LO = (f - offset) / (1 + ppm/1e6): the offset keeps the HackRF's centre
 * spike off the channel, ppm corrects its reference clock. */
static uint64_t
lo_hz(const bridge_config_t *c, uint32_t f_hz, int32_t offset_hz)
{
    double lo = ((double)f_hz - (double)offset_hz) / (1.0 + c->ppm * 1e-6);

    return (uint64_t)llround(lo);
}

/* radio_mutex held.  The device must not be streaming. */
static bool
radio_start_rx(bridge_t *b)
{
    const bridge_config_t *c = b->cfg;

    ring_clear(&b->rx_ring);
    if (!hrf_set_freq(b->dev, lo_hz(c, c->rx_freq_hz, c->rx_offset_hz)) ||
        !hrf_set_rx_gain(b->dev, c->lna_gain_db, c->vga_gain_db) ||
        !hrf_set_amp(b->dev, c->rx_amp))
    {
        return false;
    }
    atomic_store(&b->rx_reset, true);
    return hrf_start_rx(b->dev, rx_callback, b);
}

/* Re-apply tuning and gains to a receiver that is already running. */
static void
radio_retune_rx(bridge_t *b)
{
    const bridge_config_t *c = b->cfg;

    pthread_mutex_lock(&b->radio_mutex);
    if (!atomic_load(&b->keyed))
    {
        if (!hrf_set_freq(b->dev, lo_hz(c, c->rx_freq_hz, c->rx_offset_hz)) ||
            !hrf_set_rx_gain(b->dev, c->lna_gain_db, c->vga_gain_db) ||
            !hrf_set_amp(b->dev, c->rx_amp))
        {
            log_msg("bridge: retune of the receiver failed\n");
        }
    }
    /* while keyed, the new settings are picked up when RX restarts */
    pthread_mutex_unlock(&b->radio_mutex);
}

static bool
tx_key_up(bridge_t *b)
{
    const bridge_config_t *c = b->cfg;
    bool ok = false;

    pthread_mutex_lock(&b->radio_mutex);
    if (atomic_load(&b->keyed))
    {
        pthread_mutex_unlock(&b->radio_mutex);
        return true;
    }
    /* A fresh --tx-key-timeout-ms grace period starts now, not whenever the
     * last sample happened to arrive. */
    atomic_store(&b->last_tx_sample_ms, monotonic_ms());

    /* Half duplex: the receiver has to stop before the transmitter can
     * start.  Samples queued but not yet processed belong to a signal we
     * are about to step on, so they go. */
    hrf_stop_rx(b->dev);
    ring_clear(&b->tx_ring);
    atomic_store(&b->tx_go, false);
    atomic_store(&b->tx_ending, false);
    atomic_store(&b->flush, false);
    atomic_store(&b->tx_zero_cbs, 0U);
    b->hold_i = 0;
    b->hold_q = 0;

    if (hrf_set_freq(b->dev, lo_hz(c, c->tx_freq_hz, c->tx_offset_hz)) &&
        hrf_set_tx_gain(b->dev, c->tx_vga_db) &&
        hrf_set_amp(b->dev, c->tx_amp) &&
        hrf_start_tx(b->dev, tx_callback, b))
    {
        ok = true;
    }
    if (!ok)
    {
        log_msg("bridge: failed to start the transmitter, going back to receive\n");
        radio_start_rx(b);
        pthread_mutex_unlock(&b->radio_mutex);
        return false;
    }
    usleep(c->tx_key_settle_ms * 1000U);

    pthread_mutex_lock(&b->pb_mutex);
    dsp_tx_pipeline_reset(b->tx_dsp);
    hrf_tx_fe_reset(b->tx_fe);
    pthread_mutex_unlock(&b->pb_mutex);

    atomic_store(&b->keyed, true);
    log_msg("bridge: TX keyed\n");
    pthread_mutex_unlock(&b->radio_mutex);
    return true;
}

/* play_out: the host has finished (SAMP_EOT / SET_MODE idle), so first let
 * the audio it has already sent but the playback thread has not reached yet
 * be played -- Modem_Host.c sends EOT right behind its last audio frame, a
 * couple of hundred milliseconds before that audio is due on the air, so
 * unkeying at once would cut the end off.  Not wanted when unkeying because
 * the samples stopped coming (the playback thread is the caller then) or at
 * shutdown. */
static void
tx_key_down_ex(bridge_t *b, bool play_out)
{
    long long deadline;

    pthread_mutex_lock(&b->radio_mutex);
    if (!atomic_load(&b->keyed))
    {
        pthread_mutex_unlock(&b->radio_mutex);
        return;
    }
    if (play_out)
    {
        /* 24 kSa/s x 2 bytes = 48 bytes per millisecond */
        deadline = monotonic_ms() + 500 + (long long)(tx_queue_depth(&b->queue) / 48U);
        atomic_store(&b->flush, true);
        while (tx_queue_depth(&b->queue) > 0 && monotonic_ms() < deadline)
        {
            usleep(2000);
        }
    }
    /* From here no more audio goes into the ring (the playback thread works
     * on one batch at a time under pb_mutex, so taking it also waits out a
     * batch in progress); the callback runs the data already there out and
     * then sends zeros. */
    pthread_mutex_lock(&b->pb_mutex);
    atomic_store(&b->tx_ending, true);
    pthread_mutex_unlock(&b->pb_mutex);

    /* The audio is not on the air until it has gone through the USB
     * queue: wait until the ring is empty and HRF_TX_QUEUE_BUFFERS
     * further buffers have been requested -- by then the last buffer that
     * held real data has been sent.  Never primed means nothing real was
     * ever handed over, so nothing to flush. */
    deadline = monotonic_ms() + 4000;
    while (atomic_load(&b->tx_go) && monotonic_ms() < deadline)
    {
        if (ring_count(&b->tx_ring) == 0 && atomic_load(&b->tx_zero_cbs) >= HRF_TX_QUEUE_BUFFERS)
        {
            break;
        }
        usleep(5000);
    }
    hrf_stop_tx(b->dev);
    atomic_store(&b->keyed, false);
    tx_queue_clear(&b->queue);
    if (!radio_start_rx(b))
    {
        log_msg("bridge: failed to restart the receiver\n");
    }
    log_msg("bridge: TX unkeyed (%lu underrun buffer%s)\n", atomic_load(&b->tx_underruns),
        atomic_load(&b->tx_underruns) == 1UL ? "" : "s");
    atomic_store(&b->tx_underruns, 0UL);
    pthread_mutex_unlock(&b->radio_mutex);
}

static void
tx_key_down(bridge_t *b)
{
    tx_key_down_ex(b, false);
}

/* Enqueues incoming Q15 samples (already RRC/GMSK pulse-shaped by
 * whichever protocol service sent them) for the TX playback thread.
 * Returns quickly; keys up first if needed -- presence of TX sample data is
 * this bridge's primary key-up signal, as in sx1255-openmt-bridge. */
static void
tx_handle_samples(bridge_t *b, const uint8_t *payload, uint8_t payload_len)
{
    atomic_store(&b->last_tx_sample_ms, monotonic_ms());
    if (!atomic_load(&b->keyed))
    {
        tx_key_up(b);
    }
    if (!tx_queue_push(&b->queue, payload, payload_len))
    {
        static unsigned int dropped = 0U;
        static long long last_log_ms = 0;
        long long now_ms = monotonic_ms();

        dropped += payload_len / 2U;
        if (now_ms - last_log_ms >= 1000)
        {
            log_msg("bridge: [tx] queue full (%zu bytes queued), dropped %u samples (%.0f ms)\n",
                tx_queue_depth(&b->queue), dropped, dropped / 24.0);
            dropped = 0U;
            last_log_ms = now_ms;
        }
    }
}

/* ---- TX playback thread: Q15 queue -> float -> FM modulator (125 kSa/s)
 * -> HackRF-rate IQ -> tx_ring, paced so that about TX_LOOKAHEAD_MS of
 * audio is queued ahead of the USB callback. -------------------------- */
static void *
tx_playback_thread_main(void *arg)
{
    bridge_t *b = arg;
    uint8_t raw[SAMPLES_PER_FRAME * 2];
    float samples[SAMPLES_PER_FRAME];
    dsp_cplx_t *iq_buf = NULL;
    int iq_cap = 0;
    int8_t *out = NULL;
    size_t out_cap = 0;
    bool priming = true;
    size_t prime_threshold_bytes = (size_t)((double)b->cfg->tx_prime_ms / 1000.0 * 24000.0 * 2.0);
    const size_t lookahead_bytes = (size_t)BB_RATE / 1000U * TX_LOOKAHEAD_MS * b->ratio * 2U;
    unsigned long last_underruns = 0;
    long long last_report_ms = 0;

    log_msg("bridge: TX playback thread running (priming threshold: %zu bytes, ~%ums)\n",
        prime_threshold_bytes, b->cfg->tx_prime_ms);
    while (!g_stop)
    {
        size_t n_bytes;
        int n_samples, n_iq, i;

        if (atomic_load(&b->keyed) &&
            monotonic_ms() - atomic_load(&b->last_tx_sample_ms) > b->cfg->tx_key_timeout_ms)
        {
            log_msg("bridge: [tx] no samples for %ums, unkeying\n", b->cfg->tx_key_timeout_ms);
            tx_key_down(b);
            priming = true;
        }

        if (atomic_load(&b->keyed) && monotonic_ms() - last_report_ms >= 1000)
        {
            unsigned long u = atomic_load(&b->tx_underruns);

            if (u != last_underruns)
            {
                log_msg("bridge: [tx] USB buffer underrun(s): %lu so far this transmission "
                    "(carrier held through the gap)\n", u);
                last_underruns = u;
            }
            last_report_ms = monotonic_ms();
        }
        if (!atomic_load(&b->keyed))
        {
            last_underruns = 0;
        }

        if (atomic_load(&b->flush))
        {
            priming = false;   /* the host is done: play what there is, however short */
        }
        if (priming && prime_threshold_bytes > 0)
        {
            if (tx_queue_depth(&b->queue) < prime_threshold_bytes)
            {
                /* Just wait; inserting silence would stretch the symbol
                 * stream (see the sx1255 bridge's README). */
                usleep(1000);
                continue;
            }
            priming = false;
            log_msg("bridge: [tx-playback] primed, starting playback\n");
        }

        if (ring_count(&b->tx_ring) > lookahead_bytes)
        {
            usleep(1000);
            continue;
        }

        pthread_mutex_lock(&b->pb_mutex);
        n_bytes = tx_queue_pop(&b->queue, raw, sizeof(raw));
        if (n_bytes == 0)
        {
            pthread_mutex_unlock(&b->pb_mutex);
            if (prime_threshold_bytes > 0 && !atomic_load(&b->flush))
            {
                priming = true;
            }
            usleep(1000);
            continue;
        }

        n_samples = (int)(n_bytes / 2);
        for (i = 0; i < n_samples; i++)
        {
            int16_t q15;

            memcpy(&q15, raw + i * 2, 2);
            samples[i] = (float)q15 / b->cfg->tx_symbol_scale;
            if (atomic_load(&b->cfg->tx_invert))
            {
                samples[i] = -samples[i];
            }
        }

        if (!atomic_load(&b->keyed) || atomic_load(&b->tx_ending))
        {
            /* key-down is in progress: this audio is not wanted */
            pthread_mutex_unlock(&b->pb_mutex);
            continue;
        }
        n_iq = dsp_tx_pipeline_process_baseband(b->tx_dsp, samples, n_samples,
            b->cfg->deviation_hz, &iq_buf, &iq_cap);
        if (n_iq > 0)
        {
            size_t need = hrf_tx_fe_bytes(b->tx_fe, (size_t)n_iq), got;

            if (need > out_cap)
            {
                out = realloc(out, need);
                out_cap = need;
            }
            got = hrf_tx_fe_process(b->tx_fe, iq_buf, (size_t)n_iq, out);
            if (!ring_push(&b->tx_ring, (const uint8_t *)out, got))
            {
                log_msg("bridge: [tx] IQ ring full, dropped %zu bytes\n", got);
            }
            atomic_store(&b->tx_go, true);
        }
        pthread_mutex_unlock(&b->pb_mutex);
    }
    free(iq_buf);
    free(out);
    log_msg("bridge: TX playback thread exiting\n");
    return NULL;
}

/* ---- RX thread: HackRF IQ -> 125 kSa/s -> generic DSP front end -> Q15 ->
 * SAMP_DATA frames out to the host ---------------------------------------- */
typedef struct
{
    bridge_t *b;
    hrf_rx_fe_t *fe;
    int pty_fd;
} rx_thread_arg_t;

static void *
rx_thread_main(void *arg)
{
    rx_thread_arg_t *a = arg;
    bridge_t *b = a->b;
    const bridge_config_t *cfg = b->cfg;
    uint8_t q15_payload[SAMPLES_PER_FRAME * 2];
    int q15_count = 0;
    uint8_t *raw = malloc(65536);
    dsp_cplx_t *bb = malloc(((65536 / 2) / (size_t)b->ratio + 16) * sizeof(*bb));
    dsp_rx_pipeline_t *dsp = dsp_rx_pipeline_create(BB_RATE);
    unsigned long last_dropped = 0;
    long long last_report_ms = 0;
    /* --rx-stats: level of the raw 8-bit samples, to set the HackRF gains */
    double st_sum = 0.0;
    unsigned long st_n = 0, st_clip = 0;
    int st_peak = 0;
    long long st_ms = monotonic_ms();
    rfspec_t *spec = rfspec_create((uint16_t)cfg->spectrum_port, cfg->spectrum_bind, BB_RATE);

    if (cfg->spectrum_port != 0U)
    {
        if (spec != NULL)
        {
            log_msg("bridge: serving the RF spectrum to monitors on UDP port %u\n", cfg->spectrum_port);
        }
        else
        {
            log_msg("bridge: cannot serve the RF spectrum on UDP port %u (in use?): the Monitor tab will have no RF spectrum\n", cfg->spectrum_port);
        }
    }

    log_msg("bridge: RX thread running\n");
    while (!g_stop)
    {
        size_t nb, n_bb, s;

        if (atomic_exchange(&b->rx_reset, false))
        {
            /* RX (re)started: the filters hold a signal that is no longer
             * there (or was never continuous with this one) */
            hrf_rx_fe_reset(a->fe);
            dsp_rx_pipeline_destroy(dsp);
            dsp = dsp_rx_pipeline_create(BB_RATE);
            q15_count = 0;
        }
        nb = ring_wait_pop(&b->rx_ring, raw, 65536, 100);
        if (nb == 0)
        {
            continue;
        }
        if (cfg->rx_stats)
        {
            size_t k2;
            long long now = monotonic_ms();

            for (k2 = 0; k2 < nb; k2++)
            {
                int v = (int8_t)raw[k2];
                int av = v < 0 ? -v : v;

                st_sum += (double)v * (double)v;
                st_n++;
                if (av > st_peak)
                {
                    st_peak = av;
                }
                if (av >= 127)
                {
                    st_clip++;
                }
            }
            if (now - st_ms >= 2000 && st_n > 0)
            {
                /* The baseband DC filter's average is the frequency error of the signal (rad per sample at
                 * 12.5 kHz).  It is only meaningful while a signal is present (a second or more). */
                double off_hz = dsp_rx_pipeline_get_dc_bias(dsp) * 12500.0 / (2.0 * M_PI);
                double ppm_adj = (atomic_load(&cfg->rx_invert) ? 1.0 : -1.0) * off_hz / (double)cfg->rx_freq_hz * 1e6;

                log_msg("bridge: rx level: rms %.1f of 127, peak %d, clipped %.3f%%  (rule of thumb: rms 15-40 with the signal present, no clipping)\n",
                    sqrt(st_sum / (double)st_n), st_peak, 100.0 * (double)st_clip / (double)st_n);
                log_msg("bridge: rx frequency error of the signal: %+.0f Hz (a first guess for freqOffsetPpm: %+.2f more than now, "
                    "i.e. %.2f; the sign is not verified on hardware: if the error grows, use the other sign)\n",
                    off_hz, ppm_adj, cfg->ppm + ppm_adj);
                st_sum = 0.0;
                st_n = 0;
                st_clip = 0;
                st_peak = 0;
                st_ms = now;
            }
        }
        n_bb = hrf_rx_fe_process(a->fe, (const int8_t *)raw, nb / 2U, bb, (65536 / 2) / (size_t)b->ratio + 16);

        if (spec != NULL && rfspec_poll(spec))
        {
            rfspec_set_freq(spec, cfg->rx_freq_hz);
            rfspec_set_mirror(spec, atomic_load(&cfg->rx_invert));
            rfspec_adc(spec, (const int8_t *)raw, nb);
            rfspec_feed(spec, bb, n_bb);
        }

        for (s = 0; s < n_bb; s++)
        {
            float baseband[16];
            int n = dsp_rx_pipeline_process_baseband(dsp, bb[s], baseband, 16);
            int k;

            for (k = 0; k < n; k++)
            {
                float clamped = (atomic_load(&cfg->rx_invert) ? -baseband[k] : baseband[k]) * cfg->rx_gain * ((float)cfg->rx_level / (float)RX_LEVEL_UNITY);
                int16_t q15;

                if (clamped > 1.0f)
                {
                    clamped = 1.0f;
                }
                else if (clamped < -1.0f)
                {
                    clamped = -1.0f;
                }
                q15 = (int16_t)(clamped * 32767.0f);
                memcpy(q15_payload + q15_count * 2, &q15, 2);
                q15_count++;

                if (q15_count >= SAMPLES_PER_FRAME)
                {
                    write_frame(a->pty_fd, (uint8_t)(OPENMT_SAMP_DATA | 0), q15_payload,
                        (uint8_t)(q15_count * 2));
                    q15_count = 0;
                }
            }
        }

        if (monotonic_ms() - last_report_ms >= 1000)
        {
            unsigned long d = atomic_load(&b->rx_dropped);

            if (d != last_dropped)
            {
                log_msg("bridge: [rx] IQ ring overflowed: %lu bytes dropped (the DSP is not keeping "
                    "up with the HackRF -- check CPU load, or lower --sample-rate)\n", d - last_dropped);
                last_dropped = d;
            }
            last_report_ms = monotonic_ms();
        }
    }
    dsp_rx_pipeline_destroy(dsp);
    rfspec_destroy(spec);
    free(raw);
    free(bb);
    log_msg("bridge: RX thread exiting\n");
    return NULL;
}

/* ---- Command/TX processing loop (runs in main()) --------------------- */

/* "OPENMTSX" SET_CONFIG payload.  The format was defined for
 * sx1255-openmt-bridge and is used here unchanged so one Modem_Host.c
 * (modem=OPENMTSX) drives either bridge.  Multi-byte fields big-endian:
 *
 *   [0]      version (1)
 *   [1..4]   RX frequency, Hz            -> --rx-freq   (0 keeps the bridge's)
 *   [5..8]   TX frequency, Hz            -> --tx-freq   (0 keeps the bridge's)
 *   [9]      "LNA gain"  (rxLnaGain)     -> HackRF LNA gain, 0-40 in 8 dB steps
 *   [10]     "PGA gain"  (rxPgaGain)     -> HackRF RX VGA gain, 0-62 in 2 dB steps
 *   [11]     "mixer gain" x2 (txMixerGain) -> unused here
 *   [12]     "DAC gain"  (txDacGain)     -> HackRF TX VGA gain, 0-47 dB
 *   [13..14] RX PLL bandwidth            -> unused here
 *   [15..16] TX PLL bandwidth            -> unused here
 *   [17]     bit0 RX invert, bit1 TX invert
 *
 * For the three gain fields (and the frequencies) a value of 0 or below
 * means "not set": the bridge keeps what its own command line gave it.
 * That makes a configuration made for the SX1255 harmless here (its
 * txDacGain of 0/-3/-6/-9 never raises the TX power) while still letting
 * the dashboard set what it knows about. */
#define OPENMTSX_CONFIG_LEN 18
#define OPENMTSX_CONFIG_VERSION 1
#define OPENMTSX_INVERT_RX_BIT 0x01U
#define OPENMTSX_INVERT_TX_BIT 0x02U

static void
apply_openmtsx_config(bridge_t *b, const uint8_t *payload, uint8_t payload_len)
{
    bridge_config_t *cfg = b->cfg;
    uint32_t rx_freq_hz, tx_freq_hz;
    unsigned int lna, vga;
    int8_t dac_gain_db;

    if (payload_len != OPENMTSX_CONFIG_LEN)
    {
        log_msg("bridge: SET_CONFIG: unexpected openmtsx payload length %u (expected %u), "
            "ignoring\n", payload_len, OPENMTSX_CONFIG_LEN);
        return;
    }
    if (payload[0] != OPENMTSX_CONFIG_VERSION)
    {
        log_msg("bridge: SET_CONFIG: unknown openmtsx config version %u (expected %u), "
            "ignoring\n", payload[0], OPENMTSX_CONFIG_VERSION);
        return;
    }
    rx_freq_hz = ((uint32_t)payload[1] << 24) | ((uint32_t)payload[2] << 16) |
        ((uint32_t)payload[3] << 8) | (uint32_t)payload[4];
    tx_freq_hz = ((uint32_t)payload[5] << 24) | ((uint32_t)payload[6] << 16) |
        ((uint32_t)payload[7] << 8) | (uint32_t)payload[8];
    lna = payload[9];
    vga = payload[10];
    dac_gain_db = (int8_t)payload[12];

    if (rx_freq_hz >= 1000000U)
    {
        cfg->rx_freq_hz = rx_freq_hz;
    }
    if (tx_freq_hz >= 1000000U)
    {
        cfg->tx_freq_hz = tx_freq_hz;
    }
    if (lna > 0U)
    {
        cfg->lna_gain_db = (lna > 40U ? 40U : lna) / 8U * 8U;
    }
    if (vga > 0U)
    {
        cfg->vga_gain_db = (vga > 62U ? 62U : vga) / 2U * 2U;
    }
    if (dac_gain_db > 0)
    {
        cfg->tx_vga_db = (unsigned int)(dac_gain_db > 47 ? 47 : dac_gain_db);
    }
    /* read per sample by the RX thread / TX playback thread */
    atomic_store(&cfg->rx_invert, (payload[17] & OPENMTSX_INVERT_RX_BIT) != 0);
    atomic_store(&cfg->tx_invert, (payload[17] & OPENMTSX_INVERT_TX_BIT) != 0);

    log_msg("bridge: SET_CONFIG: applied openmtsx config -- rx=%u tx=%u lna=%u vga=%u txvga=%u "
        "rx_invert=%d tx_invert=%d\n", cfg->rx_freq_hz, cfg->tx_freq_hz, cfg->lna_gain_db,
        cfg->vga_gain_db, cfg->tx_vga_db, (int)atomic_load(&cfg->rx_invert),
        (int)atomic_load(&cfg->tx_invert));
    radio_retune_rx(b);
}

/*
 * settings_from_cfg() / settings_to_cfg()
 *
 * Copy the settings that live in the config database between the
 * configuration and a HrfSettings.
 */
static void
settings_from_cfg(const bridge_config_t *cfg, HrfSettings *d)
{
    memset(d, 0, sizeof(*d));
    d->rx_freq_hz = cfg->rx_freq_hz;
    d->tx_freq_hz = cfg->tx_freq_hz;
    d->lna_gain_db = cfg->lna_gain_db;
    d->vga_gain_db = cfg->vga_gain_db;
    d->tx_vga_db = cfg->tx_vga_db;
    d->rx_invert = atomic_load(&cfg->rx_invert);
    d->tx_invert = atomic_load(&cfg->tx_invert);
    d->ppm = cfg->ppm;
    d->rx_level = cfg->rx_level;
}

static void
settings_to_cfg(const HrfSettings *d, bridge_config_t *cfg)
{
    cfg->rx_freq_hz = d->rx_freq_hz;
    cfg->tx_freq_hz = d->tx_freq_hz;
    cfg->lna_gain_db = d->lna_gain_db;
    cfg->vga_gain_db = d->vga_gain_db;
    cfg->tx_vga_db = d->tx_vga_db;
    atomic_store(&cfg->rx_invert, d->rx_invert);
    atomic_store(&cfg->tx_invert, d->tx_invert);
    cfg->ppm = d->ppm;
    cfg->rx_level = d->rx_level;
}

/*
 * reload_settings()
 *
 * Reads the settings from the config database again.  When they differ from
 * the ones in use they are applied and the receiver is tuned again (a
 * transmitter that is keyed picks them up at its next key-up).
 */
static void
reload_settings(bridge_t *b)
{
    HrfSettings now, wanted;

    settings_from_cfg(b->cfg, &now);
    wanted = now;
    hrfDbLoad(b->cfg->modem_name, &wanted);
    if (!hrfDbDiffer(&now, &wanted))
    {
        log_msg("bridge: settings read, nothing changed\n");
        return;
    }
    log_msg("bridge: settings changed -- rx=%u tx=%u lna=%u vga=%u txvga=%u ppm=%g rxLevel=%u rx_invert=%d tx_invert=%d\n",
        wanted.rx_freq_hz, wanted.tx_freq_hz, wanted.lna_gain_db, wanted.vga_gain_db, wanted.tx_vga_db, wanted.ppm, wanted.rx_level,
        (int)wanted.rx_invert, (int)wanted.tx_invert);
    settings_to_cfg(&wanted, b->cfg);
    radio_retune_rx(b);
}

/* Carries out a control frame of Modem_Host (see tools/bridge_common.h). */
static void
handle_control(bridge_t *b)
{
    if (g_ctl_type == BRIDGE_CTL_RELOAD || g_ctl_type == BRIDGE_CTL_HOST_UP)
    {
        reload_settings(b);
    }
    else if (g_ctl_type == BRIDGE_CTL_COMMAND)
    {
        hrfDbAckUnsupported(b->cfg->modem_name);
    }
}

static void
handle_frame(bridge_t *b, int pty_fd, uint8_t type_and_conn, const uint8_t *payload,
    uint8_t payload_len)
{
    uint8_t type = (uint8_t)(type_and_conn & 0xF0);
    uint8_t conn_or_mode = (uint8_t)(type_and_conn & 0x0F);

    switch (type)
    {
        case OPENMT_SET_CONFIG:
            log_msg("bridge: SET_CONFIG received (%u payload bytes)\n", payload_len);
            if (payload_len == OPENMTSX_CONFIG_LEN)
            {
                apply_openmtsx_config(b, payload, payload_len);
            }
            write_frame(pty_fd, OPENMT_ACK, NULL, 0);
            break;

        case OPENMT_GET_STATUS:
        {
            /* Ample space: this runs on the host CPU, not a constrained
             * MCU.  0x0f is the most the 4-bit field can say. */
            /* Modem_Host reads the free-frame count from the low nibble of
             * the TYPE byte (buffer[2] & 0x0f), like the real modem sends
             * it, so it goes there; a 0 there reads as "no space". */
            write_frame(pty_fd, (uint8_t)(OPENMT_GET_STATUS | 0x0FU), NULL, 0);
            break;
        }

        case OPENMT_SET_MODE:
            /* The SX1255 bridge keys the transmitter on SET_MODE|conn.  Here
             * it does not, by default: the hosts send SET_MODE not only when
             * they are about to transmit but also when they decode the
             * header of a call they are RECEIVING (to hold the modem for the
             * call -- the M17, D-Star and P25 hosts do), and a half-duplex
             * radio that keyed up then would drop the call it is receiving.
             * The first TX sample is the key-up signal instead (see
             * tx_handle_samples()); the cost is that the stop-RX / start-TX
             * switch happens as the audio starts rather than just before,
             * which the TX priming delay absorbs.  --key-on-set-mode
             * restores the SX1255 behaviour. */
            if (conn_or_mode == OPENMT_MODE_IDLE)
            {
                log_msg("bridge: SET_MODE: idle\n");
                tx_key_down_ex(b, true);
            }
            else
            {
                log_msg("bridge: SET_MODE: conn %u active\n", conn_or_mode);
                if (b->cfg->key_on_set_mode)
                {
                    tx_key_up(b);
                }
            }
            write_frame(pty_fd, OPENMT_ACK, NULL, 0);
            break;

        case OPENMT_ADD_MODE:
            log_msg("bridge: ADD_MODE for conn %u\n", conn_or_mode);
            write_frame(pty_fd, OPENMT_ACK, NULL, 0);
            break;

        case OPENMT_DEL_MODE:
            log_msg("bridge: DEL_MODE for conn %u\n", conn_or_mode);
            write_frame(pty_fd, OPENMT_ACK, NULL, 0);
            break;

        case OPENMT_SAMP_DATA:
            /* Modem_Host.c's protocol-service -> modem SAMP_DATA frames
             * have a non-standard 4-byte header, [0x61][len][0x70]
             * [txLevel]: the txLevel byte is skipped here, amplitude is
             * --tx-symbol-scale / --deviation-hz's business. */
            if (payload_len < 1)
            {
                break;
            }
            tx_handle_samples(b, payload + 1, (uint8_t)(payload_len - 1));
            break;

        case OPENMT_TO_MODEM:
            /* Older 0xE0-framed MMDVM path; should not arrive on this link,
             * handled like SAMP_DATA without the extra byte for robustness. */
            tx_handle_samples(b, payload, payload_len);
            break;

        case OPENMT_SAMP_EOT:
            log_msg("bridge: SAMP_EOT received, playing out the queue and unkeying\n");
            tx_key_down_ex(b, true);
            break;

        default:
            log_msg("bridge: unhandled frame type 0x%02X (conn/mode %u, %u payload "
                "bytes)\n", type, conn_or_mode, payload_len);
            break;
    }
}

int
main(int argc, char **argv)
{
    bridge_config_t cfg;
    bridge_t b;
    rx_thread_arg_t rx_arg;
    pthread_t rx_thread, tx_thread;
    hrf_rx_fe_t *rx_fe;
    int pty_fd;
    char slave_path[256];

    if (!parse_args(argc, argv, &cfg) || !check_config(&cfg))
    {
        return 1;
    }

    {
        /* sigaction without SA_RESTART so Ctrl-C interrupts the blocking
         * read in the main loop (plain signal() would restart it). */
        struct sigaction sa;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handle_signal;
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    {
        /* The settings live in the config database: the options only give the defaults of
         * settings that are not there yet, and what is saved wins. */
        HrfSettings hs;

        settings_from_cfg(&cfg, &hs);
        hrfDbDefaults(cfg.modem_name, &hs);
        hrfDbLoad(cfg.modem_name, &hs);
        settings_to_cfg(&hs, &cfg);
    }

    pty_fd = open_bridge_pty(slave_path, sizeof(slave_path));
    if (pty_fd < 0)
    {
        return 1;
    }
    log_msg("bridge: PTY slave device: %s\n", slave_path);
    log_msg("bridge: point OpenMT's modemtty config at this path (the bridge publishes its stream type in the config database)\n");

    if (cfg.pty_symlink != NULL && cfg.pty_symlink[0] != '\0')
    {
        unlink(cfg.pty_symlink);
        if (symlink(slave_path, cfg.pty_symlink) != 0)
        {
            log_msg("bridge: warning -- could not create symlink %s -> %s: %s\n",
                cfg.pty_symlink, slave_path, strerror(errno));
        }
        else
        {
            log_msg("bridge: symlinked %s -> %s\n", cfg.pty_symlink, slave_path);
        }
    }

    hrfDbPublish(cfg.modem_name, cfg.pty_symlink);

    memset(&b, 0, sizeof(b));
    b.cfg = &cfg;
    b.ratio = hrf_fe_ratio(cfg.sample_rate);
    pthread_mutex_init(&b.radio_mutex, NULL);
    pthread_mutex_init(&b.pb_mutex, NULL);
    atomic_store(&b.keyed, false);
    atomic_store(&b.last_tx_sample_ms, monotonic_ms());
    tx_queue_init(&b.queue);
    ring_init(&b.rx_ring, IQ_RING_BYTES);
    ring_init(&b.tx_ring, IQ_RING_BYTES);

    b.dev = hrf_open(cfg.serial);
    if (b.dev == NULL)
    {
        return 1;
    }
    if (!hrf_set_sample_rate(b.dev, cfg.sample_rate) ||
        !hrf_set_baseband_bw(b.dev, cfg.sample_rate / 4U * 3U / 2U) ||
        !hrf_set_bias_tee(b.dev, cfg.bias_tee))
    {
        hrf_close(b.dev);
        return 1;
    }
    rx_fe   = hrf_rx_fe_create(cfg.sample_rate, cfg.rx_offset_hz);
    b.tx_fe = hrf_tx_fe_create(cfg.sample_rate, cfg.tx_offset_hz, cfg.tx_amplitude);
    if (rx_fe == NULL || b.tx_fe == NULL)
    {
        log_msg("bridge: cannot build the sample-rate converters for rate %u offsets %d/%d\n",
            cfg.sample_rate, cfg.rx_offset_hz, cfg.tx_offset_hz);
        hrf_close(b.dev);
        return 1;
    }
    b.tx_dsp = dsp_tx_pipeline_create(BB_RATE);

    log_msg("bridge: RX gains lna=%u vga=%u (from the config database; the command line only sets the defaults)\n", cfg.lna_gain_db, cfg.vga_gain_db);
    log_msg("bridge: HackRF at %u Sa/s (x%u to %u), RX %u Hz (LO %llu), TX %u Hz (LO %llu), ppm %.2f\n",
        cfg.sample_rate, b.ratio, BB_RATE, cfg.rx_freq_hz,
        (unsigned long long)lo_hz(&cfg, cfg.rx_freq_hz, cfg.rx_offset_hz), cfg.tx_freq_hz,
        (unsigned long long)lo_hz(&cfg, cfg.tx_freq_hz, cfg.tx_offset_hz), cfg.ppm);

    pthread_mutex_lock(&b.radio_mutex);
    if (!radio_start_rx(&b))
    {
        pthread_mutex_unlock(&b.radio_mutex);
        log_msg("bridge: could not start receiving\n");
        hrf_close(b.dev);
        return 1;
    }
    pthread_mutex_unlock(&b.radio_mutex);

    rx_arg.b = &b;
    rx_arg.fe = rx_fe;
    rx_arg.pty_fd = pty_fd;
    if (pthread_create(&rx_thread, NULL, rx_thread_main, &rx_arg) != 0 ||
        pthread_create(&tx_thread, NULL, tx_playback_thread_main, &b) != 0)
    {
        log_msg("bridge: failed to create threads\n");
        return 1;
    }

    log_msg("bridge: running. Ctrl-C to stop.\n");
    while (!g_stop)
    {
        uint8_t type_and_conn;
        uint8_t payload[MAX_FRAME_BYTES];
        uint8_t payload_len = sizeof(payload);

        if (!read_frame(pty_fd, &type_and_conn, payload, &payload_len))
        {
            /* Modem_Host.c has not opened the slave yet (or has restarted);
             * either end can come and go. */
            usleep(200 * 1000);
            continue;
        }
        if (g_ctl_pending)
        {
            g_ctl_pending = false;
            handle_control(&b);
            continue;
        }
        handle_frame(&b, pty_fd, type_and_conn, payload, payload_len);
    }

    log_msg("bridge: shutting down\n");
    g_stop = 1;
    pthread_join(rx_thread, NULL);
    pthread_join(tx_thread, NULL);

    tx_key_down(&b);
    hrf_stop_rx(b.dev);
    hrf_close(b.dev);

    hrf_rx_fe_destroy(rx_fe);
    hrf_tx_fe_destroy(b.tx_fe);
    dsp_tx_pipeline_destroy(b.tx_dsp);
    ring_free(&b.rx_ring);
    ring_free(&b.tx_ring);
    close(pty_fd);
    return 0;
}
