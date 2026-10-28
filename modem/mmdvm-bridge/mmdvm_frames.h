/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

/*
 * Translation between the frames the protocol services speak to Modem_Host
 * (0x61, 16 bit length, type length, four character type, payload) and the
 * frames of an MMDVM modem (0xE0, length, type, payload).  This is the only
 * place that knows the names of the radio protocols; Modem_Host just relays
 * the service frames.
 */

#ifndef MMDVM_FRAMES_H
#define MMDVM_FRAMES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum
{
    PROTO_NONE = 0,
    PROTO_DSTAR,
    PROTO_DMR,
    PROTO_YSF,
    PROTO_P25,
    PROTO_NXDN,
    PROTO_M17,
    PROTO_FM
} Proto;

/* Finds the protocol of a mode name ("DMR", "dstar", ...); PROTO_NONE if unknown. */
Proto protoFromName(const char* name);

/* What a transmit frame does to the transmit state. */
typedef enum
{
    TX_STATE_KEEP = 0,
    TX_STATE_ON,
    TX_STATE_OFF
} TxState;

/*
 * Turns a service frame into a modem frame.
 *
 * Parameters:
 *   tlv    - The service frame, starting at its 0x61.
 *   tlvLen - Its length.
 *   out    - Receives the modem frame (at least 256 bytes).
 *   proto  - Receives the protocol the frame belongs to.
 *   tx     - Receives what the frame does to the transmit state.
 *   eot    - Receives true for a frame that ends a transmission.
 *
 * Returns: The modem frame length, 0 if the frame is not a transmit frame of
 *   any protocol (so the caller can look at it some other way).
 */
size_t mmdvmFromService(const uint8_t* tlv, size_t tlvLen, uint8_t* out, Proto* proto, TxState* tx, bool* eot);

/*
 * Turns a modem frame into a service frame.
 *
 * Parameters:
 *   frame    - The modem frame, starting at its 0xE0.
 *   frameLen - Its length.
 *   out      - Receives the service frame (at least 256 bytes).
 *   proto    - Receives the protocol the frame belongs to.
 *
 * Returns: The service frame length, 0 if the frame is not one a service
 *   gets (status, ACK, debug text, a frame of the wrong length).
 */
size_t serviceFromMmdvm(const uint8_t* frame, size_t frameLen, uint8_t* out, Proto* proto);

/*
 * If the service frame is a raw command for the modem ("2MOD": the payload
 * after the type is a complete modem frame body), builds the modem frame.
 *
 * Returns: The modem frame length, 0 if tlv is not such a frame.
 */
size_t mmdvmRawFromService(const uint8_t* tlv, size_t tlvLen, uint8_t* out);

/* The kind of service frame, for the bridge's own bookkeeping. */
typedef enum
{
    SVC_OTHER = 0,
    SVC_SET_MODE,       /* the service wants the modem for its mode   */
    SVC_SET_IDLE,       /* the service is done with the modem         */
    SVC_ACK
} SvcKind;

SvcKind serviceKind(const uint8_t* tlv, size_t tlvLen);

/* Free space the modem reported for each mode (from its status frame). */
uint8_t getSpace(Proto proto, uint8_t slot);
void    setSpacesFromStatus(const uint8_t* frame, size_t frameLen, int m17Index);

/* Frame pacing aids of NXDN and YSF: how long to wait before the next frame. */
uint32_t nxdnFrameDelay(uint8_t space);
uint32_t ysfFrameDelay(uint8_t space);


#endif /* MMDVM_FRAMES_H */
