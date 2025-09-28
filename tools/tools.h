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

#ifndef TOOLS_H
#define TOOLS_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string>
#include <ctype.h>
#include <vector>
#include <mariadb/mysql.h>

std::vector<std::string> splitString(const std::string& input, char delimiter);
int saveHistory(const char* mode, const char* type, const char* src,
                const char* dst, float loss_BER, const char* message, uint16_t duration);
int saveLastCall(const char* mode, const char* type, const char* src,
                 const char* dst, const char* message, const char* sms, const char* gps, bool isTx);
std::string readHostConfig(const char* module_name, const char* key);
std::string readModemConfig(const char* modem_name, const char* key);
std::string readDashbCommand(const char* command);
bool addMode(const char* module_name, const char* mode);
bool delMode(const char* module_name, const char* mode);
bool addGateway(const char* module_name, const char* mode);
bool delGateway(const char* module_name, const char* mode);
int8_t setHostConfig(const char* module_name, const char* key, const char* value);
bool ackDashbCommand(const char* command, const char* result);
bool clearDashbCommands();
bool findReflector(const char* type, const char* name, char* ip4, uint16_t* port);
bool setReflectorStatus(const char* type, const char* name, const char* module);
bool clearReflectorList(const char* type);
bool delReflector(const char* type, const char* name);
bool clearReflLinkStatus(const char* type);
bool addReflector(const char* type, const char* name, const char* ip4Addr, const char* ip6Addr, uint16_t port,
                  const char* dashboardURL, const char* refl_title, const char* country);
bool logError(const char* module, const char* message);
bool addStatus(const char* property, const char* value);
bool updateStatus(const char* property, const char* value);
bool delStatus(const char* property);
bool addGPS(const int id, const float latitude, const float longitude,
            const uint16_t altitude, const uint16_t bearing, const uint16_t speed);

#endif /* TOOLS_H */
