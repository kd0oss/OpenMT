/*
 *   Copyright (C) 2015,2016,2017,2018,2020,2021,2024,2025 by Jonathan Naylor G4KLX
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

#include "dmrgateway.h"
#include "sha256.h"
#include "utils.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define BUFFER_LENGTH 500U
#define HOMEBREW_DATA_PACKET_LENGTH 55U

/* Forward declarations for private functions */
static bool writeLogin(DMRDirectNetwork* net);
static bool writeAuthorisation(DMRDirectNetwork* net);
static bool writeOptions(DMRDirectNetwork* net);
static bool writeConfig(DMRDirectNetwork* net);
static bool writePing(DMRDirectNetwork* net);
static bool write_data(DMRDirectNetwork* net, const uint8_t* data, uint32_t length);

/* Simple random number generator for stream IDs */
static uint32_t simple_rand(uint32_t* state)
{
    *state = (*state * 1103515245 + 12345) & 0x7FFFFFFF;
    return *state;
}

/* Constructor */
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
)
{
    assert(address != NULL);
    assert(port > 0);
    assert(id > 1000);
    assert(password != NULL);
    
    DMRDirectNetwork* net = (DMRDirectNetwork*)malloc(sizeof(DMRDirectNetwork));
    if (net == NULL)
        return NULL;
    
    /* Initialize members */
    memset(net, 0, sizeof(DMRDirectNetwork));
    
    strncpy(net->address, address, sizeof(net->address) - 1);
    net->port = port;
    strncpy(net->password, password, sizeof(net->password) - 1);
    net->duplex = duplex;
    strncpy(net->version, version, sizeof(net->version) - 1);
    net->debug = debug;
    net->slot1 = slot1;
    net->slot2 = slot2;
    net->hwType = hwType;
    
    /* Store ID in network byte order */
    net->id[0] = (id >> 24) & 0xFF;
    net->id[1] = (id >> 16) & 0xFF;
    net->id[2] = (id >> 8) & 0xFF;
    net->id[3] = id & 0xFF;
    
    /* Lookup address */
    if (UDPSocket_lookup(address, port, &net->addr, &net->addrLen) != 0)
        net->addrLen = 0;
    
    /* Allocate buffer */
    net->buffer = (uint8_t*)malloc(BUFFER_LENGTH);
    if (net->buffer == NULL)
    {
        free(net);
        return NULL;
    }
    
    /* Create socket */
    net->socket = UDPSocket_create(localAddress, localPort);
    if (net->socket == NULL)
    {
        free(net->buffer);
        free(net);
        return NULL;
    }
    
    /* Create ring buffer for RX data */
    RingBuffer_Init(&net->rxData, 1000);
    
    /* Create timers */
    net->retryTimer = Timer_create(1000, 10);
    net->timeoutTimer = Timer_create(1000, 60);
    
    /* Initialize random state with current time */
    net->randState = (uint32_t)time(NULL);
    
    /* Generate initial stream IDs */
    net->streamId[0] = simple_rand(&net->randState) | 0x00000001;
    net->streamId[1] = simple_rand(&net->randState) | 0x00000001;
    
    net->status = STATUS_WAITING_CONNECT;
    net->enabled = false;
    net->beacon = false;
    
    return net;
}

/* Destructor */
void DMRDirectNetwork_destroy(DMRDirectNetwork* net)
{
    if (net == NULL)
        return;
    
    if (net->socket != NULL)
        UDPSocket_destroy(net->socket);
    
    if (net->retryTimer != NULL)
        Timer_destroy(net->retryTimer);
    
    if (net->timeoutTimer != NULL)
        Timer_destroy(net->timeoutTimer);
    
    if (net->buffer != NULL)
        free(net->buffer);
    
    RingBuffer_Destroy(&net->rxData);
    
    free(net);
}

/* Set options */
void DMRDirectNetwork_setOptions(DMRDirectNetwork* net, const char* options)
{
    assert(net != NULL);
    assert(options != NULL);
    
    strncpy(net->options, options, sizeof(net->options) - 1);
}

/* Set configuration */
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
)
{
    assert(net != NULL);
    
    strncpy(net->callsign, callsign, sizeof(net->callsign) - 1);
    net->rxFrequency = rxFrequency;
    net->txFrequency = txFrequency;
    net->power = power;
    net->colorCode = colorCode;
    net->latitude = latitude;
    net->longitude = longitude;
    net->height = height;
    strncpy(net->location, location, sizeof(net->location) - 1);
    strncpy(net->description, description, sizeof(net->description) - 1);
    strncpy(net->url, url, sizeof(net->url) - 1);
}

/* Open network connection */
bool DMRDirectNetwork_open(DMRDirectNetwork* net)
{
    assert(net != NULL);
    
    if (net->addrLen == 0)
    {
        LogError("DMR, Could not lookup the address of the DMR Network");
        return false;
    }
    
    LogMessage("DMR, Opening DMR Network");
    
    net->status = STATUS_WAITING_CONNECT;
    Timer_stop(net->timeoutTimer);
    Timer_start(net->retryTimer);
    
    return UDPSocket_open(net->socket, &net->addr);
}

/* Enable/disable network */
void DMRDirectNetwork_enable(DMRDirectNetwork* net, bool enabled)
{
    assert(net != NULL);
    
    if (!enabled && net->enabled)
        RingBuffer_clear(&net->rxData);
    
    net->enabled = enabled;
}

/* Read data from network */
bool DMRDirectNetwork_read(DMRDirectNetwork* net, DMRData* data)
{
    assert(net != NULL);
    assert(data != NULL);
    
    if (net->status != STATUS_RUNNING)
        return false;
    
    if (RingBuffer_isEmpty(&net->rxData))
        return false;
    
    uint8_t length = 0;
    RingBuffer_getData(&net->rxData, &length, 1);
    RingBuffer_getData(&net->rxData, net->buffer, length);
    
    /* Check if this is a data packet */
    if (memcmp(net->buffer, "DMRD", 4) != 0)
        return false;
    
    uint8_t seqNo = net->buffer[4];
    
    uint32_t srcId = (net->buffer[5] << 16) | (net->buffer[6] << 8) | net->buffer[7];
    uint32_t dstId = (net->buffer[8] << 16) | (net->buffer[9] << 8) | net->buffer[10];
    
    uint32_t slotNo = (net->buffer[15] & 0x80) ? 2 : 1;
    
    /* DMO mode slot disabling */
    if (slotNo == 1 && !net->duplex)
        return false;
    
    /* Individual slot disabling */
    if (slotNo == 1 && !net->slot1)
        return false;
    if (slotNo == 2 && !net->slot2)
        return false;
    
    FLCO flco = (net->buffer[15] & 0x40) ? FLCO_USER_USER : FLCO_GROUP;
    
    DMRData_setSeqNo(data, seqNo);
    DMRData_setSlotNo(data, slotNo);
    DMRData_setSrcId(data, srcId);
    DMRData_setDstId(data, dstId);
    DMRData_setFLCO(data, flco);
    
    bool dataSync = (net->buffer[15] & 0x20) != 0;
    bool voiceSync = (net->buffer[15] & 0x10) != 0;
    
    if (dataSync)
    {
        uint8_t dataType = net->buffer[15] & 0x0F;
        DMRData_setData(data, net->buffer + 20);
        DMRData_setDataType(data, dataType);
        DMRData_setN(data, 0);
    }
    else if (voiceSync)
    {
        DMRData_setData(data, net->buffer + 20);
        DMRData_setDataType(data, DT_VOICE_SYNC);
        DMRData_setN(data, 0);
    }
    else
    {
        uint8_t n = net->buffer[15] & 0x0F;
        DMRData_setData(data, net->buffer + 20);
        DMRData_setDataType(data, DT_VOICE);
        DMRData_setN(data, n);
    }
    
    return true;
}

/* Write data to network */
bool DMRDirectNetwork_write(DMRDirectNetwork* net, const DMRData* data)
{
    assert(net != NULL);
    assert(data != NULL);
    
    if (net->status != STATUS_RUNNING)
        return false;
    
    uint8_t buffer[HOMEBREW_DATA_PACKET_LENGTH];
    memset(buffer, 0, HOMEBREW_DATA_PACKET_LENGTH);
    
    buffer[0] = 'D';
    buffer[1] = 'M';
    buffer[2] = 'R';
    buffer[3] = 'D';
    
    uint32_t srcId = DMRData_getSrcId(data);
    buffer[5] = (srcId >> 16) & 0xFF;
    buffer[6] = (srcId >> 8) & 0xFF;
    buffer[7] = srcId & 0xFF;
    
    uint32_t dstId = DMRData_getDstId(data);
    buffer[8] = (dstId >> 16) & 0xFF;
    buffer[9] = (dstId >> 8) & 0xFF;
    buffer[10] = dstId & 0xFF;
    
    memcpy(buffer + 11, net->id, 4);
    
    uint32_t slotNo = DMRData_getSlotNo(data);
    
    /* Individual slot disabling */
    if (slotNo == 1 && !net->slot1)
        return false;
    if (slotNo == 2 && !net->slot2)
        return false;
    
    buffer[15] = (slotNo == 1) ? 0x00 : 0x80;
    
    FLCO flco = DMRData_getFLCO(data);
    buffer[15] |= (flco == FLCO_GROUP) ? 0x00 : 0x40;
    
    uint32_t slotIndex = slotNo - 1;
    
    uint8_t dataType = DMRData_getDataType(data);
    if (dataType == DT_VOICE_SYNC)
    {
        buffer[15] |= 0x10;
    }
    else if (dataType == DT_VOICE)
    {
        buffer[15] |= DMRData_getN(data);
    }
    else
    {
        /* Generate new stream ID for headers */
        if (dataType == DT_VOICE_LC_HEADER)
            net->streamId[slotIndex] = simple_rand(&net->randState) | 0x00000001;
        
        if (dataType == DT_CSBK || dataType == DT_DATA_HEADER)
            net->streamId[slotIndex] = simple_rand(&net->randState) | 0x00000001;
        
        buffer[15] |= (0x20 | dataType);
    }
    
    buffer[4] = DMRData_getSeqNo(data);
    
    /* Copy stream ID */
    buffer[16] = (net->streamId[slotIndex] >> 24) & 0xFF;
    buffer[17] = (net->streamId[slotIndex] >> 16) & 0xFF;
    buffer[18] = (net->streamId[slotIndex] >> 8) & 0xFF;
    buffer[19] = net->streamId[slotIndex] & 0xFF;
    
    DMRData_getData(data, buffer + 20);
    
    buffer[53] = DMRData_getBER(data);
    buffer[54] = DMRData_getRSSI(data);
    
    return write_data(net, buffer, HOMEBREW_DATA_PACKET_LENGTH);
}

/* Write radio position */
bool DMRDirectNetwork_writeRadioPosition(DMRDirectNetwork* net, uint32_t id, const uint8_t* data)
{
    assert(net != NULL);
    assert(data != NULL);
    
    if (net->status != STATUS_RUNNING)
        return false;
    
    uint8_t buffer[20];
    
    memcpy(buffer, "DMRG", 4);
    
    buffer[4] = (id >> 16) & 0xFF;
    buffer[5] = (id >> 8) & 0xFF;
    buffer[6] = id & 0xFF;
    
    memcpy(buffer + 7, data + 2, 7);
    
    return write_data(net, buffer, 14);
}

/* Write talker alias */
bool DMRDirectNetwork_writeTalkerAlias(DMRDirectNetwork* net, uint32_t id, uint8_t type, const uint8_t* data)
{
    assert(net != NULL);
    assert(data != NULL);
    
    if (net->status != STATUS_RUNNING)
        return false;
    
    uint8_t buffer[20];
    
    memcpy(buffer, "DMRA", 4);
    
    buffer[4] = (id >> 16) & 0xFF;
    buffer[5] = (id >> 8) & 0xFF;
    buffer[6] = id & 0xFF;
    
    buffer[7] = type;
    
    memcpy(buffer + 8, data + 2, 7);
    
    return write_data(net, buffer, 15);
}

/* Check if network is connected */
bool DMRDirectNetwork_isConnected(const DMRDirectNetwork* net)
{
    assert(net != NULL);
    return (net->status == STATUS_RUNNING);
}

/* Close network connection */
void DMRDirectNetwork_close(DMRDirectNetwork* net, bool sayGoodbye)
{
    assert(net != NULL);
    
    LogMessage("DMR, Closing DMR Network");
    
    if (sayGoodbye && (net->status == STATUS_RUNNING))
    {
        uint8_t buffer[9];
        memcpy(buffer, "RPTCL", 5);
        memcpy(buffer + 5, net->id, 4);
        write_data(net, buffer, 9);
    }
    
    UDPSocket_close(net->socket);
    
    Timer_stop(net->retryTimer);
    Timer_stop(net->timeoutTimer);
}

/* This file continues in part 2... */


/* Clock function - main network processing */
void DMRDirectNetwork_clock(DMRDirectNetwork* net, uint32_t ms)
{
    assert(net != NULL);
    
    Timer_clock(net->retryTimer, ms);
    if (Timer_isRunning(net->retryTimer) && Timer_hasExpired(net->retryTimer))
    {
        switch (net->status)
        {
            case STATUS_WAITING_CONNECT:
                writeLogin(net);
                net->status = STATUS_WAITING_LOGIN;
                break;
            case STATUS_WAITING_LOGIN:
                writeLogin(net);
                break;
            case STATUS_WAITING_AUTHORISATION:
                writeAuthorisation(net);
                break;
            case STATUS_WAITING_OPTIONS:
                writeOptions(net);
                break;
            case STATUS_WAITING_CONFIG:
                writeConfig(net);
                break;
            case STATUS_RUNNING:
                writePing(net);
                break;
            default:
                break;
        }
        
        Timer_start(net->retryTimer);
    }
    
    struct sockaddr_storage address;
    unsigned int addrlen;
    int length = UDPSocket_read(net->socket, net->buffer, BUFFER_LENGTH, &address, &addrlen);
    
    if (length < 0)
    {
        LogError("DMR, Socket has failed, retrying connection to the master");
        DMRDirectNetwork_close(net, false);
        DMRDirectNetwork_open(net);
        return;
    }
    
    if (length > 0)
    {
        if (!UDPSocket_match(&net->addr, &address))
        {
            LogMessage("DMR, packet received from an invalid source");
            return;
        }
        
        if (net->debug)
            Utils_dump(1, "DMR, Network Received", net->buffer, length);
        
        /* Handle DMRD data packets */
        if (memcmp(net->buffer, "DMRD", 4) == 0)
        {
            if (net->enabled)
            {
                uint8_t len = (uint8_t)length;
                RingBuffer_addData(&net->rxData, &len, 1);
                RingBuffer_addData(&net->rxData, net->buffer, len);
            }
        }
        /* Handle MSTNAK - login failure */
        else if (memcmp(net->buffer, "MSTNAK", 6) == 0)
        {
            if (net->status == STATUS_RUNNING)
            {
                LogWarning("DMR, Login to the master has failed, retrying login ...");
                net->status = STATUS_WAITING_LOGIN;
                Timer_start(net->timeoutTimer);
                Timer_start(net->retryTimer);
            }
            else
            {
                LogError("DMR, Login to the master has failed, retrying network ...");
                DMRDirectNetwork_close(net, false);
                DMRDirectNetwork_open(net);
                return;
            }
        }
        /* Handle RPTACK - acknowledgments */
        else if (memcmp(net->buffer, "RPTACK", 6) == 0)
        {
            switch (net->status)
            {
                case STATUS_WAITING_LOGIN:
                    LogDebug("DMR, Sending authorisation");
                    memcpy(net->salt, net->buffer + 6, sizeof(uint32_t));
                    writeAuthorisation(net);
                    net->status = STATUS_WAITING_AUTHORISATION;
                    Timer_start(net->timeoutTimer);
                    Timer_start(net->retryTimer);
                    break;
                case STATUS_WAITING_AUTHORISATION:
                    LogDebug("DMR, Sending configuration");
                    writeConfig(net);
                    net->status = STATUS_WAITING_CONFIG;
                    Timer_start(net->timeoutTimer);
                    Timer_start(net->retryTimer);
                    break;
                case STATUS_WAITING_CONFIG:
                    if (net->options[0] == '\0')
                    {
                        LogMessage("DMR, Logged into the master successfully");
                        net->status = STATUS_RUNNING;
                    }
                    else
                    {
                        LogDebug("DMR, Sending options");
                        writeOptions(net);
                        net->status = STATUS_WAITING_OPTIONS;
                    }
                    Timer_start(net->timeoutTimer);
                    Timer_start(net->retryTimer);
                    break;
                case STATUS_WAITING_OPTIONS:
                    LogMessage("DMR, Logged into the master successfully");
                    net->status = STATUS_RUNNING;
                    Timer_start(net->timeoutTimer);
                    Timer_start(net->retryTimer);
                    break;
                default:
                    break;
            }
        }
        /* Handle MSTCL - master closing */
        else if (memcmp(net->buffer, "MSTCL", 5) == 0)
        {
            LogError("DMR, Master is closing down");
            DMRDirectNetwork_close(net, false);
            DMRDirectNetwork_open(net);
        }
        /* Handle MSTPONG - pong response */
        else if (memcmp(net->buffer, "MSTPONG", 7) == 0)
        {
            Timer_start(net->timeoutTimer);
        }
        /* Handle RPTSBKN - beacon request */
        else if (memcmp(net->buffer, "RPTSBKN", 7) == 0)
        {
            net->beacon = true;
        }
        else
        {
            Utils_dump("DMR, Unknown packet from the master", net->buffer, length);
        }
    }
    
    Timer_clock(net->timeoutTimer, ms);
    if (Timer_isRunning(net->timeoutTimer) && Timer_hasExpired(net->timeoutTimer))
    {
        LogError("DMR, Connection to the master has timed out, retrying connection");
        DMRDirectNetwork_close(net, false);
        DMRDirectNetwork_open(net);
    }
}

/* Check if beacon wanted */
bool DMRDirectNetwork_wantsBeacon(DMRDirectNetwork* net)
{
    assert(net != NULL);
    
    bool beacon = net->beacon;
    net->beacon = false;
    return beacon;
}

/* Private functions */

/* Write login packet */
static bool writeLogin(DMRDirectNetwork* net)
{
    uint8_t buffer[8];
    
    memcpy(buffer, "RPTL", 4);
    memcpy(buffer + 4, net->id, 4);
    
    return write_data(net, buffer, 8);
}

/* Write authorization packet */
static bool writeAuthorisation(DMRDirectNetwork* net)
{
    size_t passLen = strlen(net->password);
    
    uint8_t* in = (uint8_t*)malloc(passLen + sizeof(uint32_t));
    if (in == NULL)
        return false;
    
    memcpy(in, net->salt, sizeof(uint32_t));
    memcpy(in + sizeof(uint32_t), net->password, passLen);
    
    uint8_t out[40];
    memcpy(out, "RPTK", 4);
    memcpy(out + 4, net->id, 4);
    
    SHA256_buffer(in, (uint32_t)(passLen + sizeof(uint32_t)), out + 8);
    
    free(in);
    
    return write_data(net, out, 40);
}

/* Write options packet */
static bool writeOptions(DMRDirectNetwork* net)
{
    char buffer[300];
    
    memcpy(buffer, "RPTO", 4);
    memcpy(buffer + 4, net->id, 4);
    strcpy(buffer + 8, net->options);
    
    return write_data(net, (uint8_t*)buffer, (uint32_t)strlen(net->options) + 8);
}

/* Write configuration packet */
static bool writeConfig(DMRDirectNetwork* net)
{
    const char* software;
    char slots = '0';
    
    if (net->duplex)
    {
        if (net->slot1 && net->slot2)
            slots = '3';
        else if (net->slot1 && !net->slot2)
            slots = '1';
        else if (!net->slot1 && net->slot2)
            slots = '2';
        
        switch (net->hwType)
        {
            case HW_TYPE_MMDVM:
                software = "MMDVM";
                break;
            case HW_TYPE_MMDVM_HS:
                software = "MMDVM_MMDVM_HS";
                break;
            case HW_TYPE_MMDVM_HS_DUAL_HAT:
                software = "MMDVM_MMDVM_HS_Dual_Hat";
                break;
            case HW_TYPE_NANO_HOTSPOT:
                software = "MMDVM_Nano_hotSPOT";
                break;
            default:
                software = "MMDVM_Unknown";
                break;
        }
    }
    else
    {
        slots = '4';
        
        switch (net->hwType)
        {
            case HW_TYPE_MMDVM:
                software = "MMDVM_DMO";
                break;
            case HW_TYPE_DVMEGA:
                software = "MMDVM_DVMega";
                break;
            case HW_TYPE_MMDVM_ZUMSPOT:
                software = "MMDVM_ZUMspot";
                break;
            case HW_TYPE_MMDVM_HS_HAT:
                software = "MMDVM_MMDVM_HS_Hat";
                break;
            case HW_TYPE_MMDVM_HS_DUAL_HAT:
                software = "MMDVM_MMDVM_HS_Dual_Hat";
                break;
            case HW_TYPE_NANO_HOTSPOT:
                software = "MMDVM_Nano_hotSPOT";
                break;
            case HW_TYPE_NANO_DV:
                software = "MMDVM_Nano_DV";
                break;
            case HW_TYPE_D2RG_MMDVM_HS:
                software = "MMDVM_D2RG_MMDVM_HS";
                break;
            case HW_TYPE_MMDVM_HS:
                software = "MMDVM_MMDVM_HS";
                break;
            default:
                software = "MMDVM_Unknown";
                break;
        }
    }
    
    char buffer[400];
    
    memcpy(buffer, "RPTC", 4);
    memcpy(buffer + 4, net->id, 4);
    
    char latitude[20];
    snprintf(latitude, sizeof(latitude), "%08f", net->latitude);
    
    char longitude[20];
    snprintf(longitude, sizeof(longitude), "%09f", net->longitude);
    
    uint32_t power = net->power;
    if (power > 99)
        power = 99;
    
    int height = net->height;
    if (height > 999)
        height = 999;
    
    snprintf(buffer + 8, 394,
        "%-8.8s%09u%09u%02u%02u%8.8s%9.9s%03d%-20.20s%-19.19s%c%-124.124s%-40.40s%-40.40s",
        net->callsign,
        net->rxFrequency,
        net->txFrequency,
        power,
        net->colorCode,
        latitude,
        longitude,
        height,
        net->location,
        net->description,
        slots,
        net->url,
        net->version,
        software
    );
    
    return write_data(net, (uint8_t*)buffer, 302);
}

/* Write ping packet */
static bool writePing(DMRDirectNetwork* net)
{
    uint8_t buffer[11];
    
    memcpy(buffer, "RPTPING", 7);
    memcpy(buffer + 7, net->id, 4);
    
    return write_data(net, buffer, 11);
}

/* Write data to socket */
static bool write_data(DMRDirectNetwork* net, const uint8_t* data, uint32_t length)
{
    assert(net != NULL);
    assert(data != NULL);
    assert(length > 0);
    
    if (net->debug)
        Utils_dump(1, "DMR Network Transmitted", data, length);
    
    bool ret = UDPSocket_write(net->socket, data, length, &net->addr, net->addrLen);
    if (!ret)
    {
        LogError("DMR, Socket has failed when writing data to the master, retrying connection");
        DMRDirectNetwork_close(net, false);
        DMRDirectNetwork_open(net);
        return false;
    }
    
    return true;
}
