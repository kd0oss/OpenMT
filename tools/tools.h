/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
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

#ifndef TOOLS_H
#define TOOLS_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <mariadb/mysql.h>
#include "string_builder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bit counting functions */
uint8_t countBits8(uint8_t bits);
uint8_t countBits16(uint16_t bits);
uint8_t countBits32(uint32_t bits);
uint8_t countBits64(uint64_t bits);

/* Configuration file reading */
char* readConfig(const char* section, const char* key, char* value);

/* String utility functions */
void splitString(const char* input, char delimiter, StringArray* result);
void replace_char(char* str, int length, char find, char replace);

/* Database functions - History and call logging */
bool saveHistory(const char* modem_name, const char* mode, const char* type, const char* src, const char* suffix,
                 const char* dst, float loss_BER, const char* message, uint16_t duration);
bool saveLastCall(uint8_t pos, const char* modem_name, const char* mode, const char* type, const char* src, const char* suffix,
                  const char* dst, const char* message, const char* sms, const char* gps, bool isTx);

/* Database functions - Host configuration */
bool readHostConfig(const char* modem_name, const char* module_name, const char* key, char* value);
bool setHostConfig(const char* modem_name, const char* module_name, const char* key, const char* display_type, const char* value);

/* Database functions - Mode management */
bool addMode(const char* modem_name, const char* module_name, const char* mode);
bool delMode(const char* modem_name, const char* module_name, const char* mode);

/* Database functions - Gateway management */
bool addGateway(const char* modem_name, const char* module_name, const char* mode);
bool delGateway(const char* modem_name, const char* module_name, const char* mode);

/* Database functions - Dashboard commands */
bool readDashbCommand(const char* modem_name, const char* command, char* value);
bool ackDashbCommand(const char* modem_name, const char* command, const char* result);
bool clearDashbCommands(const char* modem_name);

/* Database functions - Reflector management */
bool clearReflectorList(const char* type);
bool delReflector(const char* type, const char* name);
bool addReflector(const char* type, const char* name, const char* ip4Addr, const char* ip6Addr, uint16_t port,
                  const char* dashboardURL, const char* refl_title, const char* country);
bool findReflector(const char* type, const char* name, char* ip4, uint16_t* port);
bool setReflectorStatus(const char* modem_name, const char* module, const char* name, bool linked);
bool clearReflLinkStatus(const char* modem_name, const char* module);

/* Database functions - DMR ID lookup */
bool findDMRId(const unsigned int DMRId, char* callsign, char* name);

/* Database functions - Error logging and status */
bool logError(const char* modem_name, const char* module, const char* message);
bool setStatus(const char* modem_name, const char* module, const char* property, const char* value);
bool readStatus(const char* modem_name, const char* module, const char* property, char* value);
bool delStatus(const char* modem_name, const char* module, const char* property);

/* Database functions - GPS */
bool addGPS(const int id, const float latitude, const float longitude,
            const uint16_t altitude, const uint16_t bearing, const uint16_t speed);

#ifdef __cplusplus
}
#endif

#endif /* TOOLS_H */
