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

/*
 * dstar_sim: plays the modem host and the D-STAR gateway for dstar_host.
 *
 *   1. listens on the modem host port and accepts the host's connection,
 *   2. connects to the host's gateway port,
 *   3. network -> radio: sends a call (header, voice frames, end) the way
 *      the gateway does and captures what the host sends to the modem,
 *   4. radio -> network: sends the captured transmission back to the host as
 *      what a modem of this type delivers (frames, samples or bits) and
 *      checks the header and voice frames that come out of the gateway port.
 *
 * Run once for each modem type, with the host started with the same type
 * (see run_sim.sh):
 *
 * Usage: dstar_sim [-m modem 1-10] [--modem mmdvm|openmt|openmths] [--gap ms]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "CCITTChecksumReverse.h"
#include "dstar_fec.h"
#include "sim_common.h"

#define DATA_FRAMES 42  /* two superframes of 21 voice frames, 20 ms each */
#define MAX_PACKETS 8000

enum { KIND_SAMP, KIND_BITS, KIND_FRAME };

static SimConn modemC;
static SimConn gwC;
static int     kind  = KIND_SAMP;
static int     gapMs = 20; /* time between the voice frames the gateway sends: 20 ms on air */

/* The station and the call, as the header carries them (8 characters each) */
static const char RPT2[] = "N0CALL G";
static const char RPT1[] = "N0CALL B";
static const char YOUR[] = "CQCQCQ  ";
static const char MY[]   = "N0CALL  ";
static const char SFX[]  = "TEST";

static uint8_t header[49];
static uint8_t dataFrames[DATA_FRAMES][12];

/* What the host sent to the modem */
static SimPacket* txPackets[MAX_PACKETS];
static int        txCount    = 0;
static int        txEotFlag  = 0;
static int        txCommands = 0;

/* What the host sent to the gateway */
static uint8_t gwHeader[49];
static bool    gwHaveHeader = false;
static uint8_t gwData[200][12];
static int     gwDataCount  = 0;
static int     gwEots       = 0;
static int     gwOther      = 0;

/*
 * reverseBits()
 *
 * Turns the order of the 8 bits of a byte around.
 *
 * Parameters:
 *   b - The byte.
 *
 * Returns: The byte with its bits in the opposite order.
 */
static uint8_t reverseBits(uint8_t b)
{
    uint8_t r = 0U;

    for (int i = 0; i < 8; i++)
        if (b & (1U << i))
            r |= (uint8_t)(0x80U >> i);

    return r;
}

/*
 * makeAmbe()
 *
 * Makes a 9 byte AMBE voice frame with valid error correction, so that the
 * host finds no errors in it and does not change it.  The two protected
 * groups are Golay (24,12) words and the second is scrambled, as on the air.
 *
 * Parameters:
 *   ambe - Receives the 9 bytes.
 *   seed - Number that makes each frame different.
 *
 * Returns: nothing.
 */
static void makeAmbe(uint8_t* ambe, unsigned int seed)
{
    unsigned int dataA = (seed * 211U + 17U) & 0xFFFU;
    unsigned int dataB = (seed * 97U + 5U) & 0xFFFU;
    unsigned int a     = golay24128_encode24128(dataA);
    unsigned int b     = golay24128_encode24128(dataB) ^ AMBE_PRNG_TABLE[dataA];
    unsigned int c     = (seed * 40503U + 12345U) & 0xFFFFFFU;
    unsigned int mask  = 0x800000U;

    memset(ambe, 0x00, 9);

    for (unsigned int i = 0U; i < 24U; i++, mask >>= 1)
    {
        DSTAR_FEC_WRITE(ambe, DSTAR_FEC_A[i], a & mask);
        DSTAR_FEC_WRITE(ambe, DSTAR_FEC_B[i], b & mask);
        DSTAR_FEC_WRITE(ambe, DSTAR_FEC_C[i], c & mask);
    }
}

/*
 * buildCall()
 *
 * Makes the header and voice frames of a call as the D-STAR gateway does.
 * The first voice frame of each superframe of 21 carries the frame sync in
 * its slow data bytes, the others carry the "no data" filler.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void buildCall(void)
{
    memset(header, 0x00, sizeof(header));
    header[0] = 0x61;
    header[1] = 0x00;
    header[2] = 0x31;
    header[3] = 0x04;
    memcpy(header + 4, "DSTH", 4);
    header[8] = 0x40; /* as the gateway sends it */
    memcpy(header + 11, RPT2, 8);
    memcpy(header + 19, RPT1, 8);
    memcpy(header + 27, YOUR, 8);
    memcpy(header + 35, MY, 8);
    memcpy(header + 43, SFX, 4);

    CCITTChecksumReverse ccr;
    ccitt_checksum_init(&ccr);
    ccitt_checksum_update_bytes(&ccr, header + 8, 39);
    ccitt_checksum_result_bytes(&ccr, header + 47);

    for (int i = 0; i < DATA_FRAMES; i++)
    {
        makeAmbe(dataFrames[i], (unsigned int)i + 1U);

        if (i % 21 == 0)
        {
            dataFrames[i][9]  = 0x55U;
            dataFrames[i][10] = 0x2DU;
            dataFrames[i][11] = 0x16U;
        }
        else
        {
            dataFrames[i][9]  = 0x16U;
            dataFrames[i][10] = 0x29U;
            dataFrames[i][11] = 0xF5U;
        }
    }
}

/*
 * grabTx()
 *
 * Keeps a packet the host sent to the modem: samples, bits and frames are
 * kept for the replay; commands (five bytes) are only counted.
 *
 * Parameters:
 *   p   - The packet.
 *   arg - Not used.
 *
 * Returns: nothing.
 */
static void grabTx(const SimPacket* p, void* arg)
{
    (void)arg;

    if (p->len <= 5)
    {
        txCommands++;
        return;
    }

    if (txCount < MAX_PACKETS)
    {
        txPackets[txCount] = malloc(sizeof(SimPacket));
        memcpy(txPackets[txCount], p, sizeof(SimPacket));
        txCount++;
    }

    if (memcmp(p->data + 4, "SEOT", 4) == 0)
        txEotFlag++;
}

/*
 * grabGw()
 *
 * Keeps a D-STAR packet the host sent to the gateway.
 *
 * Parameters:
 *   p   - The packet.
 *   arg - Not used.
 *
 * Returns: nothing.
 */
static void grabGw(const SimPacket* p, void* arg)
{
    (void)arg;

    if (memcmp(p->data + 4, "DSTH", 4) == 0 && p->len == 49)
    {
        memcpy(gwHeader, p->data, 49);
        gwHaveHeader = true;
    }
    else if (memcmp(p->data + 4, "DSTD", 4) == 0 && p->len == 20 && gwDataCount < 200)
        memcpy(gwData[gwDataCount++], p->data + 8, 12);
    else if (memcmp(p->data + 4, "DSTE", 4) == 0)
        gwEots++;
    else
        gwOther++;
}

/*
 * unlinkTest()
 *
 * Frame mode: a call from the network is cut off without its end frame,
 * first by the gateway telling the host it is unlinked (DISC) and then by the
 * gateway just going quiet.  Either way the host must end the call itself and
 * send the end of transmission to the modem; a host that does not stays in TX
 * for ever.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void unlinkTest(void)
{
    const uint8_t name[8] = {'R', 'E', 'F', '0', '0', '1', ' ', 'C'};

    for (int variant = 0; variant < 2; variant++)
    {
        printf("\nTest %d: the call from the network is cut off (%s)\n", 4 + variant,
               variant == 0 ? "the gateway is unlinked" : "the gateway goes quiet");

        for (int i = 0; i < txCount; i++)
            free(txPackets[i]);
        txCount = 0;
        simDrain(&modemC, 300, 1000, NULL, NULL);

        simSendRaw(&gwC, header, 49);
        simDrain(&modemC, gapMs, gapMs * 2, grabTx, NULL);

        for (int i = 0; i < 6; i++)
        {
            simSend(&gwC, "DSTD", dataFrames[i], 12);
            simDrain(&modemC, gapMs, gapMs * 2, grabTx, NULL);
        }

        int before = txCount;

        if (variant == 0)
            simSend(&gwC, "DISC", name, 8);

        simDrain(&modemC, variant == 0 ? 1500 : 2500, 8000, grabTx, NULL);

        bool eot = false;

        for (int i = before; i < txCount; i++)
            if (memcmp(txPackets[i]->data + 4, "DSTE", 4) == 0)
                eot = true;

        simCheck(eot, "the host ended the call and sent the end of transmission to the modem");
    }
}

/*
 * main()
 *
 * Program entry.  Runs the two tests described at the top of the file for
 * one modem type.
 *
 * Parameters:
 *   argc - Number of command line arguments.
 *   argv - Command line arguments.
 *
 * Returns: 0 if all checks pass, 1 if one fails, 2 on a setup error.
 */
int main(int argc, char** argv)
{
    int modem = 1;
    const char* kindName = "openmt";

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
            modem = atoi(argv[++i]);
        else if (strcmp(argv[i], "--modem") == 0 && i + 1 < argc)
            kindName = argv[++i];
        else if (strcmp(argv[i], "--gap") == 0 && i + 1 < argc)
            gapMs = atoi(argv[++i]);
        else
        {
            fprintf(stderr, "Usage: dstar_sim [-m modem] [--modem mmdvm|openmt|openmths] [--gap ms]\n");
            return 2;
        }
    }

    if (strcmp(kindName, "openmt") == 0 || strcmp(kindName, "openmtsx") == 0)
        kind = KIND_SAMP;
    else if (strcmp(kindName, "openmths") == 0)
        kind = KIND_BITS;
    else
        kind = KIND_FRAME;

    /* 1. play the modem host */
    int lfd = simListen((uint16_t)(18000 + modem - 1));

    if (lfd < 0)
    {
        printf("dstar_sim: cannot listen on the modem port\n");
        return 2;
    }

    printf("dstar_sim: waiting for dstar_host on port %d (modem type %s)...\n", 18000 + modem - 1, kindName);

    int mfd = simAccept(lfd, 30000);

    if (mfd < 0)
    {
        printf("dstar_sim: host did not connect\n");
        return 2;
    }

    simConnInit(&modemC, mfd);

    SimPacket reg;

    if (simRead(&modemC, &reg, 10000) != 1)
    {
        printf("dstar_sim: no registration from the host\n");
        return 2;
    }

    simCheck(memcmp(reg.data + 4, "MODE", 4) == 0 && memcmp(reg.data + 8, "DSTAR", 5) == 0,
             "MODE registration for DSTAR (length %d)", reg.len);

    /* 2. play the gateway */
    int gfd = simConnect((uint16_t)(18100 + modem - 1), 50);

    if (gfd < 0)
    {
        printf("dstar_sim: could not connect to the gateway port\n");
        return 2;
    }

    simConnInit(&gwC, gfd);
    usleep(1500000);
    simDrain(&modemC, 300, 1000, NULL, NULL);

    /* 3. network -> radio */
    buildCall();
    printf("\nTest 1: call from the network to the radio (header and %d voice frames)\n", DATA_FRAMES);

    simSendRaw(&gwC, header, 49);
    simDrain(&modemC, gapMs, gapMs * 2, grabTx, NULL);

    for (int i = 0; i < DATA_FRAMES; i++)
    {
        simSend(&gwC, "DSTD", dataFrames[i], 12);
        simDrain(&modemC, gapMs, gapMs * 2, grabTx, NULL);
    }

    simSend(&gwC, "DSTE", NULL, 0);
    simDrain(&modemC, 2500, 40000, grabTx, NULL);

    printf("  host sent %d packets and %d commands to the modem\n", txCount, txCommands);

    if (kind == KIND_FRAME)
    {
        int hdrs = 0, datas = 0, eots = 0;

        for (int i = 0; i < txCount; i++)
        {
            if (memcmp(txPackets[i]->data + 4, "DSTH", 4) == 0) hdrs++;
            else if (memcmp(txPackets[i]->data + 4, "DSTD", 4) == 0) datas++;
            else if (memcmp(txPackets[i]->data + 4, "DSTE", 4) == 0) eots++;
        }

        simCheck(hdrs == 1, "frame mode: one header reached the modem (%d)", hdrs);
        simCheck(datas == DATA_FRAMES, "frame mode: %d of %d voice frames reached the modem", datas, DATA_FRAMES);
        simCheck(eots >= 1, "frame mode: an end of transmission reached the modem (%d)", eots);
    }
    else
    {
        size_t bytes = 0U;
        const char* want = kind == KIND_SAMP ? "SAMP" : "BITS";
        int wrongType = 0;

        for (int i = 0; i < txCount; i++)
        {
            if (memcmp(txPackets[i]->data + 4, want, 4) == 0 || memcmp(txPackets[i]->data + 4, "SEOT", 4) == 0)
                bytes += (size_t)txPackets[i]->len - 8U;
            else
                wrongType++;
        }

        /* a voice frame is 96 bits, a bit takes 5 samples of 2 bytes in sample mode */
        size_t need = (size_t)DATA_FRAMES * 12U * (kind == KIND_SAMP ? 80U : 1U);

        simCheck(wrongType == 0, "every packet is a %s packet (%d other)", want, wrongType);
        simCheck(bytes >= need, "%zu payload bytes sent, at least %zu needed for the voice frames", bytes, need);
        if (kind == KIND_SAMP)
            simCheck(txEotFlag >= 1, "the transmission ends with an EOT packet (%d seen)", txEotFlag);
        else
            simCheck(txCount > 0 && memcmp(txPackets[txCount - 1]->data + 4, "BITS", 4) == 0,
                     "the transmission ends with a BITS packet");
    }

    /* 4. radio -> network */
    printf("\nTest 2: the transmission comes back from the radio, to the network\n");
    simDrain(&gwC, 100, 500, NULL, NULL);

    for (int i = 0; i < txCount; i++)
    {
        SimPacket q = *txPackets[i];

        if (kind == KIND_FRAME)
        {
            if (memcmp(q.data + 4, "DST", 3) != 0)
                continue;
        }
        else if (kind == KIND_SAMP)
            memcpy(q.data + 4, "SAMP", 4);
        else
        {
            /* The host sends each byte with the first bit on the air in the
               least significant place; it expects received bits with the
               first one in the most significant place.  A real modem does
               this turn, so the simulator does it here. */
            for (int k = 8; k < q.len; k++)
                q.data[k] = reverseBits(q.data[k]);
        }

        simSendRaw(&modemC, q.data, q.len);
        simDrain(&gwC, 1, 1, grabGw, NULL);
        usleep(kind == KIND_FRAME ? gapMs * 1000 : 1500);
    }

    simDrain(&gwC, 3000, 25000, grabGw, NULL);

    printf("  gateway got %s header, %d voice frames, %d end (%d other packets)\n",
           gwHaveHeader ? "a" : "no", gwDataCount, gwEots, gwOther);

    simCheck(gwHaveHeader, "a header was received");
    simCheck(gwHaveHeader && memcmp(gwHeader + 11, RPT2, 8) == 0 && memcmp(gwHeader + 19, RPT1, 8) == 0 &&
             memcmp(gwHeader + 27, YOUR, 8) == 0 && memcmp(gwHeader + 35, MY, 8) == 0 &&
             memcmp(gwHeader + 43, SFX, 4) == 0, "the header carries the same callsigns");

    int same = 0;

    for (int i = 0; i < gwDataCount; i++)
    {
        for (int j = 0; j < DATA_FRAMES; j++)
        {
            if (memcmp(gwData[i], dataFrames[j], 9) == 0)
            {
                same++;
                break;
            }
        }
    }

    simCheck(gwDataCount >= DATA_FRAMES - 2 && gwDataCount <= DATA_FRAMES + 2,
             "voice frames received: %d of %d", gwDataCount, DATA_FRAMES);
    simCheck(same == gwDataCount, "every voice frame received is one that was sent (%d of %d)", same, gwDataCount);
    simCheck(gwDataCount == DATA_FRAMES, "no voice frame lost or added");
    simCheck(gwEots >= 1, "an end of transmission was received");

    /* 5. a network call without a header packet: the header is in the slow data */
    printf("\nTest 3: network call with the header only in the slow data\n");

    size_t bytes1 = 0U;
    int    hdrs1  = 0;

    for (int i = 0; i < txCount; i++)
    {
        if (kind != KIND_FRAME)
            bytes1 += (size_t)txPackets[i]->len - 8U;
        else if (memcmp(txPackets[i]->data + 4, "DSTH", 4) == 0)
            hdrs1++;
        free(txPackets[i]);
    }
    txCount = 0;

    /* the 41 header bytes and 4 filler bytes go out five at a time, a pair of
       voice frames for each five, after the frame sync of the superframe */
    uint8_t ssb[45];
    memset(ssb, 0x66, sizeof(ssb));
    memcpy(ssb, header + 8, 41);

    for (int i = 0; i < 9; i++)
    {
        uint8_t* a = dataFrames[1 + 2 * i] + 9;
        uint8_t* b = dataFrames[2 + 2 * i] + 9;

        a[0] = (uint8_t)(0x55U ^ 0x70U);
        a[1] = (uint8_t)(ssb[5 * i]     ^ 0x4FU);
        a[2] = (uint8_t)(ssb[5 * i + 1] ^ 0x93U);
        b[0] = (uint8_t)(ssb[5 * i + 2] ^ 0x70U);
        b[1] = (uint8_t)(ssb[5 * i + 3] ^ 0x4FU);
        b[2] = (uint8_t)(ssb[5 * i + 4] ^ 0x93U);
    }

    for (int i = 0; i < DATA_FRAMES; i++)
    {
        simSend(&gwC, "DSTD", dataFrames[i], 12);
        simDrain(&modemC, gapMs, gapMs * 2, grabTx, NULL);
    }

    simSend(&gwC, "DSTE", NULL, 0);
    simDrain(&modemC, 2500, 40000, grabTx, NULL);

    size_t bytes3 = 0U;
    int    hdrs3  = 0;

    for (int i = 0; i < txCount; i++)
    {
        if (kind != KIND_FRAME)
            bytes3 += (size_t)txPackets[i]->len - 8U;
        else if (memcmp(txPackets[i]->data + 4, "DSTH", 4) == 0)
            hdrs3++;
    }

    if (kind == KIND_FRAME)
        simCheck(hdrs3 == hdrs1 && hdrs3 == 1, "frame mode: the header found in the slow data reached the modem (%d)", hdrs3);
    else
    {
        /* The voice frames are sent only once the slow data header is complete
           (after about half of the call), so the call is shorter than one that
           came with a header packet.  The header itself adds a fifth of what is
           sent here: a host that leaves the header out falls below 9/16 of the
           longer call, one that sends it is above. */
        simCheck(bytes3 * 16U > bytes1 * 9U,
                 "the header found in the slow data was sent to the modem (%zu bytes; %zu for a call with a header packet)", bytes3, bytes1);
    }

    if (kind == KIND_FRAME)
        unlinkTest();

    printf("\n%s (%d checks, %d failed)\n", simFails == 0 ? "PASS" : "FAIL", simChecks, simFails);

    return simFails == 0 ? 0 : 1;
}
