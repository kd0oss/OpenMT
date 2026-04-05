/*
 *   Copyright (C) 2020 by Jonathan Naylor G4KLX
 *   Copyright (C) 2016 by Jim McLaughlin KI6ZUM
 *   Copyright (C) 2016,2017,2018 by Andy Uribe CA6JAU
 *   Copyright (C) 2017 by Danilo DB4PLE
 *
 *   Some of the code is based on work of Guus Van Dooren PE1PLM:
 *   https://github.com/ki6zum/gmsk-dstar/blob/master/firmware/dvmega/dvmega.ino
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
- Most of the registers values are obteined from ADI eval software:
http://www.analog.com/en/products/rf-microwave/integrated-transceivers-transmitters-receivers/low-power-rf-transceivers/adf7021.html
- or ADF7021 datasheet formulas:
www.analog.com/media/en/technical-documentation/data-sheets/ADF7021.pdf
*/

#ifndef ADF7021_H
#define ADF7021_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool m_duplex;

extern bool m_tx;
extern bool m_dcd;

// HS frequency ranges
#define VHF1_MIN  144000000
#define VHF1_MAX  148000000
#define VHF2_MIN  219000000
#define VHF2_MAX  225000000
#define UHF1_MIN  420000000
#define UHF1_MAX  475000000
#define UHF2_MIN  842000000
#define UHF2_MAX  950000000

// Banned amateur frequency ranges (satellite only, ISS, etc)
#define BAN1_MIN  145800000
#define BAN1_MAX  146000000
#define BAN2_MIN  435000000
#define BAN2_MAX  438000000

// Bidirectional Data pin (Enable Standard TX/RX Data Interface of ADF7021):
#define BIDIR_DATA_PIN

// TCXO of the ADF7021
// For 14.7456 MHz:
#define ADF7021_14_7456
// For 12.2880 MHz:
// #define ADF7021_12_2880

// Configure receiver gain for ADF7021
// AGC automatic, default settings:
#define AD7021_GAIN_AUTO
// AGC automatic with high LNA linearity:
// #define AD7021_GAIN_AUTO_LIN
// AGC OFF, lowest gain:
// #define AD7021_GAIN_LOW
// AGC OFF, highest gain:
// #define AD7021_GAIN_HIGH


// Disable TX Raised Cosine filter for 4FSK modulation in ADF7021:
// #define ADF7021_DISABLE_RC_4FSK

// Support for ADF7021-N version:
// #define ADF7021_N_VER

// Enable AFC support for DMR, YSF, P25, and M17 (experimental):
// (AFC is already enabled by default in D-Star)
// #define ADF7021_ENABLE_4FSK_AFC

// Configure AFC with positive initial frequency offset:
// #define ADF7021_AFC_POS

/****** Support for 14.7456 MHz TCXO (modified RF7021SE boards) ******/
#if defined(ADF7021_14_7456)

// R = 4
// DEMOD_CLK = 2.4576 MHz (DSTAR)
// DEMOD_CLK = 4.9152 MHz (DMR, YSF_L, P25)
// DEMOD_CLK = 7.3728 MHz (YSF_H, M17)
// DEMOD CLK = 3.6864 MHz (NXDN)
// DEMOD_CLK = 7.3728 MHz (POCSAG)
#define ADF7021_PFD              3686400.0

// PLL (REG 01)
#define ADF7021_REG1_VHF1        0x021F5041
#define ADF7021_REG1_VHF2        0x021F5041
#define ADF7021_REG1_UHF1        0x00575041
#define ADF7021_REG1_UHF2        0x00535041

// IF filter (REG 05)
#define ADF7021_REG5             0x000024F5

// IF CAL (fine cal, defaults) (REG 06)
#define ADF7021_REG6             0x05070E16


/****** Support for 12.2880 MHz TCXO ******/
#elif defined(ADF7021_12_2880)

// R = 2
// DEMOD_CLK = 2.4576 MHz (DSTAR)
// DEMOD_CLK = 6.1440 MHz (DMR, YSF_H, YSF_L, P25, M17)
// DEMOD_CLK = 3.0720 MHz (NXDN)
// DEMOD_CLK = 6.1440 MHz (POCSAG)
define ADF7021_PFD              6144000.0

// PLL (REG 01)
#define ADF7021_REG1_VHF1        0x021F5021
#define ADF7021_REG1_VHF2        0x021F5021
#define ADF7021_REG1_UHF1        0x00575021
#define ADF7021_REG1_UHF2        0x00535021

// IF filter (REG 05)
#define ADF7021_REG5             0x00001ED5

// IF CAL (fine cal, defaults) (REG 06)
#define ADF7021_REG6             0x0505EBB6

#endif

// Slicer threshold for 4FSK demodulator (REG 13)
#if defined(ADF7021_N_VER)

#define ADF7021_SLICER_TH_DSTAR  0U
#define ADF7021_SLICER_TH_DMR    51U
#define ADF7021_SLICER_TH_YSF_L  35U
#define ADF7021_SLICER_TH_YSF_H  69U
#define ADF7021_SLICER_TH_P25    43U
#define ADF7021_SLICER_TH_NXDN   26U
#define ADF7021_SLICER_TH_M17    59U            // Test

#else

#define ADF7021_SLICER_TH_DSTAR  0U
#define ADF7021_SLICER_TH_DMR    57U
#define ADF7021_SLICER_TH_YSF_L  38U
#define ADF7021_SLICER_TH_YSF_H  75U
#define ADF7021_SLICER_TH_P25    47U
#define ADF7021_SLICER_TH_NXDN   26U
#define ADF7021_SLICER_TH_M17    59U            // Test

#endif

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)

void ifConf(uint8_t* bytes, uint32_t reg2, uint32_t reg3, uint32_t reg4, uint32_t reg10, uint32_t reg13,
            uint32_t frequency_rx, uint32_t frequency_tx, uint8_t txDevLevel, uint8_t rfPower, uint32_t offset, bool reset);

#if defined(DUPLEX)
void Send_AD7021_control2(bool doSle = true);
#endif

#if defined(ADF7021_DISABLE_RC_4FSK)
#define ADF7021_EVEN_BIT  true
#else
#define ADF7021_EVEN_BIT  false
#endif

#ifdef __cplusplus
}
#endif

#endif /* ADF7021_H */
