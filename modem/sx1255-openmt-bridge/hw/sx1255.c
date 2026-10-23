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

#define _DEFAULT_SOURCE
#include "sx1255.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ---- Register map (sx1255_modem.go) ------------------------------------*/
#define REG_CONTROL 0x00
#define REG_RX_FREQ_MSB 0x01
#define REG_RX_FREQ_MID 0x02
#define REG_RX_FREQ_LSB 0x03
#define REG_TX_FREQ_MSB 0x04
#define REG_TX_FREQ_MID 0x05
#define REG_TX_FREQ_LSB 0x06
#define REG_VERSION 0x07
#define REG_TX_DAC_GAIN 0x08
#define REG_TX_FILTER_BW 0x0A
#define REG_TX_DAC_TAPS 0x0B
#define REG_RX_LNA_PGA 0x0C
#define REG_RX_BW_FILTER 0x0D
#define REG_RX_PLL_BW 0x0E
#define REG_LOOPBACK 0x10
#define REG_PLL_STATUS 0x11
#define REG_I2S_RATE0 0x12
#define REG_I2S_RATE1 0x13

#define MODE_REF_ENABLE 0x01
#define MODE_RX_ENABLE 0x02
#define MODE_TX_ENABLE 0x04
#define MODE_DRIVER_ENABLE 0x08
#define MODE_MASK (MODE_REF_ENABLE | MODE_RX_ENABLE | MODE_TX_ENABLE | MODE_DRIVER_ENABLE)

#define SPI_DEFAULT_SPEED_HZ 500000
#define SPI_DEFAULT_BPW 8

/* ---- Low-level SPI transfer ---------------------------------------------*/

/*
 * spi_transfer()
 *
 * Performs one full-duplex SPI transfer on the device at the default speed
 * and word size.
 *
 * Parameters:
 *   fd  - Open SPI device.
 *   tx  - Bytes to send.
 *   rx  - Receives the bytes clocked in.
 *   len - Number of bytes.
 *
 * Returns: true if the ioctl succeeded.
 */
static bool
spi_transfer(int fd, const uint8_t* tx, uint8_t* rx, size_t len)
{
    struct spi_ioc_transfer xfer;

    memset(&xfer, 0, sizeof(xfer));
    xfer.tx_buf        = (uint64_t)(uintptr_t)tx;
    xfer.rx_buf        = (uint64_t)(uintptr_t)rx;
    xfer.len           = (uint32_t)len;
    xfer.speed_hz      = SPI_DEFAULT_SPEED_HZ;
    xfer.bits_per_word = SPI_DEFAULT_BPW;

    return ioctl(fd, SPI_IOC_MESSAGE(1), &xfer) >= 0;
}

/*
 * spi_write_reg()
 *
 * Writes one SX1255 register over SPI (address with bit 7 set) and waits 10
 * ms.
 *
 * Parameters:
 *   fd   - Open SPI device.
 *   addr - Register address.
 *   val  - Value to write.
 *
 * Returns: true if the transfer succeeded.
 */
static bool spi_write_reg(int fd, uint8_t addr, uint8_t val)
{
    uint8_t tx[2] = {(uint8_t)(addr | 0x80), val};
    uint8_t rx[2] = {0, 0};
    bool ok       = spi_transfer(fd, tx, rx, sizeof(tx));

    if (ok)
    {
        usleep(10 * 1000); /* match libsx1255 timing, as in sx1255_spi.go */
    }
    return ok;
}

/*
 * spi_read_reg()
 *
 * Reads one SX1255 register over SPI and waits 10 ms.
 *
 * Parameters:
 *   fd   - Open SPI device.
 *   addr - Register address.
 *   val  - Receives the register value.
 *
 * Returns: true if the transfer succeeded; *val is untouched on failure.
 */
static bool spi_read_reg(int fd, uint8_t addr, uint8_t* val)
{
    uint8_t tx[2] = {(uint8_t)(addr & 0x7F), 0x00};
    uint8_t rx[2] = {0, 0};
    bool ok       = spi_transfer(fd, tx, rx, sizeof(tx));

    if (ok)
    {
        usleep(10 * 1000);
        *val = rx[1];
    }
    return ok;
}

/*
 * spi_update_reg()
 *
 * Changes selected bits of a register: reads it, writes it back with the
 * masked bits replaced, then reads it again to check the bits took effect.
 *
 * Parameters:
 *   fd    - Open SPI device.
 *   addr  - Register address.
 *   mask  - Bits to change.
 *   value - New value for the masked bits.
 *
 * Returns: true if the bits read back as written, false on an SPI error or a
 *   mismatch (which is logged).
 */
static bool spi_update_reg(int fd, uint8_t addr, uint8_t mask, uint8_t value)
{
    uint8_t reg, want, got;

    if (!spi_read_reg(fd, addr, &reg))
    {
        return false;
    }
    want = (uint8_t)((reg & (uint8_t)~mask) | (value & mask));
    if (!spi_write_reg(fd, addr, want))
    {
        return false;
    }
    if (!spi_read_reg(fd, addr, &got))
    {
        return false;
    }
    if ((uint8_t)((got ^ want) & mask) != 0)
    {
        fprintf(stderr, "sx1255: reg 0x%02X readback 0x%02X, wanted 0x%02X (bits 0x%02X did not take)\n",
                addr, got, want, (unsigned)((got ^ want) & mask));
        return false;
    }
    return true;
}

/* ---- Open / init ---------------------------------------------------------*/

/*
 * sx1255_open()
 *
 * Opens the SPI device (mode 0, default word size and speed) and, if a chip
 * path is given, claims the reset GPIO line as an output held low.
 *
 * Parameters:
 *   dev             - Device structure to fill in.
 *   spi_path        - SPI device, for example /dev/spidev0.0.
 *   reset_chip_path - GPIO chip with the reset line, or NULL if there is
 *                     none.
 *   reset_pin       - Line number of the reset pin on that chip.
 *
 * Returns: true on success, false on failure (the error is logged and
 *   nothing is left open).
 */
bool sx1255_open(sx1255_t* dev, const char* spi_path, const char* reset_chip_path, unsigned int reset_pin)
{
    uint8_t mode   = 0; /* SPI mode 0: CPOL=0, CPHA=0 */
    uint8_t bpw    = SPI_DEFAULT_BPW;
    uint32_t speed = SPI_DEFAULT_SPEED_HZ;

    memset(dev, 0, sizeof(*dev));
    dev->spi_fd = -1;

    dev->spi_fd = open(spi_path, O_RDWR);
    if (dev->spi_fd < 0)
    {
        fprintf(stderr, "sx1255: open %s: %s\n", spi_path, strerror(errno));
        return false;
    }

    if (ioctl(dev->spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(dev->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bpw) < 0 ||
        ioctl(dev->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0)
    {
        fprintf(stderr, "sx1255: configure %s: %s\n", spi_path, strerror(errno));
        close(dev->spi_fd);
        dev->spi_fd = -1;
        return false;
    }

    if (reset_chip_path != NULL)
    {
        if (!gpio_line_request_output(&dev->reset_line, reset_chip_path, reset_pin, 0))
        {
            fprintf(stderr, "sx1255: request reset line %s:%u: %s\n",
                    reset_chip_path, reset_pin, strerror(errno));
            close(dev->spi_fd);
            dev->spi_fd = -1;
            return false;
        }
        dev->have_reset_line = true;
    }

    return true;
}

/*
 * sx1255_reset()
 *
 * Pulses the reset line high for 100 ms then low and waits 250 ms for the
 * chip to start.  Does nothing if no reset line is configured.
 *
 * Parameters:
 *   dev - Device.
 *
 * Returns: true on success (also when there is no reset line), false if the
 *   GPIO write fails.
 */
bool sx1255_reset(sx1255_t* dev)
{
    if (!dev->have_reset_line)
    {
        return true; /* no GPIO configured, e.g. emulation/bench setup */
    }
    /* Active-high reset: drive the line high to assert reset, hold,
     * then release it low before any SPI transaction is attempted. */
    if (!gpio_line_set_value(&dev->reset_line, 1))
    {
        return false;
    }
    usleep(100 * 1000);
    if (!gpio_line_set_value(&dev->reset_line, 0))
    {
        return false;
    }
    usleep(250 * 1000);
    return true;
}

/*
 * sx1255_init()
 *
 * Resets the chip, programs the RX bandwidth filter and TX DAC taps
 * registers and checks the chip version.
 *
 * Parameters:
 *   dev - Device.
 *
 * Returns: true on success, false if the reset or a register write fails or
 *   the version is not the expected one.
 */
bool sx1255_init(sx1255_t* dev)
{
    if (!sx1255_reset(dev))
    {
        fprintf(stderr, "sx1255: reset failed\n");
        return false;
    }

    /* Reg 0x0D: RX bandwidth filter config. */
    if (!spi_write_reg(dev->spi_fd, REG_RX_BW_FILTER, (uint8_t)((0x01 << 5) | (0x05 << 2) | 0x03)))
    {
        fprintf(stderr, "sx1255: init reg 0x0D failed\n");
        return false;
    }
    /* Reg 0x0B: TX DAC taps. */
    if (!spi_write_reg(dev->spi_fd, REG_TX_DAC_TAPS, 5))
    {
        fprintf(stderr, "sx1255: init reg 0x0B failed\n");
        return false;
    }

    uint8_t ver = sx1255_get_chip_version(dev);

    if (ver != SX1255_EXPECTED_VERSION)
    {
        fprintf(stderr, "sx1255: unexpected chip version 0x%02X (expected 0x%02X)\n",
                ver, SX1255_EXPECTED_VERSION);
        return false;
    }
    return true;
}

/* ---- Frequency -----------------------------------------------------------*/

/*
 * sx1255_freq_to_reg()
 *
 * Converts a frequency to the value of the chip's 24-bit frequency register
 * (hz * 2^20 / 32 MHz).
 *
 * Parameters:
 *   hz - Frequency in Hz.
 *
 * Returns: Register value.
 */
uint32_t sx1255_freq_to_reg(uint32_t hz)
{
    return (uint32_t)llround((double)hz * 1048576.0 / SX1255_CLK_FREQ_HZ);
}

/*
 * set_freq()
 *
 * Writes the three frequency registers for a frequency.
 *
 * Parameters:
 *   dev     - Device.
 *   hz      - Frequency in Hz.
 *   reg_msb - Address of the most significant byte register.
 *   reg_mid - Address of the middle byte register.
 *   reg_lsb - Address of the least significant byte register.
 *
 * Returns: true if all three writes succeeded.
 */
static bool set_freq(sx1255_t* dev, uint32_t hz, uint8_t reg_msb, uint8_t reg_mid, uint8_t reg_lsb)
{
    uint32_t val = sx1255_freq_to_reg(hz);

    if (!spi_write_reg(dev->spi_fd, reg_msb, (uint8_t)(val >> 16)))
    {
        return false;
    }
    if (!spi_write_reg(dev->spi_fd, reg_mid, (uint8_t)(val >> 8)))
    {
        return false;
    }
    return spi_write_reg(dev->spi_fd, reg_lsb, (uint8_t)val);
}

/*
 * sx1255_set_rx_freq()
 *
 * Sets the receive frequency.
 *
 * Parameters:
 *   dev - Device.
 *   hz  - Frequency in Hz.
 *
 * Returns: true on success.
 */
bool sx1255_set_rx_freq(sx1255_t* dev, uint32_t hz)
{
    return set_freq(dev, hz, REG_RX_FREQ_MSB, REG_RX_FREQ_MID, REG_RX_FREQ_LSB);
}

/*
 * sx1255_set_tx_freq()
 *
 * Sets the transmit frequency.
 *
 * Parameters:
 *   dev - Device.
 *   hz  - Frequency in Hz.
 *
 * Returns: true on success.
 */
bool sx1255_set_tx_freq(sx1255_t* dev, uint32_t hz)
{
    return set_freq(dev, hz, REG_TX_FREQ_MSB, REG_TX_FREQ_MID, REG_TX_FREQ_LSB);
}

/*
 * sx1255_set_rate()
 *
 * Sets the I2S sample rate registers (125 kSa/s, mode B2: standard I2S with
 * I and Q multiplexed) and logs a warning if the chip reports an I2S error.
 * Register 0x13 is written before 0x12, as in libsx1255.
 *
 * Parameters:
 *   dev - Device.
 *
 * Returns: true if the register writes succeeded.
 */
bool sx1255_set_rate(sx1255_t* dev)
{
    uint8_t reg12 = 0, reg13 = 0;

    if (!spi_write_reg(dev->spi_fd, REG_I2S_RATE1, 0x2C))
    {
        return false;
    }
    if (!spi_write_reg(dev->spi_fd, REG_I2S_RATE0, 0x22))
    {
        return false;
    }

    spi_read_reg(dev->spi_fd, REG_I2S_RATE0, &reg12);
    spi_read_reg(dev->spi_fd, REG_I2S_RATE1, &reg13);

    if (reg13 & 0x02)
    {
        fprintf(stderr, "sx1255: IISM status flag set -- I2S interface error\n");
    }
    (void)reg12;
    return true;
}

/* ---- Gain ------------------------------------------------------------------*/

/*
 * lna_code()
 *
 * Picks the LNA gain setting nearest to a wanted gain from the steps 0, 12,
 * 24, 36, 42 and 48 dB.
 *
 * Parameters:
 *   db     - Wanted gain in dB.
 *   actual - Receives the gain of the chosen step in dB.
 *
 * Returns: Register code (0 to 5) of the chosen step.
 */
static uint8_t lna_code(uint8_t db, uint8_t* actual)
{
    static const uint8_t table_db[6] = {0, 12, 24, 36, 42, 48};
    int best_i                       = 0;
    int best_diff                    = 1 << 30;
    int i;

    for (i = 0; i < 6; i++)
    {
        int diff = (int)db - (int)table_db[i];

        if (diff < 0)
        {
            diff = -diff;
        }
        if (diff < best_diff)
        {
            best_diff = diff;
            best_i    = i;
        }
    }
    *actual = table_db[best_i];
    return (uint8_t)best_i;
}

/*
 * dac_code()
 *
 * Picks the TX DAC gain setting nearest to a wanted gain from the steps 0,
 * -3, -6 and -9 dB.
 *
 * Parameters:
 *   db     - Wanted gain in dB.
 *   actual - Receives the gain of the chosen step in dB.
 *
 * Returns: Register code (0 to 3) of the chosen step.
 */
static uint8_t dac_code(int8_t db, int8_t* actual)
{
    static const int8_t table_db[4] = {0, -3, -6, -9};
    int best_i                      = 0;
    int best_diff                   = 1 << 30;
    int i;

    for (i = 0; i < 4; i++)
    {
        int diff = (int)db - (int)table_db[i];

        if (diff < 0)
        {
            diff = -diff;
        }
        if (diff < best_diff)
        {
            best_diff = diff;
            best_i    = i;
        }
    }
    *actual = table_db[best_i];
    return (uint8_t)best_i;
}

/*
 * sx1255_set_lna_gain()
 *
 * Sets the receive LNA gain to the nearest available step.
 *
 * Parameters:
 *   dev - Device.
 *   db  - Wanted gain in dB.
 *
 * Returns: true on success.
 */
bool sx1255_set_lna_gain(sx1255_t* dev, uint8_t db)
{
    uint8_t actual;
    uint8_t code = lna_code(db, &actual);

    return spi_update_reg(dev->spi_fd, REG_RX_LNA_PGA, 0xE0, (uint8_t)(code << 5));
}

/*
 * sx1255_set_pga_gain()
 *
 * Sets the receive PGA gain in 2 dB steps (value limited to 30 dB).
 *
 * Parameters:
 *   dev - Device.
 *   db  - Wanted gain in dB.
 *
 * Returns: true on success.
 */
bool sx1255_set_pga_gain(sx1255_t* dev, uint8_t db)
{
    uint8_t step = db / 2;

    if (step > 15)
    {
        step = 15;
    }
    return spi_update_reg(dev->spi_fd, REG_RX_LNA_PGA, 0x1E, (uint8_t)(step << 1));
}

/*
 * sx1255_set_dac_gain()
 *
 * Sets the transmit DAC gain to the nearest available step.
 *
 * Parameters:
 *   dev - Device.
 *   db  - Wanted gain in dB.
 *
 * Returns: true on success.
 */
bool sx1255_set_dac_gain(sx1255_t* dev, int8_t db)
{
    int8_t actual;
    uint8_t code = dac_code(db, &actual);

    /* Mask all three bits [6:4]: leaving bit 6 as found risks selecting a
     * test-Vref mode the datasheet says not to use. */
    return spi_update_reg(dev->spi_fd, REG_TX_DAC_GAIN, 0x70, (uint8_t)(code << 4));
}

/*
 * sx1255_set_mixer_gain()
 *
 * Sets the transmit mixer gain in 2 dB steps from -37.5 dB, limited to the
 * 16 available steps.
 *
 * Parameters:
 *   dev - Device.
 *   db  - Wanted gain in dB.
 *
 * Returns: true on success.
 */
bool sx1255_set_mixer_gain(sx1255_t* dev, float db)
{
    int step = (int)lround((double)(db + 37.5) / 2.0);

    if (step < 0)
    {
        step = 0;
    }
    if (step > 15)
    {
        step = 15;
    }
    return spi_update_reg(dev->spi_fd, REG_TX_DAC_GAIN, 0x0F, (uint8_t)step);
}

/*
 * bw_code()
 *
 * Converts a PLL bandwidth in kHz to the chip's 2-bit code: up to 75, 150
 * and 225 kHz give 0, 1 and 2, anything larger gives 3.
 *
 * Parameters:
 *   bw_khz - Bandwidth in kHz.
 *
 * Returns: Code from 0 to 3.
 */
static uint8_t bw_code(uint16_t bw_khz)
{
    if (bw_khz <= 75)
    {
        return 0;
    }
    if (bw_khz <= 150)
    {
        return 1;
    }
    if (bw_khz <= 225)
    {
        return 2;
    }
    return 3;
}

/*
 * sx1255_set_rx_pll_bw()
 *
 * Sets the receive PLL bandwidth.
 *
 * Parameters:
 *   dev    - Device.
 *   bw_khz - Bandwidth in kHz.
 *
 * Returns: true on success.
 */
bool sx1255_set_rx_pll_bw(sx1255_t* dev, uint16_t bw_khz)
{
    uint8_t reg, code = bw_code(bw_khz);

    if (!spi_read_reg(dev->spi_fd, REG_RX_PLL_BW, &reg))
    {
        return false;
    }
    reg = (uint8_t)((reg & 0xF9) | (code << 1));
    return spi_write_reg(dev->spi_fd, REG_RX_PLL_BW, reg);
}

/*
 * sx1255_set_tx_pll_bw()
 *
 * Sets the transmit PLL bandwidth.
 *
 * Parameters:
 *   dev    - Device.
 *   bw_khz - Bandwidth in kHz.
 *
 * Returns: true on success.
 */
bool sx1255_set_tx_pll_bw(sx1255_t* dev, uint16_t bw_khz)
{
    uint8_t reg, code = bw_code(bw_khz);

    if (!spi_read_reg(dev->spi_fd, REG_TX_FILTER_BW, &reg))
    {
        return false;
    }
    reg = (uint8_t)((reg & 0x9F) | (code << 5));
    return spi_write_reg(dev->spi_fd, REG_TX_FILTER_BW, reg);
}

/*
 * set_mode()
 *
 * Changes bits in the mode control register and checks them by reading back.
 * The reference oscillator bit is always kept on, because without it the
 * chip drives no I2S clock.
 *
 * Parameters:
 *   dev        - Device.
 *   set_bits   - Mode bits to turn on.
 *   clear_bits - Mode bits to turn off.
 *
 * Returns: true if the mode bits read back as written.
 */
static bool set_mode(sx1255_t* dev, uint8_t set_bits, uint8_t clear_bits)
{
    uint8_t reg, want, got;

    if (!spi_read_reg(dev->spi_fd, REG_CONTROL, &reg))
    {
        return false;
    }
    want = (uint8_t)((reg & (uint8_t)~clear_bits) | set_bits | MODE_REF_ENABLE);
    if (!spi_write_reg(dev->spi_fd, REG_CONTROL, want))
    {
        return false;
    }
    if (!spi_read_reg(dev->spi_fd, REG_CONTROL, &got))
    {
        return false;
    }
    if ((uint8_t)((got ^ want) & MODE_MASK) != 0)
    {
        fprintf(stderr, "sx1255: control reg 0x00 readback 0x%02X, wanted 0x%02X (mode bits differ)\n",
                got, want);
        return false;
    }
    return true;
}

/*
 * sx1255_enable_rx()
 *
 * Turns the receiver on or off.
 *
 * Parameters:
 *   dev    - Device.
 *   enable - true to turn on.
 *
 * Returns: true on success.
 */
bool sx1255_enable_rx(sx1255_t* dev, bool enable)
{
    return enable ? set_mode(dev, MODE_RX_ENABLE, 0) : set_mode(dev, 0, MODE_RX_ENABLE);
}

/*
 * sx1255_enable_tx()
 *
 * Turns the transmitter (and its driver) on or off.
 *
 * Parameters:
 *   dev    - Device.
 *   enable - true to turn on.
 *
 * Returns: true on success.
 */
bool sx1255_enable_tx(sx1255_t* dev, bool enable)
{
    uint8_t tx_bits = MODE_TX_ENABLE | MODE_DRIVER_ENABLE;

    return enable ? set_mode(dev, tx_bits, 0) : set_mode(dev, 0, tx_bits);
}

/*
 * sx1255_enable_rf_loopback()
 *
 * Turns the RF loopback bit (bit 2 of the loopback register) on or off and
 * checks that it read back.
 *
 * Parameters:
 *   dev    - Device.
 *   enable - true to turn on.
 *
 * Returns: true on success, false if the bit did not take (logged).
 */
bool sx1255_enable_rf_loopback(sx1255_t* dev, bool enable)
{
    uint8_t reg, want, got;

    if (!spi_read_reg(dev->spi_fd, REG_LOOPBACK, &reg))
    {
        return false;
    }
    if (enable)
    {
        want = (uint8_t)(reg | 0x04);
    }
    else
    {
        want = (uint8_t)(reg & (uint8_t)~0x04);
    }
    if (!spi_write_reg(dev->spi_fd, REG_LOOPBACK, want))
    {
        return false;
    }
    /* Verify the write actually took, the same way spi_update_reg() does
     * elsewhere in this file -- a bit that silently doesn't stick
     * (read-only on this chip revision, gated by another mode bit, etc.)
     * would otherwise look identical to "enabled successfully but the
     * loopback path itself doesn't work", which is a very different
     * problem to chase. */
    if (!spi_read_reg(dev->spi_fd, REG_LOOPBACK, &got))
    {
        return false;
    }
    if ((uint8_t)((got ^ want) & 0x04) != 0)
    {
        fprintf(stderr,
                "sx1255: RF loopback bit did not take -- wrote 0x%02X, register reads "
                "back 0x%02X\n",
                want, got);
        fprintf(stderr,
                "sx1255: (bit 2 unchanged: 0x10 may be read-only or gated by another "
                "mode/register on this chip revision)\n");
        return false;
    }
    return true;
}

/*
 * sx1255_get_pll_status()
 *
 * Reads the PLL lock bits.
 *
 * Parameters:
 *   dev       - Device.
 *   tx_locked - Receives true if the TX PLL is locked.
 *   rx_locked - Receives true if the RX PLL is locked.
 *
 * Returns: true if the register was read.
 */
bool sx1255_get_pll_status(sx1255_t* dev, bool* tx_locked, bool* rx_locked)
{
    uint8_t reg;

    if (!spi_read_reg(dev->spi_fd, REG_PLL_STATUS, &reg))
    {
        return false;
    }
    *tx_locked = (reg & 0x01) != 0;
    *rx_locked = (reg & 0x02) != 0;
    return true;
}

/*
 * sx1255_wait_pll_lock()
 *
 * Polls the PLL status every 10 ms until the chosen PLL locks or the time
 * runs out.
 *
 * Parameters:
 *   dev        - Device.
 *   tx         - true to wait for the TX PLL, false for the RX PLL.
 *   timeout_ms - Longest time to wait, in milliseconds.
 *
 * Returns: true if the PLL locked, false on timeout.
 */
bool sx1255_wait_pll_lock(sx1255_t* dev, bool tx, int timeout_ms)
{
    struct timespec deadline, now;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec += 1;
    }

    for (;;)
    {
        bool tx_locked = false, rx_locked = false;

        if (sx1255_get_pll_status(dev, &tx_locked, &rx_locked) &&
            ((tx && tx_locked) || (!tx && rx_locked)))
        {
            return true;
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
        {
            return false;
        }
        usleep(10 * 1000);
    }
}

/*
 * sx1255_get_chip_version()
 *
 * Reads the chip version register.
 *
 * Parameters:
 *   dev - Device.
 *
 * Returns: Version byte, or 0 if the read fails.
 */
uint8_t sx1255_get_chip_version(sx1255_t* dev)
{
    uint8_t ver = 0;

    if (!spi_read_reg(dev->spi_fd, REG_VERSION, &ver))
    {
        return 0;
    }
    return ver;
}

/*
 * sx1255_close()
 *
 * Releases the reset line and closes the SPI device.
 *
 * Parameters:
 *   dev - Device.
 *
 * Returns: nothing.
 */
void sx1255_close(sx1255_t* dev)
{
    if (dev->have_reset_line)
    {
        gpio_line_close(&dev->reset_line);
        dev->have_reset_line = false;
    }
    if (dev->spi_fd >= 0)
    {
        close(dev->spi_fd);
        dev->spi_fd = -1;
    }
}
