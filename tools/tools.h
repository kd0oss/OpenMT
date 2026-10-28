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
#include <pthread.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <mariadb/mysql.h>
#include "string_builder.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_STR_LEN 128

// Struct to hold the system details
typedef struct
{
    char model[MAX_STR_LEN];
    char os_name[MAX_STR_LEN];
} PiSystemInfo;

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
bool readHostConfigN(const char* modem_name, const char* module_name, const char* key, char* value, size_t size);
bool setHostConfig(const char* modem_name, const char* module_name, const char* key, const char* display_type, const char* value);
bool addHostConfigDefault(const char* modem_name, const char* module_name, const char* key, const char* display_type, const char* value);
bool hostConfigDigest(const char* modem_name, const char* module_name, uint64_t* digest);

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
bool sendDashbCommand(const char* modem_name, const char* command, const char* parameter, char* result, size_t size,
                      int timeoutSec);

/* Database functions - Reflector management */
bool clearReflectorList(const char* type);
bool delReflector(const char* type, const char* name);
bool addReflector(const char* type, const char* name, const char* ip4Addr, const char* ip6Addr, uint16_t port,
                  const char* dashboardURL, const char* refl_title, const char* country);
bool findReflector(const char* type, const char* name, char* ip4, uint16_t* port);
bool setReflectorStatus(const char* modem_name, const char* module, const char* name, bool linked);
bool clearReflLinkStatus(const char* modem_name, const char* module);

/* Database functions - DMR ID lookup */
bool findDMRId(const unsigned int DMRId, char* callsign, size_t callsignSize, char* name, size_t nameSize);

/* Database functions - Error logging and status */
bool logError(const char* modem_name, const char* module, const char* message);
void reportError(const char* modem_name, const char* module, const char* format, ...) __attribute__((format(printf, 3, 4)));
void errorFlush(void);
bool setStatus(const char* modem_name, const char* module, const char* property, const char* value);
bool readStatus(const char* modem_name, const char* module, const char* property, char* value);
bool delStatus(const char* modem_name, const char* module, const char* property);

/* Database functions - GPS */
bool addGPS(const int id, const float latitude, const float longitude,
            const uint16_t altitude, const uint16_t bearing, const uint16_t speed);

/* Timing helpers */
void     delay(uint32_t us);
uint64_t nowMs(void);

/* Socket and logging helpers */
void setNoDelay(int fd);
bool resolveAddress(const char* host, uint16_t port, struct sockaddr_in* addr);
void logMsg(const char* fmt, ...);
void hexDump(const char* text, const uint8_t* data, unsigned int length);
void modemWriteBits(int fd, uint8_t c, bool isEOT);
void modemQueueSamples(int fd, const int16_t* samples, unsigned int count,
                       bool isEOT, uint32_t packetDelayUs);

/* Software timers (millisecond ticks driven by the host's timer thread) */
#define TIMER_COUNT 10

typedef struct TIMERS
{
    char     name[20];
    bool     valid;
    bool     enabled;
    uint32_t duration;
    uint32_t count;
    bool     triggered;
} TIMERS;

extern TIMERS          timer[TIMER_COUNT];
extern pthread_mutex_t timerMutex;

void     initTimers(void);
int8_t   getTimer(const char* name, uint32_t duration);
uint32_t getTimerDuration(const char* name);
bool     isTimerTriggered(const char* name);
void     resetTimer(const char* name, const uint32_t duration);
void     disableTimer(uint8_t id);
/* How long host threads sleep between passes when they only watch a timer or a buffer. The threads
   that pace frames (about every 15 ms) and the timer threads (about every 10 ms) used to wake every
   0.1 to 1 ms, which kept a Raspberry Pi busy doing nothing. */
#define HOST_TIMER_PERIOD_US 10000U
#define HOST_POLL_US         15000U
#define HOST_PACE_US         2000U

void     timerTick(uint8_t id);
void     timerTickN(uint8_t id, uint32_t ticks);
uint32_t timerElapsedMs(uint64_t* last);
void     sleepUntilNext(uint64_t* next, uint32_t periodMs);
bool     hostToAddress(const char* host, struct in_addr* out);
bool     dbOption(int opt, char* arg);
const char* configFilePath(void);
bool     dbConfigOverride(const char* section, const char* key, char* value);
const char* dbUser(void);

PiSystemInfo interrogate_pi();

#ifdef __cplusplus
}
#endif

#endif /* TOOLS_H */
