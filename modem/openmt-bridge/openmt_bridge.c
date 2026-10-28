/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

/*
 * openmt_bridge: bridge between Modem_Host and the OpenMT sample modem
 * firmware (stream type SAMPLE: frames 0x61, length, type|connection, payload).
 *
 * The bridge
 *   - keeps its settings in the database and creates the missing ones,
 *   - opens the real serial port ("port" and "baud") and makes the virtual
 *     port Modem_Host connects to,
 *   - sends the modem its 196 byte SET_CONFIG frame (duplex, invert flags,
 *     RX level) and asks for the firmware version, when the host starts and
 *     again when the settings change,
 *   - passes the modem's bytes to the host as they come, and the host's
 *     frames to the modem one whole frame at a time, so a control frame
 *     of the host can be told from frame data.
 *
 * Control frames of the host: see tools/bridge_common.h.  HOST_UP and
 * RELOAD both send the configuration again; the rest are not needed by this
 * modem and are ignored.
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
#include <unistd.h>

#include "bridge_common.h"
#include "tools.h"

#define OPENMT_START      0x61U
#define OPENMT_SET_CONFIG 0x20U
#define OPENMT_GET_VERSION 0xD0U
#define CONFIG_FRAME      196U

static char      modemName[10] = "modem1";
static int       modemFd = -1;
static int       ptyFd   = -1;
static volatile sig_atomic_t running = 1;

static uint8_t   hostBuf[2048];
static size_t    hostLen = 0;

/*
 * rd()
 *
 * Reads one setting of the modem into out ("" if there is none).
 *
 * Parameters:
 *   key  - Setting name in the "config" section.
 *   out  - Receives the value.
 *   size - Size of out.
 *
 * Returns: nothing.
 */
static void rd(const char* key, char* out, size_t size)
{
    out[0] = '\0';
    readHostConfigN(modemName, "config", key, out, size);
}

/*
 * writeAll()
 *
 * Writes bytes to the modem, waiting if its port is busy.
 *
 * Parameters:
 *   buf - Bytes.
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
 * createDefaults()
 *
 * Creates the settings that do not exist yet, with their defaults.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void createDefaults(void)
{
    bridgeDefault(modemName, "port", "ttyAMA0");
    bridgeDefault(modemName, "baud", "921600");
    bridgeDefault(modemName, "mode", "simplex");
    bridgeDefault(modemName, "rxInvert", "false");
    bridgeDefault(modemName, "txInvert", "true");
    bridgeDefault(modemName, "pttInvert", "false");
    bridgeDefault(modemName, "rxLevel", "100");
}

/*
 * sendConfig()
 *
 * Sends the modem its SET_CONFIG frame: bit 0 duplex, bit 1 RX invert, bit 2
 * TX invert, bit 3 PTT invert, then the RX level.
 *
 * Parameters:
 *   askVersion - Also ask for the firmware version.  The modem sends it by
 *                itself only the first time it is configured after a reset,
 *                so a host that starts later would never see it.
 *
 * Returns: nothing.
 */
static void sendConfig(bool askVersion)
{
    uint8_t buf[CONFIG_FRAME];
    char    tmp[16];

    memset(buf, 0, sizeof(buf));
    buf[0] = OPENMT_START;
    buf[1] = (uint8_t)CONFIG_FRAME;
    buf[2] = OPENMT_SET_CONFIG;

    rd("mode", tmp, sizeof(tmp));
    if (strcasecmp(tmp, "duplex") == 0)
        buf[3] |= 0x01U;
    rd("rxInvert", tmp, sizeof(tmp));
    if (strcasecmp(tmp, "true") == 0)
        buf[3] |= 0x02U;
    rd("txInvert", tmp, sizeof(tmp));
    if (strcasecmp(tmp, "true") == 0)
        buf[3] |= 0x04U;
    rd("pttInvert", tmp, sizeof(tmp));
    if (strcasecmp(tmp, "true") == 0)
        buf[3] |= 0x08U;
    rd("rxLevel", tmp, sizeof(tmp));
    buf[4] = (uint8_t)(tmp[0] ? atoi(tmp) : 100);

    if (!writeAll(buf, sizeof(buf)))
        fprintf(stderr, "openmt_bridge: could not send the configuration to the modem\n");

    if (askVersion)
    {
        static const uint8_t getVersion[3] = {OPENMT_START, 0x03, OPENMT_GET_VERSION};

        if (!writeAll(getVersion, sizeof(getVersion)))
            fprintf(stderr, "openmt_bridge: could not ask the modem for its firmware version\n");
    }
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
                if (type == BRIDGE_CTL_HOST_UP)
                    sendConfig(true);
                else if (type == BRIDGE_CTL_RELOAD)
                    sendConfig(false);
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
            if (!writeAll(p, p[1]))
                fprintf(stderr, "openmt_bridge: write to the modem failed\n");
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
    fprintf(stderr, "usage: %s [-n modem_name] [-l /dev/link]\n", prog);
}

/*
 * main()
 *
 * Entry point: reads the settings, opens the modem and the virtual port,
 * publishes the stream type and relays until stopped.
 *
 * Returns: 0 on a normal exit, 1 if start-up failed.
 */
int main(int argc, char** argv)
{
    const char* link = "/dev/openmt0";
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
    rd("port", port, sizeof(port));
    if (port[0] == '\0')
        snprintf(port, sizeof(port), "ttyAMA0");
    rd("baud", tmp, sizeof(tmp));

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
    bridgePublishStreamType(modemName, STREAM_SAMPLE);
    fprintf(stderr, "openmt_bridge: %s at %s baud, host port %s -> %s\n", devPath, tmp, link, slave);

    sendConfig(false);

    while (running)
    {
        struct pollfd fds[2] = {{modemFd, POLLIN, 0}, {ptyFd, POLLIN, 0}};

        if (poll(fds, 2, 100) < 0 && errno != EINTR)
            break;

        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP))
        {
            uint8_t buf[1024];
            ssize_t n = read(modemFd, buf, sizeof(buf));

            if (n < 0 && errno != EAGAIN && errno != EINTR)
            {
                fprintf(stderr, "openmt_bridge: modem port error: %s\n", strerror(errno));
                break;
            }
            if (n > 0 && write(ptyFd, buf, (size_t)n) < 0 && errno != EAGAIN)
            {
                /* nothing reads the virtual port: the bytes are lost, as on a real port */
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
                hostLen = 0;
            }
        }
    }

    close(modemFd);
    close(ptyFd);
    unlink(link);
    return 0;
}
