/* Created by Rick KD0OSS */
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

/*
 * sim_common.h
 *
 * Helpers shared by the host simulators (p25_sim, m17_sim, dstar_sim and
 * dmr_sim).  A simulator plays the modem host and the gateway for one
 * protocol host: it listens on the modem port, connects to the gateway port,
 * and exchanges OpenMT packets with the host:
 *
 *     [0x61][length hi][length lo][type length][type, 4 bytes][payload]
 *
 * where the length is that of the whole packet.
 */

#ifndef SIM_COMMON_H
#define SIM_COMMON_H

#include <stdbool.h>
#include <stdint.h>

#define SIM_MAX_PACKET 1024

/* One OpenMT packet */
typedef struct
{
    uint8_t data[SIM_MAX_PACKET];
    int     len;
} SimPacket;

/* A connection with a receive buffer, so packets can be cut out of the stream */
typedef struct
{
    int     fd;
    uint8_t buf[4 * SIM_MAX_PACKET];
    int     used;
} SimConn;

/* Counts for the summary */
extern int simFails;
extern int simChecks;

/* Records a check: prints ok or FAIL and counts it */
void simCheck(bool ok, const char* fmt, ...);

/* Monotonic time in milliseconds */
uint64_t simNowMs(void);

/* Listens on a TCP port on the loopback address; returns the listening socket or -1 */
int simListen(uint16_t port);

/* Waits for a connection; returns the socket or -1 on timeout */
int simAccept(int listenFd, int timeoutMs);

/* Connects to a TCP port on the loopback address, retrying for up to tries * 200 ms; returns the socket or -1 */
int simConnect(uint16_t port, int tries);

/* Sets up a connection structure around a socket */
void simConnInit(SimConn* c, int fd);

/* Reads the next packet; returns 1 for a packet, 0 for a timeout, -1 if the connection closed */
int simRead(SimConn* c, SimPacket* p, int timeoutMs);

/* Sends a packet made of a four byte type and a payload; returns true if all of it was written */
bool simSend(SimConn* c, const char* type, const uint8_t* payload, int payloadLen);

/* Sends a packet exactly as given */
bool simSendRaw(SimConn* c, const uint8_t* data, int len);

/* Reads packets for up to timeoutMs after the last one arrived, passing each to the function; returns the count */
int simDrain(SimConn* c, int quietMs, int maxMs, void (*each)(const SimPacket*, void*), void* arg);

#endif /* SIM_COMMON_H */
