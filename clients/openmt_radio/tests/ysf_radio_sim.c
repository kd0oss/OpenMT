/* Plays the modem host and the gateway for the repo's ysf_host, like protocols/YSF/test/ysf_sim.c, but the radio is the
 * OpenMT Radio program: its transmission (samples.raw, from ysf_interop_tool capture) is delivered to ysf_host as the
 * modem delivers it, and the messages that come out of the gateway port are checked: callsigns, and the speech inside.
 *
 * Usage: ysf_radio_sim <samples.raw> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ysf_func.h"
#include "sim_common.h"

#define VOICE_FRAMES 10U
#define BODY         YSF_NET_BODY_LENGTH

static SimConn modemC, gwC;
static uint8_t gwBody[128][YSF_NET_BODY_LENGTH];
static int     gwCount = 0, gwOther = 0;

static void grabGw(const SimPacket* p, void* arg)
{
    (void)arg;

    if (p->len == 8 + 1 + (int)BODY && memcmp(p->data + 4, "YSFG", 4) == 0 && gwCount < 128)
        memcpy(gwBody[gwCount++], p->data + 9, BODY);
    else
        gwOther++;
}

static const uint8_t WHITE[13] = { 0x93U, 0xD7U, 0x51U, 0x21U, 0x9CU, 0x2FU, 0x6CU, 0xD0U, 0xEFU, 0x0FU, 0xF8U, 0x3DU, 0xF1U };

/* the five 49 bit blocks of a V/D mode 2 frame (the first 27 bits are sent three times: majority vote) */
static void extract(const uint8_t* f, uint8_t* blocks)
{
    memset(blocks, 0, 35);
    for (unsigned int j = 0U; j < 5U; j++)
    {
        unsigned int offset = 40U + 144U * j;
        uint8_t vch[13] = {0};
        uint8_t* u = blocks + 7U * j;

        for (unsigned int i = 0U; i < 104U; i++)
        {
            unsigned int p = offset + i / 26U + 4U * (i % 26U);

            if ((f[30 + (p >> 3)] >> (7 - (p & 7))) & 1)
                vch[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        }
        for (unsigned int i = 0U; i < 13U; i++)
            vch[i] ^= WHITE[i];
        for (unsigned int i = 0U; i < 27U; i++)
        {
            unsigned int v = 0U;

            for (unsigned int k = 0U; k < 3U; k++)
                v += (vch[(3U * i + k) >> 3] >> (7 - ((3U * i + k) & 7))) & 1U;
            if (v >= 2U)
                u[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        }
        for (unsigned int i = 0U; i < 22U; i++)
            if ((vch[(81U + i) >> 3] >> (7 - ((81U + i) & 7))) & 1)
                u[(27U + i) >> 3] |= (uint8_t)(0x80 >> ((27U + i) & 7U));
    }
}

/* the speech the radio was given: block b of the call (the 49 bits ysf_interop_tool made) */
static void want(unsigned int b, uint8_t* net)
{
    for (unsigned int k = 0U; k < 7U; k++)
        net[k] = (uint8_t)(0x11U * (k + 1U) + 37U * b);
    net[6] &= 0x80U;
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: ysf_radio_sim <samples.raw>\n");
        return 2;
    }

    FILE* f = fopen(argv[1], "rb");

    if (!f)
    {
        printf("ysf_radio_sim: cannot read the radio's transmission\n");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* raw = malloc((size_t)bytes + 400);
    memset(raw, 0, (size_t)bytes + 400);
    if (fread(raw, 1, (size_t)bytes, f) != (size_t)bytes)
        return 2;
    fclose(f);

    int lfd = simListen(18000);
    printf("ysf_radio_sim: waiting for ysf_host on port 18000...\n");
    int mfd = simAccept(lfd, 30000);
    if (mfd < 0)
    {
        printf("ysf_radio_sim: host did not connect\n");
        return 2;
    }
    simConnInit(&modemC, mfd);

    SimPacket reg;
    if (simRead(&modemC, &reg, 10000) != 1 || memcmp(reg.data + 8, "YSF", 3) != 0)
    {
        printf("ysf_radio_sim: no registration for YSF\n");
        return 2;
    }

    int gfd = simConnect(18500, 50);
    if (gfd < 0)
    {
        printf("ysf_radio_sim: could not connect to the gateway port\n");
        return 2;
    }
    simConnInit(&gwC, gfd);

    uint8_t conn[16] = {'0', '0', '1', '2', '3'};
    simSend(&gwC, "CONN", conn, 16);
    usleep(1500000);
    simDrain(&modemC, 300, 1000, NULL, NULL);

    printf("sending %ld samples\n", bytes / 2);
    for (long at = 0; at < bytes; at += 192)
    {
        uint8_t pkt[200] = {0x61, 0x00, 0xC8, 0x04, 'S', 'A', 'M', 'P'};

        memcpy(pkt + 8, raw + at, 192);
        simSendRaw(&modemC, pkt, 200);
        simDrain(&gwC, 1, 1, grabGw, NULL);
        usleep(4000);
    }
    for (int i = 0; i < 100; i++)
    {
        uint8_t pkt[200] = {0x61, 0x00, 0xC8, 0x04, 'S', 'A', 'M', 'P'};

        simSendRaw(&modemC, pkt, 200);
        simDrain(&gwC, 1, 1, grabGw, NULL);
        usleep(4000);
    }
    simDrain(&gwC, 4000, 30000, grabGw, NULL);

    printf("gateway got %d messages (%d other packets)\n", gwCount, gwOther);

    int voice = 0, callsOk = 1, right = 0, total = 0;

    for (int i = 0; i < gwCount; i++)
    {
        const uint8_t* fr = gwBody[i] + 31U;
        YsfFich fich;

        if (memcmp(gwBody[i] + 10, "KD0OSS    ", 10) != 0 || memcmp(gwBody[i] + 20, "ALL       ", 10) != 0)
        {
            callsOk = 0;
            printf("  message %d: source '%.10s' destination '%.10s'\n", i, gwBody[i] + 10, gwBody[i] + 20);
        }

        if (!ysfFichDecode(&fich, fr) || ysfFichGetFI(&fich) != YSF_FI_COMMUNICATIONS)
            continue;

        uint8_t got[35], exp[7];

        extract(fr, got);
        for (unsigned int b = 0U; b < 5U; b++)
        {
            want((unsigned int)voice * 5U + b, exp);
            total++;
            if (memcmp(got + 7U * b, exp, 7) == 0)
                right++;
        }
        voice++;
    }

    simCheck(voice == (int)VOICE_FRAMES, "voice frames at the gateway: %d of %u sent", voice, VOICE_FRAMES);
    simCheck(callsOk, "every message carries source KD0OSS, destination ALL (the host names a group call so, as MMDVMHost does)");
    simCheck(total > 0 && right == total, "speech blocks identical to the ones sent: %d of %d", right, total);

    printf("\n%s (%d checks, %d failed)\n", simFails == 0 ? "PASS" : "FAIL", simChecks, simFails);
    return simFails == 0 ? 0 : 1;
}
