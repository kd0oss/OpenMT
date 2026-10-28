/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "bridge_common.h"
#include "tools.h"

/*
 * bridgeStreamName()
 *
 * Gives the text form of a stream type, as stored in the database.
 *
 * Parameters:
 *   type - The stream type.
 *
 * Returns: "FRAME", "BIT", "SAMPLE", or "" for STREAM_NONE.
 */
const char* bridgeStreamName(StreamType type)
{
    switch (type)
    {
        case STREAM_FRAME:  return "FRAME";
        case STREAM_BIT:    return "BIT";
        case STREAM_SAMPLE: return "SAMPLE";
        default:            return "";
    }
}

/*
 * bridgeStreamFromName()
 *
 * Reads the text form of a stream type.  The comparison ignores case.
 *
 * Parameters:
 *   name - Text to look at, may be NULL.
 *
 * Returns: The stream type, STREAM_NONE if the text is not a stream type.
 */
StreamType bridgeStreamFromName(const char* name)
{
    if (name == NULL)
        return STREAM_NONE;
    if (strcasecmp(name, "FRAME") == 0)
        return STREAM_FRAME;
    if (strcasecmp(name, "BIT") == 0)
        return STREAM_BIT;
    if (strcasecmp(name, "SAMPLE") == 0)
        return STREAM_SAMPLE;
    return STREAM_NONE;
}

/*
 * bridgeCtlBuild()
 *
 * Builds a control frame for the bridge.
 *
 * Parameters:
 *   type    - Control frame type (BRIDGE_CTL_RELOAD and so on).
 *   payload - Payload bytes, may be NULL when plen is 0.
 *   plen    - Number of payload bytes.
 *   buf     - Receives the frame; must hold BRIDGE_CTL_MAX bytes.
 *
 * Returns: Number of bytes written to buf, 0 if the payload is too long.
 */
size_t bridgeCtlBuild(uint8_t type, const uint8_t* payload, size_t plen, uint8_t* buf)
{
    if (BRIDGE_CTL_LENGTH + plen > BRIDGE_CTL_MAX)
        return 0;
    buf[0] = BRIDGE_CTL_START;
    buf[1] = (uint8_t)(BRIDGE_CTL_LENGTH + plen);
    buf[2] = type;
    if (plen > 0)
        memcpy(buf + BRIDGE_CTL_LENGTH, payload, plen);
    return BRIDGE_CTL_LENGTH + plen;
}

/*
 * bridgeCtlParse()
 *
 * Checks whether the bytes at the start of buf are a control frame.
 *
 * Parameters:
 *   buf      - Bytes received from the host, starting at a frame boundary.
 *   len      - Number of bytes in buf.
 *   type     - Receives the control frame type when a whole frame is found.
 *   payload  - Receives a pointer to the payload inside buf.
 *   plen     - Receives the payload length.
 *   needMore - Set to true when buf starts a control frame that is not all
 *              there yet, false otherwise.
 *
 * Returns: The size of the control frame in bytes, or 0 when buf does not
 *   start with a complete control frame.
 */
size_t bridgeCtlParse(const uint8_t* buf, size_t len, uint8_t* type, const uint8_t** payload, size_t* plen, bool* needMore)
{
    *needMore = false;
    if (len == 0 || buf[0] != BRIDGE_CTL_START)
        return 0;
    if (len >= 2 && (buf[1] < BRIDGE_CTL_LENGTH || buf[1] > BRIDGE_CTL_MAX))
        return 0;               /* a start byte by chance: not ours */
    if (len < BRIDGE_CTL_LENGTH || len < buf[1])
    {
        *needMore = true;
        return 0;
    }
    *type    = buf[2];
    *payload = buf + BRIDGE_CTL_LENGTH;
    *plen    = (size_t)buf[1] - BRIDGE_CTL_LENGTH;
    return buf[1];
}

/*
 * bridgeRelayBuild()
 *
 * Wraps one service frame in a relay frame (see bridge_common.h).
 *
 * Parameters:
 *   conn   - Connection id of the service.
 *   tlv    - The service frame, starting with 0x61.
 *   tlvLen - Its length.
 *   out    - Receives the relay frame.
 *   outCap - Size of out.
 *
 * Returns: The relay frame length, 0 if it does not fit.
 */
size_t bridgeRelayBuild(uint8_t conn, const uint8_t* tlv, size_t tlvLen, uint8_t* out, size_t outCap)
{
    size_t total = BRIDGE_RELAY_HEADER + tlvLen;

    if (total > 0xFFFFU || total > outCap)
        return 0;
    out[0] = BRIDGE_RELAY_START;
    out[1] = (uint8_t)(total >> 8);
    out[2] = (uint8_t)total;
    out[3] = conn;
    memcpy(out + BRIDGE_RELAY_HEADER, tlv, tlvLen);
    return total;
}

/*
 * bridgeRelayParse()
 *
 * Checks whether the bytes at the start of buf are a relay frame.
 *
 * Parameters:
 *   buf      - Received bytes, starting at a frame boundary.
 *   len      - Number of bytes in buf.
 *   conn     - Receives the connection id.
 *   tlv      - Receives a pointer to the service frame inside buf.
 *   tlvLen   - Receives the service frame length.
 *   needMore - Set to true when a relay frame is only partly there.
 *
 * Returns: The size of the relay frame, or 0 when buf does not start with a
 *   complete one.
 */
size_t bridgeRelayParse(const uint8_t* buf, size_t len, uint8_t* conn, const uint8_t** tlv, size_t* tlvLen, bool* needMore)
{
    size_t total;

    *needMore = false;
    if (len == 0 || buf[0] != BRIDGE_RELAY_START)
        return 0;
    if (len < 3)
    {
        *needMore = true;
        return 0;
    }
    total = ((size_t)buf[1] << 8) | buf[2];
    if (total < BRIDGE_RELAY_HEADER + 4)
        return 0;               /* a start byte by chance: not ours */
    if (len < total)
    {
        *needMore = true;
        return 0;
    }
    *conn   = buf[3];
    *tlv    = buf + BRIDGE_RELAY_HEADER;
    *tlvLen = total - BRIDGE_RELAY_HEADER;
    return total;
}

/*
 * bridgeOpenPty()
 *
 * Creates the pseudo terminal pair that Modem_Host opens as its serial port.
 * Both ends are put in raw mode (the link carries binary frames) and the
 * slave end is kept open for the life of the program, so the pty does not
 * report end of file to the master when Modem_Host is not connected.
 *
 * Parameters:
 *   symlinkPath  - Optional stable name for the slave device, for example
 *                  /dev/mmdvm0; NULL or empty for none.  An existing link of
 *                  that name is replaced.
 *   slavePath    - Receives the slave device path, for example /dev/pts/4.
 *   slavePathCap - Size of slavePath in bytes.
 *
 * Returns: The master file descriptor, or -1 on failure.
 */
int bridgeOpenPty(const char* symlinkPath, char* slavePath, size_t slavePathCap)
{
    int            master = -1;
    int            slave  = -1;
    char           path[256];
    struct termios tty;

    if (openpty(&master, &slave, path, NULL, NULL) != 0)
    {
        fprintf(stderr, "bridge: openpty failed: %s\n", strerror(errno));
        return -1;
    }

    if (tcgetattr(slave, &tty) == 0)
    {
        cfmakeraw(&tty);
        tcsetattr(slave, TCSANOW, &tty);
    }
    if (tcgetattr(master, &tty) == 0)
    {
        cfmakeraw(&tty);
        tcsetattr(master, TCSANOW, &tty);
    }

    snprintf(slavePath, slavePathCap, "%s", path);

    if (symlinkPath != NULL && symlinkPath[0] != '\0')
    {
        unlink(symlinkPath);
        if (symlink(path, symlinkPath) != 0)
            fprintf(stderr, "bridge: cannot link %s to %s: %s\n", symlinkPath, path, strerror(errno));
    }
    return master;
}

/*
 * bridgePublishStreamType()
 *
 * Tells Modem_Host which kind of packet stream this bridge delivers by
 * writing the "streamType" setting of the modem.
 *
 * Parameters:
 *   modemName - Name of the modem (the modem_name column).
 *   type      - FRAME, BIT or SAMPLE.
 *
 * Returns: true if the setting was written.
 */
bool bridgePublishStreamType(const char* modemName, StreamType type)
{
    return setHostConfig(modemName, "config", "streamType", "none", bridgeStreamName(type));
}

/*
 * bridgeReadStreamType()
 *
 * Reads the "streamType" setting of the modem.
 *
 * Parameters:
 *   modemName - Name of the modem.
 *
 * Returns: The stream type, STREAM_NONE if no bridge has published one.
 */
StreamType bridgeReadStreamType(const char* modemName)
{
    char value[16] = "";

    readHostConfigN(modemName, "config", "streamType", value, sizeof(value));
    return bridgeStreamFromName(value);
}

/*
 * bridgeDefault()
 *
 * Creates one setting of the modem with a default value, unless the setting
 * exists already; saved values are never changed.
 *
 * Parameters:
 *   modemName - Name of the modem.
 *   key       - Setting name.
 *   value     - Default value.
 *
 * Returns: true on success.
 */
bool bridgeDefault(const char* modemName, const char* key, const char* value)
{
    return addHostConfigDefault(modemName, "config", key, "none", value);
}

/*
 * bridgePublishPort()
 *
 * Tells Modem_Host the name of the bridge's virtual port (below /dev) by
 * writing the "bridgePort" setting of the modem.
 *
 * Parameters:
 *   modemName - Name of the modem.
 *   portName  - Device name without /dev/, for example "mmdvm0".
 *
 * Returns: true if the setting was written.
 */
bool bridgePublishPort(const char* modemName, const char* portName)
{
    return setHostConfig(modemName, "config", "bridgePort", "none", portName);
}

/*
 * bridgeOpenSerial()
 *
 * Opens the real serial port of a modem for raw 8N1 data without flow
 * control.  Reads return after at most 100 ms.
 *
 * Parameters:
 *   path - Device, for example /dev/ttyAMA0.
 *   baud - 115200, 230400, 460800 or 921600; anything else gives 115200.
 *
 * Returns: The file descriptor, or -1 if the port could not be opened.
 */
int bridgeOpenSerial(const char* path, unsigned int baud)
{
    struct termios tty;
    speed_t        speed;
    int            fd = open(path, O_RDWR | O_NOCTTY);

    if (fd < 0)
    {
        fprintf(stderr, "bridge: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (tcgetattr(fd, &tty) != 0)
    {
        close(fd);
        return -1;
    }

    switch (baud)
    {
        case 921600: speed = B921600; break;
        case 460800: speed = B460800; break;
        case 230400: speed = B230400; break;
        default:     speed = B115200; break;
    }

    cfmakeraw(&tty);
    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tty) != 0)
    {
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

/*
 * bridgeWaitTxEmpty()
 *
 * Waits until the output queue of a serial port is empty, at most 50 ms.
 * See bridge_common.h for why.  The 16 byte FIFO still holds about 170 us of
 * data when the queue runs empty, so the line hardly ever idles.
 *
 * Parameters:
 *   fd - The serial port.
 *
 * Returns: nothing.
 */
void bridgeWaitTxEmpty(int fd)
{
    int      pending = 0;
    unsigned waited  = 0;

    while (ioctl(fd, TIOCOUTQ, &pending) == 0 && pending > 0)
    {
        if (waited >= 50000U)
            return;
        usleep(50);
        waited += 50;
    }
}

/*
 * bridgePublishCaps()
 *
 * Writes the facts about the modem that Modem_Host reads at start-up.
 *
 * Parameters:
 *   modemName      - Name of the modem.
 *   creditPacing   - true if the host has to poll the modem for free buffer space.
 *   statusM17Index - Position of one field in a FRAME status reply.
 *   calibration    - true if the bridge can calibrate the receive frequency.
 *
 * Returns: true if all settings were written.
 */
bool bridgePublishCaps(const char* modemName, bool creditPacing, int statusM17Index, bool calibration)
{
    char text[8];
    bool ok;

    ok = setHostConfig(modemName, "config", "creditPacing", "none", creditPacing ? "true" : "false");
    snprintf(text, sizeof(text), "%d", statusM17Index);
    ok = setHostConfig(modemName, "config", "statusM17Index", "none", text) && ok;
    ok = setHostConfig(modemName, "config", "calibration", "none", calibration ? "true" : "false") && ok;
    return ok;
}

/*
 * bridgeAckUnsupported()
 *
 * Answers a dashboard command ("reloadOffsets", "calibrate", "probe") that
 * this bridge's modem cannot do, so the dashboard does not wait for it.
 *
 * Parameters:
 *   modemName - Name of the modem.
 *
 * Returns: nothing.
 */
void bridgeAckUnsupported(const char* modemName)
{
    ackDashbCommand(modemName, "modemHost", "error: not supported by this modem");
}
