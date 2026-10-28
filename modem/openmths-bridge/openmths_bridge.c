/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

/*
 * openmths_bridge: bridge between Modem_Host and the OPENMT_HS firmware.
 *
 * Stream type BIT (frames 0x61, length, type|connection, payload).  The bridge
 *   - keeps its settings in the database and creates the missing ones,
 *   - opens the real serial port ("port" and "baud") and makes the virtual
 *     port Modem_Host connects to,
 *   - sends the modem its SET_CONFIG frame at start and when asked,
 *   - passes frames both ways one whole frame at a time, and in doing so
 *       * adds the RX frequency offset of a mode ("rxOffset" in the
 *         section of the mode) to the ADD_MODE block of that mode,
 *       * follows SET_MODE and DEL_MODE,
 *   - measures the RX frequency error of a mode and reports it: the
 *     dashboard commands "calibrate <MODE>", "probe <MODE> <pass> <half>" and
 *     "reloadOffsets" reach it as control frames; it acknowledges them in
 *     the database itself.
 *
 * Control frames of the host: see tools/bridge_common.h.  RELOAD reads the
 * settings and the offsets again; MODE_CONN tells which mode name goes with a
 * connection id; COMMAND carries a dashboard command.
 *
 * The result texts of "calibrate" and "probe" are those the modem host gave
 * before the bridge existed, so the dashboard and openmt_cal work unchanged.
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

#include "ADF7021.h"
#include "bridge_common.h"
#include "tools.h"

#define OPENMT_START      0x61U
#define OPENMT_SET_CONFIG 0x20U
#define OPENMT_SET_MODE   0x30U
#define OPENMT_ADD_MODE   0x40U
#define OPENMT_DEL_MODE   0x50U
#define OPENMT_MEAS       0xD0U
#define OPENMT_NAK        0xF0U

#define ADD_MODE_BLOCK    58U
#define ADD_MODE_FRAME    (4U + ADD_MODE_BLOCK)
#define MAX_CONN          16

typedef struct
{
    bool    known;                      /* a name has been given */
    char    name[BRIDGE_CTL_MAX];
    bool    haveBlock;
    uint8_t block[ADD_MODE_BLOCK];      /* as the service sent it */
    int32_t offset;                     /* RX offset in the block sent to the modem */
} Conn;

typedef struct
{
    bool    pending;
    bool    done;
    bool    nak;
    uint8_t nakErr;
    uint8_t conn;
    uint8_t data[16];
    int     probe;                      /* -1 measurement, else the pass of a register 7 diagnostic */
    int     half;
    int32_t offset;
    time_t  deadline;
} Measurement;

static char      modemName[10] = "modem1";
static int       modemFd = -1;
static int       ptyFd   = -1;
static bool      debugOn = false;
static bool      duplex  = false;
static Conn      conns[MAX_CONN];
static Measurement meas;
static uint8_t   hostMode = 0x0f;       /* last SET_MODE of the host; 0x0f idle */
static volatile sig_atomic_t running = 1;

static uint8_t   hostBuf[2048];
static size_t    hostLen = 0;
static uint8_t   modemBuf[4096];
static size_t    modemLen = 0;

/*
 * rd()
 *
 * Reads one setting of the modem into out ("" if there is none).
 *
 * Parameters:
 *   module - "config" or a mode name.
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

static long roundHz(double x)
{
    return (long)(x + ((x < 0.0) ? -0.5 : 0.5));
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

    bridgeWaitTxEmpty(modemFd);        /* PL011 driver race, see bridge_common.h */
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
 * toHost()
 *
 * Sends bytes to the host on the virtual port.  When nothing reads it the
 * bytes are dropped, as a real modem port would lose them.
 *
 * Parameters:
 *   buf - Bytes to send.
 *   len - Number of bytes.
 *
 * Returns: nothing.
 */
static void toHost(const uint8_t* buf, size_t len)
{
    if (write(ptyFd, buf, len) < 0 && debugOn)
        fprintf(stderr, "openmths_bridge: host not reading\n");
}

/* ---- RX offsets ---------------------------------------------------------------------------------------------------- */

/*
 * rxOffsetStepHz()
 *
 * Gives the size of one count of the RX synthesizer (the N.F value in
 * register 0), from the band in register 1.
 *
 * Parameters:
 *   reg1 - Register 1 of the mode.
 *
 * Returns: Hz per count.
 */
static double rxOffsetStepHz(uint32_t reg1)
{
    return (reg1 == ADF7021_REG1_UHF2) ? (ADF7021_PFD / 32768.0) : (ADF7021_PFD / 65536.0);
}

/*
 * applyRxOffset()
 *
 * Moves the RX frequency in an ADD_MODE block by hz.
 *
 * Parameters:
 *   block - The 58 byte block, changed in place.
 *   hz    - The offset.
 *
 * Returns: nothing.
 */
static void applyRxOffset(uint8_t* block, int32_t hz)
{
    uint32_t reg0 = (uint32_t)block[0] | ((uint32_t)block[1] << 8) | ((uint32_t)block[2] << 16) | ((uint32_t)block[3] << 24);
    uint32_t reg1 = (uint32_t)block[8] | ((uint32_t)block[9] << 8) | ((uint32_t)block[10] << 16) | ((uint32_t)block[11] << 24);

    if (hz == 0 || (reg0 & 0x0FU) != 0U)
        return;

    int32_t counts = (int32_t)roundHz((double)hz / rxOffsetStepHz(reg1));
    reg0 += (uint32_t)(counts * 16);
    block[0] = (uint8_t)reg0;
    block[1] = (uint8_t)(reg0 >> 8);
    block[2] = (uint8_t)(reg0 >> 16);
    block[3] = (uint8_t)(reg0 >> 24);
}

/*
 * readRxOffset()
 *
 * Reads the RX offset of a mode; creates the row with 0 if there is none.
 *
 * Parameters:
 *   modeName - Section name of the mode.
 *
 * Returns: The offset in Hz, limited to +-20 kHz.
 */
static int32_t readRxOffset(const char* modeName)
{
    char tmp[32];

    rd(modeName, "rxOffset", tmp, sizeof(tmp));
    if (tmp[0] == '\0')
    {
        setHostConfig(modemName, modeName, "rxOffset", "rxoffset", "0");
        return 0;
    }

    long v = strtol(tmp, NULL, 10);
    return (int32_t)((v > 20000L) ? 20000L : (v < -20000L) ? -20000L : v);
}

/*
 * sendAddMode()
 *
 * Sends the modem the ADD_MODE frame of a connection with its RX offset in it.
 *
 * Parameters:
 *   id - Connection id; the block must be known.
 *
 * Returns: nothing.
 */
static void sendAddMode(uint8_t id)
{
    uint8_t frame[ADD_MODE_FRAME];

    frame[0] = OPENMT_START;
    frame[1] = (uint8_t)ADD_MODE_FRAME;
    frame[2] = OPENMT_ADD_MODE;
    frame[3] = id;
    memcpy(frame + 4, conns[id].block, ADD_MODE_BLOCK);
    applyRxOffset(frame + 4, conns[id].offset);
    writeAll(frame, sizeof(frame));
}

/*
 * reloadOffsets()
 *
 * Reads the RX offsets again and sends the ADD_MODE frame of every mode
 * whose offset changed.
 *
 * Parameters: none.
 *
 * Returns: The number of modes that were changed.
 */
static int reloadOffsets(void)
{
    int changed = 0;

    for (uint8_t i = 0; i < MAX_CONN; i++)
    {
        if (!conns[i].known || !conns[i].haveBlock)
            continue;

        int32_t off = readRxOffset(conns[i].name);

        if (off != conns[i].offset)
        {
            conns[i].offset = off;
            fprintf(stderr, "[%s] RX offset now %d Hz.\n", conns[i].name, (int)off);
            sendAddMode(i);
            changed++;
        }
    }
    return changed;
}

/* ---- measurement --------------------------------------------------------------------------------------------------- */

/*
 * startMeasurement()
 *
 * Sends the modem a measurement request for a mode.
 *
 * Parameters:
 *   modeName  - Name of the mode.
 *   text      - Receives the error message when it cannot be started.
 *   size      - Size of text.
 *   probePass - -1 for a measurement, else the pass of a register 7 diagnostic.
 *   probeHalf - Second argument of the diagnostic.
 *
 * Returns: true when the request is out, false with text set.
 */
static bool startMeasurement(const char* modeName, char* text, size_t size, int probePass, int probeHalf)
{
    int conn = -1;

    for (int i = 0; i < MAX_CONN; i++)
    {
        if (conns[i].known && conns[i].haveBlock && strcasecmp(conns[i].name, modeName) == 0)
        {
            conn = i;
            break;
        }
    }

    if (conn < 0)
    {
        snprintf(text, size, "error: mode %s is not registered", modeName);
        return false;
    }
    if (meas.pending)
    {
        snprintf(text, size, "error: a measurement is already running");
        return false;
    }

    memset(&meas, 0, sizeof(meas));
    meas.pending  = true;
    meas.probe    = probePass;
    meas.half     = probeHalf;
    meas.conn     = (uint8_t)conn;
    meas.offset   = conns[conn].offset;
    meas.deadline = time(NULL) + 5;

    /* Nothing is using the modem, so make sure it is not still holding a mode for the host */
    if (hostMode == 0x0f)
    {
        uint8_t idle[3] = {OPENMT_START, 0x03, (uint8_t)(OPENMT_SET_MODE | 0x0f)};

        writeAll(idle, 3);
    }

    uint8_t buf[5] = {OPENMT_START, 0x03, (uint8_t)(OPENMT_MEAS | (conn & 0x0f)), (uint8_t)probePass, (uint8_t)probeHalf};

    if (probePass >= 0)
        buf[1] = 0x05;
    writeAll(buf, buf[1]);
    return true;
}

/*
 * finishMeasurement()
 *
 * When the modem has answered (or the time is up) turns the answer into the
 * result text of the command.
 *
 * Parameters:
 *   text - Receives the result.
 *   size - Size of text.
 *
 * Returns: true when a result is ready in text.
 */
static bool finishMeasurement(char* text, size_t size)
{
    if (meas.pending && meas.done)
    {
        if (meas.nak)
        {
            if (meas.nakErr == 5)
                snprintf(text, size, "error: modem refused (the transmitter is keyed)");
            else if (meas.nakErr == 8)
                snprintf(text, size, "error: modem refused (transmit data is waiting in the modem)");
            else if (meas.nakErr == 3)
                snprintf(text, size, "error: modem refused (mode unknown to the modem)");
            else if (meas.nakErr == 1)
                snprintf(text, size, "error: modem refused (code 1, this firmware has no MEAS: flash the current one)");
            else
                snprintf(text, size, "error: modem refused (code %u, flash the firmware that matches this Modem_Host)",
                         (unsigned)meas.nakErr);
        }
        else if (meas.probe >= 0)
        {
            int n = snprintf(text, size, "raw %d %d", meas.probe, meas.half);

            for (int i = 0; i < 8 && n > 0 && (size_t)n < size; i++)
                n += snprintf(text + n, size - n, " %04X", (unsigned)(meas.data[i * 2] | (meas.data[i * 2 + 1] << 8)));
        }
        else
        {
            int16_t  a0    = (int16_t)(meas.data[0] | (meas.data[1] << 8));
            int16_t  a1    = (int16_t)(meas.data[2] | (meas.data[3] << 8));
            int16_t  step  = (int16_t)(meas.data[4] | (meas.data[5] << 8));
            uint16_t rssi  = (uint16_t)(meas.data[6] | (meas.data[7] << 8));
            uint16_t syncs = (uint16_t)(meas.data[8] | (meas.data[9] << 8));

            /* With the modem's AFC off the readback is the intermediate frequency, 100 kHz when the receiver is on
             * the signal; step is the Hz per count in 1/64 Hz.  The error is how far the IF is from 100 kHz, the
             * average of the two readings.  A positive error means the signal is above the receive frequency. */
            if (rssi > 118)
                snprintf(text, size, "error: no signal (rssi -%u dBm), is the reference transmitting?", rssi);
            else if (step <= 0)
                snprintf(text, size, "error: AFC readback not usable (a0=%d a1=%d step=%d)", a0, a1, step);
            else
            {
                double ifHz = ((double)a0 + (double)a1) / 2.0 * (double)step / 64.0;
                double err  = ifHz - 100000.0;

                if (err > 25000.0 || err < -25000.0)
                    snprintf(text, size, "error: AFC IF %ld Hz, not near 100 kHz (a0=%d a1=%d step=%d)", roundHz(ifHz), a0, a1,
                             step);
                else
                    snprintf(text, size, "ok err=%ld off=%d rssi=%u syncs=%u a0=%d a1=%d step=%d", roundHz(err),
                             (int)meas.offset, rssi, syncs, a0, a1, step);
            }
        }
        meas.pending = false;
        return true;
    }
    if (meas.pending && time(NULL) > meas.deadline)
    {
        snprintf(text, size, "error: no answer from the modem");
        meas.pending = false;
        return true;
    }
    return false;
}

/*
 * pollMeasurement()
 *
 * Reports a finished measurement in the database.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void pollMeasurement(void)
{
    char text[96];

    if (finishMeasurement(text, sizeof(text)))
        ackDashbCommand(modemName, "modemHost", text);
}

/* ---- settings and control frames ------------------------------------------------------------------------------------ */

/*
 * createDefaults()
 *
 * Adds every setting of this bridge that does not exist yet.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void createDefaults(void)
{
    bridgeDefault(modemName, "port", "ttyAMA0");
    bridgeDefault(modemName, "baud", "460800");
    bridgeDefault(modemName, "mode", "simplex");
}

/*
 * sendConfig()
 *
 * Sends the modem its SET_CONFIG frame.  The duplex bit is always set: the
 * modem keeps the duplex handling, the simplex or duplex setting only tells
 * the services what to do.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void sendConfig(void)
{
    static const uint8_t config[4] = {OPENMT_START, 0x04, OPENMT_SET_CONFIG, 0x81};

    writeAll(config, 4);
}

/*
 * handleCommand()
 *
 * Carries out a dashboard command of the host.
 *
 * Parameters:
 *   text - The command: reloadOffsets, calibrate <MODE> or probe <MODE> <pass> <half>.
 *
 * Returns: nothing.
 */
static void handleCommand(const char* text)
{
    char answer[96];

    if (strcasecmp(text, "reloadOffsets") == 0)
    {
        snprintf(answer, sizeof(answer), "success %d", reloadOffsets());
        ackDashbCommand(modemName, "modemHost", answer);
    }
    else if (strncasecmp(text, "calibrate ", 10) == 0)
    {
        if (!startMeasurement(text + 10, answer, sizeof(answer), -1, 0))
            ackDashbCommand(modemName, "modemHost", answer);
    }
    else if (strncasecmp(text, "probe ", 6) == 0)
    {
        char name[16];
        int  pass = 0, half = 0;

        if (sscanf(text + 6, "%15s %d %d", name, &pass, &half) != 3 || pass < 0 || pass > 20 || (pass > 2 && pass < 16) || half < 0 || half > 1)
            ackDashbCommand(modemName, "modemHost", "error: probe <MODE> <pass 0-2 or 16-20> <half 0-1>");
        else if (!startMeasurement(name, answer, sizeof(answer), pass, half))
            ackDashbCommand(modemName, "modemHost", answer);
    }
    else
    {
        ackDashbCommand(modemName, "modemHost", "error: unknown command");
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
    char text[BRIDGE_CTL_MAX];

    if (type == BRIDGE_CTL_HOST_UP)
    {
        /* a new host knows no modes: the modem is configured again, which also clears its mode list */
        memset(conns, 0, sizeof(conns));
        meas.pending = false;
        hostMode     = 0x0f;
        sendConfig();
    }
    else if (type == BRIDGE_CTL_RELOAD)
    {
        /* nothing in the settings changes what the modem is told, except the RX offsets of the modes */
        char tmp[16];

        rd("config", "mode", tmp, sizeof(tmp));
        duplex = (strcasecmp(tmp, "duplex") == 0);
        reloadOffsets();
        if (debugOn)
            fprintf(stderr, "openmths_bridge: settings read again\n");
    }
    else if (type == BRIDGE_CTL_MODE_CONN && plen >= 2)
    {
        uint8_t id = payload[0];

        if (id < MAX_CONN && plen - 1 < sizeof(conns[id].name))
        {
            memcpy(conns[id].name, payload + 1, plen - 1);
            conns[id].name[plen - 1] = '\0';
            conns[id].known     = true;
            conns[id].haveBlock = false;
        }
    }
    else if (type == BRIDGE_CTL_COMMAND && plen < sizeof(text))
    {
        memcpy(text, payload, plen);
        text[plen] = '\0';
        handleCommand(text);
    }
}

/* ---- the two directions -------------------------------------------------------------------------------------------- */

/*
 * hostFrame()
 *
 * Handles one whole frame from the host: ADD_MODE gets the RX offset of its
 * mode, DEL_MODE and SET_MODE are followed, everything goes to the modem.
 *
 * Parameters:
 *   p - The frame.
 *   n - Its length.
 *
 * Returns: nothing.
 */
static void hostFrame(const uint8_t* p, size_t n)
{
    uint8_t type = p[2] & 0xf0U;

    if (type == OPENMT_ADD_MODE && n == ADD_MODE_FRAME && p[3] < MAX_CONN)
    {
        uint8_t id = p[3];

        if (conns[id].known)
        {
            memcpy(conns[id].block, p + 4, ADD_MODE_BLOCK);
            conns[id].haveBlock = true;
            conns[id].offset    = readRxOffset(conns[id].name);
            if (conns[id].offset != 0)
                fprintf(stderr, "[%s] RX offset %d Hz.\n", conns[id].name, (int)conns[id].offset);
            sendAddMode(id);
            return;
        }
    }
    else if (type == OPENMT_DEL_MODE)
    {
        conns[p[2] & 0x0fU].known     = false;
        conns[p[2] & 0x0fU].haveBlock = false;
    }
    else if (type == OPENMT_SET_MODE)
    {
        hostMode = p[2] & 0x0fU;
    }

    if (!writeAll(p, n))
        fprintf(stderr, "openmths_bridge: write to the modem failed\n");
}

/*
 * processHost()
 *
 * Takes whole frames from the bytes received from the host.  Control frames
 * are carried out, OpenMT frames go on to the modem, anything else is
 * dropped one byte at a time until a frame start is found.  A partly
 * received frame stays in the buffer.
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
        else if (p[0] == OPENMT_START)
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
            hostFrame(p, p[1]);
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

/*
 * processModem()
 *
 * Passes the modem's frames to the host, one whole frame at a time.  The
 * answers to a pending measurement stay here.  Bytes that are not part of a
 * frame go on unchanged, so the host sees what the modem sent.
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

        if (p[0] != OPENMT_START)
        {
            toHost(p, 1);
            pos++;
            continue;
        }
        if (left < 2)
            break;
        if (p[1] < 3)
        {
            toHost(p, 1);
            pos++;
            continue;
        }
        if (left < p[1])
            break;

        size_t n    = p[1];
        bool   keep = false;

        if (meas.pending && n >= 5 && (p[2] & 0xf0U) == OPENMT_NAK && (p[3] & 0xf0U) == OPENMT_MEAS)
        {
            meas.nak    = true;
            meas.nakErr = p[4];
            meas.done   = true;
            keep        = true;
        }
        else if (meas.pending && (p[2] & 0xf0U) == OPENMT_MEAS && (p[2] & 0x0fU) == meas.conn)
        {
            if (meas.probe < 0 && n == 14U)
            {
                memcpy(meas.data, p + 3, 11);
                meas.done = true;
                keep      = true;
            }
            else if (meas.probe >= 0 && n == 19U)
            {
                memcpy(meas.data, p + 3, 16);
                meas.done = true;
                keep      = true;
            }
        }
        if (!keep)
            toHost(p, n);
        pos += n;
    }
    if (pos > 0)
    {
        memmove(modemBuf, modemBuf + pos, modemLen - pos);
        modemLen -= pos;
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
            "  -l  symbolic link to the virtual port (default /dev/openmths0); the\n"
            "      host opens the link's name below /dev\n", prog);
}

int main(int argc, char** argv)
{
    const char* link = "/dev/openmths0";
    char        slave[256];
    char        devPath[64];
    char        port[32] = "";
    char        tmp[16];
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
    rd("config", "port", port, sizeof(port));
    if (port[0] == '\0')
        snprintf(port, sizeof(port), "ttyAMA0");
    rd("config", "baud", tmp, sizeof(tmp));
    rd("config", "mode", slave, sizeof(slave));
    duplex = (strcasecmp(slave, "duplex") == 0);

    snprintf(devPath, sizeof(devPath), "/dev/%s", port);
    modemFd = bridgeOpenSerial(devPath, (unsigned)atoi(tmp));
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
    bridgePublishStreamType(modemName, STREAM_BIT);
    bridgePublishCaps(modemName, false, 12, true);
    fprintf(stderr, "openmths_bridge: %s at %s baud, host port %s -> %s\n", devPath, tmp, link, slave);

    sendConfig();

    while (running)
    {
        struct pollfd fds[2] = {{modemFd, POLLIN, 0}, {ptyFd, POLLIN, 0}};

        if (poll(fds, 2, 20) < 0 && errno != EINTR)
            break;

        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP))
        {
            ssize_t n = read(modemFd, modemBuf + modemLen, sizeof(modemBuf) - modemLen);

            if (n < 0 && errno != EAGAIN && errno != EINTR)
            {
                fprintf(stderr, "openmths_bridge: modem port error: %s\n", strerror(errno));
                break;
            }
            if (n > 0)
            {
                modemLen += (size_t)n;
                processModem();
            }
            if (modemLen == sizeof(modemBuf))
                modemLen = 0;
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
                hostLen = 0;
            }
        }
        pollMeasurement();
    }

    unlink(link);
    return 0;
}
