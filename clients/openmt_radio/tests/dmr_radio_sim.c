/* Plays the modem host and the DMR gateway for the repo's dmr_host, like protocols/DMR/test/dmr_sim.c, but the radio is
 * the OpenMT Radio program: its transmission (samples.raw, from dmr_interop_tool capture) is delivered to dmr_host as the
 * modem delivers it, and the call that comes out of the gateway port is checked against the bursts that were sent.
 *
 * Usage: dmr_radio_sim <samples.raw> <bursts.txt> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sim_common.h"

#define SRC_ID 1234U
#define DST_ID 9U
#define MAX_B  400

static SimConn modemC, gwC;
static uint8_t gwCtl[MAX_B];
static uint8_t gwSlot[MAX_B];
static uint8_t gwBurst[MAX_B][33];
static uint8_t gwIds[MAX_B][7];
static int     gwCount = 0, gwOther = 0;

static uint8_t sent[MAX_B][33];
static int     sentCount = 0;

static void grabGw(const SimPacket* p, void* arg)
{
    (void)arg;

    if ((memcmp(p->data + 4, "DMD1", 4) == 0 || memcmp(p->data + 4, "DMD2", 4) == 0) && p->len == 49 && gwCount < MAX_B)
    {
        gwCtl[gwCount] = p->data[8];
        gwSlot[gwCount] = p->data[7] == '2' ? 2 : 1;
        memcpy(gwBurst[gwCount], p->data + 9, 33);
        memcpy(gwIds[gwCount], p->data + 42, 7);
        gwCount++;
    }
    else
        gwOther++;
}

/* the middle (sync, EMB, slot type) may be rewritten by the host, and the last symbol is uncertain */
static int sameBurst(const uint8_t* a, const uint8_t* b)
{
    return memcmp(a, b, 12) == 0 && memcmp(a + 22, b + 22, 10) == 0 && (a[32] & 0xFCU) == (b[32] & 0xFCU);
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: dmr_radio_sim <samples.raw> <bursts.txt>\n");
        return 2;
    }

    FILE* f = fopen(argv[2], "r");
    char line[200];

    while (f && fgets(line, sizeof(line), f) && sentCount < MAX_B)
    {
        for (int k = 0; k < 33; k++)
        {
            unsigned int v = 0;
            sscanf(line + k * 2, "%2x", &v);
            sent[sentCount][k] = (uint8_t)v;
        }
        sentCount++;
    }
    if (f)
        fclose(f);

    f = fopen(argv[1], "rb");
    if (!f || sentCount < 4)
    {
        printf("dmr_radio_sim: cannot read the radio's transmission\n");
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
    printf("dmr_radio_sim: waiting for dmr_host on port 18000...\n");
    int mfd = simAccept(lfd, 30000);
    if (mfd < 0)
    {
        printf("dmr_radio_sim: host did not connect\n");
        return 2;
    }
    simConnInit(&modemC, mfd);

    SimPacket reg;
    if (simRead(&modemC, &reg, 10000) != 1 || memcmp(reg.data + 8, "DMR", 3) != 0)
    {
        printf("dmr_radio_sim: no registration for DMR\n");
        return 2;
    }

    int gfd = simConnect(18400, 50);
    if (gfd < 0)
    {
        printf("dmr_radio_sim: could not connect to the gateway port\n");
        return 2;
    }
    simConnInit(&gwC, gfd);
    usleep(1500000);
    simDrain(&modemC, 300, 1000, NULL, NULL);

    int duplex = getenv("DMR_RADIO_SIM_DUPLEX") != NULL;

    if (duplex)
    {
        /* a repeater: the modem is duplex, the host sends its downlink while it receives */
        const uint8_t comm[1] = {0x00};

        simSend(&modemC, "COMM", comm, 1);
        usleep(300000);
        printf("the host is told the modem is duplex\n");
    }

    /* the radio transmits: 96 samples a packet, 4 ms apart as on the air */
    printf("sending %ld samples, %d bursts were sent by the radio\n", bytes / 2, sentCount);
    for (long at = 0; at < bytes; at += 192)
    {
        uint8_t pkt[200] = {0x61, 0x00, 0xC8, 0x04, 'S', 'A', 'M', 'P'};

        memcpy(pkt + 8, raw + at, 192);
        simSendRaw(&modemC, pkt, 200);
        simDrain(&gwC, 1, 1, grabGw, NULL);
        if (duplex)
            simDrain(&modemC, 1, 1, NULL, NULL);
        usleep(4000);
    }
    /* a little silence, as the modem keeps delivering */
    for (int i = 0; i < 100; i++)
    {
        uint8_t pkt[200] = {0x61, 0x00, 0xC8, 0x04, 'S', 'A', 'M', 'P'};

        simSendRaw(&modemC, pkt, 200);
        simDrain(&gwC, 1, 1, grabGw, NULL);
        usleep(4000);
    }
    simDrain(&gwC, 4000, 30000, grabGw, NULL);

    printf("gateway got %d bursts (%d other packets)\n  control bytes:", gwCount, gwOther);
    for (int i = 0; i < gwCount; i++)
        printf(" %02X", gwCtl[i]);
    printf("\n  time slots:");
    for (int i = 0; i < gwCount; i++)
        printf(" %d", gwSlot[i]);
    printf("\n");

    int header = -1, term = -1, voice = 0, voiceOk = 0, voiceSent = 0;

    for (int i = 0; i < sentCount; i++)
        if (i >= 2 && i < sentCount - 1)
            voiceSent++;

    for (int i = 0; i < gwCount; i++)
    {
        if ((gwCtl[i] & 0x4FU) == 0x41U && header < 0)
            header = i;
        else if ((gwCtl[i] & 0x4FU) == 0x42U)
            term = i;
        else if ((gwCtl[i] & 0x40U) == 0U)
        {
            voice++;
            for (int j = 2; j < sentCount - 1; j++)
                if (sameBurst(gwBurst[i], sent[j]))
                {
                    voiceOk++;
                    break;
                }
        }
    }

    simCheck(header >= 0, "the voice LC header reached the gateway");
    if (header >= 0)
    {
        unsigned int dst = ((unsigned int)gwIds[header][0] << 16) | ((unsigned int)gwIds[header][1] << 8) | gwIds[header][2];
        unsigned int src = ((unsigned int)gwIds[header][3] << 16) | ((unsigned int)gwIds[header][4] << 8) | gwIds[header][5];

        simCheck(src == SRC_ID && dst == DST_ID, "header ids: source %u destination %u (expected %u and %u)", src, dst,
                 SRC_ID, DST_ID);
        simCheck(gwIds[header][6] == 1, "the call is marked as a group call");
    }
    simCheck(voiceOk >= voiceSent - 1, "voice bursts that match one the radio sent: %d of %d (%d voice bursts in all)", voiceOk,
             voiceSent, voice);
    simCheck(voice <= voiceSent + 1, "no voice bursts added (%d)", voice);
    simCheck(term >= 0, "the terminator reached the gateway");

    printf("\n%s (%d checks, %d failed)\n", simFails == 0 ? "PASS" : "FAIL", simChecks, simFails);
    return simFails == 0 ? 0 : 1;
}
