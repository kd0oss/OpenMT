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

/*
 * Created by Rick KD0OSS
 *
 * EchoLink protocol pieces used by fm_gateway: the RTP/RTCP packets of a
 * connection (SDES keep-alive, BYE, GSM voice, info text), the directory
 * server login and station list, and the GSM codec. The packet layouts follow
 * the EchoLink protocol as implemented by SvxLink's echolib.
 */

#ifndef ELINK_H
#define ELINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EL_AUDIO_PORT        5198U
#define EL_CTRL_PORT         5199U
#define EL_DIR_PORT          5200U
#define EL_FRAMES            4U
#define EL_FRAME_SAMPLES     160U
#define EL_PACKET_SAMPLES    (EL_FRAMES * EL_FRAME_SAMPLES)
#define EL_GSM_FRAME_BYTES   33U
#define EL_VOICE_HEADER      12U
#define EL_VOICE_LENGTH      (EL_VOICE_HEADER + EL_FRAMES * EL_GSM_FRAME_BYTES)
#define EL_SDES_MAX          256U
#define EL_CALL_LEN          32U

#define EL_SDES_CNAME        1
#define EL_SDES_NAME         2
#define EL_SDES_EMAIL        3
#define EL_SDES_PHONE        4
#define EL_SDES_PRIV         8

/* One station of the directory list. */
typedef struct
{
    char     call[EL_CALL_LEN];
    uint32_t id;
    char     ip[16];
    bool     busy;
} ElStation;

/* The directory station list. */
typedef struct
{
    ElStation* list;
    int        count;
    time_t     fetched;
    char       message[256];
} ElDirList;

/* The GSM encoder and decoder of one connection. */
typedef struct
{
    void* enc;
    void* dec;
} ElCodec;

bool elCodecInit(ElCodec* c);
void elCodecFree(ElCodec* c);

int  elMakeSdes(uint8_t* p, const char* callsign, const char* name, const char* priv);
int  elMakeBye(uint8_t* p);
bool elIsBye(const uint8_t* p, int len);
bool elIsSdes(const uint8_t* p, int len);
bool elParseSdes(char* text, size_t textLen, const uint8_t* p, int len, uint8_t item);
void elSanitizeCallsign(char* call);
bool elStationCall(const uint8_t* p, int len, char* call, size_t callLen);

int  elMakeVoice(ElCodec* c, uint8_t* out, uint16_t seq, const int16_t* samples);
bool elParseVoice(ElCodec* c, const uint8_t* p, int len, int16_t* samples);
int  elMakeInfo(uint8_t* out, size_t outLen, const char* text);
bool elIsInfo(const uint8_t* p, int len);

int  elMakeLogin(char* out, size_t outLen, const char* callsign, const char* password, const char* state,
                 const char* description, const char* clock);
bool elParseList(const char* text, size_t len, ElDirList* dir);
void elFreeList(ElDirList* dir);
const ElStation* elFindStation(const ElDirList* dir, const char* name);

bool elDirRegister(const char* server, const char* callsign, const char* password, const char* state,
                   const char* description, char* reply, size_t replyLen);
bool elDirFetch(const char* server, ElDirList* dir, const char* cachePath);
bool elLoadCache(const char* cachePath, ElDirList* dir);

#ifdef __cplusplus
}
#endif

#endif
