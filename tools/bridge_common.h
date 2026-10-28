/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

/*
 * Shared pieces of the modem bridge programs.
 *
 * A bridge sits between Modem_Host and a real radio modem.  It creates a
 * virtual serial port (a pty) that Modem_Host opens as its "port", talks to
 * the real modem on the real serial port in whatever way that modem needs,
 * keeps its own default settings in the config database and re-reads them
 * when Modem_Host asks.  Modem_Host only has to know which kind of packet
 * stream the bridge delivers:
 *
 *   FRAME  - whole protocol frames, one radio mode at a time (MMDVM style)
 *   BIT    - bit stream frames with a connection id (OpenMT_HS style)
 *   SAMPLE - audio sample frames (OpenMT / sx1255 style)
 *
 * The bridge tells Modem_Host the stream type by writing the row
 * (modem, module "config", parameter "streamType") in modem_host.config.
 *
 * Control frames: Modem_Host sends the bridge a frame
 *
 *     0xFE  <total length 3..BRIDGE_CTL_MAX>  <type>  [payload]
 *
 * on the virtual port.  0xFE is not the start byte of any of the three
 * stream protocols, so a bridge that parses the host side of its stream one
 * frame at a time can recognise it at a frame boundary and must not forward
 * it to the modem.  Types:
 *
 *   BRIDGE_CTL_RELOAD    no payload: "your settings changed in the
 *                        database, read them again".
 *   BRIDGE_CTL_HOST_UP   no payload: Modem_Host has (re)started and knows
 *                        nothing about the modem; bring the modem to a
 *                        fresh state (configure it, forget the modes the
 *                        old host registered).
 *   BRIDGE_CTL_MODE_ON   payload: a mode name (ASCII, no NUL), a service
 *                        has asked for that mode to be enabled.
 *   BRIDGE_CTL_MODE_OFF  payload: a mode name, that mode is not needed
 *                        any more.
 *   BRIDGE_CTL_MODE_CONN payload: one byte, the connection id the host gave
 *                        a mode, then the mode name.  Sent before that
 *                        mode's frames, for streams where the modem
 *                        numbers its modes (BIT).
 *   BRIDGE_CTL_COMMAND   payload: the text of a dashboard command the host
 *                        does not know (for example "calibrate DMR").  The
 *                        bridge carries it out and acknowledges it in the
 *                        database itself.
 */

#ifndef BRIDGE_COMMON_H
#define BRIDGE_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_CTL_START   0xFEU
#define BRIDGE_CTL_LENGTH  3U           /* shortest control frame (no payload) */
#define BRIDGE_CTL_MAX     40U          /* longest control frame               */
#define BRIDGE_CTL_RELOAD  0x01U
#define BRIDGE_CTL_MODE_ON  0x02U
#define BRIDGE_CTL_MODE_OFF 0x03U
#define BRIDGE_CTL_MODE_CONN 0x04U       /* payload: connection id byte, mode name (BIT streams) */
#define BRIDGE_CTL_COMMAND  0x05U       /* payload: a dashboard command for the bridge, as text   */
#define BRIDGE_CTL_HOST_UP  0x06U       /* no payload: the host has started                        */
#define BRIDGE_CTL_ACTIVE   0x07U       /* bridge -> host, payload: one byte, the connection id that now owns the modem, or BRIDGE_CONN_NONE (idle) */

#define BRIDGE_CONN_NONE    0xFFU

/*
 * Relay frames (FRAME streams).  The host does not know what the frames of a
 * FRAME modem look like.  It passes the service frames (0x61, 16 bit length,
 * type, payload) to the bridge and takes them back from it unchanged, each
 * wrapped in
 *
 *     0x62  <16 bit total length>  <connection id>  <the service frame>
 *
 * The connection id is the one the host gave the service (MODE_CONN).  The
 * bridge turns the service frames into what its modem speaks and back, paces
 * the transmit side and decides which connection owns the modem; it tells
 * the host with BRIDGE_CTL_ACTIVE.
 */
#define BRIDGE_RELAY_START  0x62U
#define BRIDGE_RELAY_HEADER 4U          /* start byte, length (2), connection id */

typedef enum
{
    STREAM_NONE = 0,
    STREAM_FRAME,
    STREAM_BIT,
    STREAM_SAMPLE
} StreamType;

/* "FRAME", "BIT" or "SAMPLE"; "" for STREAM_NONE. */
const char* bridgeStreamName(StreamType type);

/* Case blind; STREAM_NONE for anything that is not one of the three names. */
StreamType bridgeStreamFromName(const char* name);

/* Builds a control frame in buf (BRIDGE_CTL_MAX bytes); returns its length,
 * 0 if the payload is too long.  payload may be NULL when plen is 0. */
size_t bridgeCtlBuild(uint8_t type, const uint8_t* payload, size_t plen, uint8_t* buf);

/*
 * Looks for a control frame at the start of buf.
 *
 * Returns: 0 if buf does not start with BRIDGE_CTL_START (not a control
 *   frame), or a control frame is only partly there (*needMore is set);
 *   otherwise the number of bytes the control frame uses, with its type in
 *   *type and its payload at *payload, *plen bytes long (inside buf).
 */
size_t bridgeCtlParse(const uint8_t* buf, size_t len, uint8_t* type, const uint8_t** payload, size_t* plen, bool* needMore);

/* Wraps a service frame for the relay; returns the size written to out
 * (BRIDGE_RELAY_HEADER + tlvLen), or 0 if it does not fit in outCap. */
size_t bridgeRelayBuild(uint8_t conn, const uint8_t* tlv, size_t tlvLen, uint8_t* out, size_t outCap);

/*
 * Looks for a relay frame at the start of buf.  Returns 0 if buf does not
 * start with one (*needMore tells a partly received frame from stray bytes),
 * otherwise the number of bytes it uses, with the connection id in *conn and
 * the service frame at *tlv, *tlvLen bytes long (inside buf).
 */
size_t bridgeRelayParse(const uint8_t* buf, size_t len, uint8_t* conn, const uint8_t** tlv, size_t* tlvLen, bool* needMore);

/*
 * Creates the pty pair, puts both ends in raw mode and keeps the slave end
 * open.  If symlink is not NULL and not empty a symbolic link of that name
 * is made to the slave device.
 *
 * Returns: The master file descriptor, or -1 on failure.
 */
int bridgeOpenPty(const char* symlinkPath, char* slavePath, size_t slavePathCap);

/* Database helpers: all use the modem's "config" module. */
bool bridgePublishStreamType(const char* modemName, StreamType type);

/*
 * Publishes what Modem_Host needs to know about the modem behind the bridge.
 * Every bridge must call it, so values of an earlier bridge do not stay.
 *   creditPacing   - the modem has a fixed TX buffer that the host has to ask
 *                    about (GET_STATUS) before it sends more sample data.
 *   statusM17Index - the position of one field in the FRAME status reply.
 *   calibration    - the bridge can measure RX frequency errors, so the
 *                    dashboard shows its calibration tab.
 */
bool bridgePublishCaps(const char* modemName, bool creditPacing, int statusM17Index, bool calibration);

/* Answers the dashboard command the host passed on, for a bridge that cannot do it. */
void bridgeAckUnsupported(const char* modemName);
bool bridgePublishPort(const char* modemName, const char* portName);

/* Opens a real serial port raw, 8N1, no flow control (115200, 230400,
 * 460800 or 921600; 115200 for any other value), reads time out after
 * 100 ms.  Returns the file descriptor or -1. */
int bridgeOpenSerial(const char* path, unsigned int baud);

/*
 * Raspberry Pi PL011 (ttyAMA0) TX race workaround: waits, at most 50 ms, until
 * the port's output queue is empty.  The kernel's amba-pl011 interrupt handler
 * can write into a full TX FIFO when a write() from another CPU fills it in the
 * wrong moment, and the bytes are lost.  A write made while the queue is empty
 * is safe.  Call it before every frame written to a real UART.
 */
void bridgeWaitTxEmpty(int fd);
StreamType bridgeReadStreamType(const char* modemName);

/* Creates a setting with its default only if it does not exist yet. */
bool bridgeDefault(const char* modemName, const char* key, const char* value);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_COMMON_H */
