/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

/*
 * Service frame <-> MMDVM frame translation of mmdvm_bridge.  The byte
 * layouts are those Modem_Host used before the bridge took them over
 * (processClientSocket() and processMMDVMSerial()), including their quirks.
 */

#include <string.h>
#include <strings.h>

#include "mmdvm.h"
#include "mmdvm_frames.h"

#define SVC_START 0x61U

#define COMM_SET_MODE 0x02U
#define COMM_SET_IDLE 0x03U
#define NET_ACK       0xF0U
#define MMDVM_START   0xE0U

#define MODEM_STATUS_TYPE 0x01U

/* NXDN: one frame is a whole 80 ms air frame and the modem queue is deep, so
 * the aim is a small, steady queue (largest free space ever reported minus
 * the free space now). */
#define NXDN_QUEUE_MIN      3U     /* frames queued before the call is paced in real time */
#define NXDN_QUEUE_MAX      5U     /* frames queued above which the sender holds back */
#define NXDN_FRAME_MS       80U    /* one frame on the air */
#define NXDN_FILL_MS        10U    /* pause between frames while building the queue */

#define YSF_QUEUE_MIN       2U
#define YSF_QUEUE_MAX       4U
#define YSF_FRAME_MS        100U
#define YSF_FILL_MS         10U

/*
 * protoFromName()
 *
 * Finds the protocol of a mode name.  Only the first two letters count, as
 * they did when the host matched a service to its frames.
 *
 * Parameters:
 *   name - Mode name the service registered with.
 *
 * Returns: The protocol, PROTO_NONE if the name is not one of the known ones.
 */
Proto protoFromName(const char* name)
{
    if (name == NULL || name[0] == '\0' || name[1] == '\0')
        return PROTO_NONE;
    if (strncasecmp(name, "DS", 2) == 0) return PROTO_DSTAR;
    if (strncasecmp(name, "DM", 2) == 0) return PROTO_DMR;
    if (strncasecmp(name, "YS", 2) == 0) return PROTO_YSF;
    if (strncasecmp(name, "P2", 2) == 0) return PROTO_P25;
    if (strncasecmp(name, "NX", 2) == 0) return PROTO_NXDN;
    if (strncasecmp(name, "M1", 2) == 0) return PROTO_M17;
    if (strncasecmp(name, "FM", 2) == 0) return PROTO_FM;
    return PROTO_NONE;
}

/*
 * getSpace()
 *
 * Reports how many frames the modem can still take for a protocol, from the
 * last status reply.  20 for a protocol without a queue (FM, unknown).
 *
 * Parameters:
 *   proto - The protocol.
 *   slot  - DMR slot, 1 or 2; ignored for the others.
 *
 * Returns: Free frame slots.
 */
uint8_t getSpace(Proto proto, uint8_t slot)
{
    switch (proto)
    {
        case PROTO_M17:   return getM17Space();
        case PROTO_DSTAR: return getDSTARSpace();
        case PROTO_P25:   return getP25Space();
        case PROTO_YSF:   return getYSFSpace();
        case PROTO_NXDN:  return getNXDNSpace();
        case PROTO_DMR:   return getDMRSpace(slot == 2 ? 2 : 1);
        default:          return 20;
    }
}

/*
 * setSpacesFromStatus()
 *
 * Takes the free space of each mode from a status reply (E0, length, 01,
 * ...): D-Star in byte 6, DMR slots in 7 and 8, YSF 9, P25 10, NXDN 11 and
 * M17 at the position the board uses.
 *
 * Parameters:
 *   frame    - The status frame.
 *   frameLen - Its length.
 *   m17Index - Position of the M17 space (13 on a hotspot board, 12 on a
 *              full board).
 *
 * Returns: nothing.
 */
void setSpacesFromStatus(const uint8_t* frame, size_t frameLen, int m17Index)
{
    if (frameLen > 11)
    {
        setDSTARSpace(frame[6]);
        setDMRSpace(frame[7], frame[8]);
        setP25Space(frame[10]);
        setYSFSpace(frame[9]);
        setNXDNSpace(frame[11]);
    }
    if (m17Index > 0 && (size_t)m17Index < frameLen)
        setM17Space(frame[m17Index]);
}

/*
 * nxdnFrameDelay()
 *
 * Works out how long to wait before sending the next NXDN frame.  Below
 * NXDN_QUEUE_MIN frames queued the call is being filled quickly; above
 * NXDN_QUEUE_MAX the sender holds back; in between it runs just under real
 * time.
 *
 * Parameters:
 *   space - Free frames the modem reported now.
 *
 * Returns: The delay in milliseconds.
 */
uint32_t nxdnFrameDelay(const uint8_t space)
{
    static uint8_t peak = 0U;

    if (space > peak)
        peak = space;

    uint8_t queued = peak > space ? (uint8_t)(peak - space) : 0U;

    if (queued < NXDN_QUEUE_MIN)
        return NXDN_FILL_MS;                 /* start of a call or after a gap: fill quickly */
    if (queued > NXDN_QUEUE_MAX)
        return NXDN_FRAME_MS + 10U;          /* running ahead: let the modem drain a little */
    return NXDN_FRAME_MS - 10U;              /* in the band: just under real time keeps the queue topped up */
}

/*
 * ysfFrameDelay()
 *
 * The same as nxdnFrameDelay() for YSF, whose frame is 100 ms of air time.
 *
 * Parameters:
 *   space - Free frames the modem reported now.
 *
 * Returns: The delay in milliseconds.
 */
uint32_t ysfFrameDelay(const uint8_t space)
{
    static uint8_t peak = 0U;

    if (space > peak)
        peak = space;

    uint8_t queued = peak > space ? (uint8_t)(peak - space) : 0U;

    if (queued < YSF_QUEUE_MIN)
        return YSF_FILL_MS;
    if (queued > YSF_QUEUE_MAX)
        return YSF_FRAME_MS + 10U;
    return YSF_FRAME_MS - 10U;
}

/*
 * typeIs()
 *
 * Compares the four character type of a service frame.
 *
 * Parameters:
 *   tlv  - The service frame.
 *   name - The four characters to look for.
 *
 * Returns: true if the frame has that type.
 */
static bool typeIs(const uint8_t* tlv, const char* name)
{
    return tlv[3] == 4 && memcmp(tlv + 4, name, 4) == 0;
}

/*
 * serviceKind()
 *
 * Tells which of the frames the bridge itself acts on a service frame is.
 *
 * Parameters:
 *   tlv    - The service frame.
 *   tlvLen - Its length.
 *
 * Returns: SVC_SET_MODE, SVC_SET_IDLE, SVC_ACK or SVC_OTHER.
 */
SvcKind serviceKind(const uint8_t* tlv, size_t tlvLen)
{
    if (tlvLen < 5 || tlv[0] != SVC_START)
        return SVC_OTHER;
    if (tlv[3] == 1)
    {
        if (tlv[4] == COMM_SET_MODE) return SVC_SET_MODE;
        if (tlv[4] == COMM_SET_IDLE) return SVC_SET_IDLE;
        if (tlv[4] == NET_ACK)       return SVC_ACK;
    }
    return SVC_OTHER;
}

/*
 * mmdvmRawFromService()
 *
 * "2MOD": a service that wants to send the modem a frame of its own.  The
 * payload begins with the modem frame's type byte.
 *
 * Parameters:
 *   tlv    - The service frame.
 *   tlvLen - Its length.
 *   out    - Receives the modem frame.
 *
 * Returns: The modem frame length, 0 if this is not a 2MOD frame.
 */
size_t mmdvmRawFromService(const uint8_t* tlv, size_t tlvLen, uint8_t* out)
{
    size_t n;

    if (tlvLen < 9 || tlv[0] != SVC_START || !typeIs(tlv, "2MOD"))
        return 0;
    n = tlvLen - 8 + 2;
    if (n > 255)
        return 0;
    out[0] = MMDVM_START;
    out[1] = (uint8_t)n;
    memcpy(out + 2, tlv + 8, tlvLen - 8);
    return n;
}

/*
 * mmdvmFromService()
 *
 * Builds the modem frame for a transmit frame of a service.  The layouts:
 *   M17     E0 34 type 00 + 48 bytes (a short end frame is padded with zeros)
 *   D-Star  header E0 2C type + 41, data E0 0F type + 12, end E0 03 type
 *   P25     header E0 len type + payload, LDU E0 DB type + 216, lost E0 03 type
 *   YSF/NXDN data E0 len type + payload, lost E0 03 type
 *   DMR     E0 len + payload (the payload starts with the modem frame type)
 *
 * Parameters:
 *   tlv, tlvLen - The service frame.
 *   out         - Receives the modem frame.
 *   proto       - Receives the protocol.
 *   tx          - Receives the effect on the transmit state.
 *   eot         - Receives true when the frame ends the transmission.
 *
 * Returns: The modem frame length, 0 if the frame is none of these.
 */
size_t mmdvmFromService(const uint8_t* tlv, size_t tlvLen, uint8_t* out, Proto* proto, TxState* tx, bool* eot)
{
    const uint8_t* pl;
    size_t         plen;
    uint8_t        type = 0;

    *proto = PROTO_NONE;
    *tx    = TX_STATE_KEEP;
    *eot   = false;

    if (tlvLen < 8 || tlv[0] != SVC_START || tlv[3] != 4)
        return 0;
    pl   = tlv + 8;
    plen = tlvLen - 8;
    out[0] = MMDVM_START;

    if (typeIs(tlv, "M17L") || typeIs(tlv, "M17S") || typeIs(tlv, "M17P") || typeIs(tlv, "M17E"))
    {
        type = typeIs(tlv, "M17L") ? TYPE_M17_LSF : typeIs(tlv, "M17S") ? TYPE_M17_STREAM :
               typeIs(tlv, "M17P") ? TYPE_M17_PACKET : TYPE_M17_EOT;
        out[1] = 0x34;
        out[2] = type;
        out[3] = 0x00;
        memset(out + 4, 0, 48);
        memcpy(out + 4, pl, plen < 48 ? plen : 48);
        *proto = PROTO_M17;
        *tx    = type == TYPE_M17_EOT ? TX_STATE_OFF : TX_STATE_ON;
        *eot   = type == TYPE_M17_EOT;
        return 0x34;
    }
    if (typeIs(tlv, "DSTH"))
    {
        out[1] = 0x2C;
        out[2] = TYPE_DSTAR_HEADER;
        memset(out + 3, 0, 41);
        memcpy(out + 3, pl, plen < 41 ? plen : 41);
        *proto = PROTO_DSTAR;
        *tx    = TX_STATE_ON;
        return 0x2C;
    }
    if (typeIs(tlv, "DSTD"))
    {
        out[1] = 0x0F;
        out[2] = TYPE_DSTAR_DATA;
        memset(out + 3, 0, 12);
        memcpy(out + 3, pl, plen < 12 ? plen : 12);
        *proto = PROTO_DSTAR;
        *tx    = TX_STATE_ON;
        return 0x0F;
    }
    if (typeIs(tlv, "DSTE") || typeIs(tlv, "P25E") || typeIs(tlv, "YSFL") || typeIs(tlv, "NXDL"))
    {
        if (typeIs(tlv, "DSTE")) { type = TYPE_DSTAR_EOT; *proto = PROTO_DSTAR; }
        else if (typeIs(tlv, "P25E")) { type = TYPE_P25_LOST; *proto = PROTO_P25; }
        else if (typeIs(tlv, "YSFL")) { type = TYPE_YSF_LOST; *proto = PROTO_YSF; }
        else { type = TYPE_NXDN_LOST; *proto = PROTO_NXDN; }
        out[1] = 0x03;
        out[2] = type;
        *tx  = TX_STATE_OFF;
        *eot = true;
        return 3;
    }
    if (typeIs(tlv, "P25H") || typeIs(tlv, "YSFD") || typeIs(tlv, "NXDD"))
    {
        if (typeIs(tlv, "P25H")) { type = TYPE_P25_HDR; *proto = PROTO_P25; }
        else if (typeIs(tlv, "YSFD")) { type = TYPE_YSF_DATA; *proto = PROTO_YSF; }
        else { type = TYPE_NXDN_DATA; *proto = PROTO_NXDN; }
        if (plen + 3 > 255)
            return 0;
        out[1] = (uint8_t)(plen + 3);
        out[2] = type;
        memcpy(out + 3, pl, plen);
        *tx = TX_STATE_ON;
        return plen + 3;
    }
    if (typeIs(tlv, "P25L"))
    {
        out[1] = 216 + 3;
        out[2] = TYPE_P25_LDU;
        memset(out + 3, 0, 216);
        memcpy(out + 3, pl, plen < 216 ? plen : 216);
        *proto = PROTO_P25;
        *tx    = TX_STATE_ON;
        return 219;
    }
    if (typeIs(tlv, "DMD1") || typeIs(tlv, "DMD2") || typeIs(tlv, "DML1") || typeIs(tlv, "DML2"))
    {
        bool lost = typeIs(tlv, "DML1") || typeIs(tlv, "DML2");

        if (plen + 2 > 255)
            return 0;
        out[1] = (uint8_t)(plen + 2);
        memcpy(out + 2, pl, plen);
        *proto = PROTO_DMR;
        *tx    = TX_STATE_ON;
        if ((plen > 0 && pl[0] == 0x42) || lost)
        {
            *tx  = TX_STATE_OFF;
            *eot = true;
        }
        return plen + 2;
    }
    return 0;
}

/*
 * wrap()
 *
 * Starts a service frame of a four character type in out.
 *
 * Parameters:
 *   out   - The frame being built.
 *   total - Total length of the service frame.
 *   name  - The four character type.
 *
 * Returns: nothing.
 */
static void wrap(uint8_t* out, size_t total, const char* name)
{
    out[0] = SVC_START;
    out[1] = (uint8_t)(total >> 8);
    out[2] = (uint8_t)total;
    out[3] = 0x04;
    memcpy(out + 4, name, 4);
}

/*
 * serviceFromMmdvm()
 *
 * Builds the service frame for a frame received from the modem.  The checks
 * on the frame length are those of the host's old receive code; a frame that
 * fails them is dropped.
 *
 * Parameters:
 *   frame, frameLen - The modem frame.
 *   out             - Receives the service frame.
 *   proto           - Receives the protocol.
 *
 * Returns: The service frame length, 0 for a frame no service gets.
 */
size_t serviceFromMmdvm(const uint8_t* frame, size_t frameLen, uint8_t* out, Proto* proto)
{
    uint8_t type;
    size_t  n = frameLen;

    *proto = PROTO_NONE;
    if (frameLen < 3 || frame[0] != MMDVM_START)
        return 0;
    type = frame[2];

    switch (type)
    {
        case TYPE_M17_LSF:
        case TYPE_M17_STREAM:
        case TYPE_M17_PACKET:
            if (n != 54 && n != 52)
                return 0;
            wrap(out, 0x38, type == TYPE_M17_LSF ? "M17L" : type == TYPE_M17_STREAM ? "M17S" : "M17P");
            memcpy(out + 8, frame + 4, 48);
            *proto = PROTO_M17;
            return 0x38;

        case TYPE_M17_EOT:
        case TYPE_M17_LOST:
            if (n != 3)
                return 0;
            wrap(out, 8, "M17E");
            *proto = PROTO_M17;
            return 8;

        case TYPE_DSTAR_HEADER:
            if (n != 46)
                return 0;
            wrap(out, 0x31, "DSTH");
            memcpy(out + 8, frame + 3, 41);
            *proto = PROTO_DSTAR;
            return 0x31;

        case TYPE_DSTAR_DATA:
            if (n != 15 && n != 17)
                return 0;
            wrap(out, 0x14, "DSTD");
            memcpy(out + 8, frame + 3, 12);
            *proto = PROTO_DSTAR;
            return 0x14;

        case TYPE_DSTAR_EOT:
        case TYPE_DSTAR_LOST:
            if (n != 3)
                return 0;
            wrap(out, 8, "DSTE");
            *proto = PROTO_DSTAR;
            return 8;

        case TYPE_FM_DATA:
        case TYPE_FM_STATUS:
        case TYPE_FM_EOT:
        case TYPE_FM_RSSI:
            if (n < 3 || n > 250)
                return 0;
            wrap(out, n + 5, type == TYPE_FM_DATA ? "FMDA" : type == TYPE_FM_STATUS ? "FMST" :
                             type == TYPE_FM_EOT ? "FMEO" : "FMRS");
            if (n > 3)
                memcpy(out + 8, frame + 3, n - 3);
            *proto = PROTO_FM;
            return n + 5;

        case TYPE_P25_HDR:
            if (frame[3] != 1 || n < 21 || n > 105)
                return 0;
            wrap(out, n + 4, "P25H");
            memcpy(out + 8, frame + 4, n - 4);
            *proto = PROTO_P25;
            return n + 4;

        case TYPE_P25_LDU:
            if (frame[3] != 1 || (n != 222 && n != 220))
                return 0;
            wrap(out, 0xE0, "P25L");
            memcpy(out + 8, frame + 4, 216);
            *proto = PROTO_P25;
            return 0xE0;

        case TYPE_P25_LOST:
            if (n != 3)
                return 0;
            wrap(out, 8, "P25E");
            *proto = PROTO_P25;
            return 8;

        case TYPE_YSF_DATA:        /* [placeholder] [120 byte frame] [optional 2 byte RSSI] */
            if (n != 124 && n != 126)
                return 0;
            wrap(out, n - 3 + 8, "YSFD");
            memcpy(out + 8, frame + 3, n - 3);
            *proto = PROTO_YSF;
            return n - 3 + 8;

        case TYPE_YSF_LOST:
            wrap(out, 8, "YSFL");
            *proto = PROTO_YSF;
            return 8;

        case TYPE_NXDN_DATA:       /* [placeholder] [48 byte frame] [optional 2 byte RSSI] */
            if (n != 52 && n != 54)
                return 0;
            wrap(out, n - 3 + 8, "NXDD");
            memcpy(out + 8, frame + 3, n - 3);
            *proto = PROTO_NXDN;
            return n - 3 + 8;

        case TYPE_NXDN_LOST:
            wrap(out, 8, "NXDL");
            *proto = PROTO_NXDN;
            return 8;

        case TYPE_DMR_DATA1:
        case TYPE_DMR_DATA2:
            if (n != 0x25 && n != 0x27)
                return 0;
            wrap(out, n - 3 + 8, type == TYPE_DMR_DATA1 ? "DMD1" : "DMD2");
            memcpy(out + 8, frame + 3, n - 3);
            *proto = PROTO_DMR;
            return n - 3 + 8;

        case TYPE_DMR_LOST1:
        case TYPE_DMR_LOST2:
            wrap(out, n - 3 + 8, type == TYPE_DMR_LOST1 ? "DML1" : "DML2");
            memcpy(out + 8, frame + 3, n - 3);
            *proto = PROTO_DMR;
            return n - 3 + 8;

        default:
            return 0;
    }
}
