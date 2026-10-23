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
 * bridge.c - SX1255 <-> OpenMT bridge.
 *
 * Presents the SX1255 HAT to OpenMT's Modem_Host.c as a virtual
 * "OPENMTHS"-class modem over a PTY (pseudo-terminal), doing the generic,
 * mode-agnostic RF front-end DSP itself (SPI/I2S control, IQ capture,
 * DC removal, decimation, FM demodulation, baseband AFC, resampling to
 * 24 kSa/s) and handing off real-valued Q15 baseband samples -- exactly
 * what a real OPENMT_HS-class MCU would produce -- for whichever
 * protocol service is currently active (M17, D-STAR, DMR, P25, ...) to
 * do its own mode-specific RRC/GMSK matched filtering, sync correlation,
 * and FEC decode. Symmetric on TX: consumes already-pulse-shaped Q15
 * samples from the active service and feeds them through the resampler
 * and FM modulator only.
 *
 * Point Modem_Host.c's config at the printed PTY slave path (or the
 * --pty-symlink path, if given), with modem=OPENMTHS. No changes to
 * Modem_Host.c or any protocol service are required -- from the host's
 * side, a plain read()/write() on a PTY behaves exactly like a real
 * serial port, and this program speaks the identical wire protocol a
 * real OPENMT_HS firmware would.
 *
 * Wire protocol constants and framing below were traced directly against
 * OpenMT's modem/Modem_Host.c and modem/mmdvm.h (see the project's own
 * conversation/design notes for exact function/line references):
 *   - Serial-side (host <-> modem) framing: 0x61, 1-byte length (max 255
 *     bytes/frame -- caps SAMP_DATA payload size), then a type byte whose
 *     high nibble is the message type and low nibble is a connection/
 *     mode ID (processSerial() in Modem_Host.c).
 *   - Fully fire-and-forget: modemTxThread() drains and writes queued
 *     frames on a fixed timer with no wait for a response; acks received
 *     back are purely logged, never gate further sends. This bridge
 *     mirrors that -- no ack-blocking implemented, none needed.
 *   - SET_MODE|conn_id means "key up, this connection is now active for
 *     TX"; SET_MODE|0x0F means "unkey, go idle" (COMM_SET_MODE/
 *     COMM_SET_IDLE handling in Modem_Host.c, both of which call
 *     setMode() with exactly these values for any non-MMDVM modem type).
 *   - GET_STATUS asks for available buffer space; since this runs on the
 *     host CPU rather than a constrained MCU, it always reports plenty.
 */
#define _DEFAULT_SOURCE
#include "sx1255.h"
#include "alsa_io.h"
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
/*
 * log_msg()
 *
 * Prints a printf-style message to stderr with a Unix timestamp
 * (seconds.milliseconds) in front, so timing can be read straight from the
 * log.
 *
 * Parameters:
 *   fmt - printf format string, followed by its arguments.
 *
 * Returns: nothing.
 */
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
#define MAX_FRAME_BYTES 255

/* Target lookahead maintained in ALSA's own hardware playback buffer
 * (in 125 kSa/s IQ frames -- the SX1255's actual output rate, not the
 * 24 kSa/s baseband rate), used to pace the TX playback thread's
 * draining loop. ~50ms: enough cushion to absorb normal OS scheduling
 * jitter without stalling, small enough that this bridge's own write
 * rate stays tightly coupled to the hardware's true, physical
 * consumption rate. Pacing against the hardware's own buffer state
 * (rather than an assumed sample rate via CLOCK_MONOTONIC) avoids
 * accumulating drift from any small mismatch between the host CPU's
 * clock and the SX1255's own separate oscillator. */
#define TX_LOOKAHEAD_TARGET_FRAMES (125000 / 1000 * 50)

/* ---- Configuration ---------------------------------------------------*/
typedef struct
{
    const char *spi_path;
    const char *gpio_chip;
    unsigned int gpio_reset_pin;
    uint32_t rx_freq_hz;
    uint32_t tx_freq_hz;
    uint8_t lna_gain_db;
    uint8_t pga_gain_db;
    float mixer_gain_db;
    int8_t dac_gain_db;
    uint16_t rx_pll_bw_khz;
    uint16_t tx_pll_bw_khz;
    const char *alsa_capture_dev;
    const char *alsa_playback_dev;
    double deviation_hz;
    atomic_bool rx_invert; /* negate the baseband sign before Q15 conversion --
                      * try this if a protocol service's own decoder
                      * consistently finds sync but fails FEC/CRC
                      * validation on every frame despite --rx-gain
                      * adjustments; a genuine sign-convention mismatch
                      * is a separate, independent possibility from the
                      * overflow issue --rx-gain addresses. */
    float rx_gain; /* extra scale factor applied to baseband before Q15
                    * conversion, on top of the DSP chain's own
                    * scalingCoeff. */
    const char *pty_symlink; /* optional stable symlink (e.g.
                               * /dev/sx1255modem) to the PTY slave path,
                               * so Modem_Host.c's config doesn't need
                               * updating every run -- the real /dev/pts/N
                               * path can change run to run. */
    float tx_symbol_scale; /* divisor for incoming Q15 TX samples. */
    atomic_bool tx_invert; /* negate incoming baseband sign before it's fed to
                      * the FM modulator. RX and TX are separate,
                      * independent DSP chains sharing the same
                      * hardware, so a sign-convention mismatch found on
                      * one side (see rx_invert) doesn't imply anything
                      * about the other -- but it's exactly the same
                      * class of issue, and just as plausible
                      * independently: e.g. how the SX1255's own I/Q
                      * convention relates to "positive frequency" could
                      * differ between its RX and TX paths, or between
                      * this bridge's FM demodulator and modulator sign
                      * conventions. */
    unsigned int tx_prime_ms; /* the TX playback thread waits until at
                                * least this many milliseconds' worth of
                                * audio is queued before it starts
                                * writing anything to ALSA, then drains
                                * continuously until the queue runs
                                * fully dry, at which point it goes back
                                * to waiting rather than immediately
                                * resuming on whatever arrives next.
                                * Upstream delivery can be bursty rather
                                * than a smooth, steady stream; draining
                                * as fast as data arrives, with no
                                * accumulated cushion, means any gap in
                                * delivery immediately starves ALSA and
                                * causes a playback underrun -- a hard
                                * discontinuity in the transmitted
                                * waveform each time, regardless of
                                * amplitude/scaling correctness
                                * elsewhere. Trades a fixed amount of
                                * latency for robustness against uneven
                                * delivery; 0 disables priming entirely
                                * (drain immediately). */
    unsigned int tx_key_timeout_ms; /* de-key automatically once this many
                                       * milliseconds have passed with no
                                       * TX sample data arriving at all --
                                       * this bridge's primary de-key
                                       * mechanism (see tx_state_t's
                                       * last_tx_sample_ms comment for the
                                       * reasoning). Key-up is symmetric:
                                       * the moment TX samples arrive
                                       * while not currently keyed, key up
                                       * immediately, rather than waiting
                                       * for any protocol message. Should
                                       * comfortably exceed tx_prime_ms
                                       * and whatever normal delivery gaps
                                       * look like in practice -- too
                                       * short falsely de-keys mid-
                                       * transmission on an ordinary
                                       * bursty gap; too long delays
                                       * ending a transmission that has
                                       * genuinely finished. SET_MODE and
                                       * OPENMT_SAMP_EOT are still handled
                                       * (see handle_frame()) as an
                                       * additional, faster de-key signal
                                       * when they do arrive correctly,
                                       * not as something this timeout
                                       * depends on. */
    unsigned int tx_key_settle_ms; /* delay after enabling TX mode,
                                      * before the PLL-lock wait and
                                      * before any real audio is
                                      * allowed to flow. */
    unsigned int tx_stream_delay_ms; /* delay at the boundary between
                                        * "TX hardware fully configured
                                        * and ready" (PLL locked, DSP
                                        * reset, ALSA prepared) and "the
                                        * playback thread is allowed to
                                        * actually start streaming."
                                        * Off (0) by default. */
} bridge_config_t;

static volatile sig_atomic_t g_stop = 0;

/*
 * handle_signal()
 *
 * Signal handler for SIGINT and SIGTERM; sets the global stop flag so the
 * loops and threads exit.
 *
 * Parameters:
 *   sig - Signal number (not used).
 *
 * Returns: nothing.
 */
static void
handle_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/*
 * config_set_defaults()
 *
 * Fills the bridge configuration with its default values (SPI and GPIO
 * devices, 434 MHz, gains, PLL bandwidths, ALSA devices, deviation and the
 * TX/RX timing and level settings).
 *
 * Parameters:
 *   cfg - Configuration to fill in.
 *
 * Returns: nothing.
 */
static void
config_set_defaults(bridge_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->spi_path = "/dev/spidev0.0";
    cfg->gpio_chip = "/dev/gpiochip0"; /* no GPIO reset attempted by default -- the
                              * reset line can already be claimed/busy
                              * via the legacy /sys/class/gpio sysfs
                              * interface on some setups. Pass
                              * --gpio-chip explicitly once the reset
                              * line is confirmed free. */
    cfg->gpio_reset_pin = 25;
    cfg->rx_freq_hz = 434000000;
    cfg->tx_freq_hz = 434000000;
    cfg->lna_gain_db = 24;
    cfg->pga_gain_db = 0;
    cfg->mixer_gain_db = -15.0f;
    cfg->dac_gain_db = 0;
    cfg->rx_pll_bw_khz = 300;
    cfg->tx_pll_bw_khz = 300;
    cfg->alsa_capture_dev = "hw:0,1";
    cfg->alsa_playback_dev = "hw:0,0";
    /* FM deviation. Retune from a fresh capture+decode measurement if
     * anything about the DSP chain upstream of this value changes. */
    cfg->deviation_hz = 950.0;
    cfg->pty_symlink = "/dev/sx1255";
    cfg->tx_symbol_scale = 494.0f;
    atomic_store(&cfg->tx_invert, true); /* see the field comment above */
    cfg->tx_prime_ms = 250; /* see the field comment above */
    cfg->tx_key_timeout_ms = 1000; /* see the field comment above --
                                      * comfortably longer than
                                      * tx_prime_ms and normal delivery
                                      * gaps */
    cfg->tx_key_settle_ms = 60;
    cfg->tx_stream_delay_ms = 0;
    atomic_store(&cfg->rx_invert, true); /* see the field comment above. Revert to
                             * false if a different protocol-service
                             * decoder, or a different SX1255 wiring/
                             * hardware setup, has the opposite
                             * convention. */
    cfg->rx_gain = 0.04f; /* extra scale factor on baseband before Q15
                            * conversion -- see this project's README
                            * for why this needs to be well below 1.0:
                            * a downstream decoder's own fixed-point
                            * RRC/DC-filter stages may use non-
                            * saturating arithmetic that wraps on
                            * overflow rather than clipping. Tuned for
                            * one specific decoder and may need
                            * adjusting for a different one. */
}

/*
 * print_usage()
 *
 * Prints the command line help text through log_msg().
 *
 * Parameters:
 *   argv0 - Program name to show in the usage line.
 *
 * Returns: nothing.
 */
static void
print_usage(const char *argv0)
{
    log_msg(
        "Usage: %s [options]\n"
        "  --spi PATH            SPI device (default /dev/spidev0.0)\n"
        "  --gpio-chip PATH      GPIO chip for hardware reset (default: none --\n"
        "                        this bridge does not reset the SX1255 on its own\n"
        "                        by default; pass this explicitly if you know the\n"
        "                        reset line is free on your setup. Empty string\n"
        "                        is equivalent to omitting this flag.\n"
        "  --gpio-reset-pin N    GPIO line for reset (default 25)\n"
        "  --rx-freq HZ          RX frequency (default 434000000)\n"
        "  --tx-freq HZ          TX frequency (default 434000000)\n"
        "  --lna-gain DB         0/12/24/36/42/48 (default 24)\n"
        "  --pga-gain DB         0-30, 2dB steps (default 0)\n"
        "  --mixer-gain DB       -37.5..-7.5, 2dB steps (default -15)\n"
        "  --dac-gain DB         0/-3/-6/-9 (default 0)\n"
        "  --rx-pll-bw KHZ       75/150/225/300 (default 300)\n"
        "  --tx-pll-bw KHZ       75/150/225/300 (default 300)\n"
        "  --capture DEV         ALSA capture device (default hw:sx1255)\n"
        "  --playback DEV        ALSA playback device (default hw:sx1255)\n"
        "  --pty-symlink PATH    create/update a stable symlink to the PTY\n"
        "                        slave device at PATH (e.g. /dev/sx1255modem)\n"
        "                        so OpenMT's modemtty config doesn't need\n"
        "                        updating every run\n"
        "  --rx-invert           negate the baseband sign before Q15 conversion\n"
        "                        (this is the default -- see --no-rx-invert).\n"
        "                        Sync correlation can tolerate a polarity flip\n"
        "                        in ways Golay/Viterbi-coded payload bits\n"
        "                        generally cannot, so a different decoder or a\n"
        "                        different SX1255 wiring/hardware setup may\n"
        "                        need the opposite.\n"
        "  --no-rx-invert        disable the above (use if a different decoder\n"
        "                        needs the opposite polarity convention)\n"
        "  --rx-gain MULT        extra scale factor on baseband before Q15\n"
        "                        conversion (default 0.04 -- see this project's\n"
        "                        README for why this needs to be so low: at\n"
        "                        least one downstream decoder's own fixed-point\n"
        "                        RRC/DC-filter stages use non-saturating\n"
        "                        arithmetic that silently wraps on overflow\n"
        "                        rather than clipping). If sync is found\n"
        "                        consistently but every frame still fails\n"
        "                        FEC/CRC, try reducing this further.\n"
        "  --tx-symbol-scale N   divisor for incoming Q15 TX samples.\n"
        "  --deviation-hz N      FM deviation, in Hz.\n"
        "                        transmitted deviation.\n"
        "  --tx-invert           negate incoming baseband sign before the FM\n"
        "                        modulator (default ON). RX and TX are separate\n"
        "                        DSP chains sharing the same hardware, so this\n"
        "                        sign convention is independent of --rx-invert.\n"
        "  --no-tx-invert        disable the above -- only useful for testing\n"
        "                        on different hardware where this convention\n"
        "                        might differ.\n"
        "  --tx-prime-ms N       milliseconds of audio to accumulate in the TX\n"
        "                        queue before the playback thread starts writing\n"
        "                        anything to ALSA (default 250), re-priming from\n"
        "                        scratch if the queue ever runs fully dry. Upstream\n"
        "                        delivery can be bursty rather than a smooth\n"
        "                        stream; increase this if ALSA playback underruns\n"
        "                        persist (each one is a hard discontinuity in the\n"
        "                        transmitted waveform), at the cost of added\n"
        "                        latency. 0 disables priming (drain immediately).\n"
        "  --tx-key-timeout-ms N de-key automatically after this many milliseconds\n"
        "                        of no TX sample data arriving at all (default\n"
        "                        1000). Presence of TX samples is this bridge's\n"
        "                        primary key-up/de-key signal, not SET_MODE/\n"
        "                        OPENMT_SAMP_EOT -- key up the instant samples\n"
        "                        arrive while unkeyed, de-key on this timeout.\n"
        "                        Should comfortably exceed --tx-prime-ms and\n"
        "                        normal delivery gaps; too short falsely de-keys\n"
        "                        mid-transmission, too long delays ending one\n"
        "                        that has actually finished.\n"
        "  --tx-key-settle-ms N  delay after enabling TX mode, before the PLL-lock\n"
        "                        wait and before any real audio flows (default\n"
        "                        60).\n"
        "  --tx-stream-delay-ms N  delay at the boundary between \"TX hardware\n"
        "                        fully ready\" (after PLL lock, DSP reset, ALSA\n"
        "                        prepare) and \"the playback thread is allowed to\n"
        "                        actually start streaming\" (default 0/off).\n"
        "  -h, --help            show this help\n"
        "\n"
        "The values above are defaults. If OpenMT is configured with\n"
        "modem=OPENMTSX (a small host-side patch -- see openmt-openmtsx-patch/),\n"
        "a SET_CONFIG packet received at startup overrides RX/TX frequency, all\n"
        "gain/PLL-bandwidth values, and --rx-invert/--tx-invert, applying them\n"
        "directly to the hardware (and, for the invert flags, to live sample\n"
        "processing immediately).\n",
        argv0);
}

#define NEXT_ARG() (i + 1 < argc ? argv[++i] : NULL)

/*
 * parse_args()
 *
 * Sets the defaults, then reads the command line options into the
 * configuration.  -h or --help prints the usage and exits the program.
 *
 * Parameters:
 *   argc - Argument count.
 *   argv - Argument vector.
 *   cfg  - Configuration to fill in.
 *
 * Returns: true if all arguments were understood, false after an unknown or
 *          incomplete option (usage is printed).
 */
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
        else if (strcmp(argv[i], "--spi") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->spi_path = val;
        }
        else if (strcmp(argv[i], "--gpio-chip") == 0 && (val = NEXT_ARG()) != NULL)
        {
            /* Empty string explicitly disables the GPIO reset (distinct
             * from just not passing this flag at all, which now
             * defaults to performing it) -- sx1255_open() checks for
             * NULL specifically, not empty string, so this has to be
             * translated here for that opt-out to actually work. */
            cfg->gpio_chip = (val[0] == '\0') ? NULL : val;
        }
        else if (strcmp(argv[i], "--gpio-reset-pin") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->gpio_reset_pin = (unsigned int)atoi(val);
        }
        else if (strcmp(argv[i], "--rx-freq") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->rx_freq_hz = (uint32_t)strtoul(val, NULL, 10);
        }
        else if (strcmp(argv[i], "--tx-freq") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->tx_freq_hz = (uint32_t)strtoul(val, NULL, 10);
        }
        else if (strcmp(argv[i], "--lna-gain") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->lna_gain_db = (uint8_t)atoi(val);
        }
        else if (strcmp(argv[i], "--pga-gain") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->pga_gain_db = (uint8_t)atoi(val);
        }
        else if (strcmp(argv[i], "--mixer-gain") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->mixer_gain_db = (float)atof(val);
        }
        else if (strcmp(argv[i], "--dac-gain") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->dac_gain_db = (int8_t)atoi(val);
        }
        else if (strcmp(argv[i], "--rx-pll-bw") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->rx_pll_bw_khz = (uint16_t)atoi(val);
        }
        else if (strcmp(argv[i], "--tx-pll-bw") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->tx_pll_bw_khz = (uint16_t)atoi(val);
        }
        else if (strcmp(argv[i], "--capture") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->alsa_capture_dev = val;
        }
        else if (strcmp(argv[i], "--playback") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->alsa_playback_dev = val;
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
        else if (strcmp(argv[i], "--tx-invert") == 0)
        {
            atomic_store(&cfg->tx_invert, true);
        }
        else if (strcmp(argv[i], "--no-tx-invert") == 0)
        {
            atomic_store(&cfg->tx_invert, false);
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
        else if (strcmp(argv[i], "--tx-stream-delay-ms") == 0 && (val = NEXT_ARG()) != NULL)
        {
            cfg->tx_stream_delay_ms = (unsigned int)atoi(val);
        }
        else
        {
            log_msg( "Unknown or incomplete option: %s\n", argv[i]);
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

/* ---- PTY setup --------------------------------------------------------*/

/* Returns the master fd (used for all subsequent read()/write() calls in
 * this program), and fills slave_path with the slave device path (e.g.
 * "/dev/pts/4") -- point OpenMT's modemtty config at that path, or at
 * --pty-symlink's path if one was requested. */
/*
 * open_bridge_pty()
 *
 * Creates the pseudo terminal pair the host connects to (the Modem_Host
 * modemtty setting), puts both ends in raw mode and keeps the slave end open
 * for the life of the program.
 *
 * Parameters:
 *   slave_path     - Receives the slave device path, for example /dev/pts/4.
 *   slave_path_cap - Size of slave_path in bytes.
 *
 * Returns: The master file descriptor, or -1 if the pty could not be created.
 */
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

/*
 * write_frame()
 *
 * Sends one OpenMT frame ([0x61][total length][type and connection][payload])
 * to the host.  The write waits at most 100 ms for the pty to accept data; if
 * nothing is reading, the frame is dropped so the RX thread and shutdown
 * cannot block.
 *
 * Parameters:
 *   fd            - Master side of the pty.
 *   type_and_conn - Frame type in the high nibble, connection or mode in the
 *                   low nibble.
 *   payload       - Frame payload, may be NULL when payload_len is 0.
 *   payload_len   - Payload length in bytes (at most 252).
 *
 * Returns: true if the whole frame was written, false if it was too large,
 *          the pty was not ready or the write was short.
 */
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
/*
 * read_frame()
 *
 * Reads one OpenMT frame from the host.  It waits up to 500 ms for data,
 * skips bytes until a 0x61 start byte, then reads the rest of the frame with
 * blocking reads so the stream cannot lose sync mid-frame.  If the payload is
 * larger than the caller's buffer it is truncated to the buffer size.
 *
 * Parameters:
 *   fd            - Master side of the pty.
 *   type_and_conn - Receives the type (high nibble) and connection or mode
 *                   (low nibble).
 *   payload       - Receives the payload.
 *   payload_len   - On entry the size of payload; on return the payload
 *                   length.
 *
 * Returns: true if a frame was read, false on timeout, read error, end of
 *          file or an invalid length.
 */
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
 * (which reads incoming PTY frames) from actually writing samples to
 * ALSA (which can block on the hardware's real-time pace). Without
 * this, a blocking alsa_playback_write() call inside the main loop
 * would delay reading the *next* incoming frame -- letting data queue
 * up unread in the PTY's own kernel buffer, then get processed in a
 * burst once the loop becomes free again, causing ALSA playback
 * underruns in between. Sized for ~2 seconds of audio at 24kSa/s, 2
 * bytes/sample -- generous headroom against bursts at the cost of up
 * to that much latency, which is an acceptable trade for a repeater
 * against otherwise-broken audio. */
#define TX_QUEUE_CAP (24000 * 2 * 2)

typedef struct
{
    uint8_t buf[TX_QUEUE_CAP];
    size_t head; /* next byte to read */
    size_t tail; /* next byte to write */
    size_t count;
    pthread_mutex_t mutex;
} tx_queue_t;

/*
 * tx_queue_init()
 *
 * Empties the TX sample queue and initialises its mutex.
 *
 * Parameters:
 *   q - Queue to initialise.
 *
 * Returns: nothing.
 */
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
/*
 * tx_queue_clear()
 *
 * Throws away everything in the queue.  Called at every de-key so old data
 * from the last transmission is not played at the start of the next one.
 *
 * Parameters:
 *   q - Queue to empty.
 *
 * Returns: nothing.
 */
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
/*
 * tx_queue_push()
 *
 * Adds bytes to the queue without ever blocking; if there is not enough room,
 * nothing is added.
 *
 * Parameters:
 *   q    - Queue.
 *   data - Bytes to add.
 *   len  - Number of bytes.
 *
 * Returns: true if the data was queued, false if the queue was too full and
 *          the data was dropped.
 */
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
/*
 * tx_queue_pop()
 *
 * Removes up to max_len bytes from the queue.
 *
 * Parameters:
 *   q       - Queue.
 *   out     - Receives the bytes.
 *   max_len - Size of out in bytes.
 *
 * Returns: Number of bytes removed (0 if the queue was empty).
 */
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
/*
 * tx_queue_depth()
 *
 * Gets the number of bytes waiting in the queue.
 *
 * Parameters:
 *   q - Queue.
 *
 * Returns: Queue depth in bytes.
 */
static size_t
tx_queue_depth(tx_queue_t *q)
{
    size_t n;

    pthread_mutex_lock(&q->mutex);
    n = q->count;
    pthread_mutex_unlock(&q->mutex);
    return n;
}

/* ---- Shared state between the RX thread and the main (command/TX)
 * thread ------------------------------------------------------------- */
typedef struct
{
    sx1255_t *sx;
    dsp_tx_pipeline_t *tx_dsp;
    alsa_playback_t *pb;
    bridge_config_t *cfg; /* not const: a live SET_CONFIG packet from
                            * the host (see handle_frame()'s
                            * OPENMT_SET_CONFIG case) can override
                            * these fields after startup, so this must
                            * be mutable through this pointer. */
    atomic_bool keyed;
    tx_queue_t queue;
    /* Milliseconds since an arbitrary but consistent CLOCK_MONOTONIC
     * reference, updated every time a TX sample message arrives.
     * Presence/absence of TX sample data is this bridge's primary
     * key-up/de-key signal (see tx_handle_samples() and the playback
     * thread's timeout check) rather than SET_MODE/OPENMT_SAMP_EOT --
     * a deliberate design choice made directly by OpenMT's author,
     * after several rounds of state-tracking bugs on the protocol-
     * message side (frame-number underflow, EOT branches not
     * resetting all the state they should, etc.) that this sidesteps
     * entirely: data presence is tied directly to reality, not to a
     * chain of variables that all have to stay in sync with it. */
    atomic_llong last_tx_sample_ms;
    pthread_mutex_t pb_mutex; /* Protects every call touching pb (the
                                * underlying snd_pcm_t handle) --
                                * alsa_playback_write/drain/prepare/
                                * delay_frames -- since ALSA's snd_pcm_t
                                * is not safe for unsynchronized
                                * concurrent access from multiple
                                * threads. Became necessary once
                                * tx_key_down() started calling
                                * alsa_playback_drain() directly: that
                                * function can run on the main thread
                                * (handling SAMP_EOT) at the same moment
                                * the dedicated TX playback thread is
                                * mid-call to alsa_playback_write() on
                                * the very same handle -- a real, not
                                * theoretical, race without this. */
} tx_state_t;

/*
 * monotonic_ms()
 *
 * Reads the monotonic clock.
 *
 * Parameters: none.
 *
 * Returns: Milliseconds since an arbitrary fixed point.
 */
static long long
monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One-time startup reset-and-configure sequence: GPIO reset (when a
 * GPIO chip is configured), full register configuration, RX enabled,
 * TX explicitly disabled, and an RX PLL lock check (with one retry)
 * plus a stabilization wait before returning -- the SX1255 is the I2S
 * bus master, and the BCM2835's I2S peripheral (slave) needs a stable
 * BCLK/WS before it can synchronize onto it, so this treats a
 * genuinely locked, stable RX PLL as a hard precondition before ALSA
 * is ever opened. */
/*
 * sx1255_startup_configure()
 *
 * Resets and configures the SX1255 from the configuration: frequencies,
 * gains, PLL bandwidths, RX on and TX off.  The RX PLL must lock within 500
 * ms; if not, the whole sequence is tried once more.  It finishes with a 500
 * ms wait for the I2S clock to settle, since the host I2S port needs a stable
 * clock before ALSA is opened.
 *
 * Parameters:
 *   sx  - Open SX1255 device.
 *   cfg - Configuration to apply.
 *
 * Returns: true on success, false if a setting failed or the RX PLL did not
 *          lock after the retry.
 */
static bool
sx1255_startup_configure(sx1255_t *sx, const bridge_config_t *cfg)
{
    if (!sx1255_init(sx))
    {
        return false;
    }
    if (!sx1255_set_rate(sx) ||
        !sx1255_set_rx_freq(sx, cfg->rx_freq_hz) ||
        !sx1255_set_tx_freq(sx, cfg->tx_freq_hz) ||
        !sx1255_set_lna_gain(sx, cfg->lna_gain_db) ||
        !sx1255_set_pga_gain(sx, cfg->pga_gain_db) ||
        !sx1255_set_mixer_gain(sx, cfg->mixer_gain_db) ||
        !sx1255_set_dac_gain(sx, cfg->dac_gain_db) ||
        !sx1255_set_rx_pll_bw(sx, cfg->rx_pll_bw_khz) ||
        !sx1255_set_tx_pll_bw(sx, cfg->tx_pll_bw_khz))
    {
        return false;
    }
    if (!sx1255_enable_rx(sx, true))
    {
        return false;
    }
    sx1255_enable_tx(sx, false);
    if (!sx1255_wait_pll_lock(sx, false, 500))
    {
        log_msg("bridge: WARNING -- RX PLL not locked after 500ms, retrying reset and "
            "configuration once\n");
        if (!sx1255_init(sx) ||
            !sx1255_set_rate(sx) ||
            !sx1255_set_rx_freq(sx, cfg->rx_freq_hz) ||
            !sx1255_set_tx_freq(sx, cfg->tx_freq_hz) ||
            !sx1255_set_lna_gain(sx, cfg->lna_gain_db) ||
            !sx1255_set_pga_gain(sx, cfg->pga_gain_db) ||
            !sx1255_set_mixer_gain(sx, cfg->mixer_gain_db) ||
            !sx1255_set_dac_gain(sx, cfg->dac_gain_db) ||
            !sx1255_set_rx_pll_bw(sx, cfg->rx_pll_bw_khz) ||
            !sx1255_set_tx_pll_bw(sx, cfg->tx_pll_bw_khz) ||
            !sx1255_enable_rx(sx, true))
        {
            return false;
        }
        sx1255_enable_tx(sx, false);
        if (!sx1255_wait_pll_lock(sx, false, 500))
        {
            log_msg("bridge: ERROR -- RX PLL failed to lock after retry: chip is "
                "producing no I2S clock -- check the reference oscillator and supply\n");
            return false;
        }
    }
    log_msg("bridge: waiting 500ms for I2S clock to stabilize...\n");
    usleep(500 * 1000);
    return true;
}

/*
 * tx_key_up()
 *
 * Keys the transmitter: refreshes the TX frequency, switches TX on (briefly
 * toggled off and on again), waits the settle time, waits up to 100 ms for
 * the TX PLL, resets the DSP and prepares ALSA, then marks the state as
 * keyed.  Does nothing if already keyed.
 *
 * Parameters:
 *   t - TX state.
 *
 * Returns: true if keyed (or already keyed), false if TX could not be
 *          enabled.
 */
static bool
tx_key_up(tx_state_t *t)
{
    if (atomic_load(&t->keyed))
    {
        return true;
    }
    /* Reset the de-key timeout clock right here, at the moment of
     * key-up itself -- not just when real sample data arrives (the
     * only other place this is updated). This gives a genuine, fresh
     * --tx-key-timeout-ms grace period for real data to arrive,
     * regardless of how long it's been since the last real sample. */
    atomic_store(&t->last_tx_sample_ms, monotonic_ms());
    /* Re-write the TX frequency register immediately before enabling
     * TX, even though it's always the same, already-configured value --
     * forces the PLL to fully reacquire lock from scratch on every
     * key-up, rather than relying on whatever residual state it was
     * left in from the previous cycle. */
    if (!sx1255_set_tx_freq(t->sx, t->cfg->tx_freq_hz))
    {
        log_msg( "bridge: WARNING -- failed to re-set TX frequency before key-up\n");
    }
    if (!sx1255_enable_tx(t->sx, true))
    {
        log_msg( "bridge: failed to key TX\n");
        return false;
    }
    /* Enable, brief settle, disable, brief settle, enable again before
     * the normal settle+PLL-wait sequence below proceeds. */
    usleep(20 * 1000);
    sx1255_enable_tx(t->sx, false);
    usleep(20 * 1000);
    if (!sx1255_enable_tx(t->sx, true))
    {
        log_msg( "bridge: failed to key TX (second half of double-toggle)\n");
        return false;
    }
    /* Settling delay + PLL wait: enabling TX mode is an SPI register
     * write that can momentarily disrupt the SX1255's I2S clock, which
     * both capture and playback share, and PLL lock can lag key-up on
     * a marginal supply. */
    usleep(t->cfg->tx_key_settle_ms * 1000);
    if (!sx1255_wait_pll_lock(t->sx, true, 100))
    {
        log_msg( "bridge: WARNING -- TX PLL not locked after keying TX and waiting "
            "100ms\n");
    }
    dsp_tx_pipeline_reset(t->tx_dsp);
    /* Reset the ALSA device's own internal state too, not just this
     * bridge's software queue and DSP state above -- a device that
     * underran during the idle gap since the last transmission may
     * otherwise carry that condition into this new transmission,
     * only getting caught reactively mid-write via
     * alsa_playback_write()'s own recovery, rather than starting
     * genuinely clean. */
    pthread_mutex_lock(&t->pb_mutex);
    alsa_playback_prepare(t->pb);
    pthread_mutex_unlock(&t->pb_mutex);
    /* Optional delay at the boundary between "TX hardware fully
     * configured and ready" and "the playback thread is allowed to
     * actually start streaming" (the atomic_store below). */
    if (t->cfg->tx_stream_delay_ms > 0)
    {
        usleep(t->cfg->tx_stream_delay_ms * 1000);
    }
    atomic_store(&t->keyed, true);
    log_msg( "bridge: TX keyed\n");
    return true;
}

/*
 * tx_key_down()
 *
 * Un-keys the transmitter: waits for queued audio to finish playing, switches
 * TX off, marks the state as not keyed and clears the sample queue.  Does
 * nothing if not keyed.
 *
 * Parameters:
 *   t - TX state.
 *
 * Returns: nothing.
 */
static void
tx_key_down(tx_state_t *t)
{
    if (!atomic_load(&t->keyed))
    {
        return;
    }
    /* Wait for all already-queued audio to actually finish playing
     * through the hardware before disabling TX, so the last ~50ms of
     * lookahead this bridge deliberately keeps buffered in ALSA's
     * hardware queue (see TX_LOOKAHEAD_TARGET_FRAMES) actually reaches
     * the antenna before TX is disabled, rather than being cut off. */
    pthread_mutex_lock(&t->pb_mutex);
    alsa_playback_drain(t->pb);
    pthread_mutex_unlock(&t->pb_mutex);
    sx1255_enable_tx(t->sx, false);
    atomic_store(&t->keyed, false);
    /* Drop any leftover queued data now, before the next transmission's
     * new samples can get appended after it in the FIFO -- see
     * tx_queue_clear()'s comment for why this is necessary, not just
     * defensive. */
    tx_queue_clear(&t->queue);
    log_msg( "bridge: TX unkeyed\n");
}

/* Enqueues incoming Q15 samples (already RRC/GMSK pulse-shaped by
 * whichever protocol service sent them) for the dedicated TX playback
 * thread to process. Deliberately does no conversion, DSP, or ALSA
 * I/O here -- this must return quickly so the main command-processing
 * loop can keep reading incoming PTY frames promptly regardless of
 * ALSA's own real-time pacing.
 *
 * Keys up immediately if not already keyed -- presence of TX sample
 * data is this bridge's primary key-up signal (see tx_state_t's
 * last_tx_sample_ms comment for why), not SET_MODE. De-key is handled
 * symmetrically by the playback thread's timeout check, plus
 * SET_MODE/OPENMT_SAMP_EOT in handle_frame() as an additional, faster
 * signal when either arrives correctly. */
/*
 * tx_handle_samples()
 *
 * Handles TX samples from the host: records the arrival time, keys up if
 * needed and queues the samples for the playback thread.  It does no DSP or
 * audio output itself so the command loop stays responsive.
 *
 * Parameters:
 *   t           - TX state.
 *   payload     - 16-bit samples (Q15, native byte order).
 *   payload_len - Length of payload in bytes.
 *
 * Returns: nothing.
 */
static void
tx_handle_samples(tx_state_t *t, const uint8_t *payload, uint8_t payload_len)
{
    atomic_store(&t->last_tx_sample_ms, monotonic_ms());
    if (!atomic_load(&t->keyed))
    {
        tx_key_up(t);
    }
    tx_queue_push(&t->queue, payload, payload_len);
}

/* ---- TX playback thread: drains the queue above at whatever pace
 * ALSA's own real-time playback naturally allows, converting to float,
 * running the generic TX DSP back end (resampler + FM modulator only --
 * no pulse shaping here, that's already been done upstream), and
 * writing the resulting IQ out. Runs independently of the main
 * command-processing loop specifically so a blocking ALSA write here
 * never delays reading the next incoming PTY frame there. */
typedef struct
{
    tx_state_t *tx_state;
} tx_playback_thread_arg_t;

/*
 * tx_playback_thread_main()
 *
 * Thread that plays the TX queue.  It un-keys after tx_key_timeout_ms without
 * samples, waits for the queue to prime before starting, paces writes against
 * the ALSA hardware queue depth, converts Q15 samples to floats (scale and
 * optional inversion), runs the TX DSP and writes the I/Q to ALSA.  It runs
 * until the stop flag is set.
 *
 * Parameters:
 *   arg - Pointer to a tx_playback_thread_arg_t.
 *
 * Returns: NULL.
 */
static void *
tx_playback_thread_main(void *arg)
{
    tx_playback_thread_arg_t *a = arg;
    tx_state_t *t = a->tx_state;
    uint8_t raw[SAMPLES_PER_FRAME * 2];
    float samples[SAMPLES_PER_FRAME];
    dsp_cplx_t *iq_buf = NULL;
    int iq_cap = 0;
    /* Prime/drain state machine: wait for at least prime_threshold_bytes
     * to accumulate before writing anything, then drain continuously
     * until the queue runs fully dry, at which point go back to
     * waiting rather than resuming immediately on whatever arrives
     * next. */
    bool priming = true;
    size_t prime_threshold_bytes = (size_t)((double)t->cfg->tx_prime_ms / 1000.0 * 24000.0 * 2.0);

    log_msg( "bridge: TX playback thread running (priming threshold: %zu bytes, "
        "~%ums)\n", prime_threshold_bytes, t->cfg->tx_prime_ms);
    while (!g_stop)
    {
        size_t n_bytes;
        int n_samples, n_iq, i;

        /* De-key timeout: presence of TX sample data is this bridge's
         * primary key-up signal (see tx_handle_samples()); this is the
         * symmetric de-key half. Checked every iteration regardless of
         * priming state, so a timeout is caught promptly even while
         * still waiting to re-prime after a gap. */
        if (atomic_load(&t->keyed) &&
            monotonic_ms() - atomic_load(&t->last_tx_sample_ms) > t->cfg->tx_key_timeout_ms)
        {
            log_msg( "bridge: [tx] no samples for %ums, unkeying\n",
                t->cfg->tx_key_timeout_ms);
            tx_key_down(t);
            priming = true;
        }

        if (priming && prime_threshold_bytes > 0)
        {
            if (tx_queue_depth(&t->queue) < prime_threshold_bytes)
            {
                /* Deliberately just wait here, writing nothing to
                 * ALSA -- not silence-filling. Silence-filling during
                 * this wait (paced or unpaced) makes decode quality
                 * worse, not better: a receiving radio expects symbols
                 * at a precise, continuous baud rate, and any inserted
                 * samples during a pause stretch the real symbol
                 * stream out in time relative to what the transmitting
                 * side intended, corrupting the receiver's frame/
                 * symbol timing regardless of how clean the IQ signal
                 * itself stays. An occasional real underrun, isolated
                 * to one bad moment, is less disruptive than reliably
                 * inserting extra time into every re-prime event. If
                 * underruns are a persistent
                 * problem in practice, increasing --tx-prime-ms (more
                 * cushion before draining starts, so re-priming
                 * happens less often in the first place) is the
                 * better lever to pull -- not trying to paper over a
                 * gap once it's already happening. */
                usleep(1000);
                continue;
            }
            priming = false;
            log_msg( "bridge: [tx-playback] primed, starting playback\n");
        }

        /* Pace this batch against ALSA's own hardware buffer state,
         * not an independent software clock -- see the
         * TX_LOOKAHEAD_TARGET_FRAMES comment below for why. If enough
         * audio is already buffered ahead in hardware, wait briefly
         * rather than writing more; the hardware's own consumption
         * (this delay figure decreasing over time) naturally gates the
         * real write rate, with no assumption about what that rate
         * "should" be. */
        {
            long delay_frames;

            pthread_mutex_lock(&t->pb_mutex);
            delay_frames = alsa_playback_delay_frames(t->pb);
            pthread_mutex_unlock(&t->pb_mutex);

            if (delay_frames >= 0 && delay_frames > TX_LOOKAHEAD_TARGET_FRAMES)
            {
                usleep(1000);
                continue;
            }
        }

        n_bytes = tx_queue_pop(&t->queue, raw, sizeof(raw));
        if (n_bytes == 0)
        {
            /* Queue ran fully dry -- rather than resume immediately on
             * whatever trickles in next (which is exactly the
             * behavior that let a single ~192-byte message get
             * written the instant it arrived, with no accumulated
             * cushion against the next gap), go back to priming from
             * scratch. */
            if (prime_threshold_bytes > 0)
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
            samples[i] = (float)q15 / t->cfg->tx_symbol_scale;
            if (atomic_load(&t->cfg->tx_invert))
            {
                samples[i] = -samples[i];
            }
        }
        n_iq = dsp_tx_pipeline_process_baseband(t->tx_dsp, samples, n_samples,
            t->cfg->deviation_hz, &iq_buf, &iq_cap);
        if (n_iq > 0)
        {
            pthread_mutex_lock(&t->pb_mutex);
            alsa_playback_write(t->pb, iq_buf, n_iq);
            pthread_mutex_unlock(&t->pb_mutex);
        }
    }
    free(iq_buf);
    log_msg( "bridge: TX playback thread exiting\n");
    return NULL;
}

/* ---- RX thread: SX1255 capture -> generic DSP front end -> Q15 ->
 * SAMP_DATA frames out to the host ------------------------------------ */
typedef struct
{
    alsa_capture_t *cap;
    dsp_rx_pipeline_t *rx_dsp;
    int pty_fd;
    const bridge_config_t *cfg; /* read directly, not copied at thread
                                  * startup, so a live SET_CONFIG
                                  * override (see apply_openmtsx_config())
                                  * takes effect on the very next sample
                                  * processed, not just at the next
                                  * restart. */
} rx_thread_arg_t;

/*
 * rx_thread_main()
 *
 * Thread that handles receive: reads I/Q from ALSA, runs the RX DSP, applies
 * inversion, gain and clipping, converts to Q15 and sends 100-sample
 * SAMP_DATA frames to the host.  It runs until the stop flag is set or a
 * capture error occurs.
 *
 * Parameters:
 *   arg - Pointer to an rx_thread_arg_t.
 *
 * Returns: NULL.
 */
static void *
rx_thread_main(void *arg)
{
    rx_thread_arg_t *a = arg;
    uint8_t q15_payload[SAMPLES_PER_FRAME * 2];
    int q15_count = 0;

    log_msg( "bridge: RX thread running\n");
    while (!g_stop)
    {
        alsa_cplx_t iq_buf[256];
        int n_iq = alsa_capture_read(a->cap, iq_buf, 256);
        int i;

        if (n_iq < 0)
        {
            log_msg( "bridge: capture error, stopping RX thread\n");
            break;
        }

        for (i = 0; i < n_iq; i++)
        {
            float baseband[16];
            int n = dsp_rx_pipeline_process_baseband(a->rx_dsp, iq_buf[i], baseband, 16);
            int k;

            for (k = 0; k < n; k++)
            {
                float clamped = (atomic_load(&a->cfg->rx_invert) ? -baseband[k] : baseband[k]) *
                    a->cfg->rx_gain;
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
    }
    log_msg( "bridge: RX thread exiting\n");
    return NULL;
}

/* ---- Command/TX processing loop (runs in main()) --------------------- */

/* New-format SET_CONFIG payload for the "openmtsx" modem type -- plain,
 * generic SX1255 hardware parameters (frequency, gains, PLL bandwidth)
 * plus RX/TX invert flags, in a format this bridge can parse directly,
 * replacing the "openmths" SET_CONFIG payload (which is just a single
 * handshake byte with no actual configuration) or ADD_MODE's
 * ADF7021-specific register encoding (which targets a completely
 * different radio chip and cannot be meaningfully decoded for the
 * SX1255). Multi-byte fields are big-endian. Distinguished from the
 * old single-byte handshake purely by payload length. */
#define OPENMTSX_CONFIG_VERSION 1
#define OPENMTSX_CONFIG_LEN 18
#define OPENMTSX_INVERT_RX_BIT 0x01U
#define OPENMTSX_INVERT_TX_BIT 0x02U

/*
 * apply_openmtsx_config()
 *
 * Applies an "openmtsx" SET_CONFIG payload (version, RX and TX frequency,
 * LNA, PGA, mixer and DAC gains, PLL bandwidths and invert flags; 18 bytes,
 * multi-byte fields big-endian) to the configuration and then re-runs the
 * hardware startup configuration.  A payload of the wrong length or version
 * is ignored.
 *
 * Parameters:
 *   tx_state    - TX state, whose configuration and SX1255 are updated.
 *   payload     - SET_CONFIG payload.
 *   payload_len - Payload length in bytes.
 *
 * Returns: nothing.
 */
static void
apply_openmtsx_config(tx_state_t *tx_state, const uint8_t *payload, uint8_t payload_len)
{
    uint8_t version;
    uint32_t rx_freq_hz, tx_freq_hz;
    uint8_t lna_gain_db, pga_gain_db;
    int8_t mixer_gain_x2, dac_gain_db;
    uint16_t rx_pll_bw_khz, tx_pll_bw_khz;
    uint8_t invert_flags;
    bool rx_invert, tx_invert;

    if (payload_len != OPENMTSX_CONFIG_LEN)
    {
        log_msg("bridge: SET_CONFIG: unexpected openmtsx payload length %u (expected %u), "
            "ignoring\n", payload_len, OPENMTSX_CONFIG_LEN);
        return;
    }
    version = payload[0];
    if (version != OPENMTSX_CONFIG_VERSION)
    {
        log_msg("bridge: SET_CONFIG: unknown openmtsx config version %u (expected %u), "
            "ignoring\n", version, OPENMTSX_CONFIG_VERSION);
        return;
    }
    rx_freq_hz = ((uint32_t)payload[1] << 24) | ((uint32_t)payload[2] << 16) |
        ((uint32_t)payload[3] << 8) | (uint32_t)payload[4];
    tx_freq_hz = ((uint32_t)payload[5] << 24) | ((uint32_t)payload[6] << 16) |
        ((uint32_t)payload[7] << 8) | (uint32_t)payload[8];
    lna_gain_db = payload[9];
    pga_gain_db = payload[10];
    mixer_gain_x2 = (int8_t)payload[11];
    dac_gain_db = (int8_t)payload[12];
    rx_pll_bw_khz = (uint16_t)(((uint16_t)payload[13] << 8) | payload[14]);
    tx_pll_bw_khz = (uint16_t)(((uint16_t)payload[15] << 8) | payload[16]);
    invert_flags = payload[17];
    rx_invert = (invert_flags & OPENMTSX_INVERT_RX_BIT) != 0;
    tx_invert = (invert_flags & OPENMTSX_INVERT_TX_BIT) != 0;

    log_msg("bridge: SET_CONFIG: applying openmtsx config -- rx=%u tx=%u lna=%u pga=%u "
        "mixer=%.1f dac=%d rx_pll_bw=%u tx_pll_bw=%u rx_invert=%d tx_invert=%d "
        "(overriding command-line defaults)\n", rx_freq_hz, tx_freq_hz, lna_gain_db,
        pga_gain_db, (double)mixer_gain_x2 / 2.0, dac_gain_db, rx_pll_bw_khz,
        tx_pll_bw_khz, (int)rx_invert, (int)tx_invert);

    tx_state->cfg->rx_freq_hz = rx_freq_hz;
    tx_state->cfg->tx_freq_hz = tx_freq_hz;
    tx_state->cfg->lna_gain_db = lna_gain_db;
    tx_state->cfg->pga_gain_db = pga_gain_db;
    tx_state->cfg->mixer_gain_db = (float)mixer_gain_x2 / 2.0f;
    tx_state->cfg->dac_gain_db = dac_gain_db;
    tx_state->cfg->rx_pll_bw_khz = rx_pll_bw_khz;
    tx_state->cfg->tx_pll_bw_khz = tx_pll_bw_khz;
    /* Atomic: read continuously, per-sample, by the RX thread and TX
     * playback thread respectively, concurrently with this write from
     * the main thread -- see the field declarations' own comments. */
    atomic_store(&tx_state->cfg->rx_invert, rx_invert);
    atomic_store(&tx_state->cfg->tx_invert, tx_invert);

    /* Re-apply to the actual hardware -- same full reset-and-configure
     * sequence used at startup, since these values need to actually
     * reach the SX1255's registers, not just this bridge's own config
     * struct. Happens once, early, before real key-up/data traffic is
     * expected. rx_invert/tx_invert don't need this step themselves
     * (they're applied per-sample in software, already updated above),
     * but the other fields do. */
    if (!sx1255_startup_configure(tx_state->sx, tx_state->cfg))
    {
        log_msg("bridge: SET_CONFIG: failed to apply openmtsx config to hardware\n");
    }
}

/*
 * handle_frame()
 *
 * Handles one frame from the host.  SET_CONFIG, SET_MODE, ADD_MODE and
 * DEL_MODE are acknowledged (SET_MODE keys or un-keys TX), GET_STATUS reports
 * free space, SAMP_DATA and TO_MODEM queue TX samples, and SAMP_EOT un-keys
 * TX at once.  Other types are logged.
 *
 * Parameters:
 *   tx_state      - TX state.
 *   pty_fd        - Master side of the pty, for the replies.
 *   type_and_conn - Frame type (high nibble) and connection or mode (low
 *                   nibble).
 *   payload       - Frame payload.
 *   payload_len   - Payload length in bytes.
 *
 * Returns: nothing.
 */
static void
handle_frame(tx_state_t *tx_state, int pty_fd, uint8_t type_and_conn, const uint8_t *payload,
    uint8_t payload_len)
{
    uint8_t type = (uint8_t)(type_and_conn & 0xF0);
    uint8_t conn_or_mode = (uint8_t)(type_and_conn & 0x0F);

    switch (type)
    {
        case OPENMT_SET_CONFIG:
            log_msg( "bridge: SET_CONFIG received (%u payload bytes)\n", payload_len);
            if (payload_len == OPENMTSX_CONFIG_LEN)
            {
                apply_openmtsx_config(tx_state, payload, payload_len);
            }
            write_frame(pty_fd, OPENMT_ACK, NULL, 0);
            break;

        case OPENMT_GET_STATUS:
        {
            /* Report ample space -- this bridge runs on the host CPU,
             * not a constrained MCU, so there's no real buffer limit to
             * report against. 0x0f is the max the 4-bit space field
             * allows. */
            uint8_t space = 0x0F;

            write_frame(pty_fd, OPENMT_GET_STATUS, &space, 1);
            break;
        }

        case OPENMT_SET_MODE:
            /* SET_MODE directly triggers key-up/key-down, in addition
             * to data presence (see tx_handle_samples()) doing the
             * same. tx_key_up() does real, slow hardware work -- enable
             * TX, a settling delay, a PLL lock wait, DSP reset, ALSA
             * prepare -- and having SET_MODE trigger it early lets that
             * latency be absorbed during the natural gap between
             * SET_MODE arriving and real sample data starting to flow,
             * rather than that same slow sequence running inline on the
             * main thread once the first real sample arrives, directly
             * competing with reading further incoming PTY frames. */
            if (conn_or_mode == OPENMT_MODE_IDLE)
            {
                log_msg( "bridge: SET_MODE: idle\n");
                tx_key_down(tx_state);
            }
            else
            {
                log_msg( "bridge: SET_MODE: conn %u active\n", conn_or_mode);
                tx_key_up(tx_state);
            }
            write_frame(pty_fd, OPENMT_ACK, NULL, 0);
            break;

        case OPENMT_ADD_MODE:
            log_msg( "bridge: ADD_MODE for conn %u\n", conn_or_mode);
            write_frame(pty_fd, OPENMT_ACK, NULL, 0);
            break;

        case OPENMT_DEL_MODE:
            log_msg( "bridge: DEL_MODE for conn %u\n", conn_or_mode);
            write_frame(pty_fd, OPENMT_ACK, NULL, 0);
            break;

        case OPENMT_SAMP_DATA:
            /* Modem_Host.c's own SAMP_DATA forwarding (protocol service
             * -> modem direction) uses a non-standard 4-byte header --
             * [0x61][len][0x70][txLevel] -- unlike the 3-byte header
             * every other message type in this protocol uses. That
             * extra txLevel byte, inserted right before the actual
             * sample payload, isn't accounted for by this bridge's
             * generic frame reader (which assumes a 3-byte header
             * throughout), so it must be skipped explicitly here or
             * every sample gets read one byte off from where it
             * actually starts. This bridge doesn't need to act on the
             * txLevel hint itself; --tx-symbol-scale/--tx-gain already
             * handle amplitude. */
            if (payload_len < 1)
            {
                break;
            }
            tx_handle_samples(tx_state, payload + 1, (uint8_t)(payload_len - 1));
            break;

        case OPENMT_TO_MODEM:
            /* Distinct from OPENMT_SAMP_DATA above: Modem_Host.c sends
             * this using the older, 0xE0-prefixed MMDVM-style framing
             * (see its own case OPENMT_TO_MODEM, which writes {0xE0,
             * len} rather than {0x61, len, type}) -- not this bridge's
             * 0x61-based "openmths" framing at all, so in practice this
             * case should never actually be reached here. Handled the
             * same way as OPENMT_SAMP_DATA (standard 3-byte header, no
             * extra byte to skip) only for robustness, not because it's
             * expected to fire. */
            tx_handle_samples(tx_state, payload, payload_len);
            break;

        case OPENMT_SAMP_EOT:
            /* Direct, immediate "TX should stop now" signal for the
             * TYPE_SAMPLE path specifically -- distinct from, and
             * faster than, the general-purpose modeHang timer, which
             * exists to return the host to idle when no further mode
             * activity is seen, not to control TX key state. Key down
             * immediately rather than waiting for a subsequent
             * SET_MODE|0x0F. Matches Modem_Host.c's actual case
             * OPENMT_SAMP_EOT: {0x61, 0x03, 0x0} -- type byte literally
             * 0x00, matching this constant's value. */
            log_msg( "bridge: SAMP_EOT received, unkeying TX immediately\n");
            tx_key_down(tx_state);
            break;

        default:
            log_msg( "bridge: unhandled frame type 0x%02X (conn/mode %u, %u payload "
                "bytes)\n", type, conn_or_mode, payload_len);
            break;
    }
}

/*
 * main()
 *
 * Entry point of the SX1255 bridge: reads the options, creates the pty,
 * starts and configures the SX1255, opens ALSA, starts the RX and TX playback
 * threads, then runs the command loop reading frames from the host until
 * SIGINT or SIGTERM.  It then stops the threads, un-keys TX and releases
 * everything.
 *
 * Parameters:
 *   argc - Argument count.
 *   argv - Argument vector.
 *
 * Returns: 0 on a normal exit, 1 if startup failed.
 */
int
main(int argc, char **argv)
{
    bridge_config_t cfg;
    sx1255_t sx;
    alsa_capture_t *cap;
    dsp_rx_pipeline_t *rx_dsp;
    tx_state_t tx_state;
    rx_thread_arg_t rx_arg;
    pthread_t rx_thread;
    tx_playback_thread_arg_t tx_playback_arg;
    pthread_t tx_playback_thread;
    int pty_fd;
    char slave_path[256];

    if (!parse_args(argc, argv, &cfg))
    {
        return 1;
    }

    {
        /* Plain signal() on Linux/glibc installs handlers with
         * SA_RESTART (BSD semantics) by default -- an interrupted
         * blocking syscall (like the read_frame() call in the main loop
         * below) is automatically restarted rather than returning
         * EINTR, meaning Ctrl-C during a period with no incoming PTY
         * traffic would never actually reach the "while (!g_stop)"
         * check at all. sigaction() with sa_flags=0 disables that. */
        struct sigaction sa;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handle_signal;
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    pty_fd = open_bridge_pty(slave_path, sizeof(slave_path));
    if (pty_fd < 0)
    {
        return 1;
    }
    log_msg( "bridge: PTY slave device: %s\n", slave_path);
    log_msg( "bridge: point OpenMT's modemtty config at this path (modem=OPENMTHS)\n");

    if (cfg.pty_symlink != NULL)
    {
        unlink(cfg.pty_symlink);
        if (symlink(slave_path, cfg.pty_symlink) != 0)
        {
            log_msg( "bridge: warning -- could not create symlink %s -> %s: %s\n",
                cfg.pty_symlink, slave_path, strerror(errno));
        }
        else
        {
            log_msg( "bridge: symlinked %s -> %s\n", cfg.pty_symlink, slave_path);
        }
    }

    log_msg( "bridge: opening SX1255 on %s (reset %s)\n", cfg.spi_path,
        cfg.gpio_chip != NULL ? cfg.gpio_chip : "none -- skipping our own GPIO reset request");
    if (!sx1255_open(&sx, cfg.spi_path, cfg.gpio_chip, cfg.gpio_reset_pin))
    {
        return 1;
    }
    if (!sx1255_startup_configure(&sx, &cfg))
    {
        log_msg( "bridge: startup reset/configuration failed\n");
        sx1255_close(&sx);
        return 1;
    }
    log_msg( "bridge: RX %u Hz, TX %u Hz\n", cfg.rx_freq_hz, cfg.tx_freq_hz);

    cap = alsa_capture_open(cfg.alsa_capture_dev, SX1255_SAMPLE_RATE_HZ);
    if (cap == NULL)
    {
        sx1255_close(&sx);
        return 1;
    }

    memset(&tx_state, 0, sizeof(tx_state));
    tx_state.sx = &sx;
    tx_state.cfg = &cfg;
    tx_state.pb = alsa_playback_open(cfg.alsa_playback_dev, SX1255_SAMPLE_RATE_HZ);
    if (tx_state.pb == NULL)
    {
        alsa_capture_close(cap);
        sx1255_close(&sx);
        return 1;
    }
    /* Default to the true SX1255-configured rate rather than whatever
     * ALSA's driver reports. */
    tx_state.tx_dsp = dsp_tx_pipeline_create(SX1255_SAMPLE_RATE_HZ);
    atomic_store(&tx_state.keyed, false);
    atomic_store(&tx_state.last_tx_sample_ms, monotonic_ms());
    tx_queue_init(&tx_state.queue);
    pthread_mutex_init(&tx_state.pb_mutex, NULL);

    rx_dsp = dsp_rx_pipeline_create(SX1255_SAMPLE_RATE_HZ);

    rx_arg.cap = cap;
    rx_arg.rx_dsp = rx_dsp;
    rx_arg.pty_fd = pty_fd;
    rx_arg.cfg = &cfg;
    if (pthread_create(&rx_thread, NULL, rx_thread_main, &rx_arg) != 0)
    {
        log_msg( "bridge: failed to create RX thread\n");
        return 1;
    }

    tx_playback_arg.tx_state = &tx_state;
    if (pthread_create(&tx_playback_thread, NULL, tx_playback_thread_main, &tx_playback_arg) != 0)
    {
        log_msg( "bridge: failed to create TX playback thread\n");
        return 1;
    }

    log_msg( "bridge: running. Ctrl-C to stop.\n");
    while (!g_stop)
    {
        uint8_t type_and_conn;
        uint8_t payload[MAX_FRAME_BYTES];
        uint8_t payload_len = sizeof(payload);

        if (!read_frame(pty_fd, &type_and_conn, payload, &payload_len))
        {
            /* Most likely cause: Modem_Host.c hasn't opened the slave
             * device yet (or has exited/restarted). Wait and retry
             * rather than treating this as fatal -- the whole point of
             * the PTY approach is that either end can be started and
             * restarted independently of the other. */
            usleep(200 * 1000);
            continue;
        }
        handle_frame(&tx_state, pty_fd, type_and_conn, payload, payload_len);
    }

    log_msg( "bridge: shutting down\n");
    g_stop = 1;
    pthread_join(rx_thread, NULL);
    pthread_join(tx_playback_thread, NULL);

    tx_key_down(&tx_state);
    sx1255_enable_rx(&sx, false);

    dsp_rx_pipeline_destroy(rx_dsp);
    dsp_tx_pipeline_destroy(tx_state.tx_dsp);
    alsa_capture_close(cap);
    alsa_playback_close(tx_state.pb);
    sx1255_close(&sx);
    close(pty_fd);
    return 0;
}
