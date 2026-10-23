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
 * EchoLink protocol pieces used by fm_gateway. See elink.h.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gsm.h>

#include "elink.h"

#define RTP_VERSION 3   /* EchoLink clients and servers put version 3 in the RTCP headers (first byte 0xC0) */
#define RTCP_RR     201
#define RTCP_SDES   202
#define RTCP_BYE    203
#define RTCP_SR     200
#define SDES_END    0

#define DIR_IO_TIMEOUT_MS   10000
#define DIR_LIST_MAX        (4U * 1024U * 1024U)

/*
 * addText()
 *
 * Appends a length prefixed text item to an RTCP packet under construction.
 *
 * Parameters:
 *   p    - The write position, moved past the item.
 *   text - The text to append (at most 255 characters).
 *
 * Returns: nothing.
 */
static void addText(uint8_t** p, const char* text)
{
    size_t len = strlen(text);

    if (len > 255)
        len = 255;
    *(*p)++ = (uint8_t)len;
    memcpy(*p, text, len);
    *p += len;
}

/*
 * elCodecInit()
 *
 * Creates the GSM encoder and decoder.
 *
 * Parameters:
 *   c - The codec.
 *
 * Returns: true if both were created, false if the library failed.
 */
bool elCodecInit(ElCodec* c)
{
    c->enc = gsm_create();
    c->dec = gsm_create();
    if (c->enc == NULL || c->dec == NULL)
    {
        elCodecFree(c);
        return false;
    }
    return true;
}

/*
 * elCodecFree()
 *
 * Releases the GSM encoder and decoder.
 *
 * Parameters:
 *   c - The codec.
 *
 * Returns: nothing.
 */
void elCodecFree(ElCodec* c)
{
    if (c->enc != NULL)
        gsm_destroy((gsm)c->enc);
    if (c->dec != NULL)
        gsm_destroy((gsm)c->dec);
    c->enc = NULL;
    c->dec = NULL;
}

/*
 * elMakeSdes()
 *
 * Builds the RTCP packet that opens an EchoLink connection and is repeated as
 * its keep-alive: an empty receiver report followed by the source
 * description with the callsign, the name, and the optional private item.
 *
 * Parameters:
 *   p        - Receives the packet (at least EL_SDES_MAX bytes).
 *   callsign - The station's callsign.
 *   name     - The operator or location text sent after the callsign.
 *   priv     - Private item text, or NULL for none.
 *
 * Returns: The length of the packet.
 */
int elMakeSdes(uint8_t* p, const char* callsign, const char* name, const char* priv)
{
    uint8_t* start = p;
    uint8_t* ap;
    char     tmp[256];
    int      len;

    *p++ = RTP_VERSION << 6;
    *p++ = RTCP_RR;
    *p++ = 0;
    *p++ = 1;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;

    uint8_t* sdes = p;

    sdes[0] = (uint8_t)((RTP_VERSION << 6) | 1);
    sdes[1] = RTCP_SDES;
    sdes[4] = sdes[5] = sdes[6] = sdes[7] = 0;

    ap = sdes + 8;
    *ap++ = EL_SDES_CNAME;
    addText(&ap, "CALLSIGN");
    *ap++ = EL_SDES_NAME;
    snprintf(tmp, sizeof(tmp), "%-15s%s", callsign, name);
    addText(&ap, tmp);
    *ap++ = EL_SDES_EMAIL;
    addText(&ap, "CALLSIGN");
    *ap++ = 4;
    addText(&ap, "08:30");
    if (priv != NULL)
    {
        *ap++ = EL_SDES_PRIV;
        addText(&ap, priv);
    }
    *ap++ = SDES_END;
    *ap++ = 0;
    while ((ap - sdes) & 3)
        *ap++ = 0;

    len      = (int)((ap - sdes) / 4) - 1;
    sdes[2]  = (uint8_t)(len >> 8);
    sdes[3]  = (uint8_t)len;
    return (int)(ap - start);
}

/*
 * elMakeBye()
 *
 * Builds the RTCP packet that ends an EchoLink connection: an empty receiver
 * report followed by a BYE with the text EchoLink clients send.
 *
 * Parameters:
 *   p - Receives the packet (at least 32 bytes).
 *
 * Returns: The length of the packet.
 */
int elMakeBye(uint8_t* p)
{
    uint8_t* start = p;

    *p++ = RTP_VERSION << 6;
    *p++ = RTCP_RR;
    *p++ = 0;
    *p++ = 1;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;

    uint8_t* bye = p;
    uint8_t* ap;

    bye[0] = (uint8_t)((RTP_VERSION << 6) | 1);
    bye[1] = RTCP_BYE;
    bye[4] = bye[5] = bye[6] = bye[7] = 0;
    ap     = bye + 8;
    addText(&ap, "jan2002");
    while ((ap - bye) & 3)
        *ap++ = 0;
    int len = (int)((ap - bye) / 4) - 1;

    bye[2] = (uint8_t)(len >> 8);
    bye[3] = (uint8_t)len;
    return (int)(ap - start);
}

/*
 * rtcpHas()
 *
 * Walks a compound RTCP packet and looks for a sub-packet of one type. The
 * packet must start with a sender or receiver report.
 *
 * Parameters:
 *   p    - The packet.
 *   len  - Length of the packet.
 *   type - The RTCP packet type to look for.
 *
 * Returns: true if the type is in the packet.
 */
static bool rtcpHas(const uint8_t* p, int len, uint8_t type)
{
    const uint8_t* end = p + len;
    bool           found = false;

    if (len < 4)
        return false;
    if ((((p[0] >> 6) & 3) < 1) || (p[0] & 0x20) != 0 ||
        (p[1] != RTCP_SR && p[1] != RTCP_RR))
        return false;

    while (p + 4 <= end && (((p[0] >> 6) & 3) >= 1))
    {
        int sub = (((p[2] << 8) | p[3]) + 1) * 4;

        if (p[1] == type)
            found = true;
        if (sub <= 0 || p + sub > end)
            break;
        p += sub;
    }
    return found;
}

/*
 * elIsBye()
 *
 * Tests if a packet on the control port contains a BYE.
 *
 * Parameters:
 *   p   - The packet.
 *   len - Length of the packet.
 *
 * Returns: true if it does.
 */
bool elIsBye(const uint8_t* p, int len)
{
    return rtcpHas(p, len, RTCP_BYE);
}

/*
 * elIsSdes()
 *
 * Tests if a packet on the control port contains a source description.
 *
 * Parameters:
 *   p   - The packet.
 *   len - Length of the packet.
 *
 * Returns: true if it does.
 */
bool elIsSdes(const uint8_t* p, int len)
{
    return rtcpHas(p, len, RTCP_SDES);
}

/*
 * elParseSdes()
 *
 * Finds one item of the source description in a compound RTCP packet. Every
 * length is checked against the end of the packet, so a damaged packet
 * cannot make it read beyond it.
 *
 * Parameters:
 *   text    - Receives the item text, zero terminated.
 *   textLen - Size of text, in bytes.
 *   p       - The packet.
 *   len     - Length of the packet.
 *   item    - The item type (EL_SDES_NAME, ...).
 *
 * Returns: true if the item was found.
 */
bool elParseSdes(char* text, size_t textLen, const uint8_t* p, int len, uint8_t item)
{
    const uint8_t* end = p + len;

    if (textLen == 0)
        return false;
    text[0] = '\0';

    while (p + 4 <= end && (((p[0] >> 6) & 3) >= 1))
    {
        int sub = (((p[2] << 8) | p[3]) + 1) * 4;

        if (sub <= 0 || p + sub > end)
            break;

        if (p[1] == RTCP_SDES && (p[0] & 0x1F) > 0)
        {
            const uint8_t* cp = p + 8;
            const uint8_t* lp = p + sub;

            while (cp + 2 <= lp)
            {
                uint8_t itype = cp[0];
                uint8_t ilen  = cp[1];

                if (itype == SDES_END || cp + 2 + ilen > lp)
                    break;
                if (itype == item)
                {
                    size_t n = ilen < textLen - 1 ? ilen : textLen - 1;

                    memcpy(text, cp + 2, n);
                    text[n] = '\0';
                    return true;
                }
                cp += ilen + 2;
            }
            return false;
        }
        p += sub;
    }
    return false;
}

/*
 * elSanitizeCallsign()
 *
 * Removes the characters that are not valid in an EchoLink callsign and
 * limits the length. The callsign in a received packet is controlled by the
 * remote station and ends up in logs and the dashboard, so only letters,
 * digits and - / * _ are kept. The string is changed in place.
 *
 * Parameters:
 *   call - The callsign.
 *
 * Returns: nothing.
 */
void elSanitizeCallsign(char* call)
{
    char*       dst = call;
    const char* src;

    for (src = call; *src != '\0' && (dst - call) < (int)EL_CALL_LEN - 1; src++)
    {
        char ch = *src;

        if (isalnum((unsigned char)ch) || ch == '-' || ch == '/' || ch == '*' || ch == '_')
            *dst++ = ch;
    }
    *dst = '\0';
}

/*
 * elStationCall()
 *
 * Gets the callsign of the station that sent a source description. The name
 * item holds the callsign in the first 15 characters followed by the name.
 *
 * Parameters:
 *   p       - The packet.
 *   len     - Length of the packet.
 *   call    - Receives the callsign, upper case and sanitized.
 *   callLen - Size of call, in bytes (at least EL_CALL_LEN).
 *
 * Returns: true if a callsign was found.
 */
bool elStationCall(const uint8_t* p, int len, char* call, size_t callLen)
{
    char text[EL_SDES_MAX];

    if (!elParseSdes(text, sizeof(text), p, len, EL_SDES_NAME))
        return false;

    char* space = strchr(text, ' ');

    if (space != NULL)
        *space = '\0';
    elSanitizeCallsign(text);
    for (char* c = text; *c != '\0'; c++)
        *c = (char)toupper((unsigned char)*c);
    snprintf(call, callLen, "%s", text);
    return strlen(call) > 0;
}

/*
 * elMakeVoice()
 *
 * Builds one EchoLink audio packet: the 12 byte header and four GSM frames
 * (80 ms of audio).
 *
 * Parameters:
 *   c       - The codec.
 *   out     - Receives the packet (EL_VOICE_LENGTH bytes).
 *   seq     - Sequence number.
 *   samples - EL_PACKET_SAMPLES samples at 8 kSa/s.
 *
 * Returns: The length of the packet.
 */
int elMakeVoice(ElCodec* c, uint8_t* out, uint16_t seq, const int16_t* samples)
{
    memset(out, 0, EL_VOICE_HEADER);
    out[0] = 0xc0;
    out[1] = 0x03;
    out[2] = (uint8_t)(seq >> 8);
    out[3] = (uint8_t)(seq & 0xff);

    for (unsigned int i = 0; i < EL_FRAMES; i++)
        gsm_encode((gsm)c->enc, (gsm_signal*)(samples + i * EL_FRAME_SAMPLES), out + EL_VOICE_HEADER + i * EL_GSM_FRAME_BYTES);
    return (int)EL_VOICE_LENGTH;
}

/*
 * elParseVoice()
 *
 * Decodes one received EchoLink audio packet. Packets that are too short or
 * use another codec than GSM are refused.
 *
 * Parameters:
 *   c       - The codec.
 *   p       - The packet.
 *   len     - Length of the packet.
 *   samples - Receives EL_PACKET_SAMPLES samples at 8 kSa/s.
 *
 * Returns: true if the packet was decoded.
 */
bool elParseVoice(ElCodec* c, const uint8_t* p, int len, int16_t* samples)
{
    if (len < (int)EL_VOICE_LENGTH || p[1] != 0x03)
        return false;

    for (unsigned int i = 0; i < EL_FRAMES; i++)
    {
        if (gsm_decode((gsm)c->dec, (gsm_byte*)(p + EL_VOICE_HEADER + i * EL_GSM_FRAME_BYTES),
                       (gsm_signal*)(samples + i * EL_FRAME_SAMPLES)) != 0)
            return false;
    }
    return true;
}

/*
 * elMakeInfo()
 *
 * Builds the information text packet a station sends on the audio port when
 * the connection is made: "oNDATA", a carriage return, the text with line
 * breaks as carriage returns, and a terminating zero.
 *
 * Parameters:
 *   out    - Receives the packet.
 *   outLen - Size of out, in bytes.
 *   text   - The information text.
 *
 * Returns: The length of the packet, 0 if it does not fit.
 */
int elMakeInfo(uint8_t* out, size_t outLen, const char* text)
{
    size_t n = strlen(text);

    if (n + 8 > outLen)
        return 0;
    memcpy(out, "oNDATA\r", 7);
    for (size_t i = 0; i < n; i++)
        out[7 + i] = (text[i] == '\n') ? '\r' : (uint8_t)text[i];
    out[7 + n] = 0;
    return (int)(n + 8);
}

/*
 * elIsInfo()
 *
 * Tests if a packet on the audio port is information or chat text instead
 * of voice.
 *
 * Parameters:
 *   p   - The packet.
 *   len - Length of the packet.
 *
 * Returns: true if it is.
 */
bool elIsInfo(const uint8_t* p, int len)
{
    return len > 6 && p[0] == 'o' && memcmp(p + 1, "NDATA", 5) == 0;
}

/*
 * elMakeLogin()
 *
 * Builds the text sent to the directory server to register the station.
 *
 * Parameters:
 *   out         - Receives the text.
 *   outLen      - Size of out, in bytes.
 *   callsign    - The login callsign (for example KD0OSS-L).
 *   password    - The EchoLink password.
 *   state       - "ONLINE", "BUSY" or "OFF".
 *   description - Location text shown in the directory.
 *   clock       - Local time as HH:MM.
 *
 * Returns: The length of the text.
 */
int elMakeLogin(char* out, size_t outLen, const char* callsign, const char* password, const char* state,
                const char* description, const char* clock)
{
    if (strcmp(state, "OFF") == 0)
        return snprintf(out, outLen, "l%s\254\254%s\rOFF-V3.40\r%s\r", callsign, password, description);
    if (strcmp(state, "BUSY") == 0)
        return snprintf(out, outLen, "l%s\254\254%s\rBUSY3.40(%s)\r%s\r", callsign, password, clock, description);
    return snprintf(out, outLen, "l%s\254\254%s\rONLINE3.38(%s)\r%s\r", callsign, password, clock, description);
}

/*
 * elFreeList()
 *
 * Releases a station list.
 *
 * Parameters:
 *   dir - The list.
 *
 * Returns: nothing.
 */
void elFreeList(ElDirList* dir)
{
    free(dir->list);
    dir->list  = NULL;
    dir->count = 0;
}

/*
 * nextLine()
 *
 * Cuts the next line off a text.
 *
 * Parameters:
 *   pos - The read position, moved past the line.
 *   end - End of the text.
 *   out - Receives the line, zero terminated.
 *   len - Size of out, in bytes.
 *
 * Returns: true if a complete line was read.
 */
static bool nextLine(const char** pos, const char* end, char* out, size_t len)
{
    const char* nl = memchr(*pos, '\n', (size_t)(end - *pos));
    size_t      n;

    if (nl == NULL)
        return false;
    n = (size_t)(nl - *pos);
    if (n >= len)
        n = len - 1;
    memcpy(out, *pos, n);
    out[n] = '\0';
    *pos = nl + 1;
    return true;
}

/*
 * elParseList()
 *
 * Parses the directory server's station list: "@@@", the count, then for each
 * station four lines (callsign, description and status, node number, address),
 * and "+++". A station with the callsign "." ends the list early and one with
 * a blank callsign is a server message.
 *
 * Parameters:
 *   text - The complete reply.
 *   len  - Length of the reply.
 *   dir  - Receives the stations (the old list is freed first).
 *
 * Returns: true if the list was well formed.
 */
bool elParseList(const char* text, size_t len, ElDirList* dir)
{
    const char* pos = text;
    const char* end = text + len;
    char        line[512];
    int         count;
    int         cap;

    elFreeList(dir);
    dir->message[0] = '\0';

    if (len < 4 || memcmp(text, "@@@\n", 4) != 0)
        return false;
    pos += 4;

    if (!nextLine(&pos, end, line, sizeof(line)))
        return false;
    count = atoi(line);
    if (count < 0 || count > 200000)
        return false;
    cap       = count > 0 ? count : 1;
    dir->list = calloc((size_t)cap, sizeof(ElStation));
    if (dir->list == NULL)
        return false;

    for (int i = 0; i < count; i++)
    {
        ElStation st;
        char      callLine[512];
        char      dataLine[512];
        char      idLine[512];
        char      ipLine[512];

        if (!nextLine(&pos, end, callLine, sizeof(callLine)) || !nextLine(&pos, end, dataLine, sizeof(dataLine)) ||
            !nextLine(&pos, end, idLine, sizeof(idLine)) || !nextLine(&pos, end, ipLine, sizeof(ipLine)))
        {
            elFreeList(dir);
            return false;
        }

        if (strcmp(callLine, ".") == 0)
            break;
        if (strcmp(callLine, " ") == 0)
        {
            size_t used = strlen(dir->message);

            snprintf(dir->message + used, sizeof(dir->message) - used, "%s\n", dataLine);
            continue;
        }

        memset(&st, 0, sizeof(st));
        snprintf(st.call, sizeof(st.call), "%s", callLine);
        elSanitizeCallsign(st.call);
        for (char* c = st.call; *c != '\0'; c++)
            *c = (char)toupper((unsigned char)*c);
        st.id   = (uint32_t)strtoul(idLine, NULL, 10);
        snprintf(st.ip, sizeof(st.ip), "%s", ipLine);
        st.busy = strstr(dataLine, "BUSY") != NULL;
        if (strlen(st.call) == 0)
            continue;
        dir->list[dir->count++] = st;
    }

    if (end - pos < 3 || memcmp(pos, "+++", 3) != 0)
    {
        elFreeList(dir);
        return false;
    }
    dir->fetched = time(NULL);
    return true;
}

/*
 * elFindStation()
 *
 * Looks a station up by node number or by callsign. A callsign without a
 * -L or -R suffix also matches the link or repeater with that callsign.
 *
 * Parameters:
 *   dir  - The station list.
 *   name - A node number (digits only) or a callsign.
 *
 * Returns: The station, or NULL if it is not in the list.
 */
const ElStation* elFindStation(const ElDirList* dir, const char* name)
{
    char     up[EL_CALL_LEN];
    bool     numeric = strlen(name) > 0;
    uint32_t id      = 0;

    snprintf(up, sizeof(up), "%s", name);
    for (char* c = up; *c != '\0'; c++)
    {
        *c = (char)toupper((unsigned char)*c);
        if (!isdigit((unsigned char)*c))
            numeric = false;
    }

    if (numeric)
    {
        id = (uint32_t)strtoul(up, NULL, 10);
        for (int i = 0; i < dir->count; i++)
        {
            if (dir->list[i].id == id)
                return &dir->list[i];
        }
        return NULL;
    }

    for (int i = 0; i < dir->count; i++)
    {
        if (strcmp(dir->list[i].call, up) == 0)
            return &dir->list[i];
    }
    for (int i = 0; i < dir->count; i++)
    {
        size_t n = strlen(up);

        if (strncmp(dir->list[i].call, up, n) == 0 && dir->list[i].call[n] == '-')
            return &dir->list[i];
    }
    return NULL;
}

/*
 * dirConnect()
 *
 * Opens a TCP connection to a directory server given as "host" or
 * "host:port", trying every address the name has.
 *
 * Parameters:
 *   server - The server.
 *
 * Returns: The connected socket, or -1.
 */
static int dirConnect(const char* server)
{
    char             host[128];
    char             port[8];
    struct addrinfo  hints;
    struct addrinfo* res = NULL;
    struct addrinfo* ai;
    const char*      colon = strrchr(server, ':');
    int              fd    = -1;

    if (colon != NULL && colon != server)
    {
        size_t n = (size_t)(colon - server);

        if (n >= sizeof(host))
            n = sizeof(host) - 1;
        memcpy(host, server, n);
        host[n] = '\0';
        snprintf(port, sizeof(port), "%s", colon + 1);
    }
    else
    {
        snprintf(host, sizeof(host), "%s", server);
        snprintf(port, sizeof(port), "%u", EL_DIR_PORT);
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0)
        return -1;

    for (ai = res; ai != NULL && fd < 0; ai = ai->ai_next)
    {
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

        if (s < 0)
            continue;

        int flags = fcntl(s, F_GETFL, 0);

        fcntl(s, F_SETFL, flags | O_NONBLOCK);

        int rc = connect(s, ai->ai_addr, ai->ai_addrlen);

        if (rc == 0)
        {
            fcntl(s, F_SETFL, flags);
            fd = s;
            continue;
        }
        if (errno == EINPROGRESS)
        {
            fd_set         wfds;
            struct timeval tv = {5, 0};
            int            err = 0;
            socklen_t      el  = sizeof(err);

            FD_ZERO(&wfds);
            FD_SET(s, &wfds);
            if (select(s + 1, NULL, &wfds, NULL, &tv) > 0 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0)
            {
                fcntl(s, F_SETFL, flags);
                fd = s;
                continue;
            }
        }
        close(s);
    }
    freeaddrinfo(res);
    return fd;
}

/*
 * dirRead()
 *
 * Reads from a directory connection until the end marker, the end of the
 * connection or the time limit. The text is grown as needed up to a limit.
 *
 * Parameters:
 *   fd     - The socket.
 *   marker - Stop when the text ends with this (NULL to read to the end).
 *   len    - Receives the number of bytes read.
 *
 * Returns: The text (zero terminated, to be freed by the caller), or NULL.
 */
static char* dirRead(int fd, const char* marker, size_t* len)
{
    size_t cap = 4096;
    size_t n   = 0;
    char*  buf = malloc(cap);

    if (buf == NULL)
        return NULL;

    for (;;)
    {
        fd_set         rfds;
        struct timeval tv = {DIR_IO_TIMEOUT_MS / 1000, 0};

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
            break;
        if (n + 2048 > cap)
        {
            if (cap >= DIR_LIST_MAX)
                break;
            cap *= 2;
            char* nb = realloc(buf, cap);

            if (nb == NULL)
            {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        ssize_t r = read(fd, buf + n, 2048);

        if (r <= 0)
            break;
        n += (size_t)r;
        buf[n] = '\0';
        if (marker != NULL && n >= strlen(marker) && memcmp(buf + n - strlen(marker), marker, strlen(marker)) == 0)
            break;
    }
    buf[n] = '\0';
    *len   = n;
    return buf;
}

/*
 * elDirRegister()
 *
 * Registers the station with one directory server with the given state.
 *
 * Parameters:
 *   server      - The server, "host" or "host:port".
 *   callsign    - The login callsign.
 *   password    - The EchoLink password.
 *   state       - "ONLINE", "BUSY" or "OFF".
 *   description - Location text.
 *   reply       - Receives the server's reply text.
 *   replyLen    - Size of reply, in bytes.
 *
 * Returns: true if the server answered OK.
 */
bool elDirRegister(const char* server, const char* callsign, const char* password, const char* state,
                   const char* description, char* reply, size_t replyLen)
{
    char      cmd[512];
    char      clock[8];
    time_t    now = time(NULL);
    struct tm tm;
    int       fd = dirConnect(server);
    size_t    n  = 0;
    char*     text;
    bool      ok;

    reply[0] = '\0';
    if (fd < 0)
    {
        snprintf(reply, replyLen, "cannot connect to %s", server);
        return false;
    }

    localtime_r(&now, &tm);
    strftime(clock, sizeof(clock), "%H:%M", &tm);
    int len = elMakeLogin(cmd, sizeof(cmd), callsign, password, state, description, clock);

    if (len <= 0 || (size_t)len >= sizeof(cmd) || write(fd, cmd, (size_t)len) != len)
    {
        close(fd);
        snprintf(reply, replyLen, "write to %s failed", server);
        return false;
    }

    text = dirRead(fd, NULL, &n);
    close(fd);
    if (text == NULL)
        return false;
    snprintf(reply, replyLen, "%s", text);
    ok = n >= 2 && memcmp(text, "OK", 2) == 0;
    free(text);
    return ok;
}

/*
 * elDirFetch()
 *
 * Gets the station list from one directory server and, when a cache path is
 * given, keeps a copy of the reply there so a restart does not need to ask
 * the server again (the servers limit how often the list may be fetched).
 *
 * Parameters:
 *   server    - The server, "host" or "host:port".
 *   dir       - Receives the list.
 *   cachePath - File to save the reply in, or NULL for none.
 *
 * Returns: true if the list was received and well formed.
 */
bool elDirFetch(const char* server, ElDirList* dir, const char* cachePath)
{
    int    fd = dirConnect(server);
    size_t n  = 0;
    char*  text;
    bool   ok;

    if (fd < 0)
        return false;
    if (write(fd, "s", 1) != 1)
    {
        close(fd);
        return false;
    }
    text = dirRead(fd, "+++", &n);
    close(fd);
    if (text == NULL)
        return false;
    ok = elParseList(text, n, dir);
    if (ok && cachePath != NULL && dir->count > 0)
    {
        FILE* f = fopen(cachePath, "w");

        if (f != NULL)
        {
            fwrite(text, 1, n, f);
            fclose(f);
        }
    }
    free(text);
    return ok;
}

/*
 * elLoadCache()
 *
 * Loads the station list saved by elDirFetch(). The age of the list is the
 * age of the file.
 *
 * Parameters:
 *   cachePath - The cache file.
 *   dir       - Receives the list.
 *
 * Returns: true if a well formed list was loaded.
 */
bool elLoadCache(const char* cachePath, ElDirList* dir)
{
    struct stat st;
    FILE*       f = fopen(cachePath, "r");
    char*       text;
    size_t      n;
    bool        ok;

    if (f == NULL)
        return false;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)DIR_LIST_MAX)
    {
        fclose(f);
        return false;
    }
    text = malloc((size_t)st.st_size + 1);
    if (text == NULL)
    {
        fclose(f);
        return false;
    }
    n = fread(text, 1, (size_t)st.st_size, f);
    fclose(f);
    text[n] = '\0';
    ok = elParseList(text, n, dir) && dir->count > 0;
    if (ok)
        dir->fetched = st.st_mtime;
    free(text);
    return ok;
}
