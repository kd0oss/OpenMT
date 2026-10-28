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
 * ambe_fake - a stand-in for an AMBE3000R on a pseudo terminal, to test the
 * packet handling of ambe3000.c without the dongle.  It is NOT a vocoder: the
 * "encoder" turns each group of three samples into one bit (positive sum or
 * not), codes the 49 bits with the Golay words into a 72 bit block, and the "decoder"
 * turns each bit back into +4000/-4000 samples.
 *
 *   ambe_fake [bits]       prints the port name; 'bits' is the channel size it
 *                          claims after a rate setting (default 49)
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "nxdn_func.h"

static int fd;
static uint8_t chField = 0x01;
static unsigned int layout = NXDN_CHIP_NOFEC;

static void readAll(uint8_t* p, size_t n)
{
    while (n > 0U)
    {
        ssize_t r = read(fd, p, n);
        if (r < 0)                  /* no slave open at the moment */
        {
            usleep(20000);
            continue;
        }
        if (r == 0)
            exit(0);
        p += r;
        n -= (size_t)r;
    }
}

static void send(uint8_t type, const uint8_t* payload, size_t len)
{
    uint8_t h[4] = { 0x61, (uint8_t)(len >> 8), (uint8_t)len, type };
    write(fd, h, 4);
    write(fd, payload, len);
}

int main(int argc, char** argv)
{
    unsigned int claim = argc > 1 ? (unsigned int)atoi(argv[1]) : 72U;

    if (argc > 2)
        chField = (uint8_t)strtoul(argv[2], NULL, 16);
    if (argc > 3)
        layout = !strcmp(argv[3], "air") ? NXDN_CHIP_AIR : (!strcmp(argv[3], "coding") ? NXDN_CHIP_CODING : NXDN_CHIP_NOFEC);

    fd = posix_openpt(O_RDWR | O_NOCTTY);
    grantpt(fd);
    unlockpt(fd);
    {
        struct termios t;
        tcgetattr(fd, &t);
        cfmakeraw(&t);
        tcsetattr(fd, TCSANOW, &t);
    }
    printf("%s\n", ptsname(fd));
    fflush(stdout);

    for (;;)
    {
        uint8_t b, h[3], p[1024];

        do { readAll(&b, 1U); } while (b != 0x61);
        readAll(h, 3U);
        size_t len = ((size_t)h[0] << 8) | h[1];
        readAll(p, len);

        if (h[2] == 0x00 && p[0] == 0x33)
        {
            const uint8_t r[1] = { 0x39 };
            send(0x00, r, 1U);
        }
        else if (h[2] == 0x00 && p[0] == 0x30)
        {
            const uint8_t r[10] = { 0x30, 'A', 'M', 'B', 'E', '3', '0', '0', '0', 'R' };
            send(0x00, r, sizeof(r));
        }
        else if (h[2] == 0x00 && (p[0] == 0x0A || p[0] == 0x09 || p[0] == 0x0B))
        {
            const uint8_t r[2] = { p[0], 0x00 };
            fprintf(stderr, "fake: rate %s", p[0] == 0x0A ? "words" : "index");
            for (size_t i = 1U; i < len; i++)
                fprintf(stderr, " %02X", p[i]);
            fprintf(stderr, "\n");
            send(0x00, r, 2U);
        }
        else if (h[2] == 0x02)
        {
            uint8_t r[2U + 9U] = { chField, (uint8_t)claim };
            uint8_t net[7] = { 0 };
            int16_t s[160];

            for (unsigned int i = 0U; i < 160U; i++)
                s[i] = (int16_t)(((uint16_t)p[2 + i * 2] << 8) | p[3 + i * 2]);
            for (unsigned int i = 0U; i < 49U; i++)
            {
                long sum = (long)s[i * 3U] + s[i * 3U + 1U] + s[i * 3U + 2U];
                if (sum > 0)
                    net[i >> 3] |= (uint8_t)(0x80U >> (i & 7U));
            }
            nxdnAudioChipEncode(layout, net, r + 2U);
            send(0x01, r, sizeof(r));
        }
        else if (h[2] == 0x01)
        {
            uint8_t r[2U + 320U] = { 0x00, 160 };
            uint8_t net[7] = { 0 };

            nxdnAudioChipDecode(layout, p + 2U, net);

            for (unsigned int i = 0U; i < 160U; i++)
            {
                int v = 0;
                if (i < 49U)
                    v = (net[i >> 3] & (0x80U >> (i & 7U))) ? 4000 : -4000;
                r[2U + i * 2U] = (uint8_t)((uint16_t)v >> 8);
                r[3U + i * 2U] = (uint8_t)v;
            }
            send(0x02, r, sizeof(r));
        }
    }
}
