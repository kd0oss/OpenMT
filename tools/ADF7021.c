/*
 *   Copyright (C) 2020,2021 by Jonathan Naylor G4KLX
 *   Copyright (C) 2016 by Jim McLaughlin KI6ZUM
 *   Copyright (C) 2016,2017,2018,2019,2020 by Andy Uribe CA6JAU
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

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "ADF7021.h"

bool totx_request = false;
bool torx_request = false;
bool even = true;
uint32_t last_clk = 2U;

uint32_t  AD7021_control_word;

uint32_t  ADF7021_RX_REG0;
uint32_t  ADF7021_TX_REG0;
uint32_t  ADF7021_REG1;

int32_t   AFC_OFFSET = 0;
int       div2;

uint8_t   m_RX_N_divider;
uint16_t  m_RX_F_divider;
uint8_t   m_TX_N_divider;
uint16_t  m_TX_F_divider;

void ifConf(uint8_t* bytes, uint32_t reg2, uint32_t reg3, uint32_t reg4, uint32_t reg10, uint32_t reg13,
            uint32_t frequency_rx, uint32_t frequency_tx, uint8_t txDevLevel, uint8_t rfPower, uint32_t offset, bool reset)
{
    float    divider;

    uint32_t ADF7021_REG2  = reg2;
    uint32_t ADF7021_REG3  = reg3;
    uint32_t ADF7021_REG4  = reg4;
    uint32_t ADF7021_REG10 = reg10;
    uint32_t ADF7021_REG13 = reg13;

    AFC_OFFSET = offset;

    uint16_t m_Dev = (uint16_t)((26U * (uint16_t)txDevLevel) / 128U);

    /* Check frequency band */
    if ((frequency_tx >= VHF1_MIN) && (frequency_tx < VHF1_MAX))
    {
        ADF7021_REG1 = ADF7021_REG1_VHF1;         /* VHF1, external VCO */
        div2 = 1U;
    }
    else if ((frequency_tx >= VHF2_MIN) && (frequency_tx < VHF2_MAX))
    {
        ADF7021_REG1 = ADF7021_REG1_VHF2;         /* VHF1, external VCO */
        div2 = 1U;
    }
    else if ((frequency_tx >= UHF1_MIN) && (frequency_tx < UHF1_MAX))
    {
        ADF7021_REG1 = ADF7021_REG1_UHF1;         /* UHF1, internal VCO */
        div2 = 1U;
    }
    else if ((frequency_tx >= UHF2_MIN) && (frequency_tx < UHF2_MAX))
    {
        ADF7021_REG1 = ADF7021_REG1_UHF2;         /* UHF2, internal VCO */
        div2 = 2U;
    }
    else
    {
        ADF7021_REG1 = ADF7021_REG1_UHF1;         /* UHF1, internal VCO */
        div2 = 1U;
    }

    if (div2 == 1U)
    {
        divider = (frequency_rx - 100000 + AFC_OFFSET) / (ADF7021_PFD / 2U);
    }
    else
    {
        divider = (frequency_rx - 100000 + (2 * AFC_OFFSET)) / ADF7021_PFD;
    }

    m_RX_N_divider = floor(divider);
    divider = (divider - m_RX_N_divider) * 32768;
    m_RX_F_divider = floor(divider + 0.5);

    ADF7021_RX_REG0  = (uint32_t) 0b0000;

#if defined(BIDIR_DATA_PIN)
    ADF7021_RX_REG0 |= (uint32_t) 0b01001   << 27;   /* mux regulator/receive */
#else
    ADF7021_RX_REG0 |= (uint32_t) 0b01011   << 27;   /* mux regulator/uart-spi enabled/receive */
#endif

    ADF7021_RX_REG0 |= (uint32_t) m_RX_N_divider << 19;   /* frequency */
    ADF7021_RX_REG0 |= (uint32_t) m_RX_F_divider << 4;    /* frequency */

    if (div2 == 1U)
    {
        divider = frequency_tx / (ADF7021_PFD / 2U);
    }
    else
    {
        divider = frequency_tx / ADF7021_PFD;
    }

    m_TX_N_divider = floor(divider);
    divider = (divider - m_TX_N_divider) * 32768;
    m_TX_F_divider = floor(divider + 0.5);

    ADF7021_TX_REG0  = (uint32_t) 0b0000;            /* register 0 */

#if defined(BIDIR_DATA_PIN)
    ADF7021_TX_REG0 |= (uint32_t) 0b01000   << 27;   /* mux regulator/transmit */
#else
    ADF7021_TX_REG0 |= (uint32_t) 0b01010   << 27;   /* mux regulator/uart-spi enabled/transmit */
#endif

    ADF7021_TX_REG0 |= (uint32_t) m_TX_N_divider << 19;   /* frequency */
    ADF7021_TX_REG0 |= (uint32_t) m_TX_F_divider << 4;    /* frequency */

    /*  ADF7021_REG2 = (uint32_t) 0b00               << 28;  clock normal */
    ADF7021_REG2 |= (uint32_t) (m_Dev / div2)    << 19;  /* deviation */
    /*  ADF7021_REG2 |= (uint32_t) 0b001             << 4;   modulation (GMSK) */

    /* MODULATION (2) */
    ADF7021_REG2 |= (uint32_t) 0b0010;                           /* register 2 */
    ADF7021_REG2 |= (uint32_t) (rfPower & 0x3F)          << 13;  /* power level */
    ADF7021_REG2 |= (uint32_t) 0b110001                  << 7;   /* PA */

    memcpy((uint8_t*)bytes, (uint8_t*)&ADF7021_RX_REG0, 4);
    memcpy((uint8_t*)bytes + 4, (uint8_t*)&ADF7021_TX_REG0, 4);
    memcpy((uint8_t*)bytes + 8, (uint8_t*)&ADF7021_REG1, 4);
    memcpy((uint8_t*)bytes + 12, (uint8_t*)&ADF7021_REG2, 4);
    memcpy((uint8_t*)bytes + 16, (uint8_t*)&ADF7021_REG3, 4);
    memcpy((uint8_t*)bytes + 20, (uint8_t*)&ADF7021_REG4, 4);
    
    uint32_t reg5 = ADF7021_REG5;
    uint32_t reg6 = ADF7021_REG6;
    memcpy((uint8_t*)bytes + 24, (uint8_t*)&reg5, 4);
    memcpy((uint8_t*)bytes + 28, (uint8_t*)&reg6, 4);
    memcpy((uint8_t*)bytes + 32, (uint8_t*)&ADF7021_REG10, 4);
    memcpy((uint8_t*)bytes + 36, (uint8_t*)&ADF7021_REG13, 4);
}
