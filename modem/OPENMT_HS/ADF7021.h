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

#if !defined(ADF7021_H)
#define  ADF7021_H

#include "Config.h"

#if defined(ENABLE_ADF7021)

/*
- Register values for the modes are built by the OpenMT protocol services
  and sent with ADD_MODE; only the clock-dependent constants live here.
- ADF7021 datasheet:
www.analog.com/media/en/technical-documentation/data-sheets/ADF7021.pdf
*/

// Disable TX Raised Cosine filter for 4FSK modulation in ADF7021:
// #define ADF7021_DISABLE_RC_4FSK

// Support for ADF7021-N version:
// #define ADF7021_N_VER

#if defined(ADF7021_14_7456)
// 14.7456 MHz TCXO, R = 4
#define ADF7021_PFD              3686400.0
#define ADF7021_XTAL             14745600UL
#define ADF7021_REG1_UHF2        0x00535041
#elif defined(ADF7021_12_2880)
// 12.2880 MHz TCXO, R = 2
#define ADF7021_PFD              6144000.0
#define ADF7021_XTAL             12288000UL
#define ADF7021_REG1_UHF2        0x00535021
#endif

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)

void Send_AD7021_control(bool doSle = true);
#if defined(DUPLEX)
void Send_AD7021_control2(bool doSle = true);
#endif

#if defined(ADF7021_DISABLE_RC_4FSK)
#define ADF7021_EVEN_BIT  true
#else
#define ADF7021_EVEN_BIT  false
#endif

#endif

#endif
