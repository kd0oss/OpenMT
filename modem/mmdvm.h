/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS             *
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

#ifndef MMDVM_H
#define MMDVM_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

// ****** MMDVM specific header values.   ****
const uint8_t  MODEM_VERSION      = 0x00;
const uint8_t  MODEM_STATUS       = 0x01;
const uint8_t  MODEM_CONFIG       = 0x02;
const uint8_t  MODEM_MODE         = 0x03;
const uint8_t  MODEM_SET_FREQ     = 0x04;
const uint8_t  MODEM_ACK          = 0x70;
const uint8_t  MODEM_NACK         = 0x7F;
const uint8_t  MODEM_FRAME_START  = 0xE0;
const uint8_t  MODEM_DEBUG        = 0xF1;

const uint8_t  TYPE_DSTAR_HEADER  = 0x10;
const uint8_t  TYPE_DSTAR_DATA    = 0x11;
const uint8_t  TYPE_DSTAR_LOST    = 0x12;
const uint8_t  TYPE_DSTAR_EOT     = 0x13;
const uint8_t  TYPE_M17_LSF       = 0x45;
const uint8_t  TYPE_M17_STREAM    = 0x46;
const uint8_t  TYPE_M17_PACKET    = 0x47;
const uint8_t  TYPE_M17_LOST      = 0x48;
const uint8_t  TYPE_M17_EOT       = 0x49;

const uint8_t  MODE_IDLE          = 0x00;
const uint8_t  MODE_LOCKOUT       = 0x63;
const uint8_t  MODE_ERROR         = 0x64;
const uint8_t  MODE_QUIT          = 0x6E;
// ********************************************

extern bool             modem_trace;
extern bool             modem_debug;
extern bool             modem_duplex;
extern bool             modem_rxInvert;
extern bool             modem_txInvert;
extern bool             modem_pttInvert;
extern bool             modem_useCOSAsLockout;
extern unsigned int     modem_txDelay;
extern char             modem_rxFrequency[11];
extern char             modem_txFrequency[11];
extern float            modem_rxLevel;
extern float            modem_rfLevel;
extern int              modem_rxDCOffset;
extern int              modem_txDCOffset;

void enableM17();
void disableM17();
void enableDSTAR();
void disableDSTAR();
bool openMTtoMMDVM(uint8_t mmdvm_in, char* openmt_out);
bool mmdvmToOpenMT(const char* openmt_in, uint8_t mmdvm_out);
void setM17Space(uint8_t space);
uint8_t getM17Space();
void setDSTARSpace(uint8_t space);
uint8_t getDSTARSpace();
bool set_Config();
void setFrequency(const char* rxFreq, const char* txFreq, const char* pocsagFreq, uint8_t rfPower);

#endif /* MMDVM_H */
