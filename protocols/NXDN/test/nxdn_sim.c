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
 * nxdn_sim: plays the modem host and the NXDN gateway for nxdn_host.
 *
 *   1. listens on the modem host port and accepts the host's connection,
 *   2. connects to the host's gateway port and links (CONN),
 *   3. radio -> network: sends a synthetic call to the host as modem samples
 *      (or MMDVM frames with --frame) and checks the NXDN messages that come
 *      out of the gateway port against a reference run of the control logic,
 *   4. network -> radio: sends the messages back at 80 ms intervals and
 *      checks that the samples (frames) the host transmits decode to the
 *      original frames,
 *   5. a data call, a noisy call and the mode hang time.
 *
 * Usage: nxdn_sim [-m modem 1-10] [--frame] [--noise rms] [--hang seconds]
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

#include "nxdn_dsp.h"
#include "nxdn_func.h"
#include "nxdn_gen.h"

#define MAXF 400
#define RAN 1

static int modemFd = -1;
static int gwFd = -1;
static bool frameMode = false;
static double noiseRms = 0.0;
static unsigned int hangSeconds = 3U;

static int fails = 0;

#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } else printf("  ok:   %s\n", msg); } while (0)

/* Packets received from the host */
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

static void writeModeFile(void)
{
    FILE* f = fopen("/tmp/nxdn_stub_mode", "w");

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
                strcpy(modeNow, "NXDN");
            setModeCount++;
            writeModeFile();
            p->len = 0;
        }
        else if (p->len == 5 && p->data[3] == 0x01 && p->data[4] == 0x03)
        {
            strcpy(modeNow, "IDLE");
            setIdleCount++;
            writeModeFile();
            p->len = 0;
        }
    }
}

static void sendGwPacket(const char* type, const uint8_t* payload, unsigned int len)
{
    uint8_t pkt[100];

    pkt[0] = 0x61;
    pkt[1] = 0x00;
    pkt[2] = 8 + len;
    pkt[3] = 0x04;
    memcpy(pkt + 4, type, 4);
    memcpy(pkt + 8, payload, len);
    sendAll(gwFd, pkt, 8 + len);
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
static void radioTransmit(uint8_t frames[][48], unsigned int n)
{
    if (frameMode)
    {
        for (unsigned int i = 0U; i < n; i++)
        {
            uint8_t pkt[8 + 1 + 48];

            pkt[0] = 0x61;
            pkt[1] = 0x00;
            pkt[2] = sizeof(pkt);
            pkt[3] = 0x04;
            memcpy(pkt + 4, "NXDD", 4);
            pkt[8] = 0x00;
            memcpy(pkt + 9, frames[i], 48);
            sendAll(modemFd, pkt, sizeof(pkt));
            usleep(2000);
            pump(0);
        }

        uint8_t lost[8] = {0x61, 0x00, 0x08, 0x04, 'N', 'X', 'D', 'L'};
        sendAll(modemFd, lost, sizeof(lost));
        return;
    }

    static NxdnTx tx;
    static int16_t samples[MAXF * 1920 + 40000];
    unsigned int pos = 0U;

    nxdnTxInit(&tx, NXDN_LEVEL_OUTER, NXDN_SHAPE_RRC);

    /* 200 ms of quiet, preamble, frames, tail, 600 ms of quiet */
    for (unsigned int i = 0U; i < 4800U; i++)
        samples[pos++] = 0;

    pos += nxdnTxPreamble(&tx, 40U, samples + pos);

    for (unsigned int i = 0U; i < n; i++)
        pos += nxdnTxFrame(&tx, frames[i], samples + pos);

    pos += nxdnTxFlush(&tx, samples + pos);

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

/* Messages that came out of the gateway socket */
static uint8_t netMsg[MAXF][41];
static unsigned int netCount = 0U;

static void collectGateway(void)
{
    while (gwTail != gwHead)
    {
        Packet* p = &gwQ[gwTail % 1024];

        gwTail++;

        if (p->len == 49 && memcmp(p->data + 4, "NXDN", 4) == 0 && netCount < MAXF)
            memcpy(netMsg[netCount++], p->data + 8, 41);
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
    uint8_t frames[MAXF][48];
    unsigned int count;
    bool eot;
    int16_t* samples;
    unsigned int nsamples;
    unsigned int cap;
} TxCapture;

static void captureFrameCb(void* ctx, const uint8_t* f, bool lost)
{
    TxCapture* c = ctx;

    if (!lost && c->count < MAXF)
        memcpy(c->frames[c->count++], f, 48);
}

/*
 * collectTransmit()
 *   Gathers what the host transmitted: decodes the samples with the receiver
 *   (or reads the frames of the MMDVM path) until the host has been quiet.
 */
static void collectTransmit(TxCapture* c, unsigned int quietMs, unsigned int maxMs)
{
    static NxdnRx rx;
    uint64_t start = nowMs();
    uint64_t lastRx = nowMs();

    nxdnRxInit(&rx, captureFrameCb, c, NXDN_SHAPE_RRC);
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

                nxdnRxSamples(&rx, s, 96);
            }
            else if (memcmp(p->data + 4, "SEOT", 4) == 0 || memcmp(p->data + 4, "NXDL", 4) == 0)
            {
                c->eot = true;
            }
            else if (memcmp(p->data + 4, "NXDD", 4) == 0 && p->len >= 57 && c->count < MAXF)
            {
                memcpy(c->frames[c->count++], p->data + 9, 48);
            }
        }
        collectGateway();
    }

    if (!frameMode)
    {
        /* push zeros so the receiver emits its last frame */
        int16_t z[96] = {0};

        for (int i = 0; i < 40; i++)
            nxdnRxSamples(&rx, z, 96);
    }
}

static unsigned int countEqual(uint8_t a[][48], unsigned int na, uint8_t b[][48], unsigned int nb)
{
    unsigned int same = 0U;

    for (unsigned int i = 0U; i < na; i++)
        for (unsigned int j = 0U; j < nb; j++)
            if (memcmp(a[i], b[j], 48) == 0)
            {
                same++;
                break;
            }

    return same;
}

/* Reference: what the control logic gives for a set of frames */
static unsigned int refNetCount;
static uint8_t refNet[MAXF][41];

static void refNetCb(void* ctx, const uint8_t* d, uint8_t t)
{
    if (refNetCount < MAXF)
    {
        refNet[refNetCount][0] = t;
        memcpy(refNet[refNetCount] + 1, d, 40);
        refNetCount++;
    }
}

static void reference(uint8_t frames[][48], unsigned int n)
{
    NxdnCtrl c;

    nxdnCtrlInit(&c);
    c.ran = RAN;
    c.sendNet = refNetCb;
    refNetCount = 0U;

    for (unsigned int i = 0U; i < n; i++)
        nxdnCtrlRf(&c, frames[i], false);
}

static uint8_t txCall[MAXF][48];

int main(int argc, char** argv)
{
    int modem = 1;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
            modem = atoi(argv[++i]);
        else if (strcmp(argv[i], "--frame") == 0)
            frameMode = true;
        else if (strcmp(argv[i], "--noise") == 0 && i + 1 < argc)
            noiseRms = atof(argv[++i]);
        else if (strcmp(argv[i], "--hang") == 0 && i + 1 < argc)
            hangSeconds = (unsigned int)atoi(argv[++i]);
        else
        {
            fprintf(stderr, "Usage: nxdn_sim [-m modem] [--frame] [--noise rms] [--hang seconds]\n");
            return 2;
        }
    }

    unlink("/tmp/nxdn_stub_mode");
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

    printf("nxdn_sim: waiting for nxdn_host on port %d (%s modem)...\n", 18000 + modem - 1, frameMode ? "MMDVM" : "sample");

    struct pollfd pf = { lfd, POLLIN, 0 };

    if (poll(&pf, 1, 30000) <= 0)
    {
        printf("nxdn_sim: host did not connect\n");
        return 2;
    }

    modemFd = accept(lfd, NULL, NULL);

    uint8_t reg[84];

    for (int got = 0; got < 84;)
    {
        ssize_t n = read(modemFd, reg + got, 84 - got);

        if (n <= 0)
        {
            printf("nxdn_sim: host closed\n");
            return 2;
        }
        got += (int)n;
    }

    printf("registration: mode \"%.4s\" type \"%.4s\" txLevel %u\n", reg + 8, reg + 19, reg[25]);
    CHECK(memcmp(reg + 4, "MODE", 4) == 0 && memcmp(reg + 8, "NXDN", 4) == 0, "MODE registration for NXDN");

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
        printf("nxdn_sim: could not connect to the gateway port\n");
        return 2;
    }

    uint8_t conn[12] = {'N', '0', 'C', 'A', 'L', 'L', 'T', 'G', '1', '2', '3', '4'};
    sendGwPacket("CONN", conn, 12);
    usleep(500000);
    pump(100);
    handleModemCommands();

    /* 3. radio -> network */
    printf("\nTest 1: voice call, radio to network\n");
    unsigned int n = nxdnGenVoiceCall(RAN, 1234, 9, true, 50, txCall, MAXF, 3);
    reference(txCall, n);
    printf("  %u frames from the radio, reference control gives %u messages\n", n, refNetCount);

    netCount = 0U;
    setModeCount = 0U;
    radioTransmit(txCall, n);
    waitGatewayQuiet(1500, 20000);

    printf("  gateway received %u messages, SETMODE sent %u time(s)\n", netCount, setModeCount);
    CHECK(setModeCount >= 1U, "host asked for NXDN mode");
    CHECK(netCount == refNetCount, "message count matches the reference");

    unsigned int same = 0U;
    for (unsigned int i = 0U; i < netCount && i < refNetCount; i++)
        if (memcmp(netMsg[i], refNet[i], 41) == 0)
            same++;
    printf("  %u of %u messages identical to the reference\n", same, refNetCount);
    CHECK(noiseRms > 0.0 ? same >= refNetCount * 8U / 10U : same == refNetCount, "messages identical to the reference");
    CHECK(netMsg[0][0] == NXDN_NET_VOICE_HEADER && netCount > 0 && netMsg[netCount - 1][0] == NXDN_NET_VOICE_TRAILER,
          "header first, trailer last");

    /* 4. network -> radio */
    printf("\nTest 2: voice call, network to radio\n");
    unsigned int nm = netCount;
    static uint8_t keep[MAXF][41];

    memcpy(keep, netMsg, sizeof(keep));
    netCount = 0U;
    modemTail = modemHead;

    for (unsigned int i = 0U; i < nm; i++)
    {
        sendGwPacket("NXDN", keep[i], 41);
        usleep(80000);
        pump(0);
        handleModemCommands();
    }

    static TxCapture cap;
    collectTransmit(&cap, 1500, 15000);

    printf("  host transmitted %u frames, end of transmission %s\n", cap.count, cap.eot ? "seen" : "NOT seen");
    CHECK(cap.eot, "transmission ended with an EOT packet");
    unsigned int eq = countEqual(txCall, n, cap.frames, cap.count);
    printf("  %u of %u original frames recovered exactly\n", eq, n);
    CHECK(eq == n || (noiseRms > 0.0 && eq >= n * 8U / 10U), "transmitted frames decode to the original frames");

    /* 5. data call */
    unsigned int idle0 = setIdleCount;

    printf("\nTest 3: data call, radio to network\n");
    n = nxdnGenDataCall(RAN, 1234, 77, false, 8, txCall, MAXF, 4);
    reference(txCall, n);
    netCount = 0U;
    radioTransmit(txCall, n);
    waitGatewayQuiet(1500, 20000);
    printf("  gateway received %u messages (reference %u)\n", netCount, refNetCount);
    CHECK(netCount == refNetCount && netCount > 0 && netMsg[0][0] == NXDN_NET_DATA_HEADER, "data call forwarded");

    /* 7. mode hang */
    printf("\nTest 4: mode hang (%u s)\n", hangSeconds);
    uint64_t t0 = nowMs();

    while (nowMs() - t0 < (uint64_t)(hangSeconds + 6U) * 1000U && setIdleCount == idle0)
    {
        pump(100);
        handleModemCommands();
    }
    printf("  SETIDLE after %.1f s\n", (nowMs() - t0) / 1000.0);
    CHECK(setIdleCount > idle0, "host released the mode");
    CHECK(nowMs() - t0 >= (uint64_t)(hangSeconds > 1U ? hangSeconds - 1U : 0U) * 1000U - 1500U, "not before the hang time");

    /* 6. wrong RAN is ignored */
    printf("\nTest 5: call with another RAN is ignored\n");
    n = nxdnGenVoiceCall(RAN + 5, 4321, 9, true, 20, txCall, MAXF, 3);
    netCount = 0U;
    radioTransmit(txCall, n);
    waitGatewayQuiet(1500, 20000);
    CHECK(netCount == 0U, "nothing forwarded");

    printf("\n%s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");

    return fails ? 1 : 0;
}
