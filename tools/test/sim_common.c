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
 * sim_common.c
 *
 * Helpers shared by the host simulators: sockets, OpenMT packet framing and
 * the pass or fail bookkeeping.  See sim_common.h.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "sim_common.h"

int simFails  = 0;
int simChecks = 0;

/*
 * simCheck()
 *
 * Records the result of a check and prints it: "ok" for a pass, "FAIL" for a
 * failure.
 *
 * Parameters:
 *   ok  - true if the check passed.
 *   fmt - printf style text that says what was checked.
 *
 * Returns: nothing.
 */
void simCheck(bool ok, const char* fmt, ...)
{
    va_list ap;

    simChecks++;
    if (!ok)
        simFails++;

    printf("  %s ", ok ? "ok:  " : "FAIL:");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/*
 * simNowMs()
 *
 * Reads the monotonic clock.
 *
 * Parameters: none.
 *
 * Returns: Time in milliseconds.
 */
uint64_t simNowMs(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/*
 * simListen()
 *
 * Opens a listening TCP socket on the loopback address.
 *
 * Parameters:
 *   port - Port number.
 *
 * Returns: The listening socket, or -1 on error.
 */
int simListen(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    struct sockaddr_in a;

    if (fd < 0)
        return -1;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(fd, 1) < 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * simAccept()
 *
 * Waits for a connection on a listening socket.
 *
 * Parameters:
 *   listenFd  - The listening socket.
 *   timeoutMs - Longest wait in milliseconds.
 *
 * Returns: The new socket, or -1 on timeout or error.
 */
int simAccept(int listenFd, int timeoutMs)
{
    struct pollfd p = { listenFd, POLLIN, 0 };

    if (poll(&p, 1, timeoutMs) <= 0)
        return -1;

    return accept(listenFd, NULL, NULL);
}

/*
 * simConnect()
 *
 * Connects to a TCP port on the loopback address, trying again every 200 ms
 * while the connection is refused (the host may not be listening yet).
 *
 * Parameters:
 *   port  - Port number.
 *   tries - Number of attempts.
 *
 * Returns: The socket, or -1 if no attempt worked.
 */
int simConnect(uint16_t port, int tries)
{
    struct sockaddr_in a;

    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(port);

    for (int i = 0; i < tries; i++)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0)
            return fd;

        close(fd);
        usleep(200000);
    }

    return -1;
}

/*
 * simConnInit()
 *
 * Prepares a connection structure around a socket.
 *
 * Parameters:
 *   c  - The connection.
 *   fd - The socket.
 *
 * Returns: nothing.
 */
void simConnInit(SimConn* c, int fd)
{
    c->fd   = fd;
    c->used = 0;
}

/*
 * simRead()
 *
 * Gets the next whole packet from a connection, reading from the socket when
 * the bytes held are not enough.  A byte that is not the packet start 0x61
 * is dropped.
 *
 * Parameters:
 *   c         - The connection.
 *   p         - Receives the packet.
 *   timeoutMs - Longest wait in milliseconds for more data.
 *
 * Returns: 1 if a packet was returned, 0 on timeout, -1 if the connection
 *   closed.
 */
int simRead(SimConn* c, SimPacket* p, int timeoutMs)
{
    uint64_t end = simNowMs() + (uint64_t)timeoutMs;

    for (;;)
    {
        while (c->used > 0 && c->buf[0] != 0x61)
        {
            memmove(c->buf, c->buf + 1, (size_t)--c->used);
        }

        if (c->used >= 3)
        {
            int len = (c->buf[1] << 8) + c->buf[2];

            if (len < 4 || len > SIM_MAX_PACKET)
            {
                memmove(c->buf, c->buf + 1, (size_t)--c->used);
                continue;
            }

            if (c->used >= len)
            {
                memcpy(p->data, c->buf, (size_t)len);
                p->len = len;
                memmove(c->buf, c->buf + len, (size_t)(c->used - len));
                c->used -= len;

                return 1;
            }
        }

        int64_t left = (int64_t)(end - simNowMs());

        if (left < 0)
            return 0;

        struct pollfd pf = { c->fd, POLLIN, 0 };
        int r = poll(&pf, 1, (int)left);

        if (r <= 0)
            return 0;

        ssize_t n = read(c->fd, c->buf + c->used, sizeof(c->buf) - (size_t)c->used);

        if (n <= 0)
            return -1;

        c->used += (int)n;
    }
}

/*
 * simSendRaw()
 *
 * Writes bytes to a connection.
 *
 * Parameters:
 *   c    - The connection.
 *   data - The bytes.
 *   len  - Number of bytes.
 *
 * Returns: true if all of it was written.
 */
bool simSendRaw(SimConn* c, const uint8_t* data, int len)
{
    int done = 0;

    while (done < len)
    {
        ssize_t n = write(c->fd, data + done, (size_t)(len - done));

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            return false;

        done += (int)n;
    }

    return true;
}

/*
 * simSend()
 *
 * Builds an OpenMT packet from a four byte type and a payload and writes it.
 *
 * Parameters:
 *   c          - The connection.
 *   type       - The four character type.
 *   payload    - The payload, may be NULL if payloadLen is 0.
 *   payloadLen - Number of payload bytes.
 *
 * Returns: true if all of it was written.
 */
bool simSend(SimConn* c, const char* type, const uint8_t* payload, int payloadLen)
{
    uint8_t buf[SIM_MAX_PACKET];
    int len = 8 + payloadLen;

    if (len > SIM_MAX_PACKET)
        return false;

    buf[0] = 0x61U;
    buf[1] = (uint8_t)(len >> 8);
    buf[2] = (uint8_t)(len & 0xFF);
    buf[3] = 0x04U;
    memcpy(buf + 4, type, 4);
    if (payloadLen > 0)
        memcpy(buf + 8, payload, (size_t)payloadLen);

    return simSendRaw(c, buf, len);
}

/*
 * simDrain()
 *
 * Reads packets and passes each to a function, until none has arrived for
 * quietMs, or maxMs have gone by in all.
 *
 * Parameters:
 *   c       - The connection.
 *   quietMs - How long without a packet ends the wait, in milliseconds.
 *   maxMs   - Longest total wait in milliseconds.
 *   each    - Function called for every packet; may be NULL to just discard.
 *   arg     - Passed to each.
 *
 * Returns: Number of packets read.
 */
int simDrain(SimConn* c, int quietMs, int maxMs, void (*each)(const SimPacket*, void*), void* arg)
{
    uint64_t end = simNowMs() + (uint64_t)maxMs;
    int count = 0;
    SimPacket p;

    while (simNowMs() < end)
    {
        int r = simRead(c, &p, quietMs);

        if (r <= 0)
            break;

        count++;
        if (each != NULL)
            each(&p, arg);
    }

    return count;
}
