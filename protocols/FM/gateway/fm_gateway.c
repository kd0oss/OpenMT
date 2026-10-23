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
 * FM EchoLink gateway.
 *
 * Connects to fm_host over TCP (port 18700 + modem - 1) and links the FM
 * repeater to an EchoLink station: outgoing, when the dashboard or a DTMF
 * command asks for it, and incoming, when a validated station connects to
 * this node. The gateway registers the node with the EchoLink directory
 * servers, so the node name and password of an EchoLink account (callsign
 * with the -L or -R suffix) are needed.
 *
 *   host -> gateway   CONN  name(12)         link to a node number or callsign
 *                     DISC                   unlink
 *                     COMM  04               re-read the configuration
 *                     ELRX  on(1)            the receiver squelch opened (1) or closed (0)
 *                     ELAU  samples          receiver audio, 8 kSa/s, 16 bit little endian
 *                     ELDT  digit(1)         DTMF digit heard on the receiver
 *   gateway -> host   CONN / NACK / DISC  name   link status (the station's callsign)
 *                     ELPT  on(1)            the linked station started (1) or stopped (0) talking
 *                     ELAU  samples          the linked station's audio, 8 kSa/s
 *
 * EchoLink uses UDP port 5198 for the audio (GSM, four 20 ms frames per
 * packet) and information text and port 5199 for the RTCP keep-alive and
 * goodbye packets. The directory servers are reached over TCP port 5200.
 * DTMF: digits then # connects to that node number, # alone disconnects, and
 * * clears what was entered.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
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

#include "elink.h"

#define VERSION "2026-10-20"
#define BUFFER_SIZE 1024

#define KEEP_ALIVE_MS       10000U
#define PEER_TIMEOUT_MS     50000U
#define CONNECT_RETRY_MS    2000U
#define CONNECT_TRIES       4U
#define LOOKUP_TIMEOUT_MS   8000U
#define TALK_TIMEOUT_MS     400U
#define REGISTER_MS         300000U
#define LIST_REFRESH_S      1800
#define LIST_MIN_AGE_S      300
#define LIST_MIN_STATIONS   100
#define DTMF_TIMEOUT_MS     10000U
#define DTMF_MAX            12U

typedef enum
{
    PEER_NONE,
    PEER_LOOKUP,
    PEER_CONNECTING,
    PEER_CONNECTED
} PeerState;

typedef struct
{
    char     callsign[32];
    char     password[64];
    char     name[32];
    char     location[64];
    char     servers[256];
    char     allow[256];
    char     deny[256];
    char     bindAddress[32];
    bool     dtmf;
    bool     allowUnlisted;
    unsigned idleTimeout;
    unsigned audioPort;
} ElConfig;

typedef struct
{
    PeerState       state;
    bool            incoming;
    struct in_addr  ip;
    char            call[EL_CALL_LEN];
    char            target[EL_CALL_LEN];
    uint64_t        startMs;
    uint64_t        lastSdesMs;
    uint64_t        lastRxMs;
    uint64_t        lastAudioMs;
    uint64_t        lastActivityMs;
    unsigned        tries;
    uint16_t        seq;
    bool            remoteTalking;
} Peer;

static int      clientPort = 18700;
static uint8_t  modemId    = 1;
static char     modemName[10] = "modem1";
static char     fmHost[80]    = "127.0.0.1";
static volatile bool connected = true;
static bool     debugM     = false;

static int             hostFd    = -1;
static pthread_mutex_t hostMutex = PTHREAD_MUTEX_INITIALIZER;

static ElConfig        cfg;
static pthread_mutex_t cfgMutex = PTHREAD_MUTEX_INITIALIZER;

static Peer            peer;
static pthread_mutex_t peerMutex = PTHREAD_MUTEX_INITIALIZER;

static ElDirList       dir;
static pthread_mutex_t dirMutex   = PTHREAD_MUTEX_INITIALIZER;
static volatile bool   wantList   = true;
static volatile bool   dirDirty   = true;
static volatile bool   dirBusy    = false;

static int      audioFd = -1;
static int      ctrlFd  = -1;
static ElCodec  codec;
static pthread_mutex_t codecMutex = PTHREAD_MUTEX_INITIALIZER;

static bool     hostTx = false;
static int16_t  txBuf[EL_PACKET_SAMPLES];
static unsigned txCnt  = 0;

static char     dtmfBuf[DTMF_MAX + 1];
static unsigned dtmfLen  = 0;
static uint64_t dtmfLast = 0;

static pthread_t hostId;
static pthread_t elinkId;
static pthread_t dirId;

static void dropPeerLocked(bool sendBye);

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
    reportError(modemName, "FM gateway", "%s: %s", msg, strerror(errno));
    exit(1);
}

/*
 * sigHandler()
 *
 * Signal handler for SIGINT and SIGTERM: asks the program to stop.
 *
 * Parameters:
 *   sig - The signal number.
 *
 * Returns: nothing.
 */
static void sigHandler(int sig)
{
    (void)sig;
    connected = false;
}

/*
 * hostSend()
 *
 * Writes one packet to fm_host. Safe to call from every thread.
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
        size_t sent = 0;

        while (sent < len)
        {
            ssize_t n = write(hostFd, buf + sent, len - sent);

            if (n < 0)
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    delay(500);
                    continue;
                }
                reportError(modemName, "FM gateway", "FM_Gateway: write to FM host failed: %s", strerror(errno));
                break;
            }
            sent += (size_t)n;
        }
    }

    pthread_mutex_unlock(&hostMutex);
}

/*
 * hostPacket()
 *
 * Builds a host packet from a four character type and a payload and sends it.
 *
 * Parameters:
 *   type    - Four character packet type.
 *   payload - The bytes after the type (may be NULL when n is 0).
 *   n       - Number of bytes in payload.
 *
 * Returns: nothing.
 */
static void hostPacket(const char* type, const uint8_t* payload, unsigned int n)
{
    uint8_t buf[BUFFER_SIZE];

    if (n + 8U > sizeof(buf))
        return;
    buf[0] = 0x61;
    buf[1] = (uint8_t)((8U + n) >> 8);
    buf[2] = (uint8_t)((8U + n) & 0xff);
    buf[3] = 4;
    memcpy(buf + 4, type, 4);
    if (n > 0)
        memcpy(buf + 8, payload, n);
    hostSend(buf, (uint16_t)(8U + n));
}

/*
 * hostReply()
 *
 * Sends a link status packet (CONN, NACK or DISC) with the station's name.
 *
 * Parameters:
 *   type - Four character packet type: CONN, NACK or DISC.
 *   name - Callsign or name of the station.
 *
 * Returns: nothing.
 */
static void hostReply(const char* type, const char* name)
{
    hostPacket(type, (const uint8_t*)name, (unsigned int)strnlen(name, 23));
}

/*
 * hostAudio()
 *
 * Sends audio from the linked station to fm_host in packets of up to 160
 * samples.
 *
 * Parameters:
 *   samples - The 8 kSa/s samples.
 *   count   - Number of samples.
 *
 * Returns: nothing.
 */
static void hostAudio(const int16_t* samples, unsigned int count)
{
    uint8_t buf[320];

    while (count > 0)
    {
        unsigned int n = count > 160U ? 160U : count;

        for (unsigned int i = 0; i < n; i++)
        {
            buf[i * 2U]      = (uint8_t)(samples[i] & 0xff);
            buf[i * 2U + 1U] = (uint8_t)((samples[i] >> 8) & 0xff);
        }
        hostPacket("ELAU", buf, n * 2U);
        samples += n;
        count -= n;
    }
}

/*
 * cfgText()
 *
 * Reads one EchoLink setting. A missing or empty setting gives the default.
 *
 * Parameters:
 *   key    - Name of the setting.
 *   def    - Default value.
 *   out    - Receives the value.
 *   outLen - Size of out, in bytes.
 *
 * Returns: nothing.
 */
static void cfgText(const char* key, const char* def, char* out, size_t outLen)
{
    char tmp[256] = "";

    readHostConfigN(modemName, "EchoLink", key, tmp, sizeof(tmp));
    snprintf(out, outLen, "%s", strlen(tmp) > 0 ? tmp : def);
}

/*
 * createDefaults()
 *
 * Adds the EchoLink settings to the dashboard database if they are not there
 * yet, in the miscellaneous tab. Settings the user already changed are kept.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void createDefaults(void)
{
    static const char* const KEYS[][2] = {
        {"callsign", ""},
        {"password", ""},
        {"name", "OpenMT"},
        {"location", "OpenMT FM repeater"},
        {"servers", "servers.echolink.org"},
        {"dtmf", "1"},
        {"allowUnlisted", "0"},
        {"allow", ""},
        {"deny", ""},
        {"idleTimeout", "0"},
        {"audioPort", "5198"},
        {"bindAddress", "0.0.0.0"}};

    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++)
        addHostConfigDefault(modemName, "EchoLink", KEYS[i][0], "misc", KEYS[i][1]);
}

/*
 * loadConfig()
 *
 * Reads the EchoLink settings from the database into cfg.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void loadConfig(void)
{
    ElConfig n;
    char     tmp[32];

    memset(&n, 0, sizeof(n));
    cfgText("callsign", "", n.callsign, sizeof(n.callsign));
    cfgText("password", "", n.password, sizeof(n.password));
    cfgText("name", "OpenMT", n.name, sizeof(n.name));
    cfgText("location", "OpenMT FM repeater", n.location, sizeof(n.location));
    cfgText("servers", "servers.echolink.org", n.servers, sizeof(n.servers));
    cfgText("allow", "", n.allow, sizeof(n.allow));
    cfgText("deny", "", n.deny, sizeof(n.deny));
    cfgText("bindAddress", "0.0.0.0", n.bindAddress, sizeof(n.bindAddress));
    cfgText("dtmf", "1", tmp, sizeof(tmp));
    n.dtmf = atoi(tmp) != 0;
    cfgText("allowUnlisted", "0", tmp, sizeof(tmp));
    n.allowUnlisted = atoi(tmp) != 0;
    cfgText("idleTimeout", "0", tmp, sizeof(tmp));
    n.idleTimeout = (unsigned)atoi(tmp);
    cfgText("audioPort", "5198", tmp, sizeof(tmp));
    n.audioPort = (unsigned)atoi(tmp);
    if (n.audioPort < 1024U || n.audioPort > 65534U)
        n.audioPort = EL_AUDIO_PORT;

    for (char* c = n.callsign; *c != '\0'; c++)
        *c = (char)toupper((unsigned char)*c);

    pthread_mutex_lock(&cfgMutex);
    cfg = n;
    pthread_mutex_unlock(&cfgMutex);
}

/*
 * cfgCopy()
 *
 * Returns a copy of the current settings.
 *
 * Parameters:
 *   out - Receives the settings.
 *
 * Returns: nothing.
 */
static void cfgCopy(ElConfig* out)
{
    pthread_mutex_lock(&cfgMutex);
    *out = cfg;
    pthread_mutex_unlock(&cfgMutex);
}

/*
 * listHas()
 *
 * Tests if a callsign is in a comma or space separated list. An entry
 * matches the callsign exactly, or without its -L or -R suffix, and an entry
 * ending in * matches every callsign that starts the same.
 *
 * Parameters:
 *   list - The list.
 *   call - The callsign, upper case.
 *
 * Returns: true if the callsign is in the list.
 */
static bool listHas(const char* list, const char* call)
{
    char  copy[256];
    char* save = NULL;
    char* tok;

    snprintf(copy, sizeof(copy), "%s", list);
    for (tok = strtok_r(copy, ", ", &save); tok != NULL; tok = strtok_r(NULL, ", ", &save))
    {
        size_t n = strlen(tok);

        for (char* c = tok; *c != '\0'; c++)
            *c = (char)toupper((unsigned char)*c);
        if (n > 0 && tok[n - 1] == '*')
        {
            if (strncmp(call, tok, n - 1) == 0)
                return true;
        }
        else if (strcmp(call, tok) == 0 || (strncmp(call, tok, n) == 0 && call[n] == '-'))
            return true;
    }
    return false;
}

/*
 * sendCtrl()
 *
 * Sends an RTCP packet to a station's control port.
 *
 * Parameters:
 *   ip  - The station's address.
 *   buf - The packet.
 *   len - Length of the packet.
 *
 * Returns: nothing.
 */
static void sendCtrl(struct in_addr ip, const uint8_t* buf, int len)
{
    struct sockaddr_in to;
    ElConfig           c;

    cfgCopy(&c);
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr   = ip;
    to.sin_port   = htons((uint16_t)(c.audioPort + 1U));
    if (sendto(ctrlFd, buf, (size_t)len, 0, (struct sockaddr*)&to, sizeof(to)) < 0)
        logMsg("EchoLink: control packet not sent: %s", strerror(errno));
    else if (debugM)
        logMsg("EchoLink: sent %d byte control packet to %s:%u", len, inet_ntoa(ip), c.audioPort + 1U);
}

/*
 * sendAudio()
 *
 * Sends a packet to a station's audio port.
 *
 * Parameters:
 *   ip  - The station's address.
 *   buf - The packet.
 *   len - Length of the packet.
 *
 * Returns: nothing.
 */
static void sendAudio(struct in_addr ip, const uint8_t* buf, int len)
{
    struct sockaddr_in to;
    ElConfig           c;

    cfgCopy(&c);
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr   = ip;
    to.sin_port   = htons((uint16_t)c.audioPort);
    sendto(audioFd, buf, (size_t)len, 0, (struct sockaddr*)&to, sizeof(to));
}

/*
 * sendSdes()
 *
 * Sends this node's source description (the keep-alive) to a station.
 *
 * Parameters:
 *   ip - The station's address.
 *
 * Returns: nothing.
 */
static void sendSdes(struct in_addr ip)
{
    uint8_t  pkt[EL_SDES_MAX];
    ElConfig c;

    cfgCopy(&c);
    int len = elMakeSdes(pkt, c.callsign, c.name, NULL);

    sendCtrl(ip, pkt, len);
}

/*
 * sendInfo()
 *
 * Sends this node's information text to a station that just connected.
 *
 * Parameters:
 *   ip - The station's address.
 *
 * Returns: nothing.
 */
static void sendInfo(struct in_addr ip)
{
    uint8_t  pkt[512];
    char     text[400];
    ElConfig c;

    cfgCopy(&c);
    snprintf(text, sizeof(text), "%s\n%s\n%s\nOpenMT FM gateway\n", c.callsign, c.name, c.location);
    int len = elMakeInfo(pkt, sizeof(pkt), text);

    if (len > 0)
        sendAudio(ip, pkt, len);
}

/*
 * peerUp()
 *
 * Marks the connection as made: tells fm_host and the directory.
 * The caller holds peerMutex.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void peerUp(void)
{
    uint64_t now = nowMs();

    peer.state          = PEER_CONNECTED;
    peer.lastRxMs       = now;
    peer.lastSdesMs     = now;
    peer.lastActivityMs = now;
    peer.remoteTalking  = false;
    dirBusy             = true;
    dirDirty            = true;
    logMsg("EchoLink: connected to %s (%s)", peer.call, peer.incoming ? "incoming" : "outgoing");
    sendInfo(peer.ip);
    hostReply("CONN", peer.call);
}

/*
 * dropPeerLocked()
 *
 * Ends the connection, if there is one: sends a goodbye when asked, and tells
 * fm_host and the directory. The caller holds peerMutex.
 *
 * Parameters:
 *   sendBye - true to send a BYE packet to the station.
 *
 * Returns: nothing.
 */
static void dropPeerLocked(bool sendBye)
{
    char call[EL_CALL_LEN];

    if (peer.state == PEER_NONE)
        return;

    if (sendBye && (peer.state == PEER_CONNECTED || peer.state == PEER_CONNECTING))
    {
        uint8_t bye[64];
        int     len = elMakeBye(bye);

        sendCtrl(peer.ip, bye, len);
    }

    snprintf(call, sizeof(call), "%s", peer.call);
    bool wasUp = peer.state == PEER_CONNECTED;

    if (peer.remoteTalking)
    {
        uint8_t off = 0;

        hostPacket("ELPT", &off, 1);
    }
    memset(&peer, 0, sizeof(peer));
    peer.state = PEER_NONE;
    dirBusy    = false;
    dirDirty   = true;
    if (wasUp)
        logMsg("EchoLink: disconnected from %s", call);
    hostReply("DISC", call);
}

/*
 * requestConnect()
 *
 * Starts a connection to a station by node number or callsign. An existing
 * connection is ended first. The lookup in the directory and the handshake
 * run on the EchoLink thread.
 *
 * Parameters:
 *   name - Node number or callsign.
 *
 * Returns: nothing.
 */
static void requestConnect(const char* name)
{
    pthread_mutex_lock(&peerMutex);
    if (peer.state != PEER_NONE)
        dropPeerLocked(true);
    peer.state   = PEER_LOOKUP;
    peer.startMs = nowMs();
    snprintf(peer.target, sizeof(peer.target), "%s", name);
    peer.incoming = false;
    pthread_mutex_unlock(&peerMutex);
}

/*
 * dtmfDigit()
 *
 * Handles one DTMF digit heard on the repeater input. Digits are collected
 * until #. Digits then # connect to that node number, # alone disconnects,
 * and * clears what was entered. Entries older than 10 seconds are dropped.
 *
 * Parameters:
 *   d - The digit.
 *
 * Returns: nothing.
 */
static void dtmfDigit(char d)
{
    ElConfig c;
    uint64_t now = nowMs();

    cfgCopy(&c);
    if (!c.dtmf)
    {
        logMsg("EchoLink: DTMF %c heard but DTMF control is turned off in the settings", d);
        return;
    }

    if (now - dtmfLast > DTMF_TIMEOUT_MS)
        dtmfLen = 0;
    dtmfLast = now;

    if (d == '*')
    {
        logMsg("EchoLink: DTMF * heard, entry cleared");
        dtmfLen = 0;
        return;
    }
    if (d == '#')
    {
        dtmfBuf[dtmfLen] = '\0';
        if (dtmfLen == 0)
        {
            logMsg("EchoLink: DTMF # heard, disconnecting");
            pthread_mutex_lock(&peerMutex);
            dropPeerLocked(true);
            pthread_mutex_unlock(&peerMutex);
        }
        else
        {
            logMsg("EchoLink: DTMF connect to %s", dtmfBuf);
            requestConnect(dtmfBuf);
        }
        dtmfLen = 0;
        return;
    }
    if (isdigit((unsigned char)d) && dtmfLen < DTMF_MAX)
    {
        dtmfBuf[dtmfLen++] = d;
        dtmfBuf[dtmfLen]   = '\0';
        logMsg("EchoLink: DTMF %c heard, entered so far: %s (# to connect, * to clear)", d, dtmfBuf);
    }
    else
    {
        logMsg("EchoLink: DTMF %c heard, entry cleared", d);
        dtmfLen = 0;
    }
}

/*
 * handleIncoming()
 *
 * Handles a source description from a station that is not connected: checks
 * that the callsign is allowed and, unless unlisted stations are allowed,
 * that the directory lists it with the address it came from, then accepts the
 * connection. A station that is not in the list yet is ignored and a refresh
 * of the list is asked for; it sends its description again.
 * The caller holds peerMutex.
 *
 * Parameters:
 *   from - The address the packet came from.
 *   pkt  - The packet.
 *   len  - Length of the packet.
 *
 * Returns: nothing.
 */
static void handleIncoming(struct in_addr from, const uint8_t* pkt, int len)
{
    char     call[EL_CALL_LEN];
    char     ipText[16];
    ElConfig c;
    bool     listed = false;
    bool     match  = false;

    if (!elStationCall(pkt, len, call, sizeof(call)))
        return;

    cfgCopy(&c);
    inet_ntop(AF_INET, &from, ipText, sizeof(ipText));

    if (listHas(c.deny, call) || (strlen(c.allow) > 0 && !listHas(c.allow, call)))
    {
        logMsg("EchoLink: refused %s (%s), not allowed", call, ipText);
        uint8_t bye[64];

        sendCtrl(from, bye, elMakeBye(bye));
        return;
    }

    if (!c.allowUnlisted)
    {
        pthread_mutex_lock(&dirMutex);
        const ElStation* st = elFindStation(&dir, call);

        if (st != NULL && strcmp(st->call, call) == 0)
        {
            listed = true;
            match  = strcmp(st->ip, ipText) == 0;
        }
        bool stale = time(NULL) - dir.fetched > LIST_MIN_AGE_S;

        pthread_mutex_unlock(&dirMutex);

        if (!listed)
        {
            if (stale)
                wantList = true;
            logMsg("EchoLink: %s (%s) is not in the directory list yet, ignored", call, ipText);
            return;
        }
        if (!match)
        {
            uint8_t bye[64];

            logMsg("EchoLink: refused %s, address %s is not the one in the directory", call, ipText);
            sendCtrl(from, bye, elMakeBye(bye));
            return;
        }
    }

    peer.state    = PEER_CONNECTED;
    peer.incoming = true;
    peer.ip       = from;
    snprintf(peer.call, sizeof(peer.call), "%s", call);
    peerUp();
    sendSdes(from);
}

/*
 * handleCtrl()
 *
 * Handles one packet from the control port: a goodbye ends the connection, a
 * source description from the connected station keeps it alive (and completes
 * an outgoing connection), and one from another station is an incoming
 * connection, or is turned away if the node is busy.
 *
 * Parameters:
 *   from - The address the packet came from.
 *   pkt  - The packet.
 *   len  - Length of the packet.
 *
 * Returns: nothing.
 */
static void handleCtrl(struct in_addr from, const uint8_t* pkt, int len)
{
    if (debugM)
    {
        char ipText[16];

        inet_ntop(AF_INET, &from, ipText, sizeof(ipText));
        logMsg("EchoLink: control packet, %d bytes from %s: %s", len, ipText,
               elIsBye(pkt, len) ? "BYE" : (elIsSdes(pkt, len) ? "SDES" : "other"));
        hexDump("EchoLink control", pkt, (unsigned int)len);
    }

    pthread_mutex_lock(&peerMutex);

    bool isPeer = peer.state != PEER_NONE && peer.state != PEER_LOOKUP && peer.ip.s_addr == from.s_addr;

    if (elIsBye(pkt, len))
    {
        if (isPeer)
            dropPeerLocked(false);
    }
    else if (elIsSdes(pkt, len))
    {
        if (isPeer)
        {
            peer.lastRxMs = nowMs();
            if (peer.state == PEER_CONNECTING)
                peerUp();
        }
        else if (peer.state == PEER_NONE)
            handleIncoming(from, pkt, len);
        else
        {
            uint8_t bye[64];

            sendCtrl(from, bye, elMakeBye(bye));
        }
    }

    pthread_mutex_unlock(&peerMutex);
}

/*
 * handleAudio()
 *
 * Handles one packet from the audio port: voice from the connected station
 * is decoded and passed to fm_host; information and chat text is logged.
 *
 * Parameters:
 *   from - The address the packet came from.
 *   pkt  - The packet.
 *   len  - Length of the packet.
 *
 * Returns: nothing.
 */
static void handleAudio(struct in_addr from, const uint8_t* pkt, int len)
{
    int16_t samples[EL_PACKET_SAMPLES];
    bool    ok;
    bool    startTalk = false;

    if (debugM && !elIsInfo(pkt, len))
    {
        char ipText[16];

        inet_ntop(AF_INET, &from, ipText, sizeof(ipText));
        logMsg("EchoLink: audio packet, %d bytes from %s", len, ipText);
    }

    pthread_mutex_lock(&peerMutex);
    ok = peer.state == PEER_CONNECTED && peer.ip.s_addr == from.s_addr;
    if (ok)
        peer.lastRxMs = nowMs();
    pthread_mutex_unlock(&peerMutex);

    if (!ok)
        return;

    if (elIsInfo(pkt, len))
    {
        if (debugM)
            logMsg("EchoLink: info from %s: %.60s", peer.call, (const char*)pkt + 6);
        return;
    }

    pthread_mutex_lock(&codecMutex);
    ok = elParseVoice(&codec, pkt, len, samples);
    pthread_mutex_unlock(&codecMutex);
    if (!ok)
        return;

    pthread_mutex_lock(&peerMutex);
    if (peer.state == PEER_CONNECTED)
    {
        startTalk           = !peer.remoteTalking;
        peer.remoteTalking  = true;
        peer.lastAudioMs    = nowMs();
        peer.lastActivityMs = peer.lastAudioMs;
    }
    pthread_mutex_unlock(&peerMutex);

    if (startTalk)
    {
        uint8_t on = 1;

        hostPacket("ELPT", &on, 1);
    }
    hostAudio(samples, EL_PACKET_SAMPLES);
}

/*
 * sendVoice()
 *
 * Encodes a full buffer of the repeater's audio and sends it to the
 * connected station.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void sendVoice(void)
{
    uint8_t        pkt[EL_VOICE_LENGTH];
    struct in_addr ip;
    uint16_t       seq;
    bool           ok;

    pthread_mutex_lock(&peerMutex);
    ok = peer.state == PEER_CONNECTED;
    ip = peer.ip;
    seq = peer.seq++;
    if (ok)
        peer.lastActivityMs = nowMs();
    pthread_mutex_unlock(&peerMutex);

    if (!ok)
        return;

    pthread_mutex_lock(&codecMutex);
    int len = elMakeVoice(&codec, pkt, seq, txBuf);
    pthread_mutex_unlock(&codecMutex);
    sendAudio(ip, pkt, len);
}

/*
 * tick()
 *
 * Periodic work of the connection, about every 50 ms: looks up a requested
 * station, repeats the handshake, sends the keep-alive, ends the connection
 * when the station stopped answering, reports when the station stopped
 * talking, and applies the idle time limit.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void tick(void)
{
    uint64_t now = nowMs();
    ElConfig c;

    cfgCopy(&c);
    pthread_mutex_lock(&peerMutex);

    switch (peer.state)
    {
        case PEER_LOOKUP:
        {
            ElStation st;
            bool      found = false;
            bool      stale = false;

            pthread_mutex_lock(&dirMutex);
            const ElStation* p = elFindStation(&dir, peer.target);

            if (p != NULL)
            {
                st    = *p;
                found = true;
            }
            stale = time(NULL) - dir.fetched > LIST_MIN_AGE_S;
            pthread_mutex_unlock(&dirMutex);

            if (found)
            {
                struct in_addr ip;

                if (inet_pton(AF_INET, st.ip, &ip) == 1)
                {
                    peer.ip = ip;
                    snprintf(peer.call, sizeof(peer.call), "%s", st.call);
                    logMsg("EchoLink: calling %s at %s", st.call, st.ip);
                    peer.state      = PEER_CONNECTING;
                    peer.tries      = 1;
                    peer.lastSdesMs = now;
                    sendSdes(ip);
                    break;
                }
            }
            if (now - peer.startMs > LOOKUP_TIMEOUT_MS)
            {
                logMsg("EchoLink: station %s not found in the directory", peer.target);
                char name[EL_CALL_LEN];

                snprintf(name, sizeof(name), "%s", peer.target);
                memset(&peer, 0, sizeof(peer));
                hostReply("NACK", name);
            }
            else if (stale)
                wantList = true;
        }
        break;

        case PEER_CONNECTING:
            if (now - peer.lastSdesMs >= CONNECT_RETRY_MS)
            {
                if (peer.tries >= CONNECT_TRIES)
                {
                    char name[EL_CALL_LEN];

                    logMsg("EchoLink: %s did not answer", peer.call);
                    snprintf(name, sizeof(name), "%s", peer.call);
                    memset(&peer, 0, sizeof(peer));
                    hostReply("NACK", name);
                }
                else
                {
                    peer.tries++;
                    peer.lastSdesMs = now;
                    sendSdes(peer.ip);
                }
            }
            break;

        case PEER_CONNECTED:
            if (now - peer.lastSdesMs >= KEEP_ALIVE_MS)
            {
                peer.lastSdesMs = now;
                sendSdes(peer.ip);
            }
            if (now - peer.lastRxMs > PEER_TIMEOUT_MS)
            {
                logMsg("EchoLink: %s timed out", peer.call);
                dropPeerLocked(true);
            }
            else if (peer.remoteTalking && now - peer.lastAudioMs > TALK_TIMEOUT_MS)
            {
                uint8_t off = 0;

                peer.remoteTalking = false;
                hostPacket("ELPT", &off, 1);
            }
            else if (c.idleTimeout > 0 && now - peer.lastActivityMs > (uint64_t)c.idleTimeout * 60000U)
            {
                logMsg("EchoLink: idle time limit, disconnecting %s", peer.call);
                dropPeerLocked(true);
            }
            break;

        default:
            break;
    }

    pthread_mutex_unlock(&peerMutex);
}

/*
 * elinkThread()
 *
 * Thread that serves the EchoLink sockets: receives the audio and control
 * packets and runs the periodic work.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL.
 */
static void* elinkThread(void* arg)
{
    uint8_t buf[2048];

    (void)arg;

    while (connected)
    {
        fd_set         rfds;
        struct timeval tv;
        int            maxfd = audioFd > ctrlFd ? audioFd : ctrlFd;

        FD_ZERO(&rfds);
        FD_SET(audioFd, &rfds);
        FD_SET(ctrlFd, &rfds);
        tv.tv_sec  = 0;
        tv.tv_usec = 50000;

        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        if (r > 0)
        {
            struct sockaddr_in from;
            socklen_t          fl = sizeof(from);

            if (FD_ISSET(ctrlFd, &rfds))
            {
                ssize_t n = recvfrom(ctrlFd, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);

                if (n > 0)
                    handleCtrl(from.sin_addr, buf, (int)n);
            }
            if (FD_ISSET(audioFd, &rfds))
            {
                fl = sizeof(from);
                ssize_t n = recvfrom(audioFd, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);

                if (n > 0)
                    handleAudio(from.sin_addr, buf, (int)n);
            }
        }
        tick();
    }

    return NULL;
}

/*
 * serverList()
 *
 * Splits the configured directory servers (comma or space separated) into
 * a list.
 *
 * Parameters:
 *   servers - The configured text.
 *   out     - Receives the servers.
 *   max     - Size of out.
 *   storage - Buffer that holds the text of the servers.
 *   size    - Size of storage, in bytes.
 *
 * Returns: The number of servers.
 */
static int serverList(const char* servers, char** out, int max, char* storage, size_t size)
{
    char* save = NULL;
    int   n    = 0;

    snprintf(storage, size, "%s", servers);
    for (char* tok = strtok_r(storage, ", ", &save); tok != NULL && n < max; tok = strtok_r(NULL, ", ", &save))
        out[n++] = tok;
    return n;
}

/*
 * registerNode()
 *
 * Registers this node with the directory, trying each server until one
 * answers OK.
 *
 * Parameters:
 *   c     - The settings.
 *   state - "ONLINE", "BUSY" or "OFF".
 *
 * Returns: true if a server accepted the registration.
 */
static char regServer[96];

static bool registerNode(const ElConfig* c, const char* state)
{
    char* servers[8];
    char  storage[256];
    char  reply[128];
    int   n = serverList(c->servers, servers, 8, storage, sizeof(storage));

    for (int i = 0; i < n; i++)
    {
        if (elDirRegister(servers[i], c->callsign, c->password, state, c->location, reply, sizeof(reply)))
        {
            snprintf(regServer, sizeof(regServer), "%s", servers[i]);
            return true;
        }
        reply[strcspn(reply, "\r\n")] = '\0';
        logMsg("EchoLink: directory %s said: %.60s", servers[i], reply);
    }
    return false;
}

/*
 * fetchList()
 *
 * Gets the station list from the first directory server that answers and
 * makes it the current list.
 *
 * Parameters:
 *   c - The settings.
 *
 * Returns: true if a list was received.
 */
static bool fetchList(const ElConfig* c)
{
    char      cache[64];

    char*     servers[8];
    char      storage[256];
    ElDirList fresh;
    int       n = serverList(c->servers, servers, 8, storage, sizeof(storage));

    snprintf(cache, sizeof(cache), "/tmp/fm_elink_%s.list", modemName);
    memset(&fresh, 0, sizeof(fresh));
    /* the list is only given to a station the server knows is logged in, so ask the server that accepted the login first */
    for (int i = 1; i < n && strlen(regServer) > 0; i++)
    {
        if (strcmp(servers[i], regServer) == 0)
        {
            char* t = servers[0];

            servers[0] = servers[i];
            servers[i] = t;
            break;
        }
    }
    for (int i = 0; i < n; i++)
    {
        if (elDirFetch(servers[i], &fresh, cache))
        {
            if (fresh.count == 0)
            {
                /* a list with no stations is the server's way of refusing, for example "NOT LOGGED IN" */
                logMsg("EchoLink: the server sent no stations: %.80s", strlen(fresh.message) > 0 ? fresh.message : "(no message)");
                elFreeList(&fresh);
                return false;
            }
            pthread_mutex_lock(&dirMutex);
            bool keepOld = fresh.count < LIST_MIN_STATIONS && dir.count >= LIST_MIN_STATIONS;

            if (keepOld)
                dir.fetched = time(NULL);
            else
            {
                elFreeList(&dir);
                dir = fresh;
            }
            pthread_mutex_unlock(&dirMutex);
            if (keepOld)
            {
                logMsg("EchoLink: the server sent only %d stations, keeping the list it sent before (it limits how often the list can be fetched)", fresh.count);
                elFreeList(&fresh);
            }
            else
                logMsg("EchoLink: directory list has %d stations", fresh.count);
            if (strlen(fresh.message) > 0)
                logMsg("EchoLink: directory message: %.80s", fresh.message);
            return true;
        }
    }
    return false;
}

/*
 * dirThread()
 *
 * Thread that keeps the node registered with the EchoLink directory (online
 * when idle, busy while connected, refreshed every five minutes) and fetches
 * the station list when it is needed to look up or validate a station.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL.
 */
static void* dirThread(void* arg)
{
    uint64_t lastReg   = 0;
    uint64_t lastWarn  = 0;
    bool     registered = false;

    (void)arg;

    while (connected)
    {
        ElConfig c;
        uint64_t now = nowMs();

        cfgCopy(&c);

        if (strlen(c.callsign) == 0 || strlen(c.password) == 0)
        {
            if (now - lastWarn > 300000U || lastWarn == 0)
            {
                lastWarn = now ? now : 1;
                logMsg("EchoLink: set the callsign and password in the EchoLink settings to register with the directory.");
            }
        }
        else if (dirDirty || !registered || now - lastReg >= REGISTER_MS)
        {
            dirDirty = false;
            if (registerNode(&c, dirBusy ? "BUSY" : "ONLINE"))
            {
                if (!registered)
                {
                    logMsg("EchoLink: registered as %s", c.callsign);
                    for (int k = 0; k < 30; k++)
                        delay(100000); /* let the directory servers see the login before asking for the list */
                }
                registered = true;
                lastReg    = now;
            }
            else
            {
                registered = false;
                lastReg    = now - REGISTER_MS + 30000U;
                logMsg("EchoLink: registration failed, trying again in 30 seconds");
            }
        }

        pthread_mutex_lock(&dirMutex);
        bool old = time(NULL) - dir.fetched > LIST_REFRESH_S;
        pthread_mutex_unlock(&dirMutex);

        if (registered && (wantList || old))
        {
            static time_t lastTry = 0;
            time_t        t       = time(NULL);

            if (lastTry == 0 || t - lastTry >= LIST_MIN_AGE_S)
            {
                lastTry  = t;
                wantList = false;
                if (!fetchList(&c))
                {
                    logMsg("EchoLink: could not get the station list, trying again in 30 seconds");
                    lastTry = t - LIST_MIN_AGE_S + 30;
                }
            }
        }

        for (int i = 0; i < 10 && connected && !dirDirty && !wantList; i++)
            delay(100000);
    }

    ElConfig c;

    cfgCopy(&c);
    if (registered)
        registerNode(&c, "OFF");
    return NULL;
}

/*
 * handleHostPacket()
 *
 * One complete packet from fm_host.
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

    if (memcmp(buffer + 4, "ELAU", 4) == 0)
    {
        unsigned int count = (respLen - 8U) / 2U;

        if (!hostTx)
            return;
        for (unsigned int i = 0; i < count; i++)
        {
            txBuf[txCnt++] = (int16_t)(buffer[8 + i * 2] | (buffer[9 + i * 2] << 8));
            if (txCnt >= EL_PACKET_SAMPLES)
            {
                sendVoice();
                txCnt = 0;
            }
        }
    }
    else if (memcmp(buffer + 4, "ELRX", 4) == 0)
    {
        if (respLen > 8 && buffer[8] != 0)
        {
            hostTx = true;
            txCnt  = 0;
        }
        else
        {
            if (hostTx && txCnt > 0)
            {
                memset(txBuf + txCnt, 0, (EL_PACKET_SAMPLES - txCnt) * sizeof(int16_t));
                sendVoice();
            }
            hostTx = false;
            txCnt  = 0;
        }
    }
    else if (memcmp(buffer + 4, "ELDT", 4) == 0)
    {
        if (respLen > 8)
            dtmfDigit((char)buffer[8]);
    }
    else if (memcmp(buffer + 4, "COMM", 4) == 0)
    {
        if (respLen >= 9 && buffer[8] == 0x04)
        {
            loadConfig();
            dirDirty = true;
        }
    }
    else if (memcmp(buffer + 4, "CONN", 4) == 0)
    {
        char name[13];

        memset(name, 0, sizeof(name));
        memcpy(name, buffer + 8, respLen - 8U > 12U ? 12U : respLen - 8U);
        name[strcspn(name, " ")] = '\0';
        if (strlen(name) > 0)
            requestConnect(name);
        else
            hostReply("NACK", "");
    }
    else if (memcmp(buffer + 4, "DISC", 4) == 0)
    {
        pthread_mutex_lock(&peerMutex);
        if (peer.state == PEER_NONE)
            hostReply("DISC", "");
        else
            dropPeerLocked(true);
        pthread_mutex_unlock(&peerMutex);
    }
}

/*
 * readFull()
 *
 * Reads exactly len bytes from a socket.
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
 * connectHost()
 *
 * Opens the TCP connection to fm_host, retrying until it is up.
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
        int                fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0)
        {
            reportError(modemName, "FM gateway", "Socket creation error: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }

        memset(&serv_addr, 0x00, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port   = htons(clientPort);

        if (!hostToAddress(fmHost, &serv_addr.sin_addr))
        {
            reportError(modemName, "FM gateway", "Invalid address/ Address not supported");
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
 * Thread that connects to fm_host and handles its packets. When the host
 * goes away the connection to the station is ended and the connection to the
 * host is made again.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL.
 */
static void* startClient(void* arg)
{
    uint8_t buffer[BUFFER_SIZE];

    (void)arg;

    while (connected)
    {
        uint16_t respLen;
        int      fd = connectHost();

        if (fd < 0)
            break;

        pthread_mutex_lock(&hostMutex);
        hostFd = fd;
        pthread_mutex_unlock(&hostMutex);

        printf("Connected to FM host.\n");
        fflush(stdout);

        loadConfig();
        hostTx = false;
        txCnt  = 0;

        while (connected)
        {
            if (!readFull(fd, buffer, 1))
                break;

            if (buffer[0] != 0x61)
            {
                fprintf(stderr, "FM_Gateway: unknown byte from FM host, 0x%02X\n", buffer[0]);
                continue;
            }

            if (!readFull(fd, buffer + 1, 3))
                break;

            respLen = (uint16_t)((buffer[1] << 8) + buffer[2]);

            if (respLen < 4 || respLen > BUFFER_SIZE)
            {
                fprintf(stderr, "FM_Gateway: bad packet length %u from FM host\n", respLen);
                continue;
            }

            if (!readFull(fd, buffer + 4, respLen - 4))
                break;

            handleHostPacket(buffer, respLen);
        }

        logMsg("FM host disconnected.");

        pthread_mutex_lock(&hostMutex);
        hostFd = -1;
        pthread_mutex_unlock(&hostMutex);

        pthread_mutex_lock(&peerMutex);
        dropPeerLocked(true);
        pthread_mutex_unlock(&peerMutex);
        close(fd);

        if (connected)
            sleep(2);
    }

    fprintf(stderr, "Client thread exited.\n");
    connected = false;
    return NULL;
}

/*
 * openSockets()
 *
 * Opens the two UDP sockets of EchoLink, the audio port and the control port
 * next to it, on the configured address.
 *
 * Parameters: none.
 *
 * Returns: true if both sockets are open.
 */
static bool openSockets(void)
{
    ElConfig           c;
    struct sockaddr_in addr;
    int                on = 1;

    cfgCopy(&c);
    audioFd = socket(AF_INET, SOCK_DGRAM, 0);
    ctrlFd  = socket(AF_INET, SOCK_DGRAM, 0);
    if (audioFd < 0 || ctrlFd < 0)
        return false;

    setsockopt(audioFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(ctrlFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, c.bindAddress, &addr.sin_addr) != 1)
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

    addr.sin_port = htons((uint16_t)c.audioPort);
    if (bind(audioFd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        reportError(modemName, "FM gateway", "FM_Gateway: cannot bind UDP port %u: %s", c.audioPort, strerror(errno));
        return false;
    }
    addr.sin_port = htons((uint16_t)(c.audioPort + 1U));
    if (bind(ctrlFd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        reportError(modemName, "FM gateway", "FM_Gateway: cannot bind UDP port %u: %s", c.audioPort + 1U, strerror(errno));
        return false;
    }
    return true;
}

/*
 * main()
 *
 * Program entry. Parses the command line (-m modem number 1-10, -d daemon,
 * -v version, -x debug), opens the EchoLink sockets, starts the threads and
 * waits until it is stopped.
 *
 * Parameters:
 *   argc - Number of command line arguments.
 *   argv - Command line arguments.
 *
 * Returns: 0 on exit, 1 on a bad argument or start-up error.
 */
int main(int argc, char** argv)
{
    bool daemon = false;
    int  c;

    while ((c = getopt(argc, argv, ":m:a:dxv")) != -1)
    {
        switch (c)
        {
            case 'm':
            {
                modemId = (uint8_t)atoi(optarg);
                if (modemId < 1 || modemId > 10)
                {
                    reportError(modemName, "FM gateway", "Invalid modem number.");
                    return 1;
                }
                sprintf(modemName, "modem%d", modemId);
                clientPort = 18700 + modemId - 1;
                fprintf(stderr, "Modem name: %s\n", modemName);
            }
            break;
            case 'a':
                snprintf(fmHost, sizeof(fmHost), "%s", optarg);   /* where the protocol service runs */
                break;
            case ':':
                fprintf(stderr, "Option '-%c' requires a value.\n", optopt);
                return 1;
            case 'd':
                daemon = true;
                break;
            case 'v':
                fprintf(stdout, "FM_Gateway: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: FM_Gateway [-m modem_number (1-10)] [-a address] [-d] [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            reportError(modemName, "FM gateway", "FM_Gateway: error in fork(), exiting");
            return 1;
        }

        if (pid > 0)
            return 0;

        setsid();
        umask(0);
    }

    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigHandler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    memset(&peer, 0, sizeof(peer));
    memset(&dir, 0, sizeof(dir));
    createDefaults();
    loadConfig();

    {
        char cache[64];

        snprintf(cache, sizeof(cache), "/tmp/fm_elink_%s.list", modemName);
        if (elLoadCache(cache, &dir) && dir.count < LIST_MIN_STATIONS)
        {
            logMsg("EchoLink: ignoring the saved list, it has only %d stations", dir.count);
            elFreeList(&dir);
            unlink(cache);
        }
        if (dir.count > 0)
        {
            logMsg("EchoLink: loaded %d stations saved earlier", dir.count);
            if (time(NULL) - dir.fetched < LIST_REFRESH_S / 3)
                wantList = false;
        }
    }

    if (!elCodecInit(&codec))
    {
        reportError(modemName, "FM gateway", "FM_Gateway: cannot start the GSM codec");
        return 1;
    }

    if (!openSockets())
        return 1;

    if (pthread_create(&elinkId, NULL, &elinkThread, NULL) != 0 || pthread_create(&dirId, NULL, &dirThread, NULL) != 0 ||
        pthread_create(&hostId, NULL, &startClient, NULL) != 0)
    {
        reportError(modemName, "FM gateway", "Can't create the gateway threads");
        return 1;
    }

    while (connected)
        delay(50000);

    pthread_mutex_lock(&peerMutex);
    dropPeerLocked(true);
    pthread_mutex_unlock(&peerMutex);

    pthread_join(dirId, NULL);
    pthread_join(elinkId, NULL);
    elFreeList(&dir);
    elCodecFree(&codec);

    fprintf(stderr, "FM Gateway terminated.\n");
    logError(modemName, "main", "FM Gateway terminated.");
    return 0;
}
