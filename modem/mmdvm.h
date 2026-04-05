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
#include <stdbool.h>
#include <ctype.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MMDVM specific header values */
#define MODEM_VERSION      0x00
#define MODEM_STATUS       0x01
#define MODEM_CONFIG       0x02
#define MODEM_MODE         0x03
#define MODEM_SET_FREQ     0x04
#define MODEM_ACK          0x70
#define MODEM_NACK         0x7F
#define MODEM_FRAME_START  0xE0
#define MODEM_DEBUG        0xF1

#define TYPE_DSTAR_HEADER  0x10
#define TYPE_DSTAR_DATA    0x11
#define TYPE_DSTAR_LOST    0x12
#define TYPE_DSTAR_EOT     0x13
#define TYPE_M17_LSF       0x45
#define TYPE_M17_STREAM    0x46
#define TYPE_M17_PACKET    0x47
#define TYPE_M17_LOST      0x48
#define TYPE_M17_EOT       0x49
#define TYPE_P25_HDR       0x30U
#define TYPE_P25_LDU       0x31U
#define TYPE_P25_LOST      0x32U
#define TYPE_DMR_DATA1     0x18U
#define TYPE_DMR_LOST1     0x19U
#define TYPE_DMR_DATA2     0x1AU
#define TYPE_DMR_LOST2     0x1BU
#define TYPE_DMR_SHORTLC   0x1CU
#define TYPE_DMR_START     0x1DU
#define TYPE_DMR_ABORT     0x1EU

#define MODE_IDLE          0x00
#define MODE_LOCKOUT       0x63
#define MODE_ERROR         0x64
#define MODE_QUIT          0x6E
/* End of MMDVM constants */

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
extern uint8_t          modem_rxLevel;
extern uint8_t          modem_rfLevel;
extern int              modem_rxDCOffset;
extern int              modem_txDCOffset;

void enableM17(const char* modem_name);
void disableM17(const char* modem_name);
void enableDSTAR(const char* modem_name);
void disableDSTAR(const char* modem_name);
void enableP25(const char* modem_name);
void disableP25(const char* modem_name);
void enableDMR(const char* modem_name);
void disableDMR(const char* modem_name);
bool openMTtoMMDVM(uint8_t mmdvm_in, char* openmt_out);
bool mmdvmToOpenMT(const char* openmt_in, uint8_t mmdvm_out);
void setM17Space(uint8_t space);
uint8_t getM17Space();
void setDSTARSpace(uint8_t space);
uint8_t getDSTARSpace();
void setP25Space(uint8_t space);
uint8_t getP25Space();
void setDMRSpace(uint8_t space1, uint8_t space2);
uint8_t getDMRSpace(uint8_t slot);
bool set_Config(const char* modem_name);
bool set_ConfigHS(const char* modem_name);
void setFrequency(const char* rxFreq, const char* txFreq, const char* pocsagFreq, uint8_t rfPower);

#ifdef __cplusplus
}
#endif

#endif /* MMDVM_H */
