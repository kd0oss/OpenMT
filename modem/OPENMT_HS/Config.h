/*
 *   Copyright (C) 2017,2018,2019,2020 by Andy Uribe CA6JAU
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

#if !defined(CONFIG_H)
#define  CONFIG_H

// Select one board (STM32F103 based boards)
// 1) ZUMspot RPi or ZUMspot USB:
// #define ZUMSPOT_ADF7021
// 2) Libre Kit board or any homebrew hotspot with modified RF7021SE and Blue Pill STM32F103:
// #define LIBRE_KIT_ADF7021
// 3) MMDVM_HS_Hat revisions 1.1, 1.2 and 1.4 (DB9MAT & DF2ET)
// #define MMDVM_HS_HAT_REV12
// 4) MMDVM_HS_Dual_Hat revisions 1.0 (DB9MAT & DF2ET & DO7EN)
#define MMDVM_HS_DUAL_HAT_REV10
// 5) Nano hotSPOT (BI7JTA)
// #define NANO_HOTSPOT
// 6) NanoDV NPi or USB revisions 1.0 (BG4TGO & BG5HHP)
// #define NANO_DV_REV10
// 7) D2RG MMDVM_HS RPi (BG3MDO, VE2GZI, CA6JAU)
// #define D2RG_MMDVM_HS
// 8) BridgeCom SkyBridge HotSpot
// #define SKYBRIDGE_HS
// 9) LoneStar USB Stick ADF7071
// #define LONESTAR_USB

// Enable ADF7021 support:
#define ENABLE_ADF7021

// Enable full duplex support with dual ADF7021 (valid for homebrew hotspots only):
#define DUPLEX

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

// Host communication selection:
#define STM32_USART1_HOST
//#define STM32_USB_HOST
// #define STM32_I2C_HOST

// I2C host address:
#define I2C_ADDR 0x22

// Disable mode LEDs blink during scan mode:
// #define QUIET_MODE_LEDS

// Engage a constant or descreet Service LED mode once repeater is running 
// #define CONSTANT_SRV_LED
// #define CONSTANT_SRV_LED_INVERTED
// #define DISCREET_SRV_LED
// #define DISCREET_SRV_LED_INVERTED

// Use the YSF and P25 LEDs for NXDN
// #define USE_ALTERNATE_NXDN_LEDS

// Use the D-Star and P25 LEDs for M17
#define USE_ALTERNATE_M17_LEDS

// Use the D-Star and DMR LEDs for POCSAG
// #define USE_ALTERNATE_POCSAG_LEDS

// Enable for RPi 3B+, USB mode
//#define LONG_USB_RESET

// Enable modem debug messages
//#define ENABLE_DEBUG

// Disable frequency bands check
// #define DISABLE_FREQ_CHECK

// Disable frequency restrictions (satellite, ISS, etc)
// #define DISABLE_FREQ_BAN

// ---------------------------------------------------------------------------
// OpenMT (modem type "openmths") options, defaults are in OpenMT.h
// ---------------------------------------------------------------------------
// Serial speed towards Modem_Host, must match modemBaud in the OpenMT config:
// #define OPENMT_SERIAL_SPEED        460800
// TX buffer size in host BIT frames (192 bytes each, 2 x 192 bytes of RAM):
// #define OPENMT_TX_BUFFER_FRAMES    8U
// Key up once this many bits are buffered, or after the timeout (ms):
// #define OPENMT_TX_START_BITS       3072U
// #define OPENMT_TX_START_TIMEOUT_MS 350U
// RX bytes per BIT frame sent to the host:
// #define OPENMT_RX_FRAME_BYTES      24U
// Stop streaming RX bits this long after the last sync (ms), unless the
// host locked the mode with SET_MODE:
// #define OPENMT_LOCK_TIMEOUT_MS     1500U
// Learn the timeout per mode from the sync gaps seen under host lock:
// #define OPENMT_LOCK_LEARN          1
// #define OPENMT_LOCK_MIN_MS         250U
// Send the first frame at the sync instead of waiting for a full frame:
// #define OPENMT_SYNC_FLUSH          1
// Skip empty channels while scanning (RSSI, as -dBm, carrier when stronger):
// #define OPENMT_RSSI_SCAN           1
// #define OPENMT_RSSI_THRESHOLD      115U
// #define OPENMT_RSSI_SETTLE_MS      2U
// Scan indicator: the D-Star and DMR LEDs blink alternately this many ms each while scanning:
// #define OPENMT_SCAN_LED_MS         250U
// RX offset measurement (MEAS command): listen time per reading (ms). The step and
// AFC range are used by the openmt_cal raw/sweep diagnostics only:
// #define OPENMT_MEAS_SETTLE_MS      60U
// #define OPENMT_MEAS_STEP_COUNTS    16
// #define OPENMT_MEAS_AFC_RANGE      12U

#endif
