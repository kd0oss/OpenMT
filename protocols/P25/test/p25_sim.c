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
 * p25_sim: plays the modem host and the P25 gateway for p25_host.
 *
 *   1. listens on the modem host port and accepts the host's connection,
 *   2. connects to the host's gateway port and links (CONN),
 *   3. network -> radio: sends a call (header, LDU1/LDU2 pairs, end) the way
 *      the gateway does and captures what the host sends to the modem,
 *   4. radio -> network: sends the captured transmission back to the host as
 *      what a modem of this type delivers (frames, samples or bits) and
 *      checks the voice frames that come out of the gateway port.
 *
 * Run once for each modem type, with the host started with the same type
 * (see run_sim.sh):
 *
 * With --duplex (frame mode) a third test follows: the host is put in duplex
 * and damaged frames are sent from the network and from the radio.
 *
 * Usage: p25_sim [-m modem 1-10] [--modem mmdvm|openmt|openmths] [--duplex]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "p25_func.h"
#include "sim_common.h"

#define NAC         0x293U
#define SRC_ID      1234567U
#define DST_ID      9U
#define LDU_PAIRS   3
#define MAX_PACKETS 4000

enum { KIND_SAMP, KIND_BITS, KIND_FRAME };

static SimConn modemC;
static SimConn gwC;
static int     kind = KIND_SAMP;

/* One frame as the gateway sends it */
typedef struct
{
    uint8_t data[P25_LDU_FRAME_LENGTH_BYTES + 8];
    int     len;
    uint8_t duid;
} Frame;

static Frame callFrames[2 + LDU_PAIRS * 2];
static int   callCount = 0;

/* What the host sent to the modem */
static SimPacket* txPackets[MAX_PACKETS];
static int        txCount    = 0;
static int        txEotFlag  = 0;
static int        txCommands = 0;

/* What the host sent to the gateway */
static Frame gwFrames[64];
static int   gwCount = 0;
static int   gwOther = 0;

/*
 * imbeFill()
 *
 * Makes a repeatable pattern to use as an IMBE voice frame.
 *
 * Parameters:
 *   imbe - Receives the 11 bytes.
 *   seed - Number that makes each frame different.
 *
 * Returns: nothing.
 */
static void imbeFill(uint8_t* imbe, unsigned int seed)
{
    for (unsigned int i = 0U; i < 11U; i++)
        imbe[i] = (uint8_t)(seed * 37U + i * 91U + 5U);
}

/*
 * addFrame()
 *
 * Finishes a frame of the call: the OpenMT header is written and the frame
 * is added to the list of frames of the call.
 *
 * Parameters:
 *   f    - The frame, already filled in from byte 8.
 *   type - Four character frame type.
 *   len  - Length of the frame data (without the 8 byte header).
 *   duid - The data unit id of the frame.
 *
 * Returns: nothing.
 */
static void addFrame(Frame* f, const char* type, int len, uint8_t duid)
{
    f->data[0] = 0x61U;
    f->data[1] = 0x00U;
    f->data[2] = (uint8_t)(len + 8);
    f->data[3] = 0x04U;
    memcpy(f->data + 4, type, 4);
    f->len  = len + 8;
    f->duid = duid;
}

/*
 * buildFrames()
 *
 * Makes the frames of a group call as the P25 gateway does: a header, pairs
 * of LDU1 and LDU2, and the terminator.
 *
 * Parameters:
 *   out    - Receives the frames.
 *   src    - Source id of the call.
 *   dst    - Destination id of the call.
 *   miByte - Value of every byte of the message indicator.
 *
 * Returns: The number of frames made.
 */
static int buildFrames(Frame* out, unsigned int src, unsigned int dst, uint8_t miByte)
{
    uint8_t imbe[11];
    uint8_t mi[P25_MI_LENGTH_BYTES];
    int     count = 0;

    memset(mi, miByte, sizeof(mi));
    p25Reset();
    resetNID(NAC);
    setLCF(P25_LCF_GROUP);
    setMFId(0x00U);
    setSrcId(src);
    setDstId(dst);
    setMI(mi);
    setAlgId(0x80U);
    setKId(0U);

    Frame* f = &out[count++];
    memset(f->data, 0x00U, sizeof(f->data));
    addSync(f->data + 8U);
    encodeNID(f->data + 8U, P25_DUID_HEADER);
    encodeHeader(f->data + 8U);
    addBusyBits(f->data + 8U, P25_HDR_FRAME_LENGTH_BITS, false, true);
    addFrame(f, "P25H", P25_HDR_FRAME_LENGTH_BYTES, P25_DUID_HEADER);

    for (int pair = 0; pair < LDU_PAIRS; pair++)
    {
        for (int which = 0; which < 2; which++)
        {
            f = &out[count++];
            memset(f->data, 0x00U, sizeof(f->data));
            addSync(f->data + 8U);
            encodeNID(f->data + 8U, which == 0 ? P25_DUID_LDU1 : P25_DUID_LDU2);
            if (which == 0)
                encodeLDU1(f->data + 8U);
            else
                encodeLDU2(f->data + 8U);

            for (unsigned int n = 0U; n < 9U; n++)
            {
                imbeFill(imbe, (unsigned int)(pair * 18 + which * 9) + n);
                audip25encode(f->data + 8U, imbe, n);
            }

            lowSpeedData_setLSD1(0x00U);
            lowSpeedData_setLSD2(0x00U);
            lowSpeedData_encode1(f->data + 8U);
            addBusyBits(f->data + 8U, P25_LDU_FRAME_LENGTH_BITS, false, true);
            addFrame(f, "P25L", P25_LDU_FRAME_LENGTH_BYTES, which == 0 ? P25_DUID_LDU1 : P25_DUID_LDU2);
        }
    }

    f = &out[count++];
    memset(f->data, 0x00U, sizeof(f->data));
    addSync(f->data + 8U);
    encodeNID(f->data + 8U, P25_DUID_TERM);
    addBusyBits(f->data + 8U, P25_TERM_FRAME_LENGTH_BITS, true, false);
    addFrame(f, "P25E", P25_TERM_FRAME_LENGTH_BYTES, P25_DUID_TERM);

    return count;
}

/*
 * buildCall()
 *
 * Makes the call that the tests send.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void buildCall(void)
{
    callCount = buildFrames(callFrames, SRC_ID, DST_ID, 0x00U);
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
 * Keeps a P25 frame the host sent to the gateway.
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

    if (memcmp(p->data + 4, "P25", 3) == 0 && gwCount < 64 && p->len - 8 <= (int)P25_LDU_FRAME_LENGTH_BYTES)
    {
        Frame* f = &gwFrames[gwCount++];

        memcpy(f->data, p->data, (size_t)p->len);
        f->len = p->len;
        decodeNID(f->data + 8U);
        f->duid = getDUID();
    }
    else
        gwOther++;
}

/*
 * duidName()
 *
 * Gives the name of a data unit id for the printed results.
 *
 * Parameters:
 *   duid - The data unit id.
 *
 * Returns: The name.
 */
static const char* duidName(uint8_t duid)
{
    switch (duid)
    {
        case P25_DUID_HEADER: return "HDU";
        case P25_DUID_LDU1:   return "LDU1";
        case P25_DUID_LDU2:   return "LDU2";
        case P25_DUID_TERM:   return "TDU";
        default:              return "?";
    }
}

/*
 * bitAt()
 *
 * Reads one bit of a frame (most significant bit first).
 *
 * Parameters:
 *   data - The frame data.
 *   pos  - Bit position.
 *
 * Returns: 0 or 1.
 */
static int bitAt(const uint8_t* data, int pos)
{
    return (data[pos / 8] >> (7 - pos % 8)) & 1;
}

/*
 * flipBit()
 *
 * Turns one bit of a frame over.
 *
 * Parameters:
 *   data - The frame data, changed in place.
 *   pos  - Bit position.
 *
 * Returns: nothing.
 */
static void flipBit(uint8_t* data, int pos)
{
    data[pos / 8] ^= (uint8_t)(0x80U >> (pos % 8));
}

/*
 * damageFrame()
 *
 * Damages a frame in ways the FEC can repair: two bits of the NID (not for
 * the terminator) and three bits spread over the link control, taken from
 * the list of bit positions that carry link control.
 *
 * Parameters:
 *   f    - The frame, changed in place.
 *   mask - Positions of the link control bits of this frame.
 *   n    - Number of positions in mask.
 *
 * Returns: nothing.
 */
static void damageFrame(Frame* f, const int* mask, int n)
{
    if (f->duid == P25_DUID_TERM)
        return;

    flipBit(f->data + 8, 60);
    flipBit(f->data + 8, 90);

    for (int k = 1; k <= 3 && n >= 4; k++)
        flipBit(f->data + 8, mask[k * n / 4]);
}

/*
 * duplexTest()
 *
 * Test 3 (duplex, frame mode): the host is put in duplex, then a damaged
 * call is sent from the network and again from the radio.  A host that
 * rebuilds the frames from what it decodes sends the modem the clean frames,
 * where one that repeats them would send the damage on.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void duplexTest(void)
{
    Frame other[2 + LDU_PAIRS * 2];
    Frame bad[2 + LDU_PAIRS * 2];
    int   masks[2 + LDU_PAIRS * 2][P25_LDU_FRAME_LENGTH_BITS];
    int   maskLen[2 + LDU_PAIRS * 2];

    printf("\nTest 3: duplex, damaged frames are rebuilt before they are relayed\n");

    /* the link control bits are the ones that change with the ids and MI */
    int n = buildFrames(other, SRC_ID + 0x55555U, DST_ID + 100U, 0xFFU);
    buildCall();

    for (int i = 0; i < n; i++)
    {
        maskLen[i] = 0;
        for (int b = 0; b < (callFrames[i].len - 8) * 8; b++)
            if (bitAt(callFrames[i].data + 8, b) != bitAt(other[i].data + 8, b))
                masks[i][maskLen[i]++] = b;

        bad[i] = callFrames[i];
        damageFrame(&bad[i], masks[i], maskLen[i]);
    }

    simCheck(maskLen[0] > 8 && maskLen[1] > 8 && maskLen[2] > 8, "link control bits found (%d, %d, %d)", maskLen[0], maskLen[1], maskLen[2]);

    uint8_t duplex = 0x00U;

    simSend(&modemC, "COMM", &duplex, 1);
    usleep(300000);
    simDrain(&modemC, 300, 1000, NULL, NULL);
    simDrain(&gwC, 300, 1000, NULL, NULL);

    /* from the network */
    for (int i = 0; i < txCount; i++)
        free(txPackets[i]);
    txCount = 0;

    for (int i = 0; i < n; i++)
    {
        simSendRaw(&gwC, bad[i].data, bad[i].len);
        simDrain(&modemC, 100, 400, grabTx, NULL);
    }
    simDrain(&modemC, 1500, 10000, grabTx, NULL);

    int frames = 0, exact = 0;

    for (int i = 0; i < txCount; i++)
    {
        if (memcmp(txPackets[i]->data + 4, "P25", 3) == 0)
        {
            if (frames < n && txPackets[i]->len == callFrames[frames].len &&
                memcmp(txPackets[i]->data + 8, callFrames[frames].data + 8, (size_t)txPackets[i]->len - 8) == 0)
                exact++;
            frames++;
        }
    }

    simCheck(frames >= n, "network: %d of %d frames reached the modem, terminator included", frames, n);
    simCheck(exact == frames && frames > 0, "network: %d of %d relayed frames are the clean frames", exact, frames);

    /* from the radio */
    for (int i = 0; i < txCount; i++)
        free(txPackets[i]);
    txCount = 0;
    gwCount = 0;
    gwOther = 0;

    for (int i = 0; i < n; i++)
    {
        simSendRaw(&modemC, bad[i].data, bad[i].len);
        simDrain(&modemC, 30, 100, grabTx, NULL);
        simDrain(&gwC, 1, 1, grabGw, NULL);
        usleep(30000);
    }
    simDrain(&modemC, 1500, 10000, grabTx, NULL);
    simDrain(&gwC, 1500, 10000, grabGw, NULL);

    int relayed = 0, lcGood = 0, nidGood = 0;

    for (int i = 0; i < txCount; i++)
    {
        if (memcmp(txPackets[i]->data + 4, "P25", 3) != 0 || relayed >= n)
            continue;

        const uint8_t* d = txPackets[i]->data + 8;
        bool lc = true;

        for (int k = 0; k < maskLen[relayed]; k++)
            if (bitAt(d, masks[relayed][k]) != bitAt(callFrames[relayed].data + 8, masks[relayed][k]))
                lc = false;

        if (lc)
            lcGood++;

        uint8_t want = callFrames[relayed].duid;
        if (want == P25_DUID_TERM || (decodeNID(txPackets[i]->data + 8) && getDUID() == want))
            nidGood++;

        relayed++;
    }

    simCheck(relayed >= n - 1, "radio: %d of %d frames relayed to the modem", relayed, n - 1);
    simCheck(lcGood == relayed && relayed > 0, "radio: link control of %d of %d relayed frames is clean", lcGood, relayed);
    simCheck(nidGood == relayed && relayed > 0, "radio: NID of %d of %d relayed frames is clean", nidGood, relayed);

    int gwGood = 0, gwLdu1 = 0;

    for (int i = 0; i < gwCount; i++)
    {
        if (gwFrames[i].duid == P25_DUID_LDU1)
        {
            gwLdu1++;
            if (decodeLDU1(gwFrames[i].data + 8U) && getSrcId() == SRC_ID && getDstId() == DST_ID)
                gwGood++;
        }
    }

    simCheck(gwLdu1 == LDU_PAIRS && gwGood == gwLdu1, "radio: gateway got %d of %d LDU1 with the right ids", gwGood, LDU_PAIRS);
}

/*
 * termLcTest()
 *
 * Test 4 (duplex, frame mode): a call from the network ends with a damaged
 * terminator with link control.  The host decodes its link control and
 * encodes it again, so the modem gets the clean terminator.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void termLcTest(void)
{
    Frame   call[2 + LDU_PAIRS * 2];
    Frame   term;
    uint8_t lc[9] = {P25_LCF_GROUP, 0x00, 0x00, 0x00, 0x00, (uint8_t)DST_ID, 0x12, 0xD6, 0x87};

    printf("\nTest 4: duplex, a damaged terminator with link control is rebuilt\n");

    buildFrames(call, SRC_ID, DST_ID, 0x00U);

    memset(term.data, 0x00U, sizeof(term.data));
    addSync(term.data + 8U);
    encodeNID(term.data + 8U, P25_DUID_TERM_LC);
    encodeTDULC(term.data + 8U, lc);
    addBusyBits(term.data + 8U, P25_TERMLC_FRAME_LENGTH_BITS, true, false);
    addFrame(&term, "P25E", P25_TERMLC_FRAME_LENGTH_BYTES, P25_DUID_TERM_LC);

    uint8_t check[9];
    simCheck(decodeTDULC(term.data + 8U, check) && memcmp(check, lc, 9) == 0, "the clean terminator decodes to its link control");

    Frame bad = term;
    flipBit(bad.data + 8, 60);
    flipBit(bad.data + 8, 90);
    flipBit(bad.data + 8, 120);
    flipBit(bad.data + 8, 200);
    flipBit(bad.data + 8, 330);

    for (int i = 0; i < txCount; i++)
        free(txPackets[i]);
    txCount = 0;
    simDrain(&modemC, 300, 1000, NULL, NULL);

    for (int i = 0; i < 3; i++)
    {
        simSendRaw(&gwC, call[i].data, call[i].len);
        simDrain(&modemC, 100, 400, grabTx, NULL);
    }
    simSendRaw(&gwC, bad.data, bad.len);
    simDrain(&modemC, 1500, 10000, grabTx, NULL);

    const SimPacket* last = NULL;

    for (int i = 0; i < txCount; i++)
        if (memcmp(txPackets[i]->data + 4, "P25", 3) == 0)
            last = txPackets[i];

    simCheck(last != NULL && last->len == term.len && memcmp(last->data + 8, term.data + 8, (size_t)term.len - 8) == 0,
             "the terminator reached the modem as the clean frame");
}

/*
 * trellisTest()
 *
 * Test 7 (duplex, frame mode): trunking signalling (TSDU) and a packet data
 * header (PDU) arrive from the radio with bit errors in their rate 1/2
 * trellis block.  The host decodes the block, checks its CRC and encodes it
 * again, so the modem gets the clean frame.  A block with a bad CRC (an
 * undecodable one) is passed on as it came.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void trellisTest(void)
{
    static const uint8_t duids[2] = {P25_DUID_TSDU, P25_DUID_PDU};
    static const char*   names[2] = {"TSDU", "PDU header"};

    printf("\nTest 7: duplex, TSDU and PDU header frames are rebuilt before they are relayed\n");

    for (int t = 0; t < 2; t++)
    {
        Frame   good;
        uint8_t block[12] = {0x07, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x00, 0x00};
        uint8_t raw[25];

        CRC_addCCITT162(block, 12U);
        memset(good.data, 0x00U, sizeof(good.data));
        memset(raw, 0x00U, sizeof(raw));
        P25Trellis_encode12(block, raw);
        addSync(good.data + 8U);
        encodeNID(good.data + 8U, duids[t]);
        encode(raw, good.data + 8U, 114U, 318U);
        addBusyBits(good.data + 8U, P25_TSDU_FRAME_LENGTH_BITS, false, true);
        addFrame(&good, "P25H", P25_TSDU_FRAME_LENGTH_BYTES, duids[t]);

        Frame bad = good;

        flipBit(bad.data + 8, 150);
        flipBit(bad.data + 8, 262);

        simCheck(rebuildTrellis12(bad.data + 8U) && memcmp(bad.data + 8, good.data + 8, P25_TSDU_FRAME_LENGTH_BYTES) == 0,
                 "%s: the host's decoder repairs the damaged block", names[t]);

        bad = good;
        flipBit(bad.data + 8, 150);
        flipBit(bad.data + 8, 262);

        for (int i = 0; i < txCount; i++)
            free(txPackets[i]);
        txCount = 0;
        simDrain(&modemC, 300, 1000, NULL, NULL);
        simDrain(&gwC, 300, 1000, NULL, NULL);

        simSendRaw(&modemC, bad.data, bad.len);
        simDrain(&modemC, 1500, 6000, grabTx, NULL);

        const SimPacket* last = NULL;

        for (int i = 0; i < txCount; i++)
            if (memcmp(txPackets[i]->data + 4, "P25", 3) == 0 && txPackets[i]->len == good.len)
                last = txPackets[i];

        simCheck(last != NULL, "%s: the frame reached the modem", names[t]);
        simCheck(last != NULL && memcmp(last->data + 8, good.data + 8, P25_TSDU_FRAME_LENGTH_BYTES) == 0,
                 "%s: the frame the modem got is the clean frame", names[t]);
    }
}

/*
 * unlinkTest()
 *
 * Frame mode: a call from the network is cut off without its terminator,
 * first by the gateway telling the host it is unlinked (DISC) and then by the
 * gateway just going quiet.  Either way the host must end the call itself and
 * send the terminator to the modem; a host that does not stays in TX for ever.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void unlinkTest(void)
{
    const uint8_t name[6] = {'P', '2', '5', 'R', 'E', 'F'};

    buildCall();

    for (int variant = 0; variant < 2; variant++)
    {
        printf("\nTest %d: the call from the network is cut off (%s)\n", 5 + variant,
               variant == 0 ? "the gateway is unlinked" : "the gateway goes quiet");

        for (int i = 0; i < txCount; i++)
            free(txPackets[i]);
        txCount = 0;
        simDrain(&modemC, 300, 1000, NULL, NULL);

        for (int i = 0; i < 3; i++)
        {
            simSendRaw(&gwC, callFrames[i].data, callFrames[i].len);
            simDrain(&modemC, 100, 400, grabTx, NULL);
        }

        int before = txCount;

        if (variant == 0)
            simSend(&gwC, "DISC", name, 6);

        simDrain(&modemC, variant == 0 ? 1500 : 2500, 8000, grabTx, NULL);

        bool term = false;

        for (int i = before; i < txCount; i++)
            if (memcmp(txPackets[i]->data + 4, "P25", 3) == 0 && txPackets[i]->len == 8 + (int)P25_TERM_FRAME_LENGTH_BYTES)
                term = true;

        simCheck(term, "the host ended the call and sent the terminator to the modem");

        /* the gateway is linked again for the next test */
        simSend(&gwC, "CONN", name, 6);
        usleep(300000);
    }
}

/*
 * lowIdCheck()
 *
 * A source id below 1000000 is a valid P25 radio id: an LDU1 with one must
 * decode.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void lowIdCheck(void)
{
    uint8_t frame[P25_LDU_FRAME_LENGTH_BYTES];

    memset(frame, 0x00U, sizeof(frame));
    p25Reset();
    resetNID(NAC);
    setLCF(P25_LCF_GROUP);
    setMFId(0x00U);
    setSrcId(1234U);
    setDstId(DST_ID);
    encodeLDU1(frame);
    p25Reset();

    simCheck(decodeLDU1(frame) && getSrcId() == 1234U && getDstId() == DST_ID, "an LDU1 with source id 1234 decodes (id %u)", getSrcId());
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
    bool duplexRun = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
            modem = atoi(argv[++i]);
        else if (strcmp(argv[i], "--modem") == 0 && i + 1 < argc)
            kindName = argv[++i];
        else if (strcmp(argv[i], "--duplex") == 0)
            duplexRun = true;
        else
        {
            fprintf(stderr, "Usage: p25_sim [-m modem] [--modem mmdvm|openmt|openmths] [--duplex]\n");
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
        printf("p25_sim: cannot listen on the modem port\n");
        return 2;
    }

    printf("p25_sim: waiting for p25_host on port %d (modem type %s)...\n", 18000 + modem - 1, kindName);

    int mfd = simAccept(lfd, 30000);

    if (mfd < 0)
    {
        printf("p25_sim: host did not connect\n");
        return 2;
    }

    simConnInit(&modemC, mfd);

    SimPacket reg;

    if (simRead(&modemC, &reg, 10000) != 1)
    {
        printf("p25_sim: no registration from the host\n");
        return 2;
    }

    simCheck(memcmp(reg.data + 4, "MODE", 4) == 0 && memcmp(reg.data + 8, "P25", 3) == 0,
             "MODE registration for P25 (length %d)", reg.len);

    /* 2. play the gateway */
    int gfd = simConnect((uint16_t)(18300 + modem - 1), 50);

    if (gfd < 0)
    {
        printf("p25_sim: could not connect to the gateway port\n");
        return 2;
    }

    simConnInit(&gwC, gfd);

    uint8_t conn[6] = {'P', '2', '5', 'R', 'E', 'F'};

    simSend(&gwC, "CONN", conn, 6);
    usleep(500000);
    simDrain(&modemC, 300, 1000, NULL, NULL);

    /* 3. network -> radio */
    printf("\nTest 1: call from the network to the radio (%d frames)\n", 2 + LDU_PAIRS * 2);
    buildCall();

    for (int i = 0; i < callCount; i++)
    {
        simSendRaw(&gwC, callFrames[i].data, callFrames[i].len);
        simDrain(&modemC, 100, 400, grabTx, NULL);
    }

    simDrain(&modemC, 2500, 30000, grabTx, NULL);

    printf("  host sent %d packets and %d commands to the modem\n", txCount, txCommands);

    int expectFrames = kind == KIND_FRAME ? callCount : callCount - 1; /* frame mode sends the terminator as a frame, the others as an end of transmission */

    if (kind == KIND_FRAME)
    {
        int frames = 0;
        bool same = true;

        for (int i = 0; i < txCount; i++)
        {
            if (memcmp(txPackets[i]->data + 4, "P25", 3) == 0)
            {
                if (frames < callCount && (txPackets[i]->len != callFrames[frames].len ||
                    memcmp(txPackets[i]->data + 8, callFrames[frames].data + 8, (size_t)txPackets[i]->len - 8) != 0))
                    same = false;
                frames++;
            }
        }

        simCheck(frames >= expectFrames, "frame mode: %d of %d frames reached the modem", frames, expectFrames);
        simCheck(same, "frame mode: the frames are the ones sent");
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

        size_t frameBytes = P25_HDR_FRAME_LENGTH_BYTES + (size_t)LDU_PAIRS * 2U * P25_LDU_FRAME_LENGTH_BYTES;
        size_t need       = frameBytes * (kind == KIND_SAMP ? 40U : 1U);

        simCheck(wrongType == 0, "every packet is a %s packet (%d other)", want, wrongType);
        simCheck(bytes >= need, "%zu payload bytes sent, at least %zu needed for the frames", bytes, need);
        if (kind == KIND_SAMP)
            simCheck(txEotFlag == 1, "the transmission ends with one EOT packet (%d seen)", txEotFlag);
        else
            simCheck(txCount > 0 && memcmp(txPackets[txCount - 1]->data + 4, "BITS", 4) == 0,
                     "the transmission ends with a BITS packet");

        if (kind == KIND_BITS)
        {
            /* the bit stream carries the frames as sent, after the preamble */
            uint8_t* stream = malloc(bytes);
            size_t n = 0U;

            for (int i = 0; i < txCount; i++)
            {
                memcpy(stream + n, txPackets[i]->data + 8, (size_t)txPackets[i]->len - 8U);
                n += (size_t)txPackets[i]->len - 8U;
            }

            bool found = false;
            for (size_t off = 0U; off + P25_HDR_FRAME_LENGTH_BYTES <= n && !found; off++)
                found = memcmp(stream + off, callFrames[0].data + 8, P25_HDR_FRAME_LENGTH_BYTES) == 0;

            simCheck(found, "the header frame is in the bit stream unchanged");
            free(stream);
        }
    }

    /* 4. radio -> network */
    printf("\nTest 2: the transmission comes back from the radio, to the network\n");
    simDrain(&gwC, 100, 500, NULL, NULL);

    for (int i = 0; i < txCount; i++)
    {
        if (kind == KIND_FRAME)
        {
            if (memcmp(txPackets[i]->data + 4, "P25", 3) == 0)
                simSendRaw(&modemC, txPackets[i]->data, txPackets[i]->len);
        }
        else
        {
            /* a modem delivers samples and bits in the same packets */
            SimPacket q = *txPackets[i];

            if (kind == KIND_SAMP)
                memcpy(q.data + 4, "SAMP", 4);
            simSendRaw(&modemC, q.data, q.len);
        }

        simDrain(&gwC, 1, 1, grabGw, NULL);
        usleep(kind == KIND_FRAME ? 20000 : 1500);
    }

    simDrain(&gwC, 2500, 20000, grabGw, NULL);

    int ldu1 = 0, ldu2 = 0, hdr = 0;
    bool idsOk = true;

    for (int i = 0; i < gwCount; i++)
    {
        if (gwFrames[i].duid == P25_DUID_LDU1)
        {
            ldu1++;
            if (decodeLDU1(gwFrames[i].data + 8U) && (getSrcId() != SRC_ID || getDstId() != DST_ID))
                idsOk = false;
        }
        else if (gwFrames[i].duid == P25_DUID_LDU2)
            ldu2++;
        else if (gwFrames[i].duid == P25_DUID_HEADER)
            hdr++;
    }

    printf("  gateway got %d frames: %d LDU1, %d LDU2, %d header (%d other packets)\n", gwCount, ldu1, ldu2, hdr, gwOther);
    for (int i = 0; i < gwCount && i < 12; i++)
        printf("    %s", duidName(gwFrames[i].duid));
    printf("\n");

    simCheck(ldu1 >= LDU_PAIRS - 1 && ldu2 >= LDU_PAIRS - 1, "voice frames received: %d LDU1 and %d LDU2 of %d each sent", ldu1, ldu2, LDU_PAIRS);
    simCheck(ldu1 == LDU_PAIRS && ldu2 == LDU_PAIRS, "no voice frame lost");
    simCheck(idsOk, "source %u and destination %u read from the LDU1", SRC_ID, DST_ID);

    lowIdCheck();

    if (kind == KIND_FRAME)
        unlinkTest();

    if (duplexRun && kind == KIND_FRAME)
    {
        duplexTest();
        termLcTest();
        trellisTest();
    }

    printf("\n%s (%d checks, %d failed)\n", simFails == 0 ? "PASS" : "FAIL", simChecks, simFails);

    return simFails == 0 ? 0 : 1;
}
