/* Plays the modem host and the gateway for the repo's dstar_host, like protocols/DSTAR/test/dstar_sim.c, but the radio is
 * the OpenMT Radio program: its transmission (samples.raw, from dstar_interop_tool capture) is delivered to dstar_host as
 * the modem delivers it, and what comes out of the gateway port is checked: the header, the voice frames, the end.
 *
 * Usage: dstar_radio_sim <samples.raw> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dstar_fec.h"
#include "sim_common.h"

#define VOICE_BLOCKS 50U

static SimConn modemC, gwC;
static uint8_t gwHeader[49];
static int     gwHaveHeader = 0, gwEots = 0, gwOther = 0, gwDataCount = 0;
static uint8_t gwData[256][12];

static void grabGw(const SimPacket* p, void* arg)
{
    (void)arg;

    if (memcmp(p->data + 4, "DSTH", 4) == 0 && p->len == 49)
    {
        memcpy(gwHeader, p->data, 49);
        gwHaveHeader = 1;
    }
    else if (memcmp(p->data + 4, "DSTD", 4) == 0 && p->len == 20 && gwDataCount < 256)
        memcpy(gwData[gwDataCount++], p->data + 8, 12);
    else if (memcmp(p->data + 4, "DSTE", 4) == 0)
        gwEots++;
    else
        gwOther++;
}

/* block b of the call, as dstar_interop_tool.cpp makes it */
static void want(unsigned int b, uint8_t* ambe)
{
    unsigned int da = (37U * b + 5U) & 0xFFFU, db = (91U * b + 17U) & 0xFFFU, c = (0x5A5A5AU + 0x10101U * b) & 0xFFFFFFU;
    unsigned int a = golay24128_encode24128(da);
    unsigned int bb = golay24128_encode24128(db) ^ AMBE_PRNG_TABLE[da];
    unsigned int m = 0x800000U;

    memset(ambe, 0, 9);
    for (unsigned int i = 0U; i < 24U; i++, m >>= 1)
    {
        DSTAR_FEC_WRITE(ambe, DSTAR_FEC_A[i], a & m);
        DSTAR_FEC_WRITE(ambe, DSTAR_FEC_B[i], bb & m);
        DSTAR_FEC_WRITE(ambe, DSTAR_FEC_C[i], c & m);
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: dstar_radio_sim <samples.raw>\n");
        return 2;
    }

    FILE* f = fopen(argv[1], "rb");

    if (!f)
    {
        printf("dstar_radio_sim: cannot read the radio's transmission\n");
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
    printf("dstar_radio_sim: waiting for dstar_host on port 18000...\n");
    int mfd = simAccept(lfd, 30000);
    if (mfd < 0)
    {
        printf("dstar_radio_sim: host did not connect\n");
        return 2;
    }
    simConnInit(&modemC, mfd);

    SimPacket reg;
    if (simRead(&modemC, &reg, 10000) != 1 || memcmp(reg.data + 8, "DSTAR", 5) != 0)
    {
        printf("dstar_radio_sim: no registration for DSTAR\n");
        return 2;
    }

    int gfd = simConnect(18100, 50);
    if (gfd < 0)
    {
        printf("dstar_radio_sim: could not connect to the gateway port\n");
        return 2;
    }
    simConnInit(&gwC, gfd);
    usleep(1500000);
    simDrain(&modemC, 300, 1000, NULL, NULL);

    printf("sending %ld samples\n", bytes / 2);
    for (long at = 0; at < bytes; at += 192)
    {
        uint8_t pkt[200] = {0x61, 0x00, 0xC8, 0x04, 'S', 'A', 'M', 'P'};

        memcpy(pkt + 8, raw + at, (bytes - at) < 192 ? (size_t)(bytes - at) : 192U);
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

    printf("gateway got %s header, %d voice frames, %d end (%d other packets)\n", gwHaveHeader ? "a" : "no", gwDataCount,
           gwEots, gwOther);

    simCheck(gwHaveHeader, "a header reached the gateway");
    simCheck(gwHaveHeader && memcmp(gwHeader + 11, "N0CALL G", 8) == 0 && memcmp(gwHeader + 19, "N0CALL B", 8) == 0 &&
             memcmp(gwHeader + 27, "CQCQCQ  ", 8) == 0 && memcmp(gwHeader + 35, "KD0OSS  ", 8) == 0,
             "the header carries RPT2 N0CALL G, RPT1 N0CALL B, UR CQCQCQ, MY KD0OSS");

    int right = 0;

    for (int i = 0; i < gwDataCount && i < (int)VOICE_BLOCKS; i++)
    {
        uint8_t exp[9];

        want((unsigned int)i, exp);
        if (memcmp(gwData[i], exp, 9) == 0)
            right++;
    }

    simCheck(gwDataCount == (int)VOICE_BLOCKS, "voice frames at the gateway: %d of %u sent", gwDataCount, VOICE_BLOCKS);
    simCheck(right == gwDataCount && right > 0, "voice frames identical to the ones sent, in order: %d of %d", right, gwDataCount);
    simCheck(gwEots >= 1, "the end of the call reached the gateway");

    printf("\n%s (%d checks, %d failed)\n", simFails == 0 ? "PASS" : "FAIL", simChecks, simFails);
    return simFails == 0 ? 0 : 1;
}
