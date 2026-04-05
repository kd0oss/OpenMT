/*
 *   Copyright (C) 2015,2016,2017,2018,2020,2021,2025 by Jonathan Naylor G4KLX
 *   Converted to C by Rick KD0OSS 2025
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

#ifndef DMR_DIRECT_NETWORK_H
#define DMR_DIRECT_NETWORK_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>

#include "RingBuffer.h"
#include "dmr_data.h"
#include "timer.h"
#include "udp_socket.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hardware types */
typedef enum
{
    HW_TYPE_MMDVM = 0,
    HW_TYPE_DVMEGA,
    HW_TYPE_MMDVM_ZUMSPOT,
    HW_TYPE_MMDVM_HS_HAT,
    HW_TYPE_MMDVM_HS_DUAL_HAT,
    HW_TYPE_NANO_HOTSPOT,
    HW_TYPE_NANO_DV,
    HW_TYPE_D2RG_MMDVM_HS,
    HW_TYPE_MMDVM_HS
} HW_TYPE;

/* Connection status */
typedef enum
{
    STATUS_WAITING_CONNECT = 0,
    STATUS_WAITING_LOGIN,
    STATUS_WAITING_AUTHORISATION,
    STATUS_WAITING_CONFIG,
    STATUS_WAITING_OPTIONS,
    STATUS_RUNNING
} DMRNetworkStatus;

/* DMR Direct Network structure */
typedef struct
{
    char             address[256];
    uint16_t         port;
    struct sockaddr_storage addr;
    unsigned int     addrLen;
    uint8_t          id[4];
    char             password[256];
    bool             duplex;
    char             version[40];
    bool             debug;
    UDPSocket*       socket;
    bool             enabled;
    bool             slot1;
    bool             slot2;
    HW_TYPE          hwType;
    
    DMRNetworkStatus status;
    Timer*           retryTimer;
    Timer*           timeoutTimer;
    uint8_t*         buffer;
    uint32_t         streamId[2];
    uint8_t          salt[4];
    
    RingBuffer*      rxData;
    
    char             options[300];
    
    /* Random state for stream IDs */
    uint32_t         randState;
    
    char             callsign[16];
    uint32_t         rxFrequency;
    uint32_t         txFrequency;
    uint32_t         power;
    uint32_t         colorCode;
    float            latitude;
    float            longitude;
    int              height;
    char             location[64];
    char             description[64];
    char             url[256];
    bool             beacon;
    
} DMRDirectNetwork;

/* Constructor/Destructor */
DMRDirectNetwork* DMRDirectNetwork_create(
    const char* address,
    uint16_t port,
    const char* localAddress,
    uint16_t localPort,
    uint32_t id,
    const char* password,
    bool duplex,
    const char* version,
    bool slot1,
    bool slot2,
    HW_TYPE hwType,
    bool debug
);

void DMRDirectNetwork_destroy(DMRDirectNetwork* net);

/* Configuration */
void DMRDirectNetwork_setOptions(DMRDirectNetwork* net, const char* options);

void DMRDirectNetwork_setConfig(
    DMRDirectNetwork* net,
    const char* callsign,
    uint32_t rxFrequency,
    uint32_t txFrequency,
    uint32_t power,
    uint32_t colorCode,
    float latitude,
    float longitude,
    int height,
    const char* location,
    const char* description,
    const char* url
);

/* Network operations */
bool DMRDirectNetwork_open(DMRDirectNetwork* net);
void DMRDirectNetwork_enable(DMRDirectNetwork* net, bool enabled);
bool DMRDirectNetwork_read(DMRDirectNetwork* net, DMRData* data);
bool DMRDirectNetwork_write(DMRDirectNetwork* net, const DMRData* data);
bool DMRDirectNetwork_writeRadioPosition(DMRDirectNetwork* net, uint32_t id, const uint8_t* data);
bool DMRDirectNetwork_writeTalkerAlias(DMRDirectNetwork* net, uint32_t id, uint8_t type, const uint8_t* data);
bool DMRDirectNetwork_wantsBeacon(DMRDirectNetwork* net);
void DMRDirectNetwork_clock(DMRDirectNetwork* net, uint32_t ms);
bool DMRDirectNetwork_isConnected(const DMRDirectNetwork* net);
void DMRDirectNetwork_close(DMRDirectNetwork* net, bool sayGoodbye);

#ifdef __cplusplus
}
#endif

#endif /* DMR_DIRECT_NETWORK_H */
