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
 * NXDN gateway.
 *
 * Connects to nxdn_host over TCP (port 18500 + modem - 1) and links it to an
 * NXDN reflector over UDP when the dashboard asks for it.
 *
 *   host -> gateway   CONN  callsign(6) name(6)   link to reflector 'name'
 *                     DISC                        unlink
 *                     COMM  04                    re-read the configuration
 *                     NXDN  type(1) netData(40)   RF voice or data to the reflector
 *   gateway -> host   CONN / NACK / DISC  name(6) link status
 *                     COMM                        configuration acknowledged
 *                     NXDN  type(1) netData(40)   reflector voice or data for RF
 *
 * netData is the 33 byte network frame used by MMDVMHost and NXDNGateway
 * (LICH, SACCH, two 14 byte audio or FACCH1 blocks) padded to 40 bytes.
 * The reflector packets are those of the NXDNReflector:
 *
 *   NXDNP callsign(10) tg(2)                            poll, 17 bytes
 *   NXDNU callsign(10) tg(2)                            unlink, 17 bytes
 *   NXDND src(2) dst(2) flags(1) netData(33)            data, 43 bytes
 *         flags: 0x01 group, 0x02 data, 0x04 start, 0x08 end
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <tools.h>
#include <unistd.h>

#define VERSION "2026-10-13"
#define BUFFER_SIZE 1024

#define NXDN_NET_DATA_LENGTH    40U     /* host <-> gateway frame, padded      */
#define NXDN_REFL_DATA_LENGTH   33U     /* frame carried by the reflector      */
#define NXDN_REFL_PACKET_LENGTH 43U     /* NXDND packet                        */
#define NXDN_REFL_POLL_LENGTH   17U     /* NXDNP and NXDNU packets             */
#define NXDN_HOST_PACKET_LENGTH (8U + 1U + NXDN_NET_DATA_LENGTH)

#define NXDN_FLAG_GROUP         0x01U
#define NXDN_FLAG_DATA          0x02U
#define NXDN_FLAG_START         0x04U
#define NXDN_FLAG_END           0x08U

#define NXDN_MSG_VOICE_HEADER   0U
#define NXDN_MSG_VOICE_BODY     1U
#define NXDN_MSG_VOICE_TRAILER  2U
#define NXDN_MSG_DATA_HEADER    3U
#define NXDN_MSG_DATA_BODY      4U
#define NXDN_MSG_DATA_TRAILER   5U

#define NXDN_TYPE_VCALL         0x01U
#define NXDN_TYPE_DCALL_HDR     0x09U
#define NXDN_TYPE_TX_REL        0x08U

#define POLL_INTERVAL_MS        5000U
#define LINK_TIMEOUT_MS         3000U
#define LOST_TIMEOUT_MS         60000U

typedef enum
{
    LINK_IDLE,
    LINK_CONNECTING,
    LINK_LINKED
} LinkState;

pthread_t clientid;
pthread_t reflid;

static int clientPort       = 18500;
static uint8_t modemId      = 1;
static char modemName[10]   = "modem1";
static char nxdnHost[80]    = "127.0.0.1";
static bool connected       = true;
static bool debugM          = false;

static int hostFd           = -1;
static pthread_mutex_t hostMutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t reflMutex = PTHREAD_MUTEX_INITIALIZER;
static LinkState linkState  = LINK_IDLE;
static bool reflStop        = false;
static bool reflRunning     = false;
static int reflFd           = -1;
static struct sockaddr_in reflAddr;
static uint16_t reflTG      = 0;
static char reflName[8]     = "";
static char callsign[11]    = "N0CALL    ";

/* Linking rules */
static bool linkAuto        = false;        /* link was made from the radio     */
static volatile uint64_t lastActivity = 0;  /* last call in either direction    */
static unsigned int hangTime = 120U;        /* seconds, 0 = never               */
static uint16_t staticTG    = 0;            /* linked at start, 0 = none        */
static pthread_mutex_t linkOpMutex = PTHREAD_MUTEX_INITIALIZER;

/* State of the call being sent to the reflector */
static bool callGroup       = true;
static uint16_t callSrc     = 0;
static uint16_t callDst     = 0;

/*
 * error()
 *
 * Wrapper for perror that ends the program.
 *
 * Parameters:
 *   msg - Text printed in front of the system error message.
 *
 * Returns: nothing.
 */
void error(char* msg)
{
    reportError(modemName, "NXDN gateway", "%s: %s", msg, strerror(errno));
    exit(1);
}

/*
 * hostSend()
 *
 * Writes one packet to nxdn_host. Safe to call from both threads.
 *
 * Parameters:
 *   buf - The packet.
 *   len - Number of bytes in buf.
 *
 * Returns: nothing.
 */
static void hostSend(const uint8_t* buf, uint16_t len)
{
    pthread_mutex_lock(&hostMutex);

    if (hostFd >= 0)
    {
        if (debugM)
            hexDump("To NXDN host", buf, len);

        if (write(hostFd, buf, len) < 0)
            reportError(modemName, "NXDN gateway", "NXDN_Gateway: write to NXDN host failed: %s", strerror(errno));
    }

    pthread_mutex_unlock(&hostMutex);
}

/*
 * hostReply()
 *
 * Sends a link status packet (CONN, NACK or DISC) with the reflector name.
 *
 * Parameters:
 *   type - Four character packet type: CONN, NACK or DISC.
 *   name - Name of the reflector.
 *
 * Returns: nothing.
 */
static void hostReply(const char* type, const char* name)
{
    uint8_t buf[14];

    memset(buf, 0x00, sizeof(buf));
    buf[0] = 0x61;
    buf[1] = 0x00;
    buf[2] = 0x0E;
    buf[3] = 0x04;
    memcpy(buf + 4, type, 4);
    memcpy(buf + 8, name, strnlen(name, 6));

    hostSend(buf, sizeof(buf));
}

/*
 * loadCallsign()
 *
 * Reads the station callsign for the reflector polls from the host
 * configuration and pads it to 10 characters.
 *
 * Parameters:
 *   fallback - Callsign to use when none is configured (may be NULL); N0CALL
 *              is used if there is still none.
 *
 * Returns: nothing.
 */
static void loadCallsign(const char* fallback)
{
    char tmp[31];

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "main", "callsign", tmp);

    if (strlen(tmp) == 0 && fallback != NULL)
        strncpy(tmp, fallback, sizeof(tmp) - 1);

    if (strlen(tmp) == 0)
        strcpy(tmp, "N0CALL");

    memset(callsign, ' ', 10);
    callsign[10] = '\0';
    memcpy(callsign, tmp, strnlen(tmp, 10));
}

/*
 * findInHostsFile()
 *
 * Looks reflector 'id' up in an NXDNHosts.txt file ("id address port" per
 * line, # starts a comment).
 *
 * Parameters:
 *   path    - Path of the hosts file.
 *   id      - Reflector id to find.
 *   address - Receives the reflector's address (up to 54 characters).
 *   port    - Receives the reflector's port.
 *
 * Returns: true if the reflector was found, false if the file could not be
 *   opened or the id is not in it.
 */
static bool findInHostsFile(const char* path, unsigned int id, char* address, uint16_t* port)
{
    FILE* fp = fopen(path, "r");
    char line[256];
    bool found = false;

    if (fp == NULL)
        return false;

    while (!found && fgets(line, sizeof(line), fp) != NULL)
    {
        unsigned int lineId;
        unsigned int linePort;
        char host[128];

        if (line[0] == '#')
            continue;

        if (sscanf(line, "%u %127s %u", &lineId, host, &linePort) != 3)
            continue;

        if (lineId == id && linePort > 0U && linePort < 65536U)
        {
            strncpy(address, host, 54);
            address[54] = '\0';
            *port       = (uint16_t)linePort;
            found       = true;
        }
    }

    fclose(fp);
    return found;
}

/*
 * lookupReflector()
 *
 * Finds a reflector by name. The dashboard database is tried first, then the
 * hosts file (NXDN/hostsFile in the host configuration, then NXDNHosts.txt
 * in the working directory and the OpenMT install directory).
 *
 * Parameters:
 *   name - Reflector name; it is a number, which is also used as the id in
 *          the hosts files.
 *   addr - Receives the address and port of the reflector.
 *
 * Returns: true if the reflector was found, false if not.
 */
static bool lookupReflector(const char* name, struct sockaddr_in* addr)
{
    char address[55];
    uint16_t port = 0;
    char path[200];
    unsigned int id = (unsigned int)strtoul(name, NULL, 10);

    memset(address, 0x00, sizeof(address));

    if (findReflector("NXDN", name, address, &port))
        return resolveAddress(address, port, addr);

    memset(path, 0x00, sizeof(path));
    readHostConfig(modemName, "NXDN", "hostsFile", path);

    if (strlen(path) > 0 && findInHostsFile(path, id, address, &port))
        return resolveAddress(address, port, addr);

    if (findInHostsFile("NXDNHosts.txt", id, address, &port))
        return resolveAddress(address, port, addr);

    if (findInHostsFile("/home/pi/OpenMT/protocols/NXDN/NXDNHosts.txt", id, address, &port))
        return resolveAddress(address, port, addr);

    return false;
}

/*
 * reflWritePoll()
 *
 * Sends a poll or an unlink to the reflector. 'kind' is 'P' or 'U'.
 *
 * Parameters:
 *   fd   - UDP socket to the reflector.
 *   addr - Address of the reflector.
 *   kind - 'P' for a poll or 'U' for an unlink.
 *   tg   - Talk group that is linked.
 *
 * Returns: nothing.
 */
static void reflWritePoll(int fd, const struct sockaddr_in* addr, char kind, uint16_t tg)
{
    uint8_t data[NXDN_REFL_POLL_LENGTH];

    memcpy(data, "NXDN", 4);
    data[4] = (uint8_t)kind;
    memcpy(data + 5, callsign, 10);
    data[15] = (tg >> 8) & 0xFFU;
    data[16] = (tg >> 0) & 0xFFU;

    if (debugM)
        hexDump(kind == 'P' ? "NXDN poll sent" : "NXDN unlink sent", data, sizeof(data));

    sendto(fd, data, sizeof(data), 0, (const struct sockaddr*)addr, sizeof(*addr));
}

/*
 * isGroupOption()
 *
 * The call type is the top three bits of the option byte of the layer 3
 * message.  Conference (1) is a talk group call; some radios send 0
 * (broadcast) for the same thing, and nxdn_host treats both as a talk group.
 * Individual calls are 4 and above.
 *
 * Parameters:
 *   option - Option byte of the layer 3 message.
 *
 * Returns: true for a talk group call (call type 0 or 1), false for an
 *   individual call.
 */
static bool isGroupOption(uint8_t option)
{
    return ((option >> 5) & 0x07U) <= 1U;
}

/*
 * trackCall()
 *
 * Follows the call information in the frames from the radio so that the
 * reflector packets can carry the source, destination and group flag. The
 * rules are those of NXDNGateway.
 *
 * Parameters:
 *   d - The frame from the radio.
 *
 * Returns: nothing.
 */
static void trackCall(const uint8_t* d)
{
    if ((d[0] == 0x81U || d[0] == 0x83U) && ((d[5] & 0x3FU) == NXDN_TYPE_VCALL || (d[5] & 0x3FU) == NXDN_TYPE_TX_REL))
    {
        callGroup = isGroupOption(d[7]);
        callSrc   = (uint16_t)((d[8] << 8) | d[9]);
        callDst   = (uint16_t)((d[10] << 8) | d[11]);
    }
    else if ((d[0] & 0xF0U) == 0x90U && (d[2] & 0x3FU) == NXDN_TYPE_DCALL_HDR)
    {
        callGroup = ((d[4] >> 5) & 0x07U) == 1U;
        callSrc   = (uint16_t)((d[5] << 8) | d[6]);
        callDst   = (uint16_t)((d[7] << 8) | d[8]);
    }
}

/*
 * reflectorFlags()
 *
 * The flag byte of an NXDND packet for the frame d.
 *
 * Parameters:
 *   d - The frame from the radio.
 *
 * Returns: The flag byte (group, start, end and data bits).
 */
static uint8_t reflectorFlags(const uint8_t* d)
{
    uint8_t flags = callGroup ? NXDN_FLAG_GROUP : 0x00U;

    if (d[0] == 0x81U || d[0] == 0x83U)
    {
        if ((d[5] & 0x3FU) == NXDN_TYPE_VCALL)
            flags |= NXDN_FLAG_START;
        if ((d[5] & 0x3FU) == NXDN_TYPE_TX_REL)
            flags |= NXDN_FLAG_END;
    }
    else if ((d[0] & 0xF0U) == 0x90U)
    {
        flags |= NXDN_FLAG_DATA;

        if (d[0] == 0x90U || d[0] == 0x92U || d[0] == 0x9CU || d[0] == 0x9EU)
        {
            if ((d[2] & 0x3FU) == NXDN_TYPE_DCALL_HDR)
                flags |= NXDN_FLAG_START;
            if ((d[2] & 0x3FU) == NXDN_TYPE_TX_REL)
                flags |= NXDN_FLAG_END;
        }
    }

    return flags;
}

/*
 * sendToReflector()
 *
 * Sends one frame from the radio to the linked reflector. The destination in
 * the packet header is the linked talk group so that the call is accepted by
 * the other gateways on that reflector; the call information inside the
 * frame is left alone.
 *
 * Parameters:
 *   netData - The frame from the radio.
 *
 * Returns: nothing.
 */
static void sendToReflector(const uint8_t* netData)
{
    uint8_t buffer[NXDN_REFL_PACKET_LENGTH];
    uint16_t dst;
    int fd;
    struct sockaddr_in addr;

    pthread_mutex_lock(&reflMutex);

    /* Follow the call even before the link is up so a call that started the
       link is complete when the link comes up. */
    trackCall(netData);

    if (linkState != LINK_LINKED)
    {
        pthread_mutex_unlock(&reflMutex);
        return;
    }

    lastActivity = nowMs();

    dst  = reflTG;
    fd   = reflFd;
    addr = reflAddr;

    memcpy(buffer, "NXDND", 5);
    buffer[5] = (callSrc >> 8) & 0xFFU;
    buffer[6] = (callSrc >> 0) & 0xFFU;
    buffer[7] = (dst >> 8) & 0xFFU;
    buffer[8] = (dst >> 0) & 0xFFU;
    buffer[9] = reflectorFlags(netData);
    memcpy(buffer + 10, netData, NXDN_REFL_DATA_LENGTH);

    pthread_mutex_unlock(&reflMutex);

    if (debugM)
        hexDump("NXDN data sent", buffer, sizeof(buffer));

    sendto(fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&addr, sizeof(addr));
}

/*
 * sendToHost()
 *
 * Passes one NXDND packet from the reflector on to nxdn_host.
 *
 * Parameters:
 *   packet - The NXDND packet from the reflector.
 *
 * Returns: nothing.
 */
static void sendToHost(const uint8_t* packet)
{
    uint8_t out[NXDN_HOST_PACKET_LENGTH];
    uint8_t flags = packet[9];
    uint8_t type;

    if (flags & NXDN_FLAG_DATA)
    {
        if (flags & NXDN_FLAG_START)
            type = NXDN_MSG_DATA_HEADER;
        else if (flags & NXDN_FLAG_END)
            type = NXDN_MSG_DATA_TRAILER;
        else
            type = NXDN_MSG_DATA_BODY;
    }
    else
    {
        if (flags & NXDN_FLAG_START)
            type = NXDN_MSG_VOICE_HEADER;
        else if (flags & NXDN_FLAG_END)
            type = NXDN_MSG_VOICE_TRAILER;
        else
            type = NXDN_MSG_VOICE_BODY;
    }

    memset(out, 0x00, sizeof(out));
    out[0] = 0x61;
    out[1] = 0x00;
    out[2] = NXDN_HOST_PACKET_LENGTH;
    out[3] = 0x04;
    memcpy(out + 4, "NXDN", 4);
    out[8] = type;
    memcpy(out + 9, packet + 10, NXDN_REFL_DATA_LENGTH);

    hostSend(out, sizeof(out));
}

/*
 * reflectorThread()
 *
 * Links to the reflector in reflAddr, polls it every five seconds and passes
 * its group calls to nxdn_host until the link is stopped, lost or the host
 * goes away.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL.
 */
void* reflectorThread(void* arg)
{
    uint8_t buf[200];
    struct sockaddr_in from;
    socklen_t fromLen;
    bool linked = false;
    uint64_t start;
    uint64_t lastPoll;
    uint64_t lastRx;
    char name[8];
    uint16_t tg;
    int fd;
    struct sockaddr_in addr;

    (void)arg;

    pthread_mutex_lock(&reflMutex);
    fd = reflFd;
    addr = reflAddr;
    tg = reflTG;
    memcpy(name, reflName, sizeof(name));
    pthread_mutex_unlock(&reflMutex);

    start = lastPoll = lastRx = nowMs();

    for (int i = 0; i < 3; i++)
        reflWritePoll(fd, &addr, 'P', tg);

    while (connected)
    {
        bool stop;
        ssize_t n;
        uint64_t now;

        pthread_mutex_lock(&reflMutex);
        stop = reflStop;
        pthread_mutex_unlock(&reflMutex);

        if (stop)
            break;

        fromLen = sizeof(from);
        n       = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromLen);
        now     = nowMs();

        if (n > 0 && from.sin_addr.s_addr == addr.sin_addr.s_addr && from.sin_port == addr.sin_port)
        {
            if (n == (ssize_t)NXDN_REFL_POLL_LENGTH && memcmp(buf, "NXDNP", 5) == 0)
            {
                if (debugM)
                    hexDump("NXDN poll received", buf, (unsigned int)n);

                lastRx = now;

                if (!linked)
                {
                    linked = true;

                    pthread_mutex_lock(&reflMutex);
                    linkState = LINK_LINKED;
                    pthread_mutex_unlock(&reflMutex);

                    logMsg("Linked to NXDN reflector %s", name);
                    hostReply("CONN", name);
                }
            }
            else if (n == (ssize_t)NXDN_REFL_PACKET_LENGTH && memcmp(buf, "NXDND", 5) == 0)
            {
                uint16_t dst = (uint16_t)((buf[7] << 8) | buf[8]);
                bool grp     = (buf[9] & NXDN_FLAG_GROUP) != 0U;

                if (debugM)
                    hexDump("NXDN data received", buf, (unsigned int)n);

                lastRx = now;

                if (!linked)
                {
                    /* Data before the poll reply: the reflector is there. */
                    linked = true;

                    pthread_mutex_lock(&reflMutex);
                    linkState = LINK_LINKED;
                    pthread_mutex_unlock(&reflMutex);

                    logMsg("Linked to NXDN reflector %s", name);
                    hostReply("CONN", name);
                }

                if (grp && dst == tg)
                {
                    lastActivity = now;
                    sendToHost(buf);
                }
            }
        }

        if (!linked && now - start > LINK_TIMEOUT_MS)
        {
            logMsg("Failed to link to NXDN reflector %s (no answer)", name);
            hostReply("NACK", name);

            pthread_mutex_lock(&reflMutex);
            linkState = LINK_IDLE;
            reflRunning = false;
            close(reflFd);
            reflFd = -1;
            pthread_mutex_unlock(&reflMutex);
            return NULL;
        }

        if (linked && now - lastPoll >= POLL_INTERVAL_MS)
        {
            reflWritePoll(fd, &addr, 'P', tg);
            lastPoll = now;
        }

        if (linked && now - lastRx > LOST_TIMEOUT_MS)
        {
            logMsg("NXDN reflector %s stopped answering.", name);
            break;
        }
    }

    if (linked)
    {
        for (int i = 0; i < 3; i++)
            reflWritePoll(fd, &addr, 'U', tg);

        logMsg("Unlinked from NXDN reflector %s", name);
        hostReply("DISC", name);
    }

    pthread_mutex_lock(&reflMutex);
    linkState   = LINK_IDLE;
    reflRunning = false;
    close(reflFd);
    reflFd = -1;
    pthread_mutex_unlock(&reflMutex);

    return NULL;
}

/*
 * stopLink()
 *
 * Asks the reflector thread to unlink and waits (up to two seconds) for it
 * to finish.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void stopLink(void)
{
    bool running;

    pthread_mutex_lock(&reflMutex);
    running = reflRunning;
    reflStop = true;
    pthread_mutex_unlock(&reflMutex);

    for (int i = 0; running && i < 200; i++)
    {
        delay(10000);

        pthread_mutex_lock(&reflMutex);
        running = reflRunning;
        pthread_mutex_unlock(&reflMutex);
    }

    pthread_mutex_lock(&reflMutex);
    reflStop = false;
    pthread_mutex_unlock(&reflMutex);
}

/*
 * startLinkLocked()
 *
 * Looks the reflector up, opens the UDP socket and starts reflectorThread().
 * A NACK is sent to the host if the reflector is unknown.  The caller holds
 * linkOpMutex.
 *
 * Parameters:
 *   hostCallsign - Callsign of the station, for the polls.
 *   name         - Name of the reflector to link to.
 *
 * Returns: nothing.
 */
static void startLinkLocked(const char* hostCallsign, const char* name)
{
    struct sockaddr_in addr;
    struct timeval timeout;
    char cleanName[8];
    int fd;

    memset(cleanName, 0x00, sizeof(cleanName));
    memcpy(cleanName, name, strnlen(name, 6));

    stopLink();
    linkAuto = false;

    loadCallsign(hostCallsign);

    if (!lookupReflector(cleanName, &addr))
    {
        logMsg("NXDN reflector %s not found in the database or NXDNHosts.txt", cleanName);
        hostReply("NACK", cleanName);
        return;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        reportError(modemName, "NXDN gateway", "Cannot open the reflector socket: %s", strerror(errno));
        hostReply("NACK", cleanName);
        return;
    }

    {
        struct sockaddr_in local;
        uint16_t port = 14050 + modemId - 1;
        int on        = 1;
        char tmp[31];

        memset(tmp, 0x00, sizeof(tmp));
        readHostConfig(modemName, "NXDN", "networkPort", tmp);
        if (atoi(tmp) > 0)
            port = (uint16_t)atoi(tmp);

        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        memset(&local, 0x00, sizeof(local));
        local.sin_family      = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port        = htons(port);

        if (bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0)
            reportError(modemName, "NXDN gateway", "Cannot bind UDP port %u (%s), using any port.", port, strerror(errno));
    }

    timeout.tv_sec  = 0;
    timeout.tv_usec = 5000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    pthread_mutex_lock(&reflMutex);
    reflFd      = fd;
    reflAddr    = addr;
    reflTG      = (uint16_t)strtoul(cleanName, NULL, 10);
    reflStop    = false;
    reflRunning = true;
    linkState   = LINK_CONNECTING;
    memcpy(reflName, cleanName, sizeof(reflName));
    lastActivity = nowMs();
    callGroup   = true;
    callSrc     = 0;
    callDst     = 0;
    pthread_mutex_unlock(&reflMutex);

    logMsg("Linking to NXDN reflector %s (%s:%u)", cleanName, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

    if (pthread_create(&reflid, NULL, &reflectorThread, NULL) == 0)
    {
        pthread_detach(reflid);
    }
    else
    {
        reportError(modemName, "NXDN gateway", "Can't create NXDN reflector thread");
        pthread_mutex_lock(&reflMutex);
        reflRunning = false;
        linkState   = LINK_IDLE;
        close(reflFd);
        reflFd = -1;
        pthread_mutex_unlock(&reflMutex);
        hostReply("NACK", cleanName);
    }
}


/*
 * startLink()
 *
 * Links to a reflector. 'automatic' marks a link made from the radio, which
 * is dropped again after the hang time.
 *
 * Parameters:
 *   hostCallsign - Callsign of the station, for the polls.
 *   name         - Name of the reflector to link to.
 *   automatic    - true if the link was made from the radio.
 *
 * Returns: nothing.
 */
static void startLink(const char* hostCallsign, const char* name, bool automatic)
{
    pthread_mutex_lock(&linkOpMutex);
    startLinkLocked(hostCallsign, name);
    linkAuto = automatic;
    pthread_mutex_unlock(&linkOpMutex);
}

/*
 * dropLink()
 *
 * Unlinks from the current reflector.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void dropLink(void)
{
    pthread_mutex_lock(&linkOpMutex);
    stopLink();
    linkAuto = false;
    pthread_mutex_unlock(&linkOpMutex);
}

/*
 * loadLinkConfig()
 *
 * Reads NXDN/staticTG and NXDN/hangTime from the host configuration.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void loadLinkConfig(void)
{
    char tmp[31];

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "NXDN", "staticTG", tmp);
    staticTG = (uint16_t)strtoul(tmp, NULL, 10);

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "NXDN", "hangTime", tmp);
    hangTime = (strlen(tmp) > 0) ? (unsigned int)strtoul(tmp, NULL, 10) : 120U;
}

/*
 * linkStatic()
 *
 * Links to the static talk group, if one is configured.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void linkStatic(void)
{
    char name[8];

    if (staticTG == 0U)
        return;

    snprintf(name, sizeof(name), "%u", staticTG);
    logMsg("Static link to NXDN reflector %s", name);
    startLink(NULL, name, false);
}

/*
 * rfLinkCheck()
 *
 * Same rule as NXDNGateway: a group call header or trailer from the radio to
 * a talk group other than the linked one links to that talk group (and
 * unlinks the old one). A talk group with no reflector just unlinks.
 *
 * Parameters:
 *   d - The frame from the radio.
 *
 * Returns: nothing.
 */
static void rfLinkCheck(const uint8_t* d)
{
    uint16_t src;
    uint16_t dst;
    uint16_t current;
    char name[8];
    bool linked;

    if (!((d[0] == 0x81U || d[0] == 0x83U) && ((d[5] & 0x3FU) == NXDN_TYPE_VCALL || (d[5] & 0x3FU) == NXDN_TYPE_TX_REL)))
        return;

    src = (uint16_t)((d[8] << 8) | d[9]);
    dst = (uint16_t)((d[10] << 8) | d[11]);

    if ((d[5] & 0x3FU) == NXDN_TYPE_VCALL)
        logMsg("RF call from %u to %s %u", src, isGroupOption(d[7]) ? "TG" : "unit (individual call, no link)", dst);

    if (!isGroupOption(d[7]))
        return;

    if (dst == 0U)
        return;

    pthread_mutex_lock(&reflMutex);
    linked  = (linkState != LINK_IDLE);
    current = reflTG;
    pthread_mutex_unlock(&reflMutex);

    if (linked && dst == current)
        return;

    snprintf(name, sizeof(name), "%u", dst);
    logMsg("Switching to NXDN reflector %s due to RF activity from %u", name, src);

    if (linked)
        dropLink();

    startLink(NULL, name, true);
}

/*
 * hangCheck()
 *
 * Drops a link that was made from the radio when nothing has been heard in
 * either direction for the hang time, then goes back to the static talk
 * group.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void hangCheck(void)
{
    bool expired;

    if (hangTime == 0U)
        return;

    pthread_mutex_lock(&reflMutex);
    expired = linkAuto && linkState != LINK_IDLE && (nowMs() - lastActivity) > (uint64_t)hangTime * 1000U;
    pthread_mutex_unlock(&reflMutex);

    if (!expired)
        return;

    logMsg("Unlinking from NXDN reflector %s due to inactivity", reflName);
    dropLink();
    linkStatic();
}

/*
 * readFull()
 *
 * Reads exactly len bytes from fd. Returns false when the connection closed.
 *
 * Parameters:
 *   fd  - Socket to read from.
 *   buf - Receives the bytes.
 *   len - Number of bytes to read.
 *
 * Returns: true if all the bytes were read, false if the connection closed.
 */
static bool readFull(int fd, uint8_t* buf, size_t len)
{
    size_t got = 0;

    while (got < len && connected)
    {
        ssize_t n = read(fd, buf + got, len - got);

        if (n > 0)
            got += (size_t)n;
        else if (n == 0)
            return false;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return false;
        else
            delay(1000);
    }

    return got == len;
}

/*
 * handleHostPacket()
 *
 * One complete packet from nxdn_host.
 *
 * Parameters:
 *   buffer  - The packet.
 *   respLen - Length of the packet.
 *
 * Returns: nothing.
 */
static void handleHostPacket(const uint8_t* buffer, uint16_t respLen)
{
    uint8_t typeLen = buffer[3];

    if (typeLen != 4 || respLen < 8)
        return;

    if (debugM && memcmp(buffer + 4, "NXDN", 4) != 0)
        hexDump("NXDN host data", buffer, respLen);

    if (memcmp(buffer + 4, "COMM", 4) == 0)
    {
        if (respLen >= 9 && buffer[8] == 0x04)
        {
            uint8_t buf[8];

            loadCallsign(NULL);
            loadLinkConfig();

            buf[0] = 0x61;
            buf[1] = 0x00;
            buf[2] = 0x08;
            buf[3] = 0x04;
            memcpy(buf + 4, "COMM", 4);
            hostSend(buf, sizeof(buf));
        }
    }
    else if (memcmp(buffer + 4, "CONN", 4) == 0)
    {
        if (respLen >= 20)
        {
            char source[8];
            char name[8];

            memset(source, 0x00, sizeof(source));
            memset(name, 0x00, sizeof(name));
            memcpy(source, buffer + 8, 6);
            memcpy(name, buffer + 14, 6);

            startLink(source, name, false);
        }
    }
    else if (memcmp(buffer + 4, "DISC", 4) == 0)
    {
        dropLink();
    }
    else if (memcmp(buffer + 4, "NXDN", 4) == 0)
    {
        if (respLen == NXDN_HOST_PACKET_LENGTH)
        {
            rfLinkCheck(buffer + 9);
            sendToReflector(buffer + 9);
        }
    }
}

/*
 * connectHost()
 *
 * Opens the TCP connection to nxdn_host, retrying until it is up.
 *
 * Parameters: none.
 *
 * Returns: The connected socket, or -1 if the program was stopped before it
 *   connected.
 */
static int connectHost(void)
{
    while (connected)
    {
        struct sockaddr_in serv_addr;
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0)
        {
            reportError(modemName, "NXDN gateway", "Socket creation error: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }

        memset(&serv_addr, 0x00, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port   = htons(clientPort);

        if (!hostToAddress(nxdnHost, &serv_addr.sin_addr))
        {
            reportError(modemName, "NXDN gateway", "Invalid address/ Address not supported");
            exit(EXIT_FAILURE);
        }

        if (connect(fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == 0)
        {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            return fd;
        }

        close(fd);
        sleep(2);
    }

    return -1;
}

/*
 * startClient()
 *
 * Thread that connects to nxdn_host and handles its packets. When the host
 * goes away the reflector is unlinked and the connection is made again.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL.
 */
void* startClient(void* arg)
{
    uint8_t buffer[BUFFER_SIZE];

    (void)arg;

    while (connected)
    {
        uint16_t respLen;
        int fd = connectHost();

        if (fd < 0)
            break;

        pthread_mutex_lock(&hostMutex);
        hostFd = fd;
        pthread_mutex_unlock(&hostMutex);

        printf("Connected to NXDN host.\n");
        fflush(stdout);

        loadLinkConfig();
        linkStatic();

        while (connected)
        {
            if (!readFull(fd, buffer, 1))
                break;

            if (buffer[0] != 0x61)
            {
                fprintf(stderr, "NXDN_Gateway: unknown byte from NXDN host, 0x%02X\n", buffer[0]);
                continue;
            }

            if (!readFull(fd, buffer + 1, 3))
                break;

            respLen = (uint16_t)((buffer[1] << 8) + buffer[2]);

            if (respLen < 4 || respLen > BUFFER_SIZE)
            {
                fprintf(stderr, "NXDN_Gateway: bad packet length %u from NXDN host\n", respLen);
                continue;
            }

            if (!readFull(fd, buffer + 4, respLen - 4))
                break;

            handleHostPacket(buffer, respLen);
        }

        logMsg("NXDN host disconnected.");

        pthread_mutex_lock(&hostMutex);
        hostFd = -1;
        pthread_mutex_unlock(&hostMutex);

        dropLink();
        close(fd);

        if (connected)
            sleep(2);
    }

    fprintf(stderr, "Client thread exited.\n");
    connected = false;
    return NULL;
}

/*
 * main()
 *
 * Program entry. Parses the command line (-m modem number 1-10, -d daemon,
 * -v version, -x debug), starts the host client and waits.
 *
 * Parameters:
 *   argc - Number of command line arguments.
 *   argv - Command line arguments.
 *
 * Returns: 0 on exit, 1 on a bad argument.
 */
int main(int argc, char** argv)
{
    bool daemon = false;
    int c;

    while ((c = getopt(argc, argv, ":m:a:s:u:p:c:dxv")) != -1)
    {
        switch (c)
        {
            case 'm':
            {
                modemId = atoi(optarg);
                if (modemId < 1 || modemId > 10)
                {
                    reportError(modemName, "NXDN gateway", "Invalid modem number.");
                    return 1;
                }
                sprintf(modemName, "modem%d", modemId);
                clientPort = 18500 + modemId - 1;
                fprintf(stderr, "Modem name: %s\n", modemName);
            }
            break;
            case 'a':
                snprintf(nxdnHost, sizeof(nxdnHost), "%s", optarg);   /* where the protocol service runs */
                break;
            case 's':
            case 'u':
            case 'p':
            case 'c':
                dbOption(c, optarg);   /* database server, user, password, configuration file */
                break;
            case ':':
                fprintf(stderr, "Option '-%c' requires a value.\n", optopt);
                return 1;
            case 'd':
                daemon = true;
                break;
            case 'v':
                fprintf(stdout, "NXDN_Gateway: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: NXDN_Gateway [-m modem_number (1-10)] [-a address] [-s dbserver] [-u dbuser] [-p dbpassword] [-c configfile] [-d] [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            reportError(modemName, "NXDN gateway", "NXDN_Gateway: error in fork(), exiting");
            return 1;
        }

        if (pid > 0)
            return 0;

        setsid();
        umask(0);
    }

    loadCallsign(NULL);

    if (pthread_create(&clientid, NULL, &startClient, NULL) != 0)
    {
        reportError(modemName, "NXDN gateway", "Can't create NXDN host thread");
        return 1;
    }

    while (connected)
    {
        delay(50000);
        hangCheck();
    }

    dropLink();
    pthread_join(clientid, NULL);

    fprintf(stderr, "NXDN Gateway terminated.\n");
    logError(modemName, "main", "NXDN Gateway terminated.");
    return 0;
}
