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
 * ysf_sim: plays the modem host and the YSF gateway for ysf_host.
 *
 *   1. listens on the modem host port and accepts the host's connection,
 *   2. connects to the host's gateway port and links (CONN),
 *   3. radio -> network: sends a synthetic call to the host as modem samples
 *      (BITS with --bits, MMDVM frames with --frame) and checks the messages that
 *      come out of the gateway port against a reference run of the control logic,
 *   4. network -> radio: sends the messages back at 100 ms intervals and checks
 *      that what the host transmits decodes to what the control logic gives,
 *   5. a data call, the mode hang time and the network watchdog.
 *
 * Usage: ysf_sim [-m modem 1-10] [--frame | --bits] [--noise rms] [--hang seconds]
 */

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <ADF7021.h>
#include <dsp_tools.h>

#include "ysf_dsp.h"
#include "ysf_func.h"

#define MAXF 300
#define FL   YSF_FRAME_LENGTH_BYTES
#define BODY YSF_NET_BODY_LENGTH

static int modemFd = -1;
static int gwFd = -1;
static bool frameMode = false;
static bool bitMode = false;
static double noiseRms = 0.0;
static unsigned int hangSeconds = 3U;

static int fails = 0;

#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } else printf("  ok:   %s\n", msg); } while (0)

/* ---- call generators ---------------------------------------------------------------- */

static const uint8_t WHITE[] = {0x93U, 0xD7U, 0x51U, 0x21U, 0x9CU, 0x2FU, 0x6CU, 0xD0U, 0xEFU, 0x0FU, 0xF8U, 0x3DU, 0xF1U};

static void pad(uint8_t* d, const char* s)
{
    memset(d, ' ', 10);
    memcpy(d, s, strlen(s));
}

static void mkHeader(uint8_t* f, uint8_t fi, uint8_t cm, uint8_t dt, const char* dest, const char* src)
{
    uint8_t csd1[20], csd2[20];
    YsfFich h;

    memset(f, 0, FL);
    ysfAddSync(f);
    ysfFichInit(&h);
    ysfFichSetFI(&h, fi);
    ysfFichSetCM(&h, cm);
    ysfFichSetFT(&h, 7);
    ysfFichSetDT(&h, dt);
    ysfFichEncode(&h, f);
    pad(csd1, dest);
    pad(csd1 + 10, src);
    pad(csd2, "");
    pad(csd2 + 10, "");
    ysfPayloadWriteHeader(f, csd1, csd2);
}

static void mkShort(uint8_t* frame, const uint8_t* ten)
{
    uint8_t out[13] = {0}, conv[25], bytes[25];

    memset(bytes, 0, sizeof(bytes));
    for (int i = 0; i < 10; i++) out[i] = ten[i] ^ WHITE[i];
    CRC_addCCITT162(out, 12);
    out[12] = 0;
    ysfConvEncode(out, conv, 100);

    unsigned j = 0;
    for (unsigned i = 0; i < 100; i++)
    {
        unsigned n = 2 * (i / 5) + 40 * (i % 5);
        int s0 = (conv[j >> 3] >> (7 - (j & 7))) & 1; j++;
        int s1 = (conv[j >> 3] >> (7 - (j & 7))) & 1; j++;
        if (s0) bytes[n >> 3] |= (uint8_t)(0x80 >> (n & 7));
        n++;
        if (s1) bytes[n >> 3] |= (uint8_t)(0x80 >> (n & 7));
    }
    for (int i = 0; i < 5; i++) memcpy(frame + 30 + i * 18, bytes + i * 5, 5);
}

/* A V/D mode 2 frame; the callsigns travel in frames 0 and 1 */
static void mkVd2(uint8_t* f, uint8_t fn, const char* dest, const char* src)
{
    YsfFich h;
    uint8_t ten[10];

    for (int i = 0; i < 120; i++) f[i] = (uint8_t)rand();
    ysfAddSync(f);
    ysfFichInit(&h);
    ysfFichSetFI(&h, YSF_FI_COMMUNICATIONS);
    ysfFichSetCM(&h, 3);
    ysfFichSetFN(&h, fn);
    ysfFichSetFT(&h, 6);
    ysfFichSetDT(&h, YSF_DT_VD_MODE2);
    ysfFichEncode(&h, f);

    memset(ten, ' ', 10);
    if (fn == 0) pad(ten, dest);
    else if (fn == 1) pad(ten, src);
    mkShort(f, ten);

    for (int j = 0; j < 5; j++)
    {
        unsigned offset = 40 + 144 * j;
        uint8_t vch[13] = {0};

        for (int i = 0; i < 81; i += 3)
        {
            int b = rand() & 1;
            for (int k = 0; k < 3; k++)
                if (b) vch[(i + k) >> 3] |= (uint8_t)(0x80 >> ((i + k) & 7));
        }
        for (int i = 0; i < 13; i++) vch[i] ^= WHITE[i];
        for (int i = 0; i < 104; i++)
        {
            unsigned n = (i / 26) + 4 * (i % 26);
            unsigned p = offset + n;
            int s = (vch[i >> 3] >> (7 - (i & 7))) & 1;
            if (s) f[30 + (p >> 3)] |= (uint8_t)(0x80 >> (p & 7)); else f[30 + (p >> 3)] &= (uint8_t)~(0x80 >> (p & 7));
        }
    }
}

static unsigned int genVoiceCall(uint8_t frames[][FL], const char* dest, const char* src, unsigned int n)
{
    unsigned int k = 0;

    mkHeader(frames[k++], YSF_FI_HEADER, 3, YSF_DT_VD_MODE2, dest, src);
    for (unsigned int i = 0; i < n; i++, k++)
        mkVd2(frames[k], (uint8_t)(i % 7), dest, src);
    mkHeader(frames[k++], YSF_FI_TERMINATOR, 3, YSF_DT_VD_MODE2, dest, src);

    return k;
}

/* A data call: header, frames of the data FR mode with valid channels, terminator */
static unsigned int genDataCall(uint8_t frames[][FL], const char* dest, const char* src, unsigned int n)
{
    unsigned int k = 0;

    mkHeader(frames[k++], YSF_FI_HEADER, 3, YSF_DT_DATA_FR_MODE, dest, src);
    for (unsigned int i = 0; i < n; i++, k++)
    {
        uint8_t csd1[20], csd2[20];
        YsfFich h;

        for (int b = 0; b < 20; b++) { csd1[b] = (uint8_t)rand(); csd2[b] = (uint8_t)rand(); }
        if (i % 8 == 0) { pad(csd1, dest); pad(csd1 + 10, src); }
        memset(frames[k], 0, FL);
        ysfAddSync(frames[k]);
        ysfFichInit(&h);
        ysfFichSetFI(&h, YSF_FI_COMMUNICATIONS);
        ysfFichSetCM(&h, 3);
        ysfFichSetFN(&h, (uint8_t)((i + 1) % 8));
        ysfFichSetFT(&h, 7);
        ysfFichSetDT(&h, YSF_DT_DATA_FR_MODE);
        ysfFichEncode(&h, frames[k]);
        ysfPayloadWriteHeader(frames[k], csd1, csd2);
    }
    mkHeader(frames[k++], YSF_FI_TERMINATOR, 3, YSF_DT_DATA_FR_MODE, dest, src);

    return k;
}

typedef struct
{
    uint8_t data[260];
    int     len;
} Packet;

static Packet modemQ[4096];
static unsigned int modemHead = 0U, modemTail = 0U;
static Packet gwQ[1024];
static unsigned int gwHead = 0U, gwTail = 0U;

static uint8_t rxbuf[2][600];
static int rxlen[2] = {0, 0};

static uint64_t nowMs(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static double gauss(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0);
    double v = (rand() + 1.0) / (RAND_MAX + 2.0);

    return sqrt(-2.0 * log(u)) * cos(6.283185307 * v);
}

/*
 * pump()
 *   Reads whatever packets have arrived from the host on either socket and
 *   queues them, waiting up to timeoutMs for the first.
 */
static void pump(int timeoutMs)
{
    struct pollfd p[2];
    int fds[2] = { modemFd, gwFd };

    for (int i = 0; i < 2; i++)
    {
        p[i].fd = fds[i];
        p[i].events = POLLIN;
        p[i].revents = 0;
    }

    int r = poll(p, 2, timeoutMs);

    if (r <= 0)
        return;

    for (int i = 0; i < 2; i++)
    {
        if (!(p[i].revents & POLLIN))
            continue;

        ssize_t n = read(fds[i], rxbuf[i] + rxlen[i], sizeof(rxbuf[i]) - rxlen[i]);

        if (n <= 0)
            continue;

        rxlen[i] += (int)n;

        while (rxlen[i] >= 4)
        {
            if (rxbuf[i][0] != 0x61)
            {
                memmove(rxbuf[i], rxbuf[i] + 1, rxlen[i] - 1);
                rxlen[i]--;
                continue;
            }

            int len = (rxbuf[i][1] << 8) | rxbuf[i][2];

            if (len < 5 || len > 260)
            {
                memmove(rxbuf[i], rxbuf[i] + 1, rxlen[i] - 1);
                rxlen[i]--;
                continue;
            }

            if (rxlen[i] < len)
                break;

            if (i == 0)
            {
                memcpy(modemQ[modemHead % 4096].data, rxbuf[i], len);
                modemQ[modemHead % 4096].len = len;
                modemHead++;
            }
            else
            {
                memcpy(gwQ[gwHead % 1024].data, rxbuf[i], len);
                gwQ[gwHead % 1024].len = len;
                gwHead++;
            }

            memmove(rxbuf[i], rxbuf[i] + len, rxlen[i] - len);
            rxlen[i] -= len;
        }
    }
}

static void sendAll(int fd, const uint8_t* data, size_t len)
{
    size_t done = 0U;

    while (done < len)
    {
        ssize_t n = write(fd, data + done, len - done);

        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n <= 0)
            return;

        done += (size_t)n;
    }
}

/* Tracks the modem host's mode like the real one does */
static char modeNow[10] = "IDLE";
static unsigned int setModeCount = 0U, setIdleCount = 0U;
static uint64_t idleAt = 0U;

static void writeModeFile(void)
{
    FILE* f = fopen("/tmp/ysf_stub_mode", "w");

    if (f != NULL)
    {
        fprintf(f, "%s\n", modeNow);
        fclose(f);
    }
}

/*
 * drainModem()
 *   Processes the packets from the host that are not transmit data: mode
 *   commands. Transmit packets stay in the queue for the caller.
 */
static void handleModemCommands(void)
{
    for (unsigned int i = modemTail; i != modemHead; i++)
    {
        Packet* p = &modemQ[i % 4096];

        if (p->len == 5 && p->data[3] == 0x01 && p->data[4] == 0x02)
        {
            if (strcmp(modeNow, "IDLE") == 0)
                strcpy(modeNow, "YSF");
            setModeCount++;
            writeModeFile();
            p->len = 0;
        }
        else if (p->len == 5 && p->data[3] == 0x01 && p->data[4] == 0x03)
        {
            strcpy(modeNow, "IDLE");
            setIdleCount++;
            idleAt = nowMs();
            writeModeFile();
            p->len = 0;
        }
    }
}


static void sendGwPacket(const char* type, const uint8_t* payload, unsigned int len)
{
    uint8_t pkt[200];

    pkt[0] = 0x61;
    pkt[1] = 0x00;
    pkt[2] = (uint8_t)(8 + len);
    pkt[3] = 0x04;
    memcpy(pkt + 4, type, 4);
    memcpy(pkt + 8, payload, len);
    sendAll(gwFd, pkt, 8 + len);
}

/* A message from the network: the end flag, then the 151 byte body */
static void sendNetBody(const uint8_t* body)
{
    uint8_t pl[1 + BODY];

    pl[0] = body[30] & 1U;
    memcpy(pl + 1, body, BODY);
    sendGwPacket("YSFG", pl, sizeof(pl));
}

/* Sends modem samples to the host as SAMP packets */
static void sendSamples(const int16_t* s, unsigned int count)
{
    uint8_t pkt[200];
    unsigned int pos = 0U;

    while (pos < count)
    {
        pkt[0] = 0x61;
        pkt[1] = 0x00;
        pkt[2] = 200;
        pkt[3] = 0x04;
        memcpy(pkt + 4, "SAMP", 4);

        for (unsigned int i = 0U; i < 96U; i++)
        {
            int16_t v = pos + i < count ? s[pos + i] : 0;

            pkt[8 + i * 2] = (uint8_t)(v & 0xFF);
            pkt[9 + i * 2] = (uint8_t)((v >> 8) & 0xFF);
        }

        sendAll(modemFd, pkt, 200);
        pos += 96U;

        if ((pos / 96U) % 8U == 0U)
        {
            usleep(1000);
            pump(0);
        }
    }
}

/* Plays a list of on air frames into the host */
static void radioTransmit(uint8_t frames[][FL], unsigned int n)
{
    if (frameMode)
    {
        for (unsigned int i = 0U; i < n; i++)
        {
            uint8_t pkt[8 + 1 + FL];

            pkt[0] = 0x61;
            pkt[1] = 0x00;
            pkt[2] = sizeof(pkt);
            pkt[3] = 0x04;
            memcpy(pkt + 4, "YSFD", 4);
            pkt[8] = 0x00;
            memcpy(pkt + 9, frames[i], FL);
            sendAll(modemFd, pkt, sizeof(pkt));
            usleep(2000);
            pump(0);
        }

        uint8_t lost[8] = {0x61, 0x00, 0x08, 0x04, 'Y', 'S', 'F', 'L'};
        sendAll(modemFd, lost, sizeof(lost));
        return;
    }

    if (bitMode)
    {
        /* what the ADF7021 streams: noise, then the preamble and the frames, 3 bits off a byte boundary */
        static uint8_t stream[MAXF * FL + 4000];
        unsigned int len = 0U;
        static uint8_t shifted[sizeof(stream) + 8];
        unsigned int bits;

        for (unsigned int i = 0U; i < 40U; i++)
            stream[len++] = (uint8_t)(rand() & 0xFF);       /* noise before the signal */

        for (unsigned int i = 0U; i < 60U; i++)
            stream[len++] = 0x77U;                          /* preamble */

        for (unsigned int i = 0U; i < n; i++)
        {
            memcpy(stream + len, frames[i], FL);
            len += FL;
        }

        for (unsigned int i = 0U; i < 3U * FL + 20U; i++)
            stream[len++] = 0x00U;                          /* nothing after the signal */

        memset(shifted, 0x00, sizeof(shifted));
        bits = len * 8U;
        for (unsigned int b = 0U; b < bits; b++)
            if (stream[b >> 3] & (0x80U >> (b & 7U)))
                shifted[(b + 3U) >> 3] |= (uint8_t)(0x80U >> ((b + 3U) & 7U));
        len++;

        for (unsigned int pos = 0U; pos < len; pos += 192U)
        {
            uint8_t pkt[200];

            pkt[0] = 0x61;
            pkt[1] = 0x00;
            pkt[2] = 200;
            pkt[3] = 0x04;
            memcpy(pkt + 4, "BITS", 4);
            memset(pkt + 8, 0x00, 192);
            memcpy(pkt + 8, shifted + pos, len - pos < 192U ? len - pos : 192U);
            sendAll(modemFd, pkt, 200);
            usleep(3000);
            pump(0);
        }

        return;
    }

    static YsfTx tx;
    static int16_t samples[MAXF * YSF_SAMPLES_PER_FRAME + 60000];
    unsigned int pos = 0U;

    ysfTxInit(&tx, YSF_LEVEL_OUTER_H, YSF_SHAPE_RRC);

    /* 200 ms of quiet, preamble, frames, tail, 600 ms of quiet */
    for (unsigned int i = 0U; i < 4800U; i++)
        samples[pos++] = 0;

    pos += ysfTxPreamble(&tx, 80U, samples + pos);

    for (unsigned int i = 0U; i < n; i++)
        pos += ysfTxFrame(&tx, frames[i], samples + pos);

    pos += ysfTxFlush(&tx, samples + pos);

    for (unsigned int i = 0U; i < 14400U; i++)
        samples[pos++] = 0;

    if (noiseRms > 0.0)
        for (unsigned int i = 0U; i < pos; i++)
        {
            double v = samples[i] + noiseRms * gauss();
            samples[i] = (int16_t)(v > 32767.0 ? 32767 : (v < -32768.0 ? -32768 : lrint(v)));
        }

    /* the host applies rxLevel 50 (x 0.195); send what the modem would send */
    for (unsigned int i = 0U; i < pos; i++)
    {
        double v = samples[i] / 0.1953125;
        samples[i] = (int16_t)(v > 32767.0 ? 32767 : (v < -32768.0 ? -32768 : lrint(v)));
    }

    sendSamples(samples, pos);
}

/* Messages that came out of the gateway socket (the 151 byte bodies) */
static uint8_t netMsg[MAXF][BODY];
static unsigned int netCount = 0U;

static void collectGateway(void)
{
    while (gwTail != gwHead)
    {
        Packet* p = &gwQ[gwTail % 1024];

        gwTail++;

        if (p->len == 8 + 1 + BODY && memcmp(p->data + 4, "YSFG", 4) == 0 && netCount < MAXF)
            memcpy(netMsg[netCount++], p->data + 9, BODY);
    }
}

/* Waits until the gateway has gone quiet for quietMs */
static void waitGatewayQuiet(unsigned int quietMs, unsigned int maxMs)
{
    uint64_t start = nowMs();
    uint64_t lastRx = nowMs();
    unsigned int before = netCount;

    while (nowMs() - start < maxMs && nowMs() - lastRx < quietMs)
    {
        pump(50);
        handleModemCommands();
        collectGateway();

        if (netCount != before)
        {
            before = netCount;
            lastRx = nowMs();
        }
    }
}

typedef struct
{
    uint8_t frames[MAXF][FL];
    unsigned int count;
    bool eot;
} TxCapture;

static void captureFrameCb(void* ctx, const uint8_t* f, bool lost)
{
    TxCapture* c = ctx;

    if (!lost && c->count < MAXF)
        memcpy(c->frames[c->count++], f, FL);
}

/*
 * collectTransmit()
 *   Gathers what the host transmitted: decodes the samples or the bits with
 *   the receivers, or reads the frames of the MMDVM path, until the host has
 *   been quiet.
 */
static void collectTransmit(TxCapture* c, unsigned int quietMs, unsigned int maxMs)
{
    static YsfRx rx;
    static YsfBitRx brx;
    unsigned int bitPackets = 0U;
    unsigned int bitBytes = 0U;
    uint64_t start = nowMs();
    uint64_t lastRx = nowMs();

    ysfRxInit(&rx, captureFrameCb, c, YSF_SHAPE_RRC);
    ysfBitRxInit(&brx, captureFrameCb, c);
    c->count = 0U;
    c->eot = false;

    while (nowMs() - start < maxMs && nowMs() - lastRx < quietMs)
    {
        pump(50);
        handleModemCommands();

        while (modemTail != modemHead)
        {
            Packet* p = &modemQ[modemTail % 4096];

            modemTail++;

            if (p->len == 0)
                continue;

            lastRx = nowMs();

            if (memcmp(p->data + 4, "SAMP", 4) == 0)
            {
                int16_t s[96];

                for (int i = 0; i < 96; i++)
                    s[i] = (int16_t)(p->data[8 + i * 2] | (p->data[9 + i * 2] << 8));

                ysfRxSamples(&rx, s, 96);
            }
            else if (memcmp(p->data + 4, "BITS", 4) == 0)
            {
                bitPackets++;
                bitBytes += 192U;
                ysfBitRxBytes(&brx, p->data + 8, 192U);
            }
            else if (memcmp(p->data + 4, "SEOT", 4) == 0 || memcmp(p->data + 4, "YSFL", 4) == 0)
            {
                c->eot = true;
            }
            else if (memcmp(p->data + 4, "YSFD", 4) == 0 && p->len >= 8 + 1 + (int)FL && c->count < MAXF)
            {
                memcpy(c->frames[c->count++], p->data + 9, FL);
            }
        }
        collectGateway();
    }

    if (bitMode)
    {
        /* no EOT packet in bit mode: the host stopped sending */
        c->eot = bitPackets > 0U;
        printf("  %u BITS packets (%u bytes) from the host\n", bitPackets, bitBytes);
    }
    else if (!frameMode)
    {
        /* push zeros so the receiver emits its last frame */
        int16_t z[96] = {0};

        for (int i = 0; i < 60; i++)
            ysfRxSamples(&rx, z, 96);
    }
}

static unsigned int countEqual(uint8_t a[][FL], unsigned int na, uint8_t b[][FL], unsigned int nb)
{
    unsigned int same = 0U;

    for (unsigned int i = 0U; i < na; i++)
        for (unsigned int j = 0U; j < nb; j++)
            if (memcmp(a[i], b[j], FL) == 0)
            {
                same++;
                break;
            }

    return same;
}

/* Reference: what the control logic gives for a set of frames */
static unsigned int refNetCount;
static uint8_t refNet[MAXF][BODY];

static void refNetCb(void* ctx, const uint8_t* body, bool end)
{
    (void)ctx;
    (void)end;

    if (refNetCount < MAXF)
        memcpy(refNet[refNetCount++], body, BODY);
}

static void reference(uint8_t frames[][FL], unsigned int n)
{
    YsfCtrl c;

    ysfCtrlInit(&c);
    ysfCtrlSetCallsign(&c, "N0CALL");
    c.sendNet = refNetCb;
    refNetCount = 0U;

    for (unsigned int i = 0U; i < n; i++)
        ysfCtrlRf(&c, frames[i], false);
}

/* Reference for the other direction: the frames the control logic transmits for a set of bodies */
static unsigned int refRfCount;
static uint8_t refRf[MAXF][FL];

static void refRfCb(void* ctx, const uint8_t* frame, bool eot)
{
    (void)ctx;
    (void)eot;

    if (refRfCount < MAXF)
        memcpy(refRf[refRfCount++], frame, FL);
}

static void referenceNet(uint8_t bodies[][BODY], unsigned int n)
{
    YsfCtrl c;

    ysfCtrlInit(&c);
    ysfCtrlSetCallsign(&c, "N0CALL");
    c.sendRf = refRfCb;
    refRfCount = 0U;

    for (unsigned int i = 0U; i < n; i++)
        ysfCtrlNet(&c, bodies[i]);
}

static uint8_t txCall[MAXF][FL];

int main(int argc, char** argv)
{
    int modem = 1;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
            modem = atoi(argv[++i]);
        else if (strcmp(argv[i], "--frame") == 0)
            frameMode = true;
        else if (strcmp(argv[i], "--bits") == 0)
            bitMode = true;
        else if (strcmp(argv[i], "--noise") == 0 && i + 1 < argc)
            noiseRms = atof(argv[++i]);
        else if (strcmp(argv[i], "--hang") == 0 && i + 1 < argc)
            hangSeconds = (unsigned int)atoi(argv[++i]);
        else
        {
            fprintf(stderr, "Usage: ysf_sim [-m modem] [--frame | --bits] [--noise rms] [--hang seconds]\n");
            return 2;
        }
    }

    unlink("/tmp/ysf_stub_mode");
    srand(12);

    /* 1. play the modem host */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    struct sockaddr_in a;

    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(18000 + modem - 1);

    if (bind(lfd, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(lfd, 1) < 0)
    {
        perror("modem port");
        return 2;
    }

    printf("ysf_sim: waiting for ysf_host on port %d (%s modem)...\n", 18000 + modem - 1,
           frameMode ? "MMDVM" : (bitMode ? "OpenMT_HS bit" : "sample"));

    struct pollfd pf = { lfd, POLLIN, 0 };

    if (poll(&pf, 1, 30000) <= 0)
    {
        printf("ysf_sim: host did not connect\n");
        return 2;
    }

    modemFd = accept(lfd, NULL, NULL);

    uint8_t reg[84];

    for (int got = 0; got < 84;)
    {
        ssize_t n = read(modemFd, reg + got, 84 - got);

        if (n <= 0)
        {
            printf("ysf_sim: host closed\n");
            return 2;
        }
        got += (int)n;
    }

    printf("registration: mode \"%.3s\" type \"%.4s\" txLevel %u\n", reg + 8, reg + 19, reg[25]);
    CHECK(memcmp(reg + 4, "MODE", 4) == 0 && memcmp(reg + 8, "YSF", 3) == 0, "MODE registration for YSF");

    if (bitMode)
    {
        uint32_t w[10];
        uint64_t sync[2];
        uint32_t reg4;
        uint32_t reg2;

        memcpy(w, reg + 26, 40);
        memcpy(sync, reg + 66, 16);
        reg2 = w[3];
        reg4 = w[5];

        printf("ADF7021: RX0 %08X TX0 %08X R1 %08X R2 %08X R3 %08X R4 %08X R5 %08X R6 %08X R10 %08X R13 %08X\n",
               w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9]);
        printf("sync %llX mask %llX flags %02X %02X\n", (unsigned long long)sync[0], (unsigned long long)sync[1], reg[82], reg[83]);

        CHECK(w[4] == 0x2A4CC093U, "REG3 is the YSF_H clock register (14.7456 MHz TCXO)");
        CHECK(((reg4 >> 4) & 7U) == 3U && ((reg4 >> 10) & 0x3FFU) == 516U && ((reg4 >> 20) & 0x3FFU) == 20U && (reg4 >> 30) == 2U,
              "REG4: 4FSK, discriminator 516, post demod 20, 25 kHz IF");
        CHECK(((reg2 >> 4) & 7U) == 7U && ((reg2 >> 28) & 3U) == 2U && ((reg2 >> 19) & 0x1FFU) == 32U,
              "REG2: RC 4FSK, data inverted, deviation 32 at 50 percent");
        CHECK((w[9] >> 4) == ADF7021_SLICER_TH_YSF_H, "REG13: slicer threshold for YSF_H");
        CHECK(w[8] == 0x049E472AU, "REG10: AFC off");
        CHECK(sync[0] == 0xD471C9634DULL && sync[1] == 0xFFFFFFFFFFULL, "frame sync word 0xD471C9634D, 40 bits");
        CHECK(((w[0] >> 27) & 0x1FU) == 0x09U && ((w[1] >> 27) & 0x1FU) == 0x08U && w[0] != w[1], "RX and TX frequency words (register 0)");
    }

    /* 2. play the gateway */
    a.sin_port = htons(18500 + modem - 1);

    for (int tries = 0; tries < 100; tries++)
    {
        gwFd = socket(AF_INET, SOCK_STREAM, 0);

        if (connect(gwFd, (struct sockaddr*)&a, sizeof(a)) == 0)
            break;

        close(gwFd);
        gwFd = -1;
        usleep(200000);
    }

    if (gwFd < 0)
    {
        printf("ysf_sim: could not connect to the gateway port\n");
        return 2;
    }

    uint8_t conn[16] = {'0', '0', '1', '2', '3'};
    sendGwPacket("CONN", conn, 16);
    usleep(500000);
    pump(100);
    handleModemCommands();

    /* 3. radio -> network */
    printf("\nTest 1: voice call, radio to network\n");
    unsigned int n = genVoiceCall(txCall, "CQCQCQ", "W1ABC", 20);
    reference(txCall, n);
    printf("  %u frames from the radio, reference control gives %u messages\n", n, refNetCount);

    netCount = 0U;
    setModeCount = 0U;
    radioTransmit(txCall, n);
    waitGatewayQuiet(1500, 20000);

    printf("  gateway received %u messages, SETMODE sent %u time(s)\n", netCount, setModeCount);
    CHECK(setModeCount >= 1U, "host asked for YSF mode");
    CHECK(netCount == refNetCount, "message count matches the reference");

    unsigned int same = 0U;
    for (unsigned int i = 0U; i < netCount && i < refNetCount; i++)
        if (memcmp(netMsg[i], refNet[i], BODY) == 0)
            same++;
    printf("  %u of %u messages identical to the reference\n", same, refNetCount);
    CHECK(noiseRms > 0.0 ? same >= refNetCount * 8U / 10U : same == refNetCount, "messages identical to the reference");
    CHECK(netCount > 0 && memcmp(netMsg[0] + 10, "W1ABC     ", 10) == 0 && memcmp(netMsg[0], "N0CALL    ", 10) == 0,
          "node and source callsigns in the message");
    CHECK(netCount > 0 && (netMsg[0][30] & 1U) == 0U && (netMsg[netCount - 1][30] & 1U) == 1U, "end flag on the last message only");

    /* 4. network -> radio */
    printf("\nTest 2: voice call, network to radio\n");
    unsigned int nm = netCount;
    static uint8_t keep[MAXF][BODY];

    memcpy(keep, netMsg, sizeof(keep));
    referenceNet(keep, nm);
    netCount = 0U;
    modemTail = modemHead;

    for (unsigned int i = 0U; i < nm; i++)
    {
        sendNetBody(keep[i]);
        usleep(100000);
        pump(0);
        handleModemCommands();
    }

    static TxCapture cap;
    collectTransmit(&cap, 1500, 15000);

    printf("  host transmitted %u frames (reference %u), end of transmission %s\n", cap.count, refRfCount, cap.eot ? "seen" : "NOT seen");
    CHECK(cap.eot, bitMode ? "transmission ended (host stopped sending bits)" : "transmission ended with an EOT packet");
    unsigned int eq = countEqual(refRf, refRfCount, cap.frames, cap.count);
    printf("  %u of %u frames recovered exactly\n", eq, refRfCount);
    CHECK(refRfCount > 0 && (eq == refRfCount || (noiseRms > 0.0 && eq >= refRfCount * 8U / 10U)),
          "transmitted frames decode to what the control gives");

    /* 5. data call */
    unsigned int idle0 = setIdleCount;

    printf("\nTest 3: data call, radio to network\n");
    n = genDataCall(txCall, "ALL", "W1ABC", 10);
    reference(txCall, n);
    netCount = 0U;
    radioTransmit(txCall, n);
    uint64_t tCall = nowMs();
    waitGatewayQuiet(1500, 20000);
    printf("  gateway received %u messages (reference %u)\n", netCount, refNetCount);
    CHECK(netCount == refNetCount && netCount > 0, "data call forwarded");
    same = 0U;
    for (unsigned int i = 0U; i < netCount && i < refNetCount; i++)
        if (memcmp(netMsg[i], refNet[i], BODY) == 0)
            same++;
    CHECK(noiseRms > 0.0 ? same >= refNetCount * 8U / 10U : same == refNetCount, "data messages identical to the reference");

    /* 6. mode hang: counted from the end of the data call */
    printf("\nTest 4: mode hang (%u s)\n", hangSeconds);
    uint64_t t0 = tCall;

    while (nowMs() - t0 < (uint64_t)(hangSeconds + 8U) * 1000U && setIdleCount == idle0)
    {
        pump(100);
        handleModemCommands();
    }
    printf("  SETIDLE after %.1f s\n", (idleAt - t0) / 1000.0);
    CHECK(setIdleCount > idle0, "host released the mode");
    CHECK(idleAt + 600U >= t0 + (uint64_t)hangSeconds * 1000U, "not before the hang time");
    CHECK(idleAt <= t0 + (uint64_t)(hangSeconds + 4U) * 1000U, "not much later than the hang time");

    /* 7. network watchdog: the reflector stops in the middle of a call */
    printf("\nTest 5: network call that stops without an end (watchdog)\n");
    modemTail = modemHead;
    for (unsigned int i = 0U; i < 6U && i < nm; i++)
    {
        sendNetBody(keep[i]);
        usleep(100000);
        pump(0);
        handleModemCommands();
    }
    collectTransmit(&cap, 2500, 15000);
    printf("  host transmitted %u frames, end of transmission %s\n", cap.count, cap.eot ? "seen" : "NOT seen");
    CHECK(cap.count >= 5U, "frames sent to the radio");
    CHECK(cap.eot, "host ended the transmission after the watchdog time");

    printf("\n%s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");

    return fails ? 1 : 0;
}
