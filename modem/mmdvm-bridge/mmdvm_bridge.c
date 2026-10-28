/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

/*
 * mmdvm_bridge: bridge between Modem_Host and an MMDVM or MMDVM_HS modem.
 *
 * Stream type FRAME.  The bridge
 *   - keeps the MMDVM settings in the database (creating the missing ones
 *     with their defaults) and reads them at start and on request,
 *   - opens the real serial port of the modem ("port" and "baud"),
 *   - makes the virtual port Modem_Host connects to,
 *   - turns the frames of the protocol services (relayed by the host, see
 *     BRIDGE_RELAY_START in tools/bridge_common.h) into MMDVM frames and the
 *     modem's frames into service frames (mmdvm_frames.c), so Modem_Host
 *     needs to know nothing about the radio protocols,
 *   - paces the transmit frames by the free space the modem reports, decides
 *     which service owns the modem (BRIDGE_CTL_ACTIVE tells the host),
 *     ends a transmission that lasts too long,
 *   - sends the modem its configuration and frequency frames, and keeps the
 *     list of enabled modes the configuration frame carries,
 *   - obeys the control frames of the host (see tools/bridge_common.h):
 *     RELOAD reads the settings again and sends them to the modem;
 *     MODE_ON / MODE_OFF (a mode name) enable or disable that mode.
 *
 * Settings, module "config" of the modem in modem_host.config: modem
 * (mmdvmhs or mmdvm), port, baud, mode (duplex or simplex), rxInvert,
 * txInvert, pttInvert, useCOSAsLockout, debug, trace, txdelay, rxLevel,
 * rxDCOffset, txDCOffset, cwIdTxLevel, rxFrequency, txFrequency; and the
 * txLevel of each mode in the module named after the mode (DMR, YSF ...).
 *
 * The frame layouts follow set_Config(), set_ConfigHS() and setFrequency()
 * of mmdvm.c, including their quirks, so the modem sees the same bytes as
 * when Modem_Host sent them itself.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "bridge_common.h"
#include "mmdvm.h"
#include "mmdvm_frames.h"
#include "tools.h"

#define MMDVM_START   0xE0U
#define MMDVM_VERSION 0x00U
#define MMDVM_STATUS  0x01U
#define MMDVM_CONFIG  0x02U
#define MMDVM_MODE    0x03U
#define MMDVM_FREQ    0x04U
#define MMDVM_ACK     0x70U
#define MMDVM_NACK    0x7FU

#define MODE_IDLE_CODE 0x00U

#define LEVEL(x) ((uint8_t)((x) * 2.55F + 0.5F))

/* One setting block of the modem, read from the database. */
typedef struct
{
    bool     hs;                    /* hotspot board (modem = mmdvmhs) */
    char     port[32];
    unsigned baud;
    bool     duplex, rxInvert, txInvert, pttInvert, useCOS, debug, trace, ysfLoDev;
    unsigned txDelay;
    uint8_t  rxLevel;
    int      rxDCOffset, txDCOffset;
    uint8_t  cwIdLevel, dstarLevel, dmrLevel, ysfLevel, p25Level, nxdnLevel, m17Level, fmLevel;
    char     rxFreq[16], txFreq[16];
} Settings;

typedef struct
{
    bool dstar, dmr, ysf, p25, nxdn, fm, m17;
} Enabled;

typedef struct
{
    uint8_t  frame[4];
    bool     used;
    uint64_t due;
} Pending;

static char      modemName[10] = "modem1";
static Settings  cfg;
static Enabled   enabled;
static int       modemFd  = -1;
static int       ptyFd    = -1;
static bool      debugOn  = false;
static volatile sig_atomic_t running = 1;
static Pending   pending[8];

static uint8_t   hostBuf[4096];
static size_t    hostLen = 0;
static uint8_t   modemBuf[2048];
static size_t    modemLen = 0;

/* The services connected to the host, and which one owns the modem. */
#define MAX_CONN 14U
typedef struct
{
    bool  used;
    char  name[12];
    Proto proto;
} Conn;

static Conn      conns[MAX_CONN];
static int       activeConn = -1;       /* -1: idle */

/* Transmit queue (whole MMDVM frames) and its pacing. */
#ifndef TX_TIMEOUT_MS
#define TX_TIMEOUT_MS 300000U
#endif
static uint8_t   txq[8192];
static size_t    txqLen = 0;
static bool      bufReady = false;      /* enough queued to start sending */
static bool      txOn = false;
static uint64_t  txOnSince = 0;
static uint64_t  frameClock = 0;        /* last frame sent, or first frame queued */
static uint64_t  lastSteer = 0;
static uint64_t  statusClock = 0;
static unsigned  frameDelay = 19;
static uint8_t   currDelay = 19;

/* defaults of the values the frames use when nothing is read for them */
static const uint8_t ysfTXHang = 4, p25TXHang = 5, nxdnTXHang = 5, m17TXHang = 5;
static const uint8_t dmrColorCode = 1, dmrDelay = 0;

/*
 * rd()
 *
 * Reads one setting of the modem into out ("" if there is none).
 *
 * Parameters:
 *   module - Module name, "config" or a mode name.
 *   key    - Setting name.
 *   out    - Receives the value.
 *   size   - Size of out.
 *
 * Returns: nothing.
 */
static void rd(const char* module, const char* key, char* out, size_t size)
{
    out[0] = '\0';
    readHostConfigN(modemName, module, key, out, size);
}

static bool rdBool(const char* module, const char* key)
{
    char tmp[16];

    rd(module, key, tmp, sizeof(tmp));
    return strcasecmp(tmp, "true") == 0;
}

static int rdInt(const char* module, const char* key)
{
    char tmp[16];

    rd(module, key, tmp, sizeof(tmp));
    return atoi(tmp);
}

/*
 * createDefaults()
 *
 * Adds every setting of this bridge that does not exist yet.  An older
 * "modem" setting only decides the default of "board"; it is not changed.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void createDefaults(void)
{
    static const char* const kv[][2] =
    {
        {"port", "ttyAMA0"}, {"baud", "460800"}, {"mode", "simplex"},
        {"rxInvert", "false"}, {"txInvert", "true"}, {"pttInvert", "false"},
        {"useCOSAsLockout", "false"}, {"debug", "false"}, {"trace", "false"},
        {"txDelay", "100"}, {"rxLevel", "50"}, {"rxFrequency", "449900000"},
        {"txFrequency", "449900000"}, {"rxDCOffset", "0"}, {"txDCOffset", "0"},
        {"cwIdTxLevel", "50"}, {"fmTxLevel", "50"}
    };

    for (size_t i = 0; i < sizeof(kv) / sizeof(kv[0]); i++)
        bridgeDefault(modemName, kv[i][0], kv[i][1]);

    {
        char old[16] = "";

        rd("config", "modem", old, sizeof(old));
        bridgeDefault(modemName, "board", strcasecmp(old, "mmdvm") == 0 ? "full" : "hotspot");
    }
}

/*
 * loadSettings()
 *
 * Reads all the settings the bridge uses from the database.  A hotspot board
 * (board = hotspot) keeps the values its configuration frame has always
 * used: the receive level, the DC offsets and the FM level are not read for
 * it.
 *
 * Parameters: none.
 *
 * Returns: nothing; fills cfg.
 */
static void loadSettings(void)
{
    char tmp[32];
    Settings s;

    memset(&s, 0, sizeof(s));
    /* "board": hotspot or full.  Older installs have it in the "modem" setting (mmdvmhs or mmdvm). */
    rd("config", "board", tmp, sizeof(tmp));
    if (tmp[0] != '\0')
    {
        s.hs = (strcasecmp(tmp, "full") != 0);
    }
    else
    {
        rd("config", "modem", tmp, sizeof(tmp));
        s.hs = (tmp[0] == '\0' || strcasecmp(tmp, "mmdvmhs") == 0);
    }

    rd("config", "port", s.port, sizeof(s.port));
    if (s.port[0] == '\0')
        snprintf(s.port, sizeof(s.port), "ttyAMA0");
    s.baud = (unsigned)rdInt("config", "baud");
    if (s.baud == 0)
        s.baud = 460800;

    rd("config", "mode", tmp, sizeof(tmp));
    s.duplex     = strcasecmp(tmp, "duplex") == 0;
    s.rxInvert   = rdBool("config", "rxInvert");
    s.txInvert   = rdBool("config", "txInvert");
    s.pttInvert  = rdBool("config", "pttInvert");
    s.useCOS     = rdBool("config", "useCOSAsLockout");
    s.debug      = rdBool("config", "debug");
    s.trace      = rdBool("config", "trace");
    s.ysfLoDev   = rdBool("YSF", "loDev");
    s.txDelay    = (unsigned)rdInt("config", "txdelay");

    if (s.hs)
    {
        s.rxLevel    = 100;     /* as before: the hotspot frame carried the built-in default */
        s.rxDCOffset = 0;
        s.txDCOffset = 0;
        s.fmLevel    = 50;
    }
    else
    {
        s.rxLevel    = (uint8_t)rdInt("config", "rxLevel");
        s.rxDCOffset = rdInt("config", "rxDCOffset");
        s.txDCOffset = rdInt("config", "txDCOffset");
        s.fmLevel    = (uint8_t)rdInt("FM", "txLevel");
    }

    s.cwIdLevel = (uint8_t)rdInt("config", "cwIdTxLevel");
    s.dstarLevel = (uint8_t)rdInt("DSTAR", "txLevel");
    s.dmrLevel   = (uint8_t)rdInt("DMR", "txLevel");
    s.p25Level   = (uint8_t)rdInt("P25", "txLevel");
    s.m17Level   = (uint8_t)rdInt("M17", "txLevel");
    s.ysfLevel   = 50;
    rd("YSF", "txLevel", tmp, sizeof(tmp));
    if (tmp[0] != '\0')
        s.ysfLevel = (uint8_t)atoi(tmp);
    s.nxdnLevel = 50;
    rd("NXDN", "txLevel", tmp, sizeof(tmp));
    if (tmp[0] != '\0')
        s.nxdnLevel = (uint8_t)atoi(tmp);

    rd("config", "rxFrequency", s.rxFreq, sizeof(s.rxFreq));
    rd("config", "txFrequency", s.txFreq, sizeof(s.txFreq));

    cfg     = s;
    debugOn = s.debug;
}

/*
 * writeAll()
 *
 * Writes bytes to the modem, waiting up to a second for room.
 *
 * Parameters:
 *   buf - Bytes to write.
 *   len - Number of bytes.
 *
 * Returns: true if everything was written.
 */
static bool writeAll(const uint8_t* buf, size_t len)
{
    size_t done = 0;

    while (done < len)
    {
        ssize_t n = write(modemFd, buf + done, len - done);

        if (n > 0)
        {
            done += (size_t)n;
        }
        else if (n < 0 && (errno == EAGAIN || errno == EINTR))
        {
            struct pollfd p = {modemFd, POLLOUT, 0};

            if (poll(&p, 1, 1000) <= 0)
                return false;
        }
        else
        {
            return false;
        }
    }
    return true;
}

/*
 * restingMode()
 *
 * The mode the modem rests in when no service has the modem.  With several
 * modes enabled that is the idle mode, in which the modem scans them.  With
 * one mode enabled there is nothing to scan, and a hotspot board only
 * listens for a mode it has been put into, so the modem rests in that mode.
 *
 * Parameters: none.
 *
 * Returns: The MMDVM mode number.
 */
static uint8_t restingMode(void)
{
    uint8_t code  = MODE_IDLE_CODE;
    int     count = 0;

    if (enabled.dstar) { code = 0x01; count++; }
    if (enabled.dmr)   { code = 0x02; count++; }
    if (enabled.ysf)   { code = 0x03; count++; }
    if (enabled.p25)   { code = 0x04; count++; }
    if (enabled.nxdn)  { code = 0x05; count++; }
    if (enabled.m17)   { code = 0x07; count++; }
    if (enabled.fm)    { code = 0x0A; count++; }
    return count == 1 ? code : MODE_IDLE_CODE;
}

/*
 * buildConfigFull()
 *
 * Builds the 40 byte configuration frame of a full MMDVM board.
 *
 * Parameters:
 *   b - Receives the frame (at least 40 bytes).
 *
 * Returns: The frame length.
 */
static size_t buildConfigFull(uint8_t* b)
{
    memset(b, 0, 40);
    b[0] = MMDVM_START;
    b[1] = 40;
    b[2] = MMDVM_CONFIG;
    if (cfg.rxInvert)  b[3] |= 0x01U;
    if (cfg.txInvert)  b[3] |= 0x02U;
    if (cfg.pttInvert) b[3] |= 0x04U;
    if (cfg.ysfLoDev)  b[3] |= 0x08U;
    if (cfg.debug)     b[3] |= 0x10U;
    if (cfg.useCOS)    b[3] |= 0x20U;
    if (!cfg.duplex)   b[3] |= 0x80U;

    if (enabled.dstar) b[4] |= 0x01U;
    if (enabled.dmr)   b[4] |= 0x02U;
    if (enabled.ysf)   b[4] |= 0x04U;
    if (enabled.p25)   b[4] |= 0x08U;
    if (enabled.nxdn)  b[4] |= 0x10U;
    if (enabled.fm)    b[4] |= 0x20U;
    if (enabled.m17)   b[4] |= 0x40U;

    b[6]  = (uint8_t)(cfg.txDelay / 10U);
    b[7]  = restingMode();
    b[8]  = (uint8_t)(cfg.txDCOffset + 128);
    b[9]  = (uint8_t)(cfg.rxDCOffset + 128);
    b[10] = LEVEL(cfg.rxLevel);
    b[11] = LEVEL(cfg.cwIdLevel);
    b[12] = LEVEL(cfg.dstarLevel);
    b[13] = LEVEL(cfg.dmrLevel);
    b[14] = LEVEL(cfg.ysfLevel);
    b[15] = LEVEL(cfg.p25Level);
    b[16] = LEVEL(cfg.nxdnLevel);
    b[17] = LEVEL(cfg.m17Level);
    b[18] = LEVEL(50);              /* POCSAG */
    b[19] = LEVEL(cfg.fmLevel);
    b[20] = LEVEL(50);              /* AX.25 */
    b[23] = ysfTXHang;
    b[24] = p25TXHang;
    b[25] = nxdnTXHang;
    b[26] = m17TXHang;
    b[29] = dmrColorCode;
    b[30] = dmrDelay;
    b[31] = (uint8_t)(6 + 128);     /* AX.25 RX twist  */
    b[32] = 300 / 10;               /* AX.25 TX delay  */
    b[33] = 50 / 10;                /* AX.25 slot time */
    b[34] = 128;                    /* AX.25 p persist */
    return 40;
}

/*
 * buildConfigHS()
 *
 * Builds the 26 byte configuration frame of a hotspot board.
 *
 * Parameters:
 *   b - Receives the frame (at least 26 bytes).
 *
 * Returns: The frame length.
 */
static size_t buildConfigHS(uint8_t* b)
{
    memset(b, 0, 26);
    b[0] = MMDVM_START;
    b[1] = 26;
    b[2] = MMDVM_CONFIG;
    if (cfg.ysfLoDev) b[3] |= 0x08U;
    if (cfg.debug)    b[3] |= 0x10U;
    if (!cfg.duplex)  b[3] |= 0x80U;

    if (enabled.dstar) b[4] |= 0x01U;
    if (enabled.dmr)   b[4] |= 0x02U;
    if (enabled.ysf)   b[4] |= 0x04U;
    if (enabled.p25)   b[4] |= 0x08U;
    if (enabled.nxdn)  b[4] |= 0x10U;
    /* POCSAG (0x20) is never enabled */
    if (enabled.m17)   b[4] |= 0x40U;

    b[5]  = (uint8_t)(cfg.txDelay / 10U);
    b[6]  = restingMode();
    b[7]  = LEVEL(cfg.rxLevel);
    b[8]  = (uint8_t)(LEVEL(cfg.cwIdLevel) << 2);
    b[9]  = dmrColorCode;
    b[10] = dmrDelay;
    b[11] = 128;                    /* was OscOffset */
    b[12] = LEVEL(cfg.dstarLevel);
    b[13] = LEVEL(cfg.dmrLevel);
    b[14] = LEVEL(cfg.ysfLevel);
    b[15] = LEVEL(cfg.p25Level);
    b[16] = (uint8_t)(cfg.txDCOffset + 128);
    b[17] = (uint8_t)(cfg.rxDCOffset + 128);
    b[18] = LEVEL(cfg.nxdnLevel);
    b[19] = ysfTXHang;
    b[20] = LEVEL(50);              /* POCSAG */
    b[21] = LEVEL(cfg.fmLevel);
    b[22] = p25TXHang;
    b[23] = nxdnTXHang;
    b[24] = LEVEL(cfg.m17Level);
    b[25] = m17TXHang;
    return 26;
}

/*
 * sendConfig()
 *
 * Sends the modem its configuration frame.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void sendConfig(void)
{
    uint8_t b[40];
    size_t  n = cfg.hs ? buildConfigHS(b) : buildConfigFull(b);

    if (!writeAll(b, n))
        fprintf(stderr, "mmdvm_bridge: could not write the configuration to the modem\n");
}

/*
 * sendFrequency()
 *
 * Sends a hotspot board its frequencies (full boards have none to set).
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void sendFrequency(void)
{
    uint8_t  b[17];
    uint32_t f;

    if (!cfg.hs)
        return;
    b[0] = MMDVM_START;
    b[1] = 17;
    b[2] = MMDVM_FREQ;
    b[3] = 0;
    f = (uint32_t)atol(cfg.rxFreq);
    b[4] = (uint8_t)f; b[5] = (uint8_t)(f >> 8); b[6] = (uint8_t)(f >> 16); b[7] = (uint8_t)(f >> 24);
    f = (uint32_t)atol(cfg.txFreq);
    b[8] = (uint8_t)f; b[9] = (uint8_t)(f >> 8); b[10] = (uint8_t)(f >> 16); b[11] = (uint8_t)(f >> 24);
    b[12] = 255;                    /* RF power */
    b[13] = b[8]; b[14] = b[9]; b[15] = b[10]; b[16] = b[11];       /* POCSAG on the transmit frequency */
    writeAll(b, 17);
}

/*
 * startModem()
 *
 * Sends the start-up sequence: configuration, frequencies, version request.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void startModem(void)
{
    static const uint8_t version[3] = {MMDVM_START, 0x03, MMDVM_VERSION};

    sendConfig();
    usleep(10000);
    sendFrequency();
    usleep(10000);
    writeAll(version, 3);
}

/*
 * modeFlag()
 *
 * Finds the enable flag and the MMDVM mode number of a mode name.
 *
 * Parameters:
 *   name - Mode name: DSTAR, DMR, YSF, P25, NXDN, FM or M17.
 *   code - Receives the MMDVM mode number.
 *
 * Returns: Pointer to the flag, NULL for an unknown name.
 */
static bool* modeFlag(const char* name, uint8_t* code)
{
    if (strcasecmp(name, "DSTAR") == 0) { *code = 0x01; return &enabled.dstar; }
    if (strcasecmp(name, "DMR") == 0)   { *code = 0x02; return &enabled.dmr;   }
    if (strcasecmp(name, "YSF") == 0)   { *code = 0x03; return &enabled.ysf;   }
    if (strcasecmp(name, "P25") == 0)   { *code = 0x04; return &enabled.p25;   }
    if (strcasecmp(name, "NXDN") == 0)  { *code = 0x05; return &enabled.nxdn;  }
    if (strcasecmp(name, "FM") == 0)    { *code = 0x0A; return &enabled.fm;    }
    if (strcasecmp(name, "M17") == 0)   { *code = 0x07; return &enabled.m17;   }
    return NULL;
}

/*
 * queueModeCommand()
 *
 * Sends, now or after delayMs, the command that puts the modem into a mode.
 *
 * Parameters:
 *   code    - MMDVM mode number.
 *   delayMs - Wait before sending, 0 for now.
 *
 * Returns: nothing.
 */
static void queueModeCommand(uint8_t code, unsigned delayMs)
{
    uint8_t frame[4] = {MMDVM_START, 0x04, MMDVM_MODE, code};

    if (delayMs == 0)
    {
        writeAll(frame, 4);
        return;
    }
    for (size_t i = 0; i < sizeof(pending) / sizeof(pending[0]); i++)
    {
        if (!pending[i].used)
        {
            memcpy(pending[i].frame, frame, 4);
            pending[i].due  = nowMs() + delayMs;
            pending[i].used = true;
            return;
        }
    }
    writeAll(frame, 4);     /* no free slot: send at once */
}

/*
 * setMode()
 *
 * Enables or disables a mode, as the host asked.  Enabling a mode that was
 * off, or disabling one, sends the configuration again and the mode command
 * a second later (the modem needs time to apply the configuration).
 *
 * Parameters:
 *   name - Mode name.
 *   on   - true to enable.
 *
 * Returns: nothing.
 */
static void setMode(const char* name, bool on)
{
    uint8_t code;
    bool*   flag = modeFlag(name, &code);

    if (flag == NULL)
        return;
    if (debugOn)
        fprintf(stderr, "mmdvm_bridge: mode %s %s\n", name, on ? "on" : "off");
    if (on)
    {
        if (!*flag)
        {
            *flag = true;
            sendConfig();
            queueModeCommand(code, 1000);
        }
        else
        {
            queueModeCommand(code, 0);
        }
    }
    else
    {
        *flag = false;
        sendConfig();
        queueModeCommand(restingMode(), 1000);
    }
}

/*
 * runPending()
 *
 * Sends the delayed mode commands that are due.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void runPending(void)
{
    uint64_t t = nowMs();

    for (size_t i = 0; i < sizeof(pending) / sizeof(pending[0]); i++)
    {
        if (pending[i].used && t >= pending[i].due)
        {
            pending[i].used = false;
            writeAll(pending[i].frame, 4);
        }
    }
}

/*
 * hostWrite()
 *
 * Writes bytes to the virtual port for the host.  Gives up after a short
 * wait when the host is not reading.
 *
 * Parameters:
 *   buf - Bytes to write.
 *   len - Number of bytes.
 *
 * Returns: nothing.
 */
static void hostWrite(const uint8_t* buf, size_t len)
{
    size_t done  = 0;
    int    waits = 0;

    while (done < len)
    {
        ssize_t n = write(ptyFd, buf + done, len - done);

        if (n > 0)
        {
            done += (size_t)n;
        }
        else if (n < 0 && (errno == EAGAIN || errno == EINTR) && waits++ < 20)
        {
            struct pollfd p = {ptyFd, POLLOUT, 0};

            poll(&p, 1, 2);
        }
        else
        {
            if (debugOn)
                fprintf(stderr, "mmdvm_bridge: host not reading\n");
            return;
        }
    }
}

/*
 * notifyActive()
 *
 * Tells the host which connection owns the modem now.
 *
 * Parameters:
 *   conn - Connection id, -1 for none.
 *
 * Returns: nothing.
 */
static void notifyActive(int conn)
{
    uint8_t c = conn < 0 ? (uint8_t)BRIDGE_CONN_NONE : (uint8_t)conn;
    uint8_t f[BRIDGE_CTL_MAX];
    size_t  n = bridgeCtlBuild(BRIDGE_CTL_ACTIVE, &c, 1, f);

    if (n > 0)
        hostWrite(f, n);
}

/*
 * goIdle()
 *
 * Nobody owns the modem any more: the transmit queue is thrown away and the
 * host is told.
 *
 * Parameters:
 *   modeIdle - true to put the modem in its idle mode as well.
 *
 * Returns: nothing.
 */
static void goIdle(bool modeIdle)
{
    uint8_t idle[4] = {MMDVM_START, 0x04, MMDVM_MODE, 0};

    idle[3]    = restingMode();
    activeConn = -1;
    txOn       = false;
    txqLen     = 0;
    bufReady   = false;
    if (modeIdle)
        writeAll(idle, 4);
    notifyActive(-1);
}

/*
 * queueTx()
 *
 * Adds one modem frame to the transmit queue.  A full queue is emptied, as
 * the host's buffer was.
 *
 * Parameters:
 *   frame - The frame.
 *   len   - Its length.
 *
 * Returns: nothing.
 */
static void queueTx(const uint8_t* frame, size_t len)
{
    if (txqLen + len > sizeof(txq))
    {
        fprintf(stderr, "mmdvm_bridge: transmit queue full, cleared\n");
        txqLen = 0;
    }
    if (txqLen == 0)
        frameClock = nowMs();
    memcpy(txq + txqLen, frame, len);
    txqLen += len;
}

/*
 * handleRelay()
 *
 * Carries out one service frame from the host: the request for the modem or
 * to give it up, a raw command for the modem, or a frame to transmit (only
 * from the service that owns the modem).
 *
 * Parameters:
 *   conn   - Connection id of the service.
 *   tlv    - The service frame.
 *   tlvLen - Its length.
 *
 * Returns: nothing.
 */
static void handleRelay(uint8_t conn, const uint8_t* tlv, size_t tlvLen)
{
    uint8_t f[300];
    Proto   proto;
    TxState tx;
    bool    eot;
    size_t  n;

    if (conn >= MAX_CONN || !conns[conn].used)
        return;

    switch (serviceKind(tlv, tlvLen))
    {
        case SVC_SET_MODE:
            if (activeConn < 0)
            {
                activeConn = conn;
                setMode(conns[conn].name, true);
                notifyActive(conn);
            }
            return;
        case SVC_SET_IDLE:
            if (activeConn == conn)
            {
                goIdle(true);
                fprintf(stderr, "mmdvm_bridge: %s done, modem idle\n", conns[conn].name);
            }
            return;
        case SVC_ACK:
            return;
        default:
            break;
    }

    n = mmdvmRawFromService(tlv, tlvLen, f);
    if (n > 0)
    {
        writeAll(f, n);
        return;
    }

    n = mmdvmFromService(tlv, tlvLen, f, &proto, &tx, &eot);
    if (n == 0 || activeConn != (int)conn)
        return;

    /* A service ends what it received with a "lost" frame too.  With nothing being transmitted the modem
     * would only answer it with a NACK.  (DMR is left alone: its slots end separately.) */
    if (eot && proto != PROTO_DMR && !txOn && txqLen == 0)
    {
        if (debugOn)
            fprintf(stderr, "mmdvm_bridge: end of transmission frame dropped, nothing is being sent\n");
        return;
    }

    /* when the sending may start */
    switch (proto)
    {
        case PROTO_DSTAR:
            if (txqLen >= 200 || eot)
                bufReady = true;
            break;
        case PROTO_M17:
            if (txqLen >= 500 || eot)
                bufReady = true;
            break;
        case PROTO_P25:
        case PROTO_NXDN:
        case PROTO_YSF:
        case PROTO_DMR:
            bufReady = true;
            break;
        default:
            break;
    }

    queueTx(f, n);
    if (tx == TX_STATE_ON && !txOn)
    {
        txOn      = true;
        txOnSince = nowMs();
    }
    else if (tx == TX_STATE_OFF)
    {
        txOn = false;
    }
}

/*
 * registerConn()
 *
 * Notes the name of a connection of the host, so the frames of its mode can
 * be told apart.  A mode that is registered while the modem is idle leaves
 * the modem in its idle mode (it keeps scanning all the enabled modes).
 *
 * Parameters:
 *   conn - Connection id.
 *   name - Mode name.
 *
 * Returns: nothing.
 */
static void registerConn(uint8_t conn, const char* name)
{
    if (conn >= MAX_CONN)
        return;
    conns[conn].used = true;
    strncpy(conns[conn].name, name, sizeof(conns[conn].name) - 1);      /* longer names are cut, as the host cuts them */
    conns[conn].name[sizeof(conns[conn].name) - 1] = '\0';
    conns[conn].proto = protoFromName(conns[conn].name);
    if (debugOn)
        fprintf(stderr, "mmdvm_bridge: connection %u is %s (protocol %d)\n", (unsigned)conn, conns[conn].name,
                (int)conns[conn].proto);
    if (activeConn < 0 || activeConn == (int)conn)
    {
        /* Enabling the mode queued a mode command for a second later.  A mode
         * that is only registered must leave the modem idle (scanning all the
         * enabled modes); the command is sent when the service asks for the
         * modem. */
        for (size_t i = 0; i < sizeof(pending) / sizeof(pending[0]); i++)
            pending[i].used = false;
        goIdle(true);
    }
}

/*
 * unregisterMode()
 *
 * Forgets the connections of a mode that is not needed any more.
 *
 * Parameters:
 *   name - Mode name.
 *
 * Returns: nothing.
 */
static void unregisterMode(const char* name)
{
    for (unsigned i = 0; i < MAX_CONN; i++)
    {
        if (conns[i].used && strcasecmp(conns[i].name, name) == 0)
        {
            conns[i].used = false;
            if (activeConn == (int)i)
                goIdle(false);
        }
    }
}

/*
 * pace()
 *
 * Sends the queued transmit frames.  The delay between frames follows the
 * free space the modem reports: NXDN and YSF have their own rules (long
 * frames, a deep queue), for the other modes the delay moves by a
 * millisecond at a time so that the free space stays between 10 and 12
 * frames.  After every frame the modem is asked for its status.  Also asks
 * for the status twice a second and ends a transmission that has lasted
 * longer than the transmit timeout.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void pace(void)
{
    static const uint8_t statusReq[3] = {MMDVM_START, 0x03, MMDVM_STATUS};
    uint64_t t = nowMs();

    if (t - statusClock >= 500)
    {
        statusClock = t;
        if (txqLen < 3)
            writeAll(statusReq, 3);
    }

    if (txOn && t - txOnSince >= TX_TIMEOUT_MS)
    {
        fprintf(stderr, "mmdvm_bridge: TX timeout. Setting mode to idle.\n");
        goIdle(true);
    }

    if (txqLen == 0)
    {
        bufReady   = false;
        frameDelay = 19;
        frameClock = t;
        return;
    }

    if (t != lastSteer)
    {
        Proto p = activeConn >= 0 ? conns[activeConn].proto : PROTO_NONE;

        lastSteer = t;
        if (p == PROTO_NXDN)
        {
            frameDelay = nxdnFrameDelay(getSpace(PROTO_NXDN, 1));
        }
        else if (p == PROTO_YSF)
        {
            frameDelay = ysfFrameDelay(getSpace(PROTO_YSF, 1));
        }
        else
        {
            uint8_t space1 = getSpace(p, 1);
            uint8_t space2 = getSpace(p, 2);
            uint8_t space  = space1 < space2 ? space1 : space2;

            if (space < 10)
            {
                if (currDelay > 19)
                    currDelay = 19;
                frameDelay = ++currDelay;
            }
            else if (space > 12)
            {
                if (currDelay <= 1)
                    currDelay = 2;
                frameDelay = --currDelay;
            }
        }
    }

    if (txqLen >= 3 && bufReady && t - frameClock >= frameDelay)
    {
        if (txq[0] != MMDVM_START || txq[1] < 3)
        {
            memmove(txq, txq + 1, --txqLen);       /* not at a frame: find the next one */
            return;
        }
        if (txqLen >= txq[1])
        {
            size_t  len   = txq[1];
            Proto   p     = activeConn >= 0 ? conns[activeConn].proto : PROTO_NONE;
            uint8_t space = getSpace(p, txq[2] == TYPE_DMR_DATA2 ? 2 : 1);

            if (space > 1)
            {
                if (!writeAll(txq, len))
                    fprintf(stderr, "mmdvm_bridge: modem TX write failed\n");
                memmove(txq, txq + len, txqLen - len);
                txqLen -= len;
            }
            writeAll(statusReq, 3);
            frameClock = t;
        }
    }
}

/*
 * handleModemFrame()
 *
 * Deals with one frame from the modem: answers and debug text are shown, the
 * status updates the free space, the frames of a protocol go to the service
 * that owns the modem (or to the service of that protocol while the modem is
 * idle).
 *
 * Parameters:
 *   f   - The frame.
 *   len - Its length.
 *
 * Returns: nothing.
 */
static void handleModemFrame(const uint8_t* f, size_t len)
{
    uint8_t type = f[2];
    uint8_t out[300];
    uint8_t rf[320];
    Proto   proto;
    size_t  n;
    int     c = -1;

    if (type == MMDVM_NACK)
    {
        fprintf(stderr, "Nack == Type: %02X  Err: %02X\n", len > 3 ? f[3] : 0, len > 4 ? f[4] : 0);
        return;
    }
    if (type == MMDVM_ACK)
        return;
    if ((type & 0xf0U) == 0xf0U)
    {
        char text[256];
        size_t tl = len > 3 ? len - 3 : 0;

        if (tl >= sizeof(text))
            tl = sizeof(text) - 1;
        memcpy(text, f + 3, tl);
        text[tl] = '\0';
        fprintf(stderr, "DEBUG: %s\n", text);
        return;
    }
    if (type == MMDVM_VERSION)
    {
        char text[256];
        size_t tl = len > 4 ? len - 4 : 0;

        if (tl >= sizeof(text))
            tl = sizeof(text) - 1;
        memcpy(text, f + 4, tl);
        text[tl] = '\0';
        fprintf(stderr, "mmdvm_bridge: modem version: %s\n", text);
        return;
    }
    if (type == MMDVM_STATUS)
    {
        setSpacesFromStatus(f, len, cfg.hs ? 13 : 12);
        return;
    }

    n = serviceFromMmdvm(f, len, out, &proto);
    if (n == 0)
    {
        if (debugOn)
            fprintf(stderr, "mmdvm_bridge: modem frame %02X (%zu bytes) dropped\n", type, len);
        return;
    }
    if (debugOn)
    {
        static unsigned traced;
        int             k;

        /* the first frames of a burst are enough; a voice frame comes every 20 to 100 ms */
        if (traced++ < 400)
        {
            fprintf(stderr, "mmdvm_bridge: modem frame %02X (%zu bytes) proto %d, owner %d, modem mode connections:",
                    type, len, (int)proto, activeConn);
            for (k = 0; k < (int)MAX_CONN; k++)
            {
                if (conns[k].used)
                    fprintf(stderr, " %d=%s", k, conns[k].name);
            }
            fprintf(stderr, "\n");
        }
    }
    if (activeConn >= 0)
    {
        if (conns[activeConn].proto == proto)
            c = activeConn;
    }
    else
    {
        for (unsigned i = 0; i < MAX_CONN; i++)
        {
            if (conns[i].used && conns[i].proto == proto)
            {
                c = (int)i;
                break;
            }
        }
    }
    if (c < 0)
    {
        if (debugOn)
            fprintf(stderr, "mmdvm_bridge: no connection takes this frame (owner %d)\n", activeConn);
        return;
    }
    n = bridgeRelayBuild((uint8_t)c, out, n, rf, sizeof(rf));
    if (n > 0)
        hostWrite(rf, n);
}

/*
 * processModem()
 *
 * Takes whole frames from the bytes received from the modem.  Bytes that are
 * not at a frame start are dropped one at a time; a partly received frame
 * stays in the buffer.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void processModem(void)
{
    size_t pos = 0;

    while (pos < modemLen)
    {
        const uint8_t* p    = modemBuf + pos;
        size_t         left = modemLen - pos;

        if (p[0] != MMDVM_START)
        {
            static unsigned stray;

            if (stray++ < 20)
                fprintf(stderr, "mmdvm_bridge: unknown byte from the modem, 0x%02X\n", p[0]);
            pos++;
            continue;
        }
        if (left < 2)
            break;
        if (p[1] < 3)
        {
            pos++;
            continue;
        }
        if (left < p[1])
            break;
        handleModemFrame(p, p[1]);
        pos += p[1];
    }
    if (pos > 0)
    {
        memmove(modemBuf, modemBuf + pos, modemLen - pos);
        modemLen -= pos;
    }
}

/*
 * handleControl()
 *
 * Carries out one control frame of the host.
 *
 * Parameters:
 *   type    - Control frame type.
 *   payload - Payload bytes.
 *   plen    - Payload length.
 *
 * Returns: nothing.
 */
static void handleControl(uint8_t type, const uint8_t* payload, size_t plen)
{
    char name[BRIDGE_CTL_MAX];

    if (type == BRIDGE_CTL_MODE_CONN)
    {
        if (plen >= 2 && plen < sizeof(name))
        {
            memcpy(name, payload + 1, plen - 1);
            name[plen - 1] = '\0';
            registerConn(payload[0], name);
        }
        return;
    }

    if (plen >= sizeof(name))
        return;
    memcpy(name, payload, plen);
    name[plen] = '\0';

    if (type == BRIDGE_CTL_RELOAD || type == BRIDGE_CTL_HOST_UP)
    {
        if (type == BRIDGE_CTL_HOST_UP)
        {
            memset(conns, 0, sizeof(conns));
            activeConn = -1;
            txOn       = false;
            txqLen     = 0;
            bufReady   = false;
        }
        loadSettings();
        if (debugOn)
            fprintf(stderr, "mmdvm_bridge: settings read again\n");
        startModem();
    }
    else if (type == BRIDGE_CTL_MODE_ON)
    {
        setMode(name, true);
    }
    else if (type == BRIDGE_CTL_MODE_OFF)
    {
        unregisterMode(name);
        setMode(name, false);
    }
    else if (type == BRIDGE_CTL_COMMAND)
    {
        bridgeAckUnsupported(modemName);
    }
}

/*
 * processHost()
 *
 * Takes whole frames from the bytes received from the host: control frames
 * are carried out, MMDVM frames go to the modem, anything else is dropped
 * one byte at a time until a frame start is found.  A partly received frame
 * stays in the buffer.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void processHost(void)
{
    size_t pos = 0;

    while (pos < hostLen)
    {
        const uint8_t* p    = hostBuf + pos;
        size_t         left = hostLen - pos;

        if (p[0] == BRIDGE_CTL_START)
        {
            uint8_t        type;
            const uint8_t* payload;
            size_t         plen;
            bool           more;
            size_t         used = bridgeCtlParse(p, left, &type, &payload, &plen, &more);

            if (used > 0)
            {
                handleControl(type, payload, plen);
                pos += used;
            }
            else if (more)
            {
                break;
            }
            else
            {
                pos++;
            }
        }
        else if (p[0] == BRIDGE_RELAY_START)
        {
            uint8_t        conn;
            const uint8_t* tlv;
            size_t         tlvLen;
            bool           more;
            size_t         used = bridgeRelayParse(p, left, &conn, &tlv, &tlvLen, &more);

            if (used > 0)
            {
                handleRelay(conn, tlv, tlvLen);
                pos += used;
            }
            else if (more)
            {
                break;
            }
            else
            {
                pos++;
            }
        }
        else if (p[0] == MMDVM_START)
        {
            if (left < 2)
                break;
            if (p[1] < 3)
            {
                pos++;
                continue;
            }
            if (left < p[1])
                break;
            if (!writeAll(p, p[1]))
                fprintf(stderr, "mmdvm_bridge: write to the modem failed\n");
            pos += p[1];
        }
        else
        {
            pos++;
        }
    }
    if (pos > 0)
    {
        memmove(hostBuf, hostBuf + pos, hostLen - pos);
        hostLen -= pos;
    }
}

static void onSignal(int sig)
{
    (void)sig;
    running = 0;
}

static void usage(const char* prog)
{
    fprintf(stderr,
            "Usage: %s [-n modem_name] [-l link_path]\n"
            "  -n  modem name in the database (default modem1)\n"
            "  -l  symbolic link to the virtual port (default /dev/mmdvm0); the\n"
            "      host opens the link's name below /dev\n", prog);
}

int main(int argc, char** argv)
{
    const char* link = "/dev/mmdvm0";
    char        slave[256];
    char        devPath[64];
    int         opt;

    while ((opt = getopt(argc, argv, "n:l:h")) != -1)
    {
        if (opt == 'n')
            snprintf(modemName, sizeof(modemName), "%s", optarg);
        else if (opt == 'l')
            link = optarg;
        else
        {
            usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGPIPE, SIG_IGN);

    createDefaults();
    loadSettings();

    snprintf(devPath, sizeof(devPath), "/dev/%s", cfg.port);
    modemFd = bridgeOpenSerial(devPath, cfg.baud);
    if (modemFd < 0)
        return 1;

    ptyFd = bridgeOpenPty(link, slave, sizeof(slave));
    if (ptyFd < 0)
        return 1;
    fcntl(ptyFd, F_SETFL, fcntl(ptyFd, F_GETFL) | O_NONBLOCK);

    {
        char linkCopy[128];

        snprintf(linkCopy, sizeof(linkCopy), "%s", link);
        bridgePublishPort(modemName, basename(linkCopy));
    }
    bridgePublishStreamType(modemName, STREAM_FRAME);
    bridgePublishCaps(modemName, false, cfg.hs ? 13 : 12, false);
    fprintf(stderr, "mmdvm_bridge: %s (%s) at %u baud, host port %s -> %s\n", devPath,
            cfg.hs ? "hotspot" : "full board", cfg.baud, link, slave);

    startModem();
    statusClock = nowMs();

    while (running)
    {
        struct pollfd fds[2] = {{modemFd, POLLIN, 0}, {ptyFd, POLLIN, 0}};
        if (poll(fds, 2, txqLen > 0 ? 1 : 50) < 0 && errno != EINTR)
            break;

        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP))
        {
            ssize_t n = read(modemFd, modemBuf + modemLen, sizeof(modemBuf) - modemLen);

            if (n < 0 && errno != EAGAIN && errno != EINTR)
            {
                fprintf(stderr, "mmdvm_bridge: modem port error: %s\n", strerror(errno));
                break;
            }
            if (n > 0)
            {
                modemLen += (size_t)n;
                processModem();
            }
            else if (modemLen == sizeof(modemBuf))
            {
                modemLen = 0;
            }
        }
        if (fds[1].revents & POLLIN)
        {
            ssize_t n = read(ptyFd, hostBuf + hostLen, sizeof(hostBuf) - hostLen);

            if (n > 0)
            {
                hostLen += (size_t)n;
                processHost();
            }
            else if (hostLen == sizeof(hostBuf))
            {
                hostLen = 0;        /* a full buffer that holds no frame: start again */
            }
        }
        runPending();
        pace();
    }

    unlink(link);
    return 0;
}
