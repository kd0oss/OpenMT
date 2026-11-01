/* Plays the modem host and the gateway for the repo's p25_host, like protocols/P25/test/p25_sim.c, but the radio is the
 * OpenMT Radio program: its transmission (samples.raw, from p25_interop_tool capture) is delivered to p25_host as the
 * modem delivers it, and the frames that come out of the gateway port are checked: ids, NAC, and the speech inside.
 *
 * Usage: p25_radio_sim <samples.raw> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "p25_func.h"
#include "sim_common.h"

#define SRC_ID 1234U
#define DST_ID 9U
#define NAC    0x293U
#define LDUS   6U

typedef struct
{
    uint8_t data[P25_LDU_FRAME_LENGTH_BYTES + 8];
    int     len;
    uint8_t duid;
} Frame;

static SimConn modemC, gwC;
static Frame   gwFrames[64];
static int     gwCount = 0, gwOther = 0;

static void grabGw(const SimPacket* p, void* arg)
{
    (void)arg;

    if (memcmp(p->data + 4, "P25", 3) == 0 && gwCount < 64 && p->len - 8 <= (int)P25_LDU_FRAME_LENGTH_BYTES)
    {
        Frame* f = &gwFrames[gwCount++];

        memcpy(f->data, p->data, (size_t)p->len);
        f->len = p->len;
        resetNID(NAC);
        decodeNID(f->data + 8U);
        f->duid = getDUID();
    }
    else
        gwOther++;
}

/* the speech the radio was given: frame b of the call (the stand in vocoder keeps the 11 bytes) */
static void want(unsigned int b, uint8_t* chip)
{
    for (unsigned int k = 0U; k < 11U; k++)
        chip[k] = (uint8_t)((b * 37U + k * 11U + 5U) & 0xFFU);
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: p25_radio_sim <samples.raw>\n");
        return 2;
    }

    FILE* f = fopen(argv[1], "rb");

    if (!f)
    {
        printf("p25_radio_sim: cannot read the radio's transmission\n");
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
    printf("p25_radio_sim: waiting for p25_host on port 18000...\n");
    int mfd = simAccept(lfd, 30000);
    if (mfd < 0)
    {
        printf("p25_radio_sim: host did not connect\n");
        return 2;
    }
    simConnInit(&modemC, mfd);

    SimPacket reg;
    if (simRead(&modemC, &reg, 10000) != 1 || memcmp(reg.data + 8, "P25", 3) != 0)
    {
        printf("p25_radio_sim: no registration for P25\n");
        return 2;
    }

    int gfd = simConnect(18300, 50);
    if (gfd < 0)
    {
        printf("p25_radio_sim: could not connect to the gateway port\n");
        return 2;
    }
    simConnInit(&gwC, gfd);

    uint8_t conn[6] = {'P', '2', '5', 'R', 'E', 'F'};
    simSend(&gwC, "CONN", conn, 6);
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

    int hdr = 0, ldu1 = 0, ldu2 = 0, term = 0, idsOk = 1, right = 0, total = 0;
    unsigned int index = 0U;

    printf("gateway got %d frames (%d other packets):", gwCount, gwOther);
    for (int i = 0; i < gwCount; i++)
        printf(" %02X", gwFrames[i].duid);
    printf("\n");

    for (int i = 0; i < gwCount; i++)
    {
        const uint8_t* fr = gwFrames[i].data + 8U;

        if (gwFrames[i].duid == P25_DUID_HEADER)
            hdr++;
        else if (gwFrames[i].duid == P25_DUID_TERM)
            term++;
        else if (gwFrames[i].duid == P25_DUID_LDU1 || gwFrames[i].duid == P25_DUID_LDU2)
        {
            if (gwFrames[i].duid == P25_DUID_LDU1)
            {
                ldu1++;
                if (!decodeLDU1(fr) || getSrcId() != SRC_ID || getDstId() != DST_ID || getLCF() != P25_LCF_GROUP)
                    idsOk = 0;
            }
            else
                ldu2++;

            for (unsigned int n = 0U; n < 9U; n++)
            {
                uint8_t got[11], exp[11];

                audip25decode(fr, got, n);
                want(index * 9U + n, exp);
                total++;
                if (memcmp(got, exp, 11) == 0)
                    right++;
            }
            index++;
        }
    }

    simCheck(ldu1 == (int)(LDUS / 2U) && ldu2 == (int)(LDUS / 2U), "voice frames: %d LDU1 and %d LDU2 of %u each sent", ldu1,
             ldu2, LDUS / 2U);
    simCheck(idsOk, "the LDU1 carries source %u, destination %u, group call", SRC_ID, DST_ID);
    simCheck(total > 0 && right == total, "speech frames identical to the ones sent: %d of %d", right, total);
    (void)hdr;
    (void)term;     /* p25_host takes the header and the terminator itself: the host log shows them, the gateway gets the LDUs */

    printf("\n%s (%d checks, %d failed)\n", simFails == 0 ? "PASS" : "FAIL", simChecks, simFails);
    return simFails == 0 ? 0 : 1;
}
