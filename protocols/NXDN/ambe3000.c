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

#include "ambe3000.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define START_BYTE      0x61U
#define TYPE_CONTROL    0x00U
#define TYPE_CHANNEL    0x01U
#define TYPE_SPEECH     0x02U

#define FIELD_SPEECHD   0x00U
#define FIELD_CHANNEL   0x01U
#define FIELD_RATET     0x09U
#define FIELD_RATEP     0x0AU
#define FIELD_INIT      0x0BU
#define FIELD_PRODID    0x30U
#define FIELD_RESET     0x33U
#define FIELD_READY     0x39U

#define PKT_MAX         1024U

/* Channel data field id: 0x01 on most AMBE3000R dongles, 0x40 in some DVSI documents. Accepted on receive. */
#define FIELD_CHANNEL0  0x40U

int ambeDebug = 0;
int ambeChunk = 16;         /* bytes written at a time; the chip's UART has little buffering and no flow control */
int ambeGapUs = 2000;       /* pause after each chunk, microseconds */
int ambeFlow = 0;           /* 1: RTS/CTS hardware flow control */
int ambeHwReset = 0;        /* 1 pulse DTR low, 2 pulse RTS low, 3 both, before the chip is used */
int ambeInit = 0x00;        /* PKT_INIT flags sent after the rate (bit 0 encoder, bit 1 decoder); 0 = none */

/*
 * dump()
 *
 * Debug output.  When ambeDebug is set, prints the direction, packet type,
 * length and the first bytes of a packet to stderr.
 *
 * Parameters:
 *   dir  - Text giving the direction, "rx" or "tx".
 *   type - Packet type.
 *   p    - The packet payload.
 *   len  - Number of bytes in p.
 *
 * Returns: nothing.
 */
static void dump(const char* dir, uint8_t type, const uint8_t* p, size_t len)
{
    if (!ambeDebug)
        return;
    fprintf(stderr, "AMBE %s type %u len %zu:", dir, type, len);
    for (size_t i = 0U; i < len && i < 24U; i++)
        fprintf(stderr, " %02X", p[i]);
    if (len > 24U)
        fprintf(stderr, " ...");
    fprintf(stderr, "\n");
}

/*
 * ambeDefaultRate()
 *
 * Sets the chip's rate control words to the default (a 3600 bit/s rate with
 * FEC) and selects no rate index.
 *
 * Parameters:
 *   a - The AMBE connection.
 *
 * Returns: nothing.
 */
void ambeDefaultRate(Ambe* a)
{
    static const uint16_t words[6] = { 0x0431U, 0x0754U, 0x2400U, 0x0000U, 0x0000U, 0x6F48U };

    memcpy(a->rate, words, sizeof(words));
    a->rateIndex = -1;
}

/*
 * baudConstant()
 *
 * Converts a baud rate to the termios speed constant.
 *
 * Parameters:
 *   baud - Baud rate: 115200, 230400, 460800 or 921600.
 *
 * Returns: The speed constant, or B0 if the rate is not supported.
 */
static speed_t baudConstant(unsigned int baud)
{
    switch (baud)
    {
        case 115200U: return B115200;
        case 230400U: return B230400;
        case 460800U: return B460800;
        case 921600U: return B921600;
        default:      return B0;
    }
}

/*
 * writeAll()
 *
 * Writes a whole buffer to a file descriptor, retrying when the write is
 * interrupted.
 *
 * Parameters:
 *   fd   - File descriptor to write to.
 *   data - Bytes to write.
 *   len  - Number of bytes in data.
 *
 * Returns: true if everything was written, false on an error.
 */
static bool writeAll(int fd, const uint8_t* data, size_t len)
{
    while (len > 0U)
    {
        ssize_t n = write(fd, data, len);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += n;
        len  -= (size_t)n;
    }
    return true;
}

/*
 * readBytes()
 *
 * Reads an exact number of bytes, waiting up to a timeout for each part.
 *
 * Parameters:
 *   fd        - File descriptor to read from.
 *   data      - Receives the bytes.
 *   len       - Number of bytes to read.
 *   timeoutMs - Longest wait for more data, in milliseconds.
 *
 * Returns: true if all the bytes were read, false on a timeout or error.
 */
static bool readBytes(int fd, uint8_t* data, size_t len, int timeoutMs)
{
    while (len > 0U)
    {
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, timeoutMs);

        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;

        ssize_t n = read(fd, data, len);

        if (n <= 0)
            return false;
        data += n;
        len  -= (size_t)n;
    }
    return true;
}

/*
 * readPacket()
 *
 * Reads one packet. Returns its type, or -1 on timeout. 'payload' receives
 * the bytes after the 4 byte header.
 *
 * Parameters:
 *   fd        - Serial port.
 *   payload   - Receives the bytes after the 4 byte header.
 *   length    - Receives the number of bytes in payload.
 *   timeoutMs - Longest wait for data, in milliseconds.
 *
 * Returns: The packet type, or -1 on a timeout or error.
 */
static int readPacket(int fd, uint8_t* payload, size_t* length, int timeoutMs)
{
    uint8_t b, hdr[3];

    for (;;)
    {
        if (!readBytes(fd, &b, 1U, timeoutMs))
            return -1;
        if (b == START_BYTE)
            break;
    }

    if (!readBytes(fd, hdr, sizeof(hdr), timeoutMs))
        return -1;

    size_t len = ((size_t)hdr[0] << 8) | hdr[1];

    if (len > PKT_MAX)
        return -1;
    if (len > 0U && !readBytes(fd, payload, len, timeoutMs))
        return -1;

    *length = len;
    dump("rx", hdr[2], payload, len);
    return hdr[2];
}

/*
 * sendPacket()
 *
 * Sends one packet to the chip: the start byte, the 16 bit length, the type
 * and the payload.  The packet can be written in small chunks with a pause
 * between them (ambeChunk and ambeGapUs) for slow serial adapters.
 *
 * Parameters:
 *   fd      - Serial port.
 *   type    - Packet type.
 *   payload - The payload.
 *   len     - Number of bytes in payload.
 *
 * Returns: true if the whole packet was written, false if it is too long or
 *   the write failed.
 */
static bool sendPacket(int fd, uint8_t type, const uint8_t* payload, size_t len)
{
    uint8_t buf[PKT_MAX + 4U];

    if (len > PKT_MAX)
        return false;

    buf[0] = START_BYTE;
    buf[1] = (uint8_t)(len >> 8);
    buf[2] = (uint8_t)len;
    buf[3] = type;
    memcpy(buf + 4U, payload, len);
    dump("tx", type, payload, len);

    size_t total = len + 4U, pos = 0U;
    size_t chunk = ambeChunk > 0 ? (size_t)ambeChunk : total;

    while (pos < total)
    {
        size_t n = total - pos < chunk ? total - pos : chunk;

        if (!writeAll(fd, buf + pos, n))
            return false;
        pos += n;
        if (pos < total && ambeGapUs > 0)
        {
            tcdrain(fd);
            usleep((useconds_t)ambeGapUs);
        }
    }
    return true;
}

/*
 * control()
 *
 * Sends a control packet and waits for the reply with the same field id.
 *
 * Parameters:
 *   a         - The AMBE connection.
 *   payload   - The control packet payload; its first byte is the field id.
 *   len       - Number of bytes in payload.
 *   reply     - Receives the reply payload (may be NULL).
 *   replyLen  - Receives the length of the reply.
 *   timeoutMs - Longest wait for each packet, in milliseconds.
 *
 * Returns: true if a reply with the same field id arrived, false on a
 *   timeout or failure.
 */
static bool control(Ambe* a, const uint8_t* payload, size_t len, uint8_t* reply, size_t* replyLen, int timeoutMs)
{
    uint8_t rx[PKT_MAX];
    size_t  rxLen;

    if (!sendPacket(a->fd, TYPE_CONTROL, payload, len))
        return false;

    for (unsigned int tries = 0U; tries < 8U; tries++)
    {
        int type = readPacket(a->fd, rx, &rxLen, timeoutMs);

        if (type < 0)
            return false;
        if (type == (int)TYPE_CONTROL && rxLen >= 1U && rx[0] == payload[0])
        {
            if (reply != NULL)
            {
                memcpy(reply, rx, rxLen);
                *replyLen = rxLen;
            }
            return true;
        }
    }
    return false;
}

/*
 * ambeOpen()
 *
 * Opens the serial port of an AMBE3000 vocoder chip and starts it.  The port
 * is set to raw 8N1 at the given baud rate (with RTS/CTS if ambeFlow is
 * set), the chip is reset through DTR or RTS if ambeHwReset asks for it, and
 * the READY packet is waited for.  The product id is read, the rate is set
 * and the vocoder is initialised.
 *
 * Parameters:
 *   a      - The AMBE connection, filled in.
 *   device - Path of the serial port.
 *   baud   - Baud rate: 115200, 230400, 460800 or 921600.
 *
 * Returns: true if the chip is ready, false if the port could not be opened
 *   or the chip did not answer or refused the rate.
 */
bool ambeOpen(Ambe* a, const char* device, unsigned int baud)
{
    struct termios tio;
    speed_t speed = baudConstant(baud);

    a->fd = -1;
    a->product[0] = '\0';

    if (speed == B0)
    {
        fprintf(stderr, "AMBE: unsupported baud rate %u (use 115200, 230400, 460800 or 921600)\n", baud);
        return false;
    }

    a->fd = open(device, O_RDWR | O_NOCTTY);
    if (a->fd < 0)
    {
        fprintf(stderr, "AMBE: cannot open %s: %s\n", device, strerror(errno));
        return false;
    }

    if (tcgetattr(a->fd, &tio) != 0)
    {
        fprintf(stderr, "AMBE: %s is not a serial port: %s\n", device, strerror(errno));
        close(a->fd);
        a->fd = -1;
        return false;
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS | HUPCL);
    if (ambeFlow)
        tio.c_cflag |= CRTSCTS;   /* HUPCL off: closing the port must not drop DTR */
    tio.c_cflag |= CS8;
    tio.c_cc[VMIN]  = 1;
    tio.c_cc[VTIME] = 0;
    tcsetattr(a->fd, TCSANOW, &tio);
    tcflush(a->fd, TCIOFLUSH);

    if (ambeHwReset != 0)
    {
        int bits = ((ambeHwReset & 1) ? TIOCM_DTR : 0) | ((ambeHwReset & 2) ? TIOCM_RTS : 0);

        ioctl(a->fd, TIOCMBIC, &bits);
        usleep(300000);
        ioctl(a->fd, TIOCMBIS, &bits);
        usleep(100000);
        tcflush(a->fd, TCIOFLUSH);
        if (ambeDebug)
            fprintf(stderr, "AMBE: pulsed %s%s%s low\n", (bits & TIOCM_DTR) ? "DTR" : "",
                    (bits & TIOCM_DTR) && (bits & TIOCM_RTS) ? " and " : "", (bits & TIOCM_RTS) ? "RTS" : "");
    }

    /*
     * Opening the port can reset the dongle (DTR), after which the chip needs a few seconds
     * and announces itself with READY.  Wait for that first, then reset it if it stayed silent.
     */
    {
        static const uint8_t reset[1] = { FIELD_RESET };
        uint8_t rx[PKT_MAX];
        size_t  rxLen;
        bool ready = false;

        for (unsigned int tries = 0U; tries < 4U && !ready; tries++)
        {
            int type = readPacket(a->fd, rx, &rxLen, 1000);

            if (type < 0)
                break;
            if (type == (int)TYPE_CONTROL && rxLen >= 1U && rx[0] == FIELD_READY)
                ready = true;
        }

        if (ambeDebug && ready)
            fprintf(stderr, "AMBE: the chip announced READY after the port was opened\n");

        for (unsigned int attempt = 0U; attempt < 4U && !ready; attempt++)
        {
            if (attempt > 0U)
            {
                /*
                 * The chip may be waiting for the rest of a packet that lost bytes on the way in.
                 * Zeros are ignored between packets, so enough of them complete and discard it.
                 */
                uint8_t zeros[16];

                memset(zeros, 0, sizeof(zeros));
                for (unsigned int k = 0U; k < 24U; k++)
                {
                    writeAll(a->fd, zeros, sizeof(zeros));
                    tcdrain(a->fd);
                    usleep(2000);
                }
                if (ambeDebug)
                    fprintf(stderr, "AMBE: sent filler to finish a possible partial packet\n");
            }

            sendPacket(a->fd, TYPE_CONTROL, reset, sizeof(reset));

            for (unsigned int tries = 0U; tries < 6U && !ready; tries++)
            {
                int type = readPacket(a->fd, rx, &rxLen, 2000);

                if (type < 0)
                    break;
                if (type == (int)TYPE_CONTROL && rxLen >= 1U && rx[0] == FIELD_READY)
                    ready = true;
            }
        }

        if (!ready)
        {
            fprintf(stderr, "AMBE: no READY from %s at %u baud. Check the port and try the other baud rate "
                            "(230400 or 460800). If it answered before, unplug the dongle and plug it in again.\n",
                    device, baud);
            close(a->fd);
            a->fd = -1;
            return false;
        }
    }

    /* Product id, for the log only */
    {
        static const uint8_t req[1] = { FIELD_PRODID };
        uint8_t rx[PKT_MAX];
        size_t  rxLen = 0U;

        if (control(a, req, sizeof(req), rx, &rxLen, 1000) && rxLen > 1U)
        {
            size_t n = rxLen - 1U;

            if (n >= sizeof(a->product))
                n = sizeof(a->product) - 1U;
            memcpy(a->product, rx + 1U, n);
            a->product[n] = '\0';
        }
    }

    /* Rate */
    {
        uint8_t req[13];
        uint8_t rx[PKT_MAX];
        size_t  rxLen = 0U;
        size_t  reqLen;

        if (a->rateIndex >= 0)
        {
            req[0] = FIELD_RATET;
            req[1] = (uint8_t)a->rateIndex;
            reqLen = 2U;
        }
        else
        {
            req[0] = FIELD_RATEP;
            for (unsigned int i = 0U; i < 6U; i++)
            {
                req[1U + i * 2U] = (uint8_t)(a->rate[i] >> 8);
                req[2U + i * 2U] = (uint8_t)a->rate[i];
            }
            reqLen = 13U;
        }

        if (!control(a, req, reqLen, rx, &rxLen, 1000))
        {
            fprintf(stderr, "AMBE: no answer to the rate setting\n");
            ambeClose(a);
            return false;
        }

        if (rxLen >= 2U && rx[1] != 0x00U)
        {
            fprintf(stderr, "AMBE: the chip refused the rate setting (status 0x%02X)\n", rx[1]);
            ambeClose(a);
            return false;
        }
    }

    /* Start the vocoder afresh */
    if (ambeInit != 0)
    {
        uint8_t req[2] = { FIELD_INIT, (uint8_t)ambeInit };
        uint8_t rx[PKT_MAX];
        size_t  rxLen = 0U;

        if (!control(a, req, sizeof(req), rx, &rxLen, 2000))
            fprintf(stderr, "AMBE: no answer to the initialise packet, carrying on\n");
        else if (rxLen >= 2U && rx[1] != 0x00U)
            fprintf(stderr, "AMBE: initialise returned status 0x%02X, carrying on\n", rx[1]);
    }

    return true;
}

/*
 * ambeClose()
 *
 * Closes the serial port of the chip.
 *
 * Parameters:
 *   a - The AMBE connection.
 *
 * Returns: nothing.
 */
void ambeClose(Ambe* a)
{
    if (a->fd >= 0)
        close(a->fd);
    a->fd = -1;
}

/*
 * ambeEncode()
 *
 * Encodes one frame of speech to a channel block.  The samples are sent to
 * the chip as a speech packet and the channel data packet that comes back is
 * checked to be the expected number of bits.
 *
 * Parameters:
 *   a     - The AMBE connection.
 *   pcm   - The AMBE_SAMPLES speech samples (16 bit).
 *   block - Receives the channel block (AMBE_BLOCK_BYTES).
 *
 * Returns: true if a block was returned, false on a timeout or an unexpected
 *   answer.
 */
bool ambeEncode(Ambe* a, const int16_t* pcm, uint8_t* block)
{
    uint8_t tx[2U + AMBE_SAMPLES * 2U];
    uint8_t rx[PKT_MAX];
    size_t  rxLen;

    tx[0] = FIELD_SPEECHD;
    tx[1] = (uint8_t)AMBE_SAMPLES;
    for (unsigned int i = 0U; i < AMBE_SAMPLES; i++)
    {
        tx[2U + i * 2U] = (uint8_t)((uint16_t)pcm[i] >> 8);
        tx[3U + i * 2U] = (uint8_t)pcm[i];
    }

    if (!sendPacket(a->fd, TYPE_SPEECH, tx, sizeof(tx)))
        return false;

    for (unsigned int tries = 0U; tries < 8U; tries++)
    {
        int type = readPacket(a->fd, rx, &rxLen, 3000);

        if (type < 0)
        {
            fprintf(stderr, "AMBE: no channel data came back for a speech frame (run with --ambe-debug to see "
                            "what the chip sends)\n");
            return false;
        }

        if (type == (int)TYPE_CHANNEL)
        {
            size_t pos = 0U;

            while (pos + 2U <= rxLen)
            {
                if (rx[pos] == FIELD_CHANNEL || rx[pos] == FIELD_CHANNEL0)
                {
                    unsigned int bits = rx[pos + 1U];
                    size_t bytes = (bits + 7U) / 8U;

                    if (bits != AMBE_BITS || pos + 2U + bytes > rxLen)
                    {
                        fprintf(stderr, "AMBE: the chip returned %u bits per frame, expected %u. "
                                        "The rate setting is not the 3600 bit/s rate with FEC.\n", bits, AMBE_BITS);
                        return false;
                    }
                    memcpy(block, rx + pos + 2U, AMBE_BLOCK_BYTES);
                    return true;
                }
                break;
            }
        }
    }
    return false;
}

/*
 * ambeDecode()
 *
 * Decodes one channel block to a frame of speech.  The block is sent to the
 * chip as a channel packet and the speech packet that comes back is read.
 *
 * Parameters:
 *   a     - The AMBE connection.
 *   block - The channel block (AMBE_BLOCK_BYTES).
 *   pcm   - Receives the AMBE_SAMPLES speech samples (16 bit).
 *
 * Returns: true if speech was returned, false on a timeout or an unexpected
 *   answer.
 */
bool ambeDecode(Ambe* a, const uint8_t* block, int16_t* pcm)
{
    uint8_t tx[2U + AMBE_BLOCK_BYTES];
    uint8_t rx[PKT_MAX];
    size_t  rxLen;

    tx[0] = FIELD_CHANNEL;
    tx[1] = (uint8_t)AMBE_BITS;
    memcpy(tx + 2U, block, AMBE_BLOCK_BYTES);

    if (!sendPacket(a->fd, TYPE_CHANNEL, tx, sizeof(tx)))
        return false;

    for (unsigned int tries = 0U; tries < 8U; tries++)
    {
        int type = readPacket(a->fd, rx, &rxLen, 1000);

        if (type < 0)
        {
            fprintf(stderr, "AMBE: no answer to a channel frame\n");
            return false;
        }

        if (type == (int)TYPE_SPEECH && rxLen >= 2U + AMBE_SAMPLES * 2U && rx[0] == FIELD_SPEECHD)
        {
            for (unsigned int i = 0U; i < AMBE_SAMPLES; i++)
                pcm[i] = (int16_t)(((uint16_t)rx[2U + i * 2U] << 8) | rx[3U + i * 2U]);
            return true;
        }
    }
    return false;
}
