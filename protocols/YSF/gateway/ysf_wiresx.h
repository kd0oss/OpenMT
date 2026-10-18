/*
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

#ifndef YSF_WIRESX_H
#define YSF_WIRESX_H

#include "../ysf_func.h"
#include "ysf_reflist.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wires-X for the YSF gateway, after G4KLX's WiresX.cpp.
 *
 * The radio sends its Wires-X requests (DX, ALL/SEARCH, CONNECT, DISCONNECT,
 * CATEGORY) as data FR mode frames.  wxProcess() collects them and tells the
 * caller what to do; the answers are made as frames, one second after the
 * request, and handed to the send function at one frame per 90 ms, ready for
 * the host to transmit.
 */
typedef enum
{
    WX_NONE = 0,
    WX_CONNECT_YSF,
    WX_CONNECT_FCS,
    WX_DISCONNECT,
    WX_RECONNECT_CURRENT
} WxStatus;

typedef enum
{
    WXSI_NONE = 0,
    WXSI_DX,
    WXSI_CONNECT,
    WXSI_DISCONNECT,
    WXSI_ALL,
    WXSI_SEARCH,
    WXSI_CATEGORY
} WxPending;

/* body: the 151 byte host packet body (gateway, source, destination, counter, frame) */
typedef void (*WxSendFn)(void* ctx, const uint8_t* body, bool end);

#define WX_TX_QUEUE 192U

typedef struct
{
    char callsign[11];          /* 10 characters, space padded         */
    char node[11];              /* callsign-suffix                      */
    char name[15];              /* 14 characters                        */
    char id[6];                 /* five digits made from the name       */
    RefList* reflectors;
    const Reflector* reflector; /* the reflector linked now             */
    uint8_t command[300];
    uint32_t txFrequency;
    uint32_t rxFrequency;
    WxPending status;
    uint32_t timerMs;
    bool timerRunning;
    uint8_t seqNo;
    uint8_t csd1[20];
    uint8_t csd2[20];
    uint8_t csd3[20];
    uint8_t header[34];
    unsigned int start;
    char search[17];
    const Reflector* category[20];
    unsigned int categoryCount;
    bool busy;
    uint32_t busyMs;
    uint32_t txMs;
    uint8_t queue[WX_TX_QUEUE][152];    /* end flag, then the 151 byte body */
    unsigned int qHead;
    unsigned int qTail;
    WxSendFn send;
    void* ctx;
} WiresX;

void wxInit(WiresX* w, const char* callsign, const char* suffix, RefList* reflectors, WxSendFn send, void* ctx);
void wxSetInfo(WiresX* w, const char* name, uint32_t txFrequency, uint32_t rxFrequency);

/* One frame from the radio (120 bytes) and the 10 character source callsign of its call */
WxStatus wxProcess(WiresX* w, const uint8_t* frame, const uint8_t* source);

void wxClock(WiresX* w, uint32_t ms);
bool wxIsBusy(const WiresX* w);

const Reflector* wxGetReflector(const WiresX* w);
void wxSetReflector(WiresX* w, const Reflector* reflector);
void wxProcessConnect(WiresX* w, const Reflector* reflector);
void wxProcessDisconnect(WiresX* w);

/* Queues the data FR frames that carry 'data' (a Wires-X request or answer); used by tests to play a radio */
void wxCreateCommand(WiresX* w, const uint8_t* data, unsigned int length);

#ifdef __cplusplus
}
#endif

#endif
