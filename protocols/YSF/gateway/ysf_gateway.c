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
 * YSF gateway.
 *
 * Connects to ysf_host over TCP (port 18500 + modem - 1) and links it to a YSF
 * reflector over UDP when the dashboard asks for it.
 *
 *   host -> gateway   CONN  callsign(10) name(16)  link to reflector 'name' (a 5 digit id or a name)
 *                     DISC                         unlink
 *                     COMM  04                     re-read the configuration
 *                     YSFG  end(1) body(151)       RF voice or data to the reflector
 *   gateway -> host   CONN / NACK / DISC name(16)  link status
 *                     COMM                         configuration acknowledged
 *                     YSFG  end(1) body(151)       reflector voice or data for RF
 *
 * The body is the reflector packet without its "YSFD" tag: gateway callsign(10),
 * source(10), destination(10), frame counter and end flag (1) and the 120 byte
 * frame.  The reflector packets are those of the YSFReflector:
 *
 *   YSFP callsign(10)               poll, 14 bytes; the reflector answers with its own YSFP
 *   YSFU callsign(10)               unlink, 14 bytes
 *   YSFD body(151)                  data, 155 bytes
 *
 * Wires-X (YSF/wiresX, default on): the radio's DX, ALL, SEARCH, CONNECT,
 * DISCONNECT and CATEGORY requests are answered from the reflector list, as
 * G4KLX's WiresX.cpp does.  FCS (YSF/enableFCS): a link name that starts with
 * FCS (FCS00101 or FCS001-01) links to the FCS room over UDP port 62500:
 *
 *   PING (25)  every 800 ms, INFO (100) and OPTIONS (50) after every answer,
 *   CLOSE (11) to leave; data is the 120 byte frame, the counter and the room
 *   in 130 bytes.
 *
 * Not included (they are in G4KLX's YSFGateway): DTMF reflector selection,
 * GPS/APRS, the Wires-X command passthrough, the parrot and the
 * YSF2DMR/NXDN/P25 cross links.
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
#include <math.h>
#include <tools.h>
#include <unistd.h>

#include "../ysf_func.h"
#include "ysf_reflist.h"
#include "ysf_wiresx.h"

#define VERSION "2026-10-14"
#define BUFFER_SIZE 1024

#define YSF_BODY_LENGTH         151U    /* host <-> gateway, and the reflector packet after its tag */
#define YSF_REFL_PACKET_LENGTH  155U    /* YSFD packet                                              */
#define YSF_REFL_POLL_LENGTH    14U     /* YSFP and YSFU packets                                    */
#define YSF_HOST_PACKET_LENGTH  (8U + 1U + YSF_BODY_LENGTH)
#define YSF_NAME_LENGTH         16U     /* reflector name in the link status packets                */

#define POLL_INTERVAL_MS        5000U
#define LINK_TIMEOUT_MS         3000U
#define LOST_TIMEOUT_MS         120000U /* as YSFGateway                                            */

#define FCS_PORT                62500
#define FCS_PING_LENGTH         25U
#define FCS_INFO_LENGTH         100U
#define FCS_OPTIONS_LENGTH      50U
#define FCS_DATA_LENGTH         130U
#define FCS_PING_INTERVAL_MS    800U
#define FCS_RESET_MS            1000U   /* the frame counter starts again after this much silence   */
#define FCS_LINK_TIMEOUT_MS     5000U

typedef enum
{
    LINK_IDLE,
    LINK_CONNECTING,
    LINK_LINKED
} LinkState;

typedef enum
{
    KIND_YSF,
    KIND_FCS
} LinkKind;

pthread_t clientid;
pthread_t reflid;

static int clientPort       = 18600;
static uint8_t modemId      = 1;
static char modemName[10]   = "modem1";
static char ysfHost[80]     = "127.0.0.1";
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
static char reflName[YSF_NAME_LENGTH + 1] = "";
static char callsign[11]    = "N0CALL    ";       /* the node name in polls, 10 characters */
static char baseCall[11]    = "N0CALL";           /* the station callsign                  */
static char nodeSuffix[11]  = "";                 /* YSF/suffix                            */

/* What is linked: a YSF reflector or an FCS room */
static LinkKind reflKind    = KIND_YSF;
static char fcsRoom[9]      = "";                 /* FCS00101                              */
static char fcsPrint[10]    = "";                 /* FCS001-01                             */
static bool linkedWiresX    = false;              /* XLX: Wires-X goes to the network      */
static char lastLink[17]    = "";                 /* for the Wires-X reconnect             */

/* FCS node information and options */
static bool fcsEnabled      = false;
static char fcsOptions[40]  = "";
static uint16_t fcsLocalPort = 0;                 /* 0 = any port                          */
static char fcsServer[40]   = "";                 /* YSF/fcsServer: one server for all rooms */
static uint32_t rxFrequency = 0;
static uint32_t txFrequency = 0;
static uint32_t nodeId      = 0;
static char locator[8]      = "AA00AA";

/* The reflector list and Wires-X, used by both threads with wxMutex */
static pthread_mutex_t wxMutex = PTHREAD_MUTEX_INITIALIZER;
static RefList refList;
static WiresX wx;
static bool wxEnabled       = true;

/* Linking rules */
static volatile uint64_t lastActivity = 0;  /* last call in either direction    */
static unsigned int inactivityMinutes = 0U; /* 0 = never                        */
static char staticReflector[YSF_NAME_LENGTH + 1] = "";   /* linked at start     */
static pthread_mutex_t linkOpMutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * error()
 *
 * Wrapper for perror that ends the program.
 *
 * Parameters:
 *   msg - Text printed before the system error message.
 *
 * Returns: nothing.
 */
void error(char* msg)
{
    reportError(modemName, "YSF gateway", "%s: %s", msg, strerror(errno));
    exit(1);
}

/*
 * hostSend()
 *
 * Writes one packet to ysf_host. Safe to call from both threads.
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
            hexDump("To YSF host", buf, len);

        if (write(hostFd, buf, len) < 0)
            reportError(modemName, "YSF gateway", "YSF_Gateway: write to YSF host failed: %s", strerror(errno));
    }

    pthread_mutex_unlock(&hostMutex);
}

/*
 * hostReply()
 *
 * Sends a link status packet (CONN, NACK or DISC) with the reflector name to
 * ysf_host.
 *
 * Parameters:
 *   type - The four character status.
 *   name - The name of the reflector.
 *
 * Returns: nothing.
 */
static void hostReply(const char* type, const char* name)
{
    uint8_t buf[8 + YSF_NAME_LENGTH];

    memset(buf, 0x00, sizeof(buf));
    buf[0] = 0x61;
    buf[1] = 0x00;
    buf[2] = sizeof(buf);
    buf[3] = 0x04;
    memcpy(buf + 4, type, 4);
    memcpy(buf + 8, name, strnlen(name, YSF_NAME_LENGTH));

    hostSend(buf, sizeof(buf));
}

/*
 * loadCallsign()
 *
 * Reads the station callsign for the reflector polls from the host
 * configuration and pads it to 10 characters.  YSF/suffix, when set, is
 * added with a dash (W1ABC-ND), which is how a node announces itself to a
 * reflector.
 *
 * Parameters:
 *   fallback - Callsign used when none is configured, or NULL.
 *
 * Returns: nothing.
 */
static void loadCallsign(const char* fallback)
{
    char tmp[31];
    char suffix[11];
    char node[32];

    memset(tmp, 0x00, sizeof(tmp));
    memset(suffix, 0x00, sizeof(suffix));
    readHostConfig(modemName, "main", "callsign", tmp);
    readHostConfig(modemName, "YSF", "suffix", suffix);

    if (strlen(tmp) == 0 && fallback != NULL)
        strncpy(tmp, fallback, sizeof(tmp) - 1);

    if (strlen(tmp) == 0)
        strcpy(tmp, "N0CALL");

    if (strlen(suffix) > 0)
        snprintf(node, sizeof(node), "%s-%s", tmp, suffix);
    else
        snprintf(node, sizeof(node), "%s", tmp);

    memset(callsign, ' ', 10);
    callsign[10] = '\0';
    memcpy(callsign, node, strnlen(node, 10));

    memset(baseCall, 0x00, sizeof(baseCall));
    memcpy(baseCall, tmp, strnlen(tmp, 10));
    memset(nodeSuffix, 0x00, sizeof(nodeSuffix));
    memcpy(nodeSuffix, suffix, strnlen(suffix, 10));
}

/* ------------------------------------------------------------------------ */
/* Reflector kinds */
/* ------------------------------------------------------------------------ */

/*
 * isFcsName()
 *
 * Tells if a name is an FCS room name: 6 to 9 characters, starting with FCS,
 * letters and digits only (a dash is allowed after the sixth).
 *
 * Parameters:
 *   s - The name.
 *
 * Returns: true if s looks like an FCS room name.
 */
static bool isFcsName(const char* s)
{
    size_t n = strlen(s);

    if (n < 6U || n > 9U || strncasecmp(s, "FCS", 3) != 0)
        return false;

    for (size_t i = 3U; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && !(i == 6U && s[i] == '-'))
            return false;

    return true;
}

/*
 * fcsNormalise()
 *
 * "FCS001", "FCS00101" or "FCS001-01" become the 8 character room name
 * (FCS00101) and the printed form (FCS001-01); a short name is padded with
 * zeroes, as YSFGateway does.
 *
 * Parameters:
 *   in    - The name as given.
 *   name  - Receives the 8 character room name, as a string.
 *   print - Receives the printed form, as a string of 9 characters.
 *
 * Returns: nothing.
 */
static void fcsNormalise(const char* in, char* name, char* print)
{
    char tmp[12];
    size_t n = 0U;

    for (const char* c = in; *c != '\0' && n < 8U; c++)
    {
        if (*c == '-')
            continue;

        tmp[n++] = (char)toupper((unsigned char)*c);
    }

    while (n < 8U)
        tmp[n++] = '0';

    tmp[8] = '\0';

    memcpy(name, tmp, 9U);
    snprintf(print, 10U, "%.6s-%.2s", tmp, tmp + 6);
}

/* ------------------------------------------------------------------------ */
/* Wires-X and the reflector list                                            */
/* ------------------------------------------------------------------------ */

/*
 * bodyToHost()
 *
 * Sends a frame of a call to ysf_host as a YSFG packet, to be transmitted.
 *
 * Parameters:
 *   body - The body of the frame, YSF_BODY_LENGTH bytes.
 *   end  - true for the last frame of the call.
 *
 * Returns: nothing.
 */
static void bodyToHost(const uint8_t* body, bool end)
{
    uint8_t out[YSF_HOST_PACKET_LENGTH];

    memset(out, 0x00, sizeof(out));
    out[0] = 0x61;
    out[1] = 0x00;
    out[2] = YSF_HOST_PACKET_LENGTH;
    out[3] = 0x04;
    memcpy(out + 4, "YSFG", 4);
    out[8] = end ? 1U : 0U;
    memcpy(out + 9, body, YSF_BODY_LENGTH);

    hostSend(out, sizeof(out));
}

/*
 * wxSendCb()
 *
 * Called by Wires-X for every frame of a reply; the frames go to the host to
 * be sent over the air.
 *
 * Parameters:
 *   ctx  - Not used.
 *   body - The body of the frame.
 *   end  - true for the last frame of the reply.
 *
 * Returns: nothing.
 */
static void wxSendCb(void* ctx, const uint8_t* body, bool end)
{
    (void)ctx;
    bodyToHost(body, end);
}

/*
 * calculateLocator()
 *
 * Works out the six character Maidenhead locator of the station from the
 * configured latitude and longitude, as YSFGateway does.  AA00AA is used
 * when the values are out of range.
 *
 * Parameters:
 *   out - Receives the locator as a string; must hold 7 bytes.
 *
 * Returns: nothing.
 */
static void calculateLocator(char* out)
{
    char tmp[31];
    float latitude;
    float longitude;
    float lon;
    float lat;

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "main", "latitude", tmp);
    latitude = (float)atof(tmp);

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "main", "longitude", tmp);
    longitude = (float)atof(tmp);

    if (latitude < -90.0F || latitude > 90.0F || longitude < -360.0F || longitude > 360.0F)
    {
        strcpy(out, "AA00AA");
        return;
    }

    latitude += 90.0F;

    if (longitude > 180.0F)
        longitude -= 360.0F;

    if (longitude < -180.0F)
        longitude += 360.0F;

    longitude += 180.0F;

    lon = floorf(longitude / 20.0F);
    lat = floorf(latitude / 10.0F);
    out[0] = (char)('A' + (unsigned int)lon);
    out[1] = (char)('A' + (unsigned int)lat);

    longitude -= lon * 20.0F;
    latitude  -= lat * 10.0F;

    lon = floorf(longitude / 2.0F);
    lat = floorf(latitude / 1.0F);
    out[2] = (char)('0' + (unsigned int)lon);
    out[3] = (char)('0' + (unsigned int)lat);

    longitude -= lon * 2.0F;
    latitude  -= lat * 1.0F;

    lon = floorf(longitude / (2.0F / 24.0F));
    lat = floorf(latitude / (1.0F / 24.0F));
    out[4] = (char)('A' + (unsigned int)lon);
    out[5] = (char)('A' + (unsigned int)lat);
    out[6] = '\0';
}

/*
 * loadFcsConfig()
 *
 * YSF/enableFCS, YSF/options, YSF/fcsRoomsFile, YSF/fcsPort and the node
 * information the FCS reflectors are given (frequencies, locator, id).
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void loadFcsConfig(void)
{
    char tmp[64];

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "YSF", "enableFCS", tmp);
    fcsEnabled = strlen(tmp) > 0 && atoi(tmp) != 0;

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "YSF", "options", tmp);
    memset(fcsOptions, 0x00, sizeof(fcsOptions));
    memcpy(fcsOptions, tmp, strnlen(tmp, sizeof(fcsOptions) - 1));

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "YSF", "fcsPort", tmp);
    fcsLocalPort = (uint16_t)atoi(tmp);

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "YSF", "fcsServer", tmp);
    memset(fcsServer, 0x00, sizeof(fcsServer));
    memcpy(fcsServer, tmp, strnlen(tmp, sizeof(fcsServer) - 1));

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "config", "rxFrequency", tmp);
    rxFrequency = (uint32_t)strtoul(tmp, NULL, 10);

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "config", "txFrequency", tmp);
    txFrequency = (uint32_t)strtoul(tmp, NULL, 10);

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "YSF", "id", tmp);

    if (strlen(tmp) == 0)
        readHostConfig(modemName, "DMR", "ID", tmp);

    nodeId = (uint32_t)strtoul(tmp, NULL, 10);

    calculateLocator(locator);
}

/*
 * loadReflectorList()
 *
 * Reads the reflector list for Wires-X and for linking by id: YSF/hostsFile,
 * then YSFHosts.json and YSFHosts.txt in the working directory and the
 * OpenMT install directory.  FCS rooms are added when FCS is enabled
 * (YSF/fcsRoomsFile, ./FCSRooms.txt, the OpenMT install directory).
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void loadReflectorList(void)
{
    static const char* const files[] =
    {
        "YSFHosts.json", "YSFHosts.txt",
        "/home/pi/OpenMT/protocols/YSF/YSFHosts.json", "/home/pi/OpenMT/protocols/YSF/YSFHosts.txt"
    };
    static const char* const rooms[] =
    {
        "FCSRooms.txt", "/home/pi/OpenMT/protocols/YSF/FCSRooms.txt"
    };
    char path[200];
    bool loaded = false;

    pthread_mutex_lock(&wxMutex);

    reflistFree(&refList);
    reflistInit(&refList);

    if (fcsEnabled)
    {
        unsigned int n = 0U;

        memset(path, 0x00, sizeof(path));
        readHostConfig(modemName, "YSF", "fcsRoomsFile", path);

        if (strlen(path) > 0)
            n = reflistReadFcsRooms(&refList, path);

        for (unsigned int i = 0U; n == 0U && i < sizeof(rooms) / sizeof(rooms[0]); i++)
            n = reflistReadFcsRooms(&refList, rooms[i]);

        logMsg("Loaded %u FCS room descriptions", n);
    }

    memset(path, 0x00, sizeof(path));
    readHostConfig(modemName, "YSF", "hostsFile", path);

    if (strlen(path) > 0)
        loaded = reflistLoad(&refList, path);

    for (unsigned int i = 0U; !loaded && i < sizeof(files) / sizeof(files[0]); i++)
        loaded = reflistLoad(&refList, files[i]);

    if (loaded)
        logMsg("Loaded %u reflectors from %s", refList.count, refList.path);
    else
        logMsg("No reflector list found; links by name use the dashboard database only.");

    pthread_mutex_unlock(&wxMutex);
}

/*
 * wxReflectorSet()
 *
 * Tells Wires-X which reflector is linked, found by its id in the reflector
 * list.
 *
 * Parameters:
 *   id - The id of the reflector, or NULL or an empty string when there is
 *        none.
 *
 * Returns: nothing.
 */
static void wxReflectorSet(const char* id)
{
    pthread_mutex_lock(&wxMutex);
    wxSetReflector(&wx, id != NULL && id[0] != '\0' ? reflistFindById(&refList, id) : NULL);
    pthread_mutex_unlock(&wxMutex);
}

/*
 * wxNotifyDisconnect()
 *
 * Tells Wires-X that the link was lost or dropped.  Does nothing when
 * Wires-X is off.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void wxNotifyDisconnect(void)
{
    if (!wxEnabled)
        return;

    pthread_mutex_lock(&wxMutex);
    wxProcessDisconnect(&wx);
    pthread_mutex_unlock(&wxMutex);
}

/*
 * wxBusyNow()
 *
 * Tells if Wires-X is busy sending a reply, in which case the reflector's
 * frames are held back.
 *
 * Parameters: none.
 *
 * Returns: true if a Wires-X answer is being sent.
 */
static bool wxBusyNow(void)
{
    bool busy;

    if (!wxEnabled)
        return false;

    pthread_mutex_lock(&wxMutex);
    busy = wxIsBusy(&wx);
    pthread_mutex_unlock(&wxMutex);

    return busy;
}

/*
 * wxListRefresh()
 *
 * Re-reads the list when the file changed, between Wires-X requests.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void wxListRefresh(void)
{
    char id[6] = "";

    pthread_mutex_lock(&wxMutex);

    if (refList.path[0] != '\0' && !wxIsBusy(&wx) && wx.status == WXSI_NONE && wx.qHead == wx.qTail)
    {
        const Reflector* current = wxGetReflector(&wx);

        if (current != NULL)
            memcpy(id, current->id, sizeof(id));

        if (reflistRefresh(&refList))
        {
            logMsg("Reflector list reloaded, %u entries", refList.count);
            wxSetReflector(&wx, id[0] != '\0' ? reflistFindById(&refList, id) : NULL);
        }
    }

    pthread_mutex_unlock(&wxMutex);
}

/* ------------------------------------------------------------------------ */
/* Finding the reflector                                                     */
/* ------------------------------------------------------------------------ */

typedef struct
{
    LinkKind kind;
    struct sockaddr_in addr;
    char fcsName[9];            /* FCS00101   */
    char fcsPrint[10];          /* FCS001-01  */
    bool wiresX;                /* XLX: Wires-X goes to the network */
    char listId[6];             /* the id in the reflector list, if there */
    char display[YSF_NAME_LENGTH + 1];
} Target;

/*
 * findInOtherFiles()
 *
 * A reflector that is not in the list loaded at start may be in one of the
 * other hosts files (a text list next to a JSON one, say).  Each is read
 * into a list of its own and searched.
 *
 * Parameters:
 *   key - The reflector id or name.
 *   out - Receives the reflector.
 *
 * Returns: true if it was found in one of the other files.
 */
static bool findInOtherFiles(const char* key, Reflector* out)
{
    static const char* const files[] =
    {
        "YSFHosts.json", "YSFHosts.txt",
        "/home/pi/OpenMT/protocols/YSF/YSFHosts.json", "/home/pi/OpenMT/protocols/YSF/YSFHosts.txt"
    };
    char current[256];
    char path[200];
    RefList other;
    bool found = false;

    pthread_mutex_lock(&wxMutex);
    snprintf(current, sizeof(current), "%s", refList.path);
    pthread_mutex_unlock(&wxMutex);

    memset(path, 0x00, sizeof(path));
    readHostConfig(modemName, "YSF", "hostsFile", path);

    for (unsigned int i = 0U; !found && i < 1U + sizeof(files) / sizeof(files[0]); i++)
    {
        const char* file = (i == 0U) ? path : files[i - 1U];
        const Reflector* r;

        if (file[0] == '\0' || strcmp(file, current) == 0)
            continue;

        reflistInit(&other);

        if (reflistLoad(&other, file) && (r = reflistFind(&other, key)) != NULL)
        {
            *out  = *r;
            found = true;
        }

        reflistFree(&other);
    }

    return found;
}

/*
 * resolveTarget()
 *
 * Finds a reflector by id or name.  A name that starts with FCS is an FCS
 * room.  Otherwise the dashboard database is tried first (the id is its
 * Nick), then the reflector list.  'entry' is a reflector chosen from the
 * list by Wires-X, which is used as it is.
 *
 * Parameters:
 *   key   - The reflector id or name.
 *   entry - The reflector picked on the radio by Wires-X, or NULL.
 *   t     - Receives the kind, address and names of the target.
 *
 * Returns: true if the target was found (and for a YSF reflector its address
 *   resolved), false otherwise.  The address of an FCS room is worked out by
 *   the caller.
 */
static bool resolveTarget(const char* key, const Reflector* entry, Target* t)
{
    char address[55];
    uint16_t port = 0;
    Reflector copy;
    bool inList = false;
    bool safe = true;

    memset(t, 0x00, sizeof(*t));
    snprintf(t->display, sizeof(t->display), "%s", key);

    if (entry != NULL)
    {
        copy   = *entry;
        inList = true;
    }
    else if (isFcsName(key))
    {
        if (!fcsEnabled)
        {
            logMsg("FCS is not enabled (YSF/enableFCS), cannot link to %s", key);
            return false;
        }

        t->kind = KIND_FCS;
        fcsNormalise(key, t->fcsName, t->fcsPrint);
        return true;
    }
    else
    {
        pthread_mutex_lock(&wxMutex);
        {
            const Reflector* r = reflistFind(&refList, key);

            if (r != NULL)
            {
                copy   = *r;
                inList = true;
            }
        }
        pthread_mutex_unlock(&wxMutex);

        if (!inList)
            inList = findInOtherFiles(key, &copy);
    }

    if (inList)
    {
        memcpy(t->listId, copy.id, sizeof(t->listId));
        t->wiresX = copy.wiresX;

        if (copy.type == REFL_FCS)
        {
            if (!fcsEnabled)
            {
                logMsg("FCS is not enabled (YSF/enableFCS), cannot link to %s", key);
                return false;
            }

            char room[17];

            memcpy(room, copy.name, 16U);
            room[16] = '\0';

            for (size_t i = strlen(room); i > 0U && room[i - 1U] == ' '; i--)
                room[i - 1U] = '\0';

            t->kind = KIND_FCS;
            fcsNormalise(room, t->fcsName, t->fcsPrint);
            return true;
        }
    }

    t->kind = KIND_YSF;

    /* the name goes into an SQL statement of the tools library */
    for (const char* c = key; *c != '\0'; c++)
        if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_' && *c != '.')
            safe = false;

    memset(address, 0x00, sizeof(address));

    if (entry == NULL && safe && findReflector("YSF", key, address, &port) && resolveAddress(address, port, &t->addr))
        return true;

    if (inList && copy.address[0] != '\0')
        return resolveAddress(copy.address, copy.port, &t->addr);

    return false;
}

/* ------------------------------------------------------------------------ */
/* Reflector link                                                            */
/* ------------------------------------------------------------------------ */

/*
 * reflWritePoll()
 *
 * Sends a poll or an unlink to a YSF reflector: "YSF", the kind and the node
 * name.
 *
 * Parameters:
 *   fd   - UDP socket to the reflector.
 *   addr - Address of the reflector.
 *   kind - 'P' for a poll or 'U' for an unlink.
 *
 * Returns: nothing.
 */
static void reflWritePoll(int fd, const struct sockaddr_in* addr, char kind)
{
    uint8_t data[YSF_REFL_POLL_LENGTH];

    memcpy(data, "YSF", 3);
    data[3] = (uint8_t)kind;
    memcpy(data + 4, callsign, 10);

    if (debugM)
        hexDump(kind == 'P' ? "YSF poll sent" : "YSF unlink sent", data, sizeof(data));

    sendto(fd, data, sizeof(data), 0, (const struct sockaddr*)addr, sizeof(*addr));
}

/*
 * fcsWritePing()
 *
 * Sends the FCS PING (25 bytes): the station callsign and the room.
 *
 * Parameters:
 *   fd   - UDP socket to the FCS server.
 *   addr - Address of the server.
 *   room - The 8 character room name.
 *
 * Returns: nothing.
 */
static void fcsWritePing(int fd, const struct sockaddr_in* addr, const char* room)
{
    uint8_t data[FCS_PING_LENGTH];
    size_t n = strnlen(baseCall, 6U);

    memcpy(data, "PING", 4);
    memset(data + 4, ' ', 6);
    memcpy(data + 4, baseCall, n);
    memset(data + 10, 0x00, 15);
    memcpy(data + 10, room, 8);

    if (debugM)
        hexDump("FCS ping sent", data, sizeof(data));

    sendto(fd, data, sizeof(data), 0, (const struct sockaddr*)addr, sizeof(*addr));
}

/*
 * fcsWriteInfo()
 *
 * Sends the node information to an FCS server (100 bytes): receive and
 * transmit frequency, locator, "MMDVM" and the node id.
 *
 * Parameters:
 *   fd   - UDP socket to the FCS server.
 *   addr - Address of the server.
 *
 * Returns: nothing.
 */
static void fcsWriteInfo(int fd, const struct sockaddr_in* addr)
{
    char data[FCS_INFO_LENGTH + 1];

    memset(data, ' ', sizeof(data));
    snprintf(data, sizeof(data), "%9u%9u%-6.6s%-12.12s%7u", rxFrequency, txFrequency, locator, "MMDVM", nodeId);
    memset(data + 43, ' ', 57);

    if (debugM)
        hexDump("FCS info sent", (const uint8_t*)data, FCS_INFO_LENGTH);

    sendto(fd, data, FCS_INFO_LENGTH, 0, (const struct sockaddr*)addr, sizeof(*addr));
}

/*
 * fcsWriteOptions()
 *
 * Sends the options to an FCS server (50 bytes, FCSO), when YSF/options is
 * set.
 *
 * Parameters:
 *   fd    - UDP socket to the FCS server.
 *   addr  - Address of the server.
 *   print - The printed room name (FCS001-01).
 *
 * Returns: nothing.
 */
static void fcsWriteOptions(int fd, const struct sockaddr_in* addr, const char* print)
{
    uint8_t data[FCS_OPTIONS_LENGTH];
    size_t n = strnlen(fcsOptions, 38U);

    if (n == 0U)
        return;

    memcpy(data, "FCSO", 4);
    memset(data + 4, ' ', 46);
    memcpy(data + 4, print, 6);                 /* FCS001 */
    memcpy(data + 10, print + 7, 2);            /* 01     */
    memcpy(data + 12, fcsOptions, n);

    if (debugM)
        hexDump("FCS options sent", data, sizeof(data));

    sendto(fd, data, sizeof(data), 0, (const struct sockaddr*)addr, sizeof(*addr));
}

/*
 * sendToReflector()
 *
 * Sends one frame from the radio to the linked reflector.
 *
 * Parameters:
 *   body - The body of the frame, YSF_BODY_LENGTH bytes.
 *
 * Returns: nothing.
 */
static void sendToReflector(const uint8_t* body)
{
    uint8_t buffer[YSF_REFL_PACKET_LENGTH];
    int fd;
    struct sockaddr_in addr;
    LinkKind kind;
    char room[9];

    pthread_mutex_lock(&reflMutex);

    if (linkState != LINK_LINKED)
    {
        pthread_mutex_unlock(&reflMutex);
        return;
    }

    lastActivity = nowMs();
    fd   = reflFd;
    addr = reflAddr;
    kind = reflKind;
    memcpy(room, fcsRoom, sizeof(room));

    pthread_mutex_unlock(&reflMutex);

    if (kind == KIND_FCS)
    {
        uint8_t data[FCS_DATA_LENGTH];

        memset(data, ' ', sizeof(data));
        memcpy(data, body + 31, 120);
        data[120] = body[30];
        memcpy(data + 121, room, 8);

        if (debugM)
            hexDump("FCS data sent", data, sizeof(data));

        sendto(fd, data, sizeof(data), 0, (struct sockaddr*)&addr, sizeof(addr));
        return;
    }

    memcpy(buffer, "YSFD", 4);
    memcpy(buffer + 4, body, YSF_BODY_LENGTH);

    if (debugM)
        hexDump("YSF data sent", buffer, sizeof(buffer));

    sendto(fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&addr, sizeof(addr));
}

/*
 * sendToHost()
 *
 * Passes one YSFD packet from the reflector on to ysf_host.
 *
 * Parameters:
 *   packet - The YSFD packet received from the reflector.
 *
 * Returns: nothing.
 */
static void sendToHost(const uint8_t* packet)
{
    if (wxBusyNow())
        return;                 /* a Wires-X answer is being sent */

    bodyToHost(packet + 4, (packet[4 + 30] & 0x01U) != 0U);
}

/*
 * reflectorThread()
 *
 * Links to the reflector in reflAddr, polls it (every five seconds for a YSF
 * reflector, every 800 ms for an FCS room) and passes its calls to ysf_host
 * until the link is stopped, lost or the host goes away.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL when the thread ends.
 */
void* reflectorThread(void* arg)
{
    uint8_t buf[200];
    struct sockaddr_in from;
    socklen_t fromLen;
    bool linked = false;
    bool lost = false;
    uint64_t start;
    uint64_t lastPoll;
    uint64_t lastRx;
    uint64_t lastData = 0;
    uint8_t counter = 0;
    char name[YSF_NAME_LENGTH + 1];
    char room[9];
    char print[10];
    LinkKind kind;
    int fd;
    struct sockaddr_in addr;

    (void)arg;

    pthread_mutex_lock(&reflMutex);
    fd = reflFd;
    addr = reflAddr;
    kind = reflKind;
    memcpy(name, reflName, sizeof(name));
    memcpy(room, fcsRoom, sizeof(room));
    memcpy(print, fcsPrint, sizeof(print));
    pthread_mutex_unlock(&reflMutex);

    start = lastPoll = lastRx = nowMs();

    if (kind == KIND_FCS)
    {
        fcsWritePing(fd, &addr, room);
    }
    else
    {
        for (int i = 0; i < 3; i++)
            reflWritePoll(fd, &addr, 'P');
    }

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
            if (kind == KIND_YSF)
            {
                bool isPoll = n >= 4 && memcmp(buf, "YSFP", 4) == 0;
                bool isData = n == (ssize_t)YSF_REFL_PACKET_LENGTH && memcmp(buf, "YSFD", 4) == 0;

                if (isPoll || isData)
                {
                    if (debugM)
                        hexDump(isPoll ? "YSF poll received" : "YSF data received", buf, (unsigned int)n);

                    lastRx = now;

                    if (!linked)
                    {
                        linked = true;

                        pthread_mutex_lock(&reflMutex);
                        linkState = LINK_LINKED;
                        pthread_mutex_unlock(&reflMutex);

                        logMsg("Linked to YSF reflector %s", name);
                        hostReply("CONN", name);
                    }

                    if (isData)
                    {
                        lastActivity = now;
                        sendToHost(buf);
                    }
                }
            }
            else if (n == 7 || n == 10 || n == (ssize_t)FCS_DATA_LENGTH)
            {
                if (debugM)
                    hexDump("FCS received", buf, (unsigned int)n);

                lastRx = now;

                if ((n == 7 || n == 10) && !linked)
                {
                    linked = true;

                    pthread_mutex_lock(&reflMutex);
                    linkState = LINK_LINKED;
                    pthread_mutex_unlock(&reflMutex);

                    logMsg("Linked to FCS room %s", print);
                    hostReply("CONN", name);
                    fcsWriteInfo(fd, &addr);
                    fcsWriteOptions(fd, &addr, print);
                }
                else if (n == 7)
                {
                    /* YSFGateway sends the information again with every answer */
                    fcsWriteInfo(fd, &addr);
                    fcsWriteOptions(fd, &addr, print);
                }
                else if (n == (ssize_t)FCS_DATA_LENGTH)
                {
                    uint8_t packet[YSF_REFL_PACKET_LENGTH];

                    if (now - lastData > FCS_RESET_MS)
                        counter = 0;

                    lastData = now;
                    lastActivity = now;

                    memset(packet, ' ', 35);
                    memcpy(packet, "YSFD", 4);
                    memcpy(packet + 35, buf, 120);
                    memcpy(packet + 4, print, 9);       /* the room as the via callsign */
                    packet[34] = counter;
                    counter = (uint8_t)(counter + 2U);

                    sendToHost(packet);
                }
            }
        }

        if (!linked && now - start > (kind == KIND_FCS ? FCS_LINK_TIMEOUT_MS : LINK_TIMEOUT_MS))
        {
            logMsg("Failed to link to %s %s (no answer)", kind == KIND_FCS ? "FCS room" : "YSF reflector", name);
            hostReply("NACK", name);
            wxReflectorSet(NULL);

            pthread_mutex_lock(&reflMutex);
            linkState = LINK_IDLE;
            reflRunning = false;
            close(reflFd);
            reflFd = -1;
            pthread_mutex_unlock(&reflMutex);
            return NULL;
        }

        if (kind == KIND_FCS && now - lastPoll >= FCS_PING_INTERVAL_MS)
        {
            fcsWritePing(fd, &addr, room);
            lastPoll = now;
        }
        else if (kind == KIND_YSF && linked && now - lastPoll >= POLL_INTERVAL_MS)
        {
            reflWritePoll(fd, &addr, 'P');
            lastPoll = now;
        }

        if (linked && now - lastRx > LOST_TIMEOUT_MS)
        {
            logMsg("%s %s stopped answering.", kind == KIND_FCS ? "FCS room" : "YSF reflector", name);
            lost = true;
            break;
        }
    }

    if (linked)
    {
        if (kind == KIND_FCS)
        {
            for (int i = 0; i < 3; i++)
                sendto(fd, "CLOSE      ", 11, 0, (struct sockaddr*)&addr, sizeof(addr));
        }
        else
        {
            for (int i = 0; i < 3; i++)
                reflWritePoll(fd, &addr, 'U');
        }

        logMsg("Unlinked from %s %s", kind == KIND_FCS ? "FCS room" : "YSF reflector", name);
        hostReply("DISC", name);
    }

    if (lost)
        wxNotifyDisconnect();

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
 * startLink()
 *
 * Finds the reflector (or FCS room), opens the UDP socket and starts
 * reflectorThread().  Replies NACK to the host if it is unknown.  'entry' is
 * the list entry picked on the radio by Wires-X, or NULL.
 *
 * Parameters:
 *   hostCallsign - The callsign sent by the host for the polls, or NULL to
 *                  use the configured one.
 *   name         - The reflector name or id.
 *   entry        - The list entry picked on the radio, or NULL.
 *
 * Returns: nothing.
 */
static void startLink(const char* hostCallsign, const char* name, const Reflector* entry)
{
    Target target;
    struct timeval timeout;
    char cleanName[YSF_NAME_LENGTH + 1];
    int fd;

    pthread_mutex_lock(&linkOpMutex);

    memset(cleanName, 0x00, sizeof(cleanName));
    memcpy(cleanName, name, strnlen(name, YSF_NAME_LENGTH));

    /* a name padded with spaces, as the reflector lists have it */
    for (size_t i = strlen(cleanName); i > 0U && cleanName[i - 1U] == ' '; i--)
        cleanName[i - 1U] = '\0';

    stopLink();

    loadCallsign(hostCallsign);

    if (!resolveTarget(cleanName, entry, &target))
    {
        logMsg("Reflector %s not found in the database or the reflector list", cleanName);
        hostReply("NACK", cleanName);
        wxReflectorSet(NULL);
        pthread_mutex_unlock(&linkOpMutex);
        return;
    }

    if (target.kind == KIND_FCS)
    {
        char host[48];
        uint16_t port = FCS_PORT;

        if (fcsServer[0] != '\0')
        {
            /* YSF/fcsServer: host or host:port for every room */
            char* colon;

            snprintf(host, sizeof(host), "%s", fcsServer);
            colon = strrchr(host, ':');

            if (colon != NULL)
            {
                *colon = '\0';
                port = (uint16_t)atoi(colon + 1);
            }
        }
        else
        {
            snprintf(host, sizeof(host), "%.6s.xreflector.net", target.fcsName);
        }

        if (!resolveAddress(host, port, &target.addr))
        {
            logMsg("Unknown FCS reflector - %.6s", target.fcsName);
            hostReply("NACK", cleanName);
            wxReflectorSet(NULL);
            pthread_mutex_unlock(&linkOpMutex);
            return;
        }
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        reportError(modemName, "YSF gateway", "Cannot open the reflector socket: %s", strerror(errno));
        hostReply("NACK", cleanName);
        wxReflectorSet(NULL);
        pthread_mutex_unlock(&linkOpMutex);
        return;
    }

    {
        char tmp[31];
        int port = fcsLocalPort;

        if (target.kind == KIND_YSF)
        {
            memset(tmp, 0x00, sizeof(tmp));
            readHostConfig(modemName, "YSF", "networkPort", tmp);
            port = atoi(tmp);
        }

        if (port > 0)
        {
            struct sockaddr_in local;
            int on = 1;

            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

            memset(&local, 0x00, sizeof(local));
            local.sin_family      = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port        = htons((uint16_t)port);

            if (bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0)
                reportError(modemName, "YSF gateway", "Cannot bind UDP port %d (%s), using any port.", port, strerror(errno));
        }
    }

    timeout.tv_sec  = 0;
    timeout.tv_usec = 5000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    pthread_mutex_lock(&reflMutex);
    reflFd       = fd;
    reflAddr     = target.addr;
    reflKind     = target.kind;
    reflStop     = false;
    reflRunning  = true;
    linkState    = LINK_CONNECTING;
    linkedWiresX = target.wiresX;
    memcpy(reflName, cleanName, sizeof(reflName));
    memcpy(fcsRoom, target.fcsName, sizeof(fcsRoom));
    memcpy(fcsPrint, target.fcsPrint, sizeof(fcsPrint));
    memcpy(lastLink, cleanName, sizeof(lastLink));
    lastActivity = nowMs();
    pthread_mutex_unlock(&reflMutex);

    wxReflectorSet(target.listId);

    if (target.kind == KIND_FCS)
        logMsg("Linking to FCS room %s (%s:%u)", target.fcsPrint, inet_ntoa(target.addr.sin_addr), ntohs(target.addr.sin_port));
    else
        logMsg("Linking to YSF reflector %s (%s:%u)", cleanName, inet_ntoa(target.addr.sin_addr), ntohs(target.addr.sin_port));

    if (pthread_create(&reflid, NULL, &reflectorThread, NULL) == 0)
    {
        pthread_detach(reflid);
    }
    else
    {
        reportError(modemName, "YSF gateway", "Can't create YSF reflector thread");
        pthread_mutex_lock(&reflMutex);
        reflRunning = false;
        linkState   = LINK_IDLE;
        close(reflFd);
        reflFd = -1;
        pthread_mutex_unlock(&reflMutex);
        hostReply("NACK", cleanName);
        wxReflectorSet(NULL);
    }

    pthread_mutex_unlock(&linkOpMutex);
}

/*
 * dropLink()
 *
 * Unlinks from the current reflector.  'user' is an unlink asked for by the
 * dashboard or the radio: it also forgets the reflector for the Wires-X
 * reconnect.
 *
 * Parameters:
 *   user - true if the unlink was asked for.
 *
 * Returns: nothing.
 */
static void dropLink(bool user)
{
    pthread_mutex_lock(&linkOpMutex);
    stopLink();

    if (user)
    {
        pthread_mutex_lock(&reflMutex);
        lastLink[0] = '\0';
        pthread_mutex_unlock(&reflMutex);
    }

    wxReflectorSet(NULL);
    pthread_mutex_unlock(&linkOpMutex);
}

/*
 * loadLinkConfig()
 *
 * Reads YSF/staticReflector and YSF/inactivityTime (minutes) from the host
 * configuration.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void loadLinkConfig(void)
{
    char tmp[31];

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "YSF", "staticReflector", tmp);
    memset(staticReflector, 0x00, sizeof(staticReflector));
    memcpy(staticReflector, tmp, strnlen(tmp, YSF_NAME_LENGTH));

    memset(tmp, 0x00, sizeof(tmp));
    readHostConfig(modemName, "YSF", "inactivityTime", tmp);
    inactivityMinutes = (strlen(tmp) > 0) ? (unsigned int)strtoul(tmp, NULL, 10) : 0U;
}

/*
 * linkStatic()
 *
 * Links to the static reflector, if one is configured.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void linkStatic(void)
{
    if (strlen(staticReflector) == 0U)
        return;

    logMsg("Static link to %s", staticReflector);
    startLink(NULL, staticReflector, NULL);
}

/*
 * inactivityCheck()
 *
 * Drops a link when nothing has been heard in either direction for the
 * inactivity time, then goes back to the static reflector.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void inactivityCheck(void)
{
    bool expired;
    char name[YSF_NAME_LENGTH + 1];

    if (inactivityMinutes == 0U)
        return;

    pthread_mutex_lock(&reflMutex);
    expired = linkState != LINK_IDLE && (nowMs() - lastActivity) > (uint64_t)inactivityMinutes * 60000U;
    memcpy(name, reflName, sizeof(name));
    pthread_mutex_unlock(&reflMutex);

    if (!expired)
        return;

    logMsg("Unlinking from %s due to inactivity", name);
    dropLink(false);
    wxNotifyDisconnect();

    /* no point in linking again to the one that was just dropped */
    if (strcasecmp(staticReflector, name) != 0)
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
 * Returns: true if len bytes were read, false when the connection closed or
 *   failed.
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
 * handleRadioFrame()
 *
 * One frame from the radio.  Wires-X requests are answered here; the frame
 * goes on to the reflector except for the Wires-X data of a reflector that
 * does Wires-X itself.
 *
 * Parameters:
 *   body - The body of the frame, YSF_BODY_LENGTH bytes.
 *
 * Returns: nothing.
 */
static void handleRadioFrame(const uint8_t* body)
{
    WxStatus status = WX_NONE;
    Reflector chosen;
    bool haveChosen = false;
    bool exclude = false;
    bool wiresXLink;
    YsfFich fich;

    pthread_mutex_lock(&reflMutex);
    wiresXLink = linkedWiresX && linkState != LINK_IDLE && reflKind == KIND_YSF;
    pthread_mutex_unlock(&reflMutex);

    if (wxEnabled)
    {
        pthread_mutex_lock(&wxMutex);
        status = wxProcess(&wx, body + 31, body + 10);

        if (status == WX_CONNECT_YSF || status == WX_CONNECT_FCS)
        {
            const Reflector* r = wxGetReflector(&wx);

            if (r != NULL)
            {
                chosen     = *r;
                haveChosen = true;
            }
        }

        pthread_mutex_unlock(&wxMutex);

        if (wiresXLink && ysfFichDecode(&fich, body + 31) && ysfFichGetDT(&fich) == YSF_DT_DATA_FR_MODE)
            exclude = true;
    }

    switch (status)
    {
        case WX_CONNECT_YSF:
        case WX_CONNECT_FCS:
            if (haveChosen)
            {
                char who[11];
                char room[17];

                memcpy(who, body + 10, 10);
                who[10] = '\0';
                memcpy(room, chosen.name, 16U);
                room[16] = '\0';

                logMsg("Connect to %.5s - \"%s\" has been requested by %s", chosen.id, room, who);

                if (chosen.type == REFL_FCS)
                    startLink(NULL, room, &chosen);
                else
                    startLink(NULL, chosen.id, &chosen);
            }
            break;
        case WX_DISCONNECT:
            logMsg("Disconnect has been requested by %.10s", (const char*)(body + 10));
            dropLink(true);
            break;
        case WX_RECONNECT_CURRENT:
        {
            char last[YSF_NAME_LENGTH + 1];
            bool idle;

            pthread_mutex_lock(&reflMutex);
            memcpy(last, lastLink, sizeof(last));
            idle = linkState == LINK_IDLE;
            pthread_mutex_unlock(&reflMutex);

            if (idle && last[0] != '\0')
                startLink(NULL, last, NULL);
        }
        break;
        default:
            break;
    }

    if (!exclude)
        sendToReflector(body);
}

/*
 * handleHostPacket()
 *
 * One complete packet from ysf_host.
 *
 * Parameters:
 *   buffer  - The packet.
 *   respLen - Length of the packet, in bytes.
 *
 * Returns: nothing.
 */
static void handleHostPacket(const uint8_t* buffer, uint16_t respLen)
{
    uint8_t typeLen = buffer[3];

    if (typeLen != 4 || respLen < 8)
        return;

    if (debugM && memcmp(buffer + 4, "YSFG", 4) != 0)
        hexDump("YSF host data", buffer, respLen);

    if (memcmp(buffer + 4, "COMM", 4) == 0)
    {
        if (respLen >= 9 && buffer[8] == 0x04)
        {
            uint8_t buf[8];

            loadCallsign(NULL);
            loadLinkConfig();
            loadFcsConfig();

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
        if (respLen >= 8 + 10 + 1)
        {
            char source[11];
            char name[YSF_NAME_LENGTH + 1];

            memset(source, 0x00, sizeof(source));
            memset(name, 0x00, sizeof(name));
            memcpy(source, buffer + 8, 10);
            memcpy(name, buffer + 18, respLen - 18U < YSF_NAME_LENGTH ? respLen - 18U : YSF_NAME_LENGTH);

            startLink(source, name, NULL);
        }
    }
    else if (memcmp(buffer + 4, "DISC", 4) == 0)
    {
        dropLink(true);
        wxNotifyDisconnect();
    }
    else if (memcmp(buffer + 4, "YSFG", 4) == 0)
    {
        if (respLen == YSF_HOST_PACKET_LENGTH)
            handleRadioFrame(buffer + 9);
    }
}

/*
 * connectHost()
 *
 * Opens the TCP connection to ysf_host, retrying until it is up.
 *
 * Parameters: none.
 *
 * Returns: The connected socket, or -1 if the gateway is stopping.
 */
static int connectHost(void)
{
    while (connected)
    {
        struct sockaddr_in serv_addr;
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0)
        {
            reportError(modemName, "YSF gateway", "Socket creation error: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }

        memset(&serv_addr, 0x00, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port   = htons(clientPort);

        if (!hostToAddress(ysfHost, &serv_addr.sin_addr))
        {
            reportError(modemName, "YSF gateway", "Invalid address/ Address not supported");
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
 * Thread that connects to ysf_host and handles its packets. When the host
 * goes away the reflector is unlinked and the connection is made again.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL when the thread ends.
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

        printf("Connected to YSF host.\n");
        fflush(stdout);

        loadCallsign(NULL);
        loadLinkConfig();
        linkStatic();

        while (connected)
        {
            if (!readFull(fd, buffer, 1))
                break;

            if (buffer[0] != 0x61)
            {
                fprintf(stderr, "YSF_Gateway: unknown byte from YSF host, 0x%02X\n", buffer[0]);
                continue;
            }

            if (!readFull(fd, buffer + 1, 3))
                break;

            respLen = (uint16_t)((buffer[1] << 8) + buffer[2]);

            if (respLen < 4 || respLen > BUFFER_SIZE)
            {
                fprintf(stderr, "YSF_Gateway: bad packet length %u from YSF host\n", respLen);
                continue;
            }

            if (!readFull(fd, buffer + 4, respLen - 4))
                break;

            handleHostPacket(buffer, respLen);
        }

        logMsg("YSF host disconnected.");

        pthread_mutex_lock(&hostMutex);
        hostFd = -1;
        pthread_mutex_unlock(&hostMutex);

        dropLink(false);
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
 * Returns: 0 on normal exit, 1 on a command line or startup error.
 */
int main(int argc, char** argv)
{
    bool daemon = false;
    int c;
    uint64_t last;
    unsigned int seconds = 0U;
    uint64_t lastSecond;

    while ((c = getopt(argc, argv, ":m:a:dxv")) != -1)
    {
        switch (c)
        {
            case 'm':
            {
                modemId = atoi(optarg);
                if (modemId < 1 || modemId > 10)
                {
                    reportError(modemName, "YSF gateway", "Invalid modem number.");
                    return 1;
                }
                sprintf(modemName, "modem%d", modemId);
                clientPort = 18500 + modemId - 1;
                fprintf(stderr, "Modem name: %s\n", modemName);
            }
            break;
            case 'a':
                snprintf(ysfHost, sizeof(ysfHost), "%s", optarg);   /* where the protocol service runs */
                break;
            case ':':
                fprintf(stderr, "Option '-%c' requires a value.\n", optopt);
                return 1;
            case 'd':
                daemon = true;
                break;
            case 'v':
                fprintf(stdout, "YSF_Gateway: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: YSF_Gateway [-m modem_number (1-10)] [-a address] [-d] [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            reportError(modemName, "YSF gateway", "YSF_Gateway: error in fork(), exiting");
            return 1;
        }

        if (pid > 0)
            return 0;

        setsid();
        umask(0);
    }

    loadCallsign(NULL);
    loadFcsConfig();
    loadReflectorList();

    {
        char tmp[64];

        memset(tmp, 0x00, sizeof(tmp));
        readHostConfig(modemName, "YSF", "wiresX", tmp);
        wxEnabled = strlen(tmp) == 0 || atoi(tmp) != 0;

        memset(tmp, 0x00, sizeof(tmp));
        readHostConfig(modemName, "YSF", "name", tmp);

        if (strlen(tmp) == 0)
            strcpy(tmp, "OpenMT");

        wxInit(&wx, baseCall, nodeSuffix, &refList, wxSendCb, NULL);
        wxSetInfo(&wx, tmp, txFrequency, rxFrequency);
    }

    if (pthread_create(&clientid, NULL, &startClient, NULL) != 0)
    {
        reportError(modemName, "YSF gateway", "Can't create YSF host thread");
        return 1;
    }

    last       = nowMs();
    lastSecond = last;

    while (connected)
    {
        uint64_t now;

        delay(10000);
        now = nowMs();

        if (wxEnabled)
        {
            pthread_mutex_lock(&wxMutex);
            wxClock(&wx, (uint32_t)(now - last));
            pthread_mutex_unlock(&wxMutex);
        }

        last = now;

        if (now - lastSecond >= 1000U)
        {
            lastSecond = now;
            inactivityCheck();

            if (++seconds % 60U == 0U)
                wxListRefresh();
        }
    }

    dropLink(false);
    pthread_join(clientid, NULL);

    fprintf(stderr, "YSF Gateway terminated.\n");
    logError(modemName, "main", "YSF Gateway terminated.");
    return 0;
}
