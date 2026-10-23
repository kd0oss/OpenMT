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
 * dmr_sim: plays the modem host and the DMR gateway for dmr_host, with the
 * host in simplex (hotspot) mode, which is its default.
 *
 *   1. listens on the modem host port and accepts the host's connection,
 *   2. connects to the host's gateway port,
 *   3. network -> radio: sends a call on slot 1 (voice LC header, 12 voice
 *      bursts, terminator) the way the gateway does, and captures what the
 *      host sends to the modem,
 *   4. radio -> network: sends the captured transmission back to the host as
 *      the modem delivers it (frames, samples or bits), and checks the call
 *      that comes out of the gateway port: the ids in the header and the
 *      voice bursts and the terminator.
 *
 * Run once for each modem type, with the host started with the same type
 * (see run_sim.sh):
 *
 * Usage: dmr_sim [-m modem 1-10] [--modem mmdvm|openmt|openmths] [--gap ms] [--duplex]
 *
 * With --duplex (frame mode only) the host is told it is a duplex modem and
 * the sim checks that every burst it relays is rebuilt from what it decodes:
 * bursts that arrive damaged (from the network and from the radio) go out
 * clean, and the voice bursts carry the embedded link control of the call.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dmr_fec.h"
#include "dmr_func.h"
#include "sim_common.h"

#define SRC_ID      1234U /* known to the stub ID lookup */
#define DST_ID      9U
#define VOICE_COUNT 12    /* two superframes of six bursts */
#define FRAMES      (VOICE_COUNT + 2)
#define MAX_PACKETS 12000

enum { KIND_SAMP, KIND_BITS, KIND_FRAME };

static SimConn modemC;
static SimConn gwC;
static int     kind  = KIND_SAMP;
static bool    duplex = false;
static int     gapMs = 50; /* time between the bursts the gateway sends: a little under the 60 ms of the air,
                              so the host's queue never runs dry in the middle of a call */

/* The call: control byte and 33 byte burst of each frame */
static uint8_t ctl[FRAMES];
static uint8_t burst[FRAMES][33];

/* What the host sent to the modem */
static SimPacket* txPackets[MAX_PACKETS];
static int        txCount    = 0;
static int        txEots     = 0;
static int        txCommands = 0;

/* What the host sent to the gateway */
static uint8_t gwCtl[400];
static uint8_t gwBurst[400][33];
static uint8_t gwIds[400][7];
static int     gwCount = 0;
static int     gwOther = 0;

/*
 * makeVoice()
 *
 * Fills a 33 byte voice burst with three vocoder frames that have valid
 * error correction, so that the host finds no errors in them and does not
 * change them.  The bits of each frame are placed as dmr_fec.h says.
 *
 * Parameters:
 *   b    - The burst, 33 bytes.
 *   seed - Number that makes each burst different.
 *
 * Returns: nothing.
 */
static void makeVoice(uint8_t* b, unsigned int seed)
{
    memset(b, 0x00, 33);

    for (unsigned int n = 0U; n < 3U; n++)
    {
        unsigned int s     = seed * 3U + n;
        unsigned int data  = (s * 211U + 17U) & 0xFFFU;
        unsigned int datb  = (s * 97U + 5U) & 0xFFFU;
        unsigned int p     = AMBE_PRNG_TABLE[data] >> 1;
        unsigned int a     = golay24128_encode24128(data);
        unsigned int bw    = (golay24128_encode23127(datb) >> 1) ^ p;
        unsigned int c     = (s * 40503U + 12345U) & 0x1FFFFFFU;

        for (unsigned int i = 0U; i < 24U; i++)
            DMR_FEC_WRITE(b, dmrFecPos(n, DMR_FEC_A[i]), (a >> (23U - i)) & 1U);
        for (unsigned int i = 0U; i < 23U; i++)
            DMR_FEC_WRITE(b, dmrFecPos(n, DMR_FEC_B[i]), (bw >> (22U - i)) & 1U);
        for (unsigned int i = 0U; i < 25U; i++)
            DMR_FEC_WRITE(b, dmrFecPos(n, DMR_FEC_C[i]), (c >> (24U - i)) & 1U);
    }
}

/*
 * makeCall()
 *
 * Builds the frames of a call as the gateway sends them to the host: control
 * byte 0x41 and the voice LC header, then voice bursts (0x20 for the burst
 * with the voice sync, 0x01 to 0x05 for the five that follow it, which carry
 * the EMB), then 0x42 and the terminator with LC.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void makeCall(void)
{
    setColorCode(1U, 0U);
    setPI(false, 0U);
    setLCSS(0U, 0U);

    DMRLC_setFLCO(0U, 0U); /* group call */
    DMRLC_setFID(0U, 0U);
    DMRLC_setSrcId(SRC_ID, 0U);
    DMRLC_setDstId(DST_ID, 0U);
    DMRLC_setPF(false, 0U);
    DMRLC_setOVCM(false, 0U);

    /* voice LC header */
    ctl[0] = 0x41U;
    memset(burst[0], 0x00, 33);
    DMRFullLC_encode(burst[0], DT_VOICE_LC_HEADER, 0U);
    DMRSlotType_encode(1U, DT_VOICE_LC_HEADER, burst[0]);
    addDMRDataSync(burst[0], false);

    /* voice bursts */
    for (int i = 0; i < VOICE_COUNT; i++)
    {
        uint8_t* b = burst[1 + i];

        makeVoice(b, (unsigned int)i + 1U);

        if (i % 6 == 0)
        {
            ctl[1 + i] = 0x20U;
            addDMRAudioSync(b, false);
        }
        else
        {
            ctl[1 + i] = (uint8_t)(i % 6);
            DMREMB_encode(b, 0U);
        }
    }

    /* terminator with LC */
    ctl[FRAMES - 1] = 0x42U;
    memset(burst[FRAMES - 1], 0x00, 33);
    DMRFullLC_encode(burst[FRAMES - 1], DT_TERMINATOR_WITH_LC, 0U);
    DMRSlotType_encode(1U, DT_TERMINATOR_WITH_LC, burst[FRAMES - 1]);
    addDMRDataSync(burst[FRAMES - 1], false);
}

/*
 * sameBurst()
 *
 * Compares two bursts, leaving out the middle (bytes 12 to 21) where the
 * sync, EMB and slot type are: the host may legitimately rewrite those.  The
 * last two bits are left out as well: the last symbol of a burst is the one
 * a demodulator can not be sure of.
 *
 * Parameters:
 *   a - First burst, 33 bytes.
 *   b - Second burst, 33 bytes.
 *
 * Returns: true if the two bursts are the same outside the middle.
 */
static bool sameBurst(const uint8_t* a, const uint8_t* b)
{
    return memcmp(a, b, 12) == 0 && memcmp(a + 22, b + 22, 10) == 0 && (a[32] & 0xFCU) == (b[32] & 0xFCU);
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
        txEots++;
}

/*
 * grabGw()
 *
 * Keeps a DMR frame the host sent to the gateway: the control byte, the 33
 * byte burst and the 7 id bytes (destination, source, group flag).
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

    if (memcmp(p->data + 4, "DMD1", 4) == 0 && p->len == 49 && gwCount < 400)
    {
        gwCtl[gwCount] = p->data[8];
        memcpy(gwBurst[gwCount], p->data + 9, 33);
        memcpy(gwIds[gwCount], p->data + 42, 7);
        gwCount++;
    }
    else
        gwOther++;
}

/*
 * damage()
 *
 * Damages a burst the way a weak signal does, but only in ways the error
 * protection can repair: data bursts get two bit errors in their BPTC block;
 * voice bursts B to F get a bad embedded signalling field and a bit error in
 * the EMB.
 *
 * Parameters:
 *   control - The control byte of the burst.
 *   b       - The burst, 33 bytes, changed in place.
 *
 * Returns: nothing.
 */
static void damage(uint8_t control, uint8_t* b)
{
    if ((control & 0x40U) != 0U)
    {
        b[3] ^= 0x20U;
        b[25] ^= 0x04U;
    }
    else if ((control & 0x20U) == 0U)
    {
        b[13] ^= 0x01U;
        b[14] = (uint8_t)((b[14] & 0xF0U) | 0x0AU);
        b[15] = 0x5AU;
        b[16] = 0xA5U;
        b[17] = 0x5AU;
    }
}

/*
 * checkRebuilt()
 *
 * Checks a relayed call against what the host should have rebuilt: the
 * header and the terminator decode to the link control of the call, with the
 * slot type of this host; the voice bursts B to E carry the embedded link
 * control (decoded from their four fragments), burst F carries none, and
 * every voice burst has the colour code of this host in its EMB.
 *
 * Parameters:
 *   label - Where the call was seen, for the messages.
 *   ctlv  - The control bytes of the bursts, in order.
 *   b     - The bursts, 33 bytes each.
 *   count - Number of bursts.
 *
 * Returns: nothing.
 */
static void checkRebuilt(const char* label, const uint8_t* ctlv, uint8_t (*b)[33], int count)
{
    const uint8_t want[9] = {0x00, 0x00, 0x00, 0x00, 0x00, (uint8_t)DST_ID, 0x00, 0x04, 0xD2};
    int headers = 0, terms = 0, headOk = 0, termOk = 0, voiceN = 0, embOk = 0, emb = 0, fOk = 0, fN = 0, ccOk = 0;
    int bad = 0;

    for (int i = 0; i < count; i++)
    {
        if ((ctlv[i] & 0x40U) != 0U)
        {
            uint8_t cc, dt;
            uint8_t lc[12];

            DMRSlotType_decode(b[i], &cc, &dt);
            if (dt == DT_VOICE_LC_HEADER || dt == DT_TERMINATOR_WITH_LC)
            {
                bool isHead = dt == DT_VOICE_LC_HEADER;
                bool ok     = cc == 1U && DMRFullLC_decode(b[i], dt, lc, 0U);

                if (ok)
                {
                    DMRLC_decode(lc, 0U);
                    ok = DMRLC_getSrcId(0U) == SRC_ID && DMRLC_getDstId(0U) == DST_ID;
                }

                /* the payload must be exactly what a clean encoder gives */
                if (ok && isHead)
                    ok = sameBurst(b[i], burst[0]);
                else if (ok)
                    ok = sameBurst(b[i], burst[FRAMES - 1]);

                if (isHead)
                {
                    headers++;
                    headOk += ok ? 1 : 0;
                }
                else
                {
                    terms++;
                    termOk += ok ? 1 : 0;
                }
            }
        }
        else if ((ctlv[i] & 0x20U) == 0U)
        {
            uint8_t n = ctlv[i] & 0x0FU;

            voiceN++;
            DMREMB_decode(b[i], 0U);
            if (getColorCode(0U) == 1U && !getPI(0U))
                ccOk++;

            if (n >= 1U && n <= 4U)
            {
                emb++;
                bool valid = DMREmbeddedData_decode(b[i], getLCSS(0U), 0U);
                uint8_t raw[9];

                if (n == 4U && valid && DMREmbeddedData_getRawData(raw, 0U) && memcmp(raw, want, 9) == 0)
                    embOk++;
                else if (n != 4U && getLCSS(0U) == (n == 1U ? 1U : 3U))
                    embOk++;
                else if (n == 4U)
                    bad++;
            }
            else if (n == 5U)
            {
                fN++;
                if (getLCSS(0U) == 0U && (b[i][14] & 0x0FU) == 0U && b[i][15] == 0U && b[i][16] == 0U && b[i][17] == 0U)
                    fOk++;
            }
        }
    }

    printf("  %s: %d bursts, %d voice\n", label, count, voiceN);
    simCheck(headers == 1 && headOk == 1, "%s: the header is rebuilt (%d of %d)", label, headOk, headers);
    simCheck(terms == 1 && termOk == 1, "%s: the terminator is rebuilt (%d of %d)", label, termOk, terms);
    simCheck(voiceN == VOICE_COUNT / 6 * 5 && ccOk == voiceN, "%s: every voice burst B to F has the EMB of this host (%d of %d)", label,
             ccOk, voiceN);
    simCheck(emb > 0 && embOk == emb && bad == 0, "%s: the embedded link control is rebuilt and decodes to the call (%d of %d)",
             label, embOk, emb);
    simCheck(fN > 0 && fOk == fN, "%s: burst F carries no embedded data (%d of %d)", label, fOk, fN);
}

/*
 * duplexTest()
 *
 * The duplex test (frame mode): a damaged call from the network, and a damaged
 * call from the radio, must both come out rebuilt, to the modem and to the
 * gateway respectively.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void duplexTest(void)
{
    uint8_t dirty[FRAMES][33];
    uint8_t outCtl[FRAMES + 4];
    uint8_t out[FRAMES + 4][33];
    int     outN = 0;

    memcpy(dirty, burst, sizeof(dirty));
    for (int i = 0; i < FRAMES; i++)
        damage(ctl[i], dirty[i]);

    printf("\nDuplex test 1: a damaged call from the network, relayed to the radio\n");

    for (int i = 0; i < FRAMES; i++)
    {
        uint8_t payload[34];

        payload[0] = ctl[i];
        memcpy(payload + 1, dirty[i], 33);
        simSend(&gwC, "DMD1", payload, 34);
        simDrain(&modemC, 20, 20, grabTx, NULL);
    }

    simDrain(&modemC, 1500, 6000, grabTx, NULL);

    for (int i = 0; i < txCount && outN < FRAMES + 4; i++)
    {
        SimPacket* p = txPackets[i];

        if (memcmp(p->data + 4, "DMD1", 4) == 0 && p->len == 43 && p->data[8] == 0x18U)
        {
            outCtl[outN] = p->data[9];
            memcpy(out[outN], p->data + 10, 33);
            outN++;
        }
    }

    checkRebuilt("to the modem", outCtl, out, outN);

    printf("\nDuplex test 2: a damaged call from the radio, relayed to the gateway and the radio\n");
    simDrain(&gwC, 100, 500, NULL, NULL);
    gwCount = 0;
    txCount = 0;

    for (int i = 0; i < FRAMES; i++)
    {
        uint8_t pkt[42] = {0x61, 0x00, 42, 0x04, 'D', 'M', 'D', '1'};

        pkt[8] = ctl[i];
        memcpy(pkt + 9, dirty[i], 33);
        simSendRaw(&modemC, pkt, 42);
        simDrain(&gwC, 20, 20, grabGw, NULL);
    }

    simDrain(&gwC, 1500, 6000, grabGw, NULL);

    uint8_t gb[400][33];

    memcpy(gb, gwBurst, sizeof(gb));
    checkRebuilt("to the gateway", gwCtl, gb, gwCount);
}

/*
 * dataTest()
 *
 * Duplex test of a data call from the network: a data header and four rate
 * 3/4 data blocks, two of them with bit errors that the trellis code can
 * repair.  The blocks the host sends to the modem must decode (trellis) to
 * the payloads that were sent and must be clean: encoding the decoded
 * payload again gives the same bits.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void dataTest(void)
{
    uint8_t  pl[4][18];
    uint8_t  frames[5][33];
    uint8_t  hdr[12] = {0x80, 0x00, 0x00, 0x00, 0x00, (uint8_t)DST_ID, 0x00, 0x04, 0xD2, 0x01, 0x00, 0x00};
    int      got = 0, good = 0, clean = 0;

    printf("\nDuplex test 4: a data call (rate 3/4 blocks) from the network, relayed to the radio\n");

    /* let what is left of the earlier tests go by first */
    simDrain(&modemC, 3000, 3000, grabTx, NULL);
    txCount = 0;

    /* data header, BPTC coded, then the rate 3/4 blocks */
    memset(frames[0], 0x00, 33);
    BPTC19696_encode(hdr, frames[0], 0U);
    DMRSlotType_encode(1U, DT_DATA_HEADER, frames[0]);
    addDMRDataSync(frames[0], false);

    for (int i = 0; i < 4; i++)
    {
        for (int k = 0; k < 18; k++)
            pl[i][k] = (uint8_t)(i * 53 + k * 29 + 7);

        memset(frames[1 + i], 0x00, 33);
        DMRTrellis_encode(pl[i], frames[1 + i]);
        DMRSlotType_encode(1U, DT_RATE_34_DATA, frames[1 + i]);
        addDMRDataSync(frames[1 + i], false);
    }

    /* two blocks get a bit error in the trellis part */
    frames[2][3] ^= 0x20U;
    frames[4][25] ^= 0x04U;

    for (int i = 0; i < 5; i++)
    {
        uint8_t payload[34];

        payload[0] = 0x41U;
        memcpy(payload + 1, frames[i], 33);
        simSend(&gwC, "DMD1", payload, 34);
        simDrain(&modemC, 60, 60, grabTx, NULL);
    }

    simDrain(&modemC, 2500, 8000, grabTx, NULL);

    for (int i = 0; i < txCount; i++)
    {
        SimPacket* p = txPackets[i];
        uint8_t    cc, dt, out[18], again[33];

        if (memcmp(p->data + 4, "DMD1", 4) != 0 || p->len != 43 || p->data[8] != 0x18U)
            continue;

        DMRSlotType_decode(p->data + 10, &cc, &dt);
        if (dt != DT_RATE_34_DATA)
            continue;

        got++;
        if (DMRTrellis_decode(p->data + 10, out))
        {
            for (int j = 0; j < 4; j++)
                if (memcmp(out, pl[j], 18) == 0)
                    good++;

            memcpy(again, p->data + 10, 33);
            DMRTrellis_encode(out, again);
            if (memcmp(again, p->data + 10, 33) == 0)
                clean++;
        }
    }

    simCheck(got == 4, "four rate 3/4 blocks reached the modem (%d)", got);
    simCheck(good == 4, "each block carries the payload that was sent (%d of 4)", good);
    simCheck(clean == 4, "each block is a clean code word, the bit errors are repaired (%d of 4)", clean);
}

/*
 * lateEntryTest()
 *
 * Late entry (frame mode, duplex): the call reaches the host from the radio
 * without its voice LC header, only as voice bursts that carry the embedded
 * link control.  The first superframe is clean; the second has damaged
 * embedded fields.  The host learns the link control from the first
 * superframe, so the bursts of the second come out rebuilt.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void lateEntryTest(void)
{
    const uint8_t want[9] = {0x00, 0x00, 0x00, 0x00, 0x00, (uint8_t)DST_ID, 0x00, 0x04, 0xD2};
    uint8_t late[VOICE_COUNT][33];
    uint8_t lctl[VOICE_COUNT];

    printf("\nDuplex test 3: late entry, a call from the radio that has no header\n");

    setColorCode(1U, 0U);
    setPI(false, 0U);
    DMRLC_setFLCO(0U, 0U);
    DMRLC_setFID(0U, 0U);
    DMRLC_setSrcId(SRC_ID, 0U);
    DMRLC_setDstId(DST_ID, 0U);
    DMRLC_setPF(false, 0U);
    DMRLC_setOVCM(false, 0U);
    DMREmbeddedData_setLC(0U);

    for (int i = 0; i < VOICE_COUNT; i++)
    {
        memcpy(late[i], burst[1 + i], 33);
        lctl[i] = ctl[1 + i];

        if ((lctl[i] & 0x20U) == 0U)
        {
            setLCSS(DMREmbeddedData_getData(late[i], lctl[i] & 0x0FU, 0U), 0U);
            DMREMB_encode(late[i], 0U);
        }

        if (i >= 6)
            damage(lctl[i], late[i]);
    }

    simDrain(&gwC, 100, 500, NULL, NULL);
    gwCount = 0;

    for (int i = 0; i < VOICE_COUNT; i++)
    {
        uint8_t pkt[42] = {0x61, 0x00, 42, 0x04, 'D', 'M', 'D', '1'};

        pkt[8] = lctl[i];
        memcpy(pkt + 9, late[i], 33);
        simSendRaw(&modemC, pkt, 42);
        simDrain(&gwC, 20, 20, grabGw, NULL);
    }

    simDrain(&gwC, 1500, 6000, grabGw, NULL);

    int emb = 0, embOk = 0, fN = 0, fOk = 0, ccOk = 0, later = 0;

    for (int i = 6; i < gwCount; i++)
    {
        uint8_t n = gwCtl[i] & 0x0FU;

        if ((gwCtl[i] & 0x60U) != 0U)
            continue;

        later++;
        DMREMB_decode(gwBurst[i], 0U);
        if (getColorCode(0U) == 1U)
            ccOk++;

        if (n >= 1U && n <= 4U)
        {
            emb++;
            bool valid = DMREmbeddedData_decode(gwBurst[i], getLCSS(0U), 0U);
            uint8_t raw[9];

            if (n == 4U && valid && DMREmbeddedData_getRawData(raw, 0U) && memcmp(raw, want, 9) == 0)
                embOk++;
            else if (n != 4U && getLCSS(0U) == (n == 1U ? 1U : 3U))
                embOk++;
        }
        else if (n == 5U)
        {
            fN++;
            if (getLCSS(0U) == 0U && (gwBurst[i][14] & 0x0FU) == 0U && gwBurst[i][15] == 0U && gwBurst[i][16] == 0U &&
                gwBurst[i][17] == 0U)
                fOk++;
        }
    }

    printf("  to the gateway: %d bursts, %d voice bursts after the first superframe\n", gwCount, later);
    simCheck(later == 5, "late entry: the second superframe reached the gateway (%d of 5 bursts)", later);
    simCheck(ccOk == later && later > 0, "late entry: the EMB of the second superframe is rebuilt (%d of %d)", ccOk, later);
    simCheck(emb > 0 && embOk == emb, "late entry: the embedded link control of the second superframe is rebuilt (%d of %d)", embOk, emb);
    simCheck(fN > 0 && fOk == fN, "late entry: burst F carries no embedded data (%d of %d)", fOk, fN);
}

/*
 * unlinkTest()
 *
 * Frame mode: a call from the network is cut off without its terminator,
 * first by the gateway telling the host it is unlinked (DISC) and then by the
 * gateway just going quiet.  Either way the host must end the call itself and
 * tell the modem to stop (a start packet with the value 0); a host that does
 * not stays in TX for ever.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void unlinkTest(void)
{
    const uint8_t name[9] = {'B', 'M', ' ', '3', '1', '0', '2', ' ', ' '};

    makeCall();

    for (int variant = 0; variant < 2; variant++)
    {
        printf("\nTest %d: the call from the network is cut off (%s)\n", 4 + variant,
               variant == 0 ? "the gateway is unlinked" : "the gateway goes quiet");

        for (int i = 0; i < txCount; i++)
            free(txPackets[i]);
        txCount = 0;
        simDrain(&modemC, 300, 1000, NULL, NULL);

        for (int i = 0; i < 8; i++)
        {
            uint8_t payload[34];

            payload[0] = ctl[i];
            memcpy(payload + 1, burst[i], 33);
            simSend(&gwC, "DMD1", payload, 34);
            simDrain(&modemC, gapMs, gapMs, grabTx, NULL);
        }

        int before = txCount;

        if (variant == 0)
            simSend(&gwC, "DISC", name, 9);

        simDrain(&modemC, variant == 0 ? 1500 : 2500, 8000, grabTx, NULL);

        bool stop = false;

        for (int i = before; i < txCount; i++)
            if (memcmp(txPackets[i]->data + 4, "DMD1", 4) == 0 && txPackets[i]->len == 10 && txPackets[i]->data[8] == 0x1DU &&
                txPackets[i]->data[9] == 0x00U)
                stop = true;

        simCheck(stop, "the host ended the call and told the modem to stop");
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
        else if (strcmp(argv[i], "--duplex") == 0)
            duplex = true;
        else
        {
            fprintf(stderr, "Usage: dmr_sim [-m modem] [--modem mmdvm|openmt|openmths] [--gap ms] [--duplex]\n");
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
        printf("dmr_sim: cannot listen on the modem port\n");
        return 2;
    }

    printf("dmr_sim: waiting for dmr_host on port %d (modem type %s)...\n", 18000 + modem - 1, kindName);

    int mfd = simAccept(lfd, 30000);

    if (mfd < 0)
    {
        printf("dmr_sim: host did not connect\n");
        return 2;
    }

    simConnInit(&modemC, mfd);

    SimPacket reg;

    if (simRead(&modemC, &reg, 10000) != 1)
    {
        printf("dmr_sim: no registration from the host\n");
        return 2;
    }

    simCheck(memcmp(reg.data + 4, "MODE", 4) == 0 && memcmp(reg.data + 8, "DMR", 3) == 0,
             "MODE registration for DMR (length %d)", reg.len);

    /* 2. play the gateway */
    int gfd = simConnect((uint16_t)(18400 + modem - 1), 50);

    if (gfd < 0)
    {
        printf("dmr_sim: could not connect to the gateway port\n");
        return 2;
    }

    simConnInit(&gwC, gfd);
    usleep(1500000);
    simDrain(&modemC, 300, 1000, NULL, NULL);

    if (duplex)
    {
        if (kind != KIND_FRAME)
        {
            printf("dmr_sim: the duplex test works with the mmdvm (frame) modem only\n");
            return 2;
        }

        /* tell the host the modem is a duplex one (sent as the modem host does) */
        const uint8_t comm[1] = {0x00};

        simSend(&modemC, "COMM", comm, 1);
        usleep(300000);
        makeCall();
        duplexTest();
        lateEntryTest();
        dataTest();
        printf("\n%s (%d checks, %d failed)\n", simFails == 0 ? "PASS" : "FAIL", simChecks, simFails);

        return simFails == 0 ? 0 : 1;
    }

    /* 3. network -> radio */
    makeCall();
    printf("\nTest 1: call from the network to the radio (header, %d voice bursts, terminator)\n", VOICE_COUNT);

    if (getenv("DMR_SIM_DEBUG") != NULL)
        for (int i = 0; i < FRAMES; i++)
        {
            printf("  sent ctl %02X:", ctl[i]);
            for (int k = 0; k < 33; k++)
                printf(" %02X", burst[i][k]);
            printf("\n");
        }

    for (int i = 0; i < FRAMES; i++)
    {
        uint8_t payload[34];

        payload[0] = ctl[i];
        memcpy(payload + 1, burst[i], 33);
        simSend(&gwC, "DMD1", payload, 34);
        simDrain(&modemC, gapMs, gapMs, grabTx, NULL); /* exactly one gap, so the bursts leave at the rate of the air */
    }

    simDrain(&modemC, 4000, 60000, grabTx, NULL);

    printf("  host sent %d packets and %d commands to the modem\n", txCount, txCommands);

    if (getenv("DMR_SIM_DUMP") != NULL)
    {
        FILE* f = fopen(getenv("DMR_SIM_DUMP"), "wb");

        for (int i = 0; f != NULL && i < txCount; i++)
            fwrite(txPackets[i]->data + 8, 1, (size_t)txPackets[i]->len - 8U, f);
        if (f != NULL)
            fclose(f);
    }

    if (kind == KIND_FRAME)
    {
        int sent = 0, matched = 0;

        for (int i = 0; i < txCount; i++)
        {
            SimPacket* p = txPackets[i];

            if (memcmp(p->data + 4, "DMD1", 4) == 0 && p->len == 43 && p->data[8] == 0x18U)
            {
                sent++;
                for (int j = 0; j < FRAMES; j++)
                    if (p->data[9] == ctl[j] && sameBurst(p->data + 10, burst[j]))
                    {
                        matched++;
                        break;
                    }
            }
        }

        simCheck(sent == FRAMES, "frame mode: %d of %d bursts reached the modem, the terminator included", sent, FRAMES);
        simCheck(matched == sent, "frame mode: every burst is one that was sent (%d of %d)", matched, sent);
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

        /* a burst is 33 bytes, 132 symbols; in sample mode a symbol takes 5 samples of 2 bytes */
        size_t need = (size_t)FRAMES * 33U * (kind == KIND_SAMP ? 40U : 1U);

        simCheck(wrongType == 0, "every packet is a %s packet (%d other)", want, wrongType);
        simCheck(bytes >= need, "%zu payload bytes sent, at least %zu needed for the bursts", bytes, need);
        if (kind == KIND_SAMP)
            simCheck(txEots >= 1, "the transmission ends with an EOT packet (%d seen)", txEots);
    }

    /* 4. radio -> network */
    printf("\nTest 2: the transmission comes back from the radio, to the network\n");
    simDrain(&gwC, 100, 500, NULL, NULL);

    if (kind == KIND_BITS)
    {
        /* The modem sends 190 bytes of bits in each BITS packet and ends the
           packet with two bytes that tell where in the transmit stream the
           last bit came (0xFFFF when it is not transmitting).  So the bits
           of the transmission are cut into packets of 190 bytes again. */
        size_t total = 0U;

        for (int i = 0; i < txCount; i++)
            total += (size_t)txPackets[i]->len - 8U;

        uint8_t* stream = malloc(total + 190U);
        size_t   fill   = 0U;

        memset(stream, 0x00, total + 190U);
        for (int i = 0; i < txCount; i++)
        {
            memcpy(stream + fill, txPackets[i]->data + 8, (size_t)txPackets[i]->len - 8U);
            fill += (size_t)txPackets[i]->len - 8U;
        }

        for (size_t at = 0U; at < total; at += 190U)
        {
            uint8_t pkt[200] = {0x61, 0x00, 0xC8, 0x04, 'B', 'I', 'T', 'S'};

            memcpy(pkt + 8, stream + at, 190);
            pkt[198] = 0xFFU;
            pkt[199] = 0xFFU;

            simSendRaw(&modemC, pkt, 200);
            simDrain(&gwC, 1, 1, grabGw, NULL);
            usleep(158000); /* 190 bytes are 1520 bits, at 9600 bit/s */
        }

        free(stream);
    }
    else
        for (int i = 0; i < txCount; i++)
        {
            SimPacket q = *txPackets[i];

            if (kind == KIND_FRAME)
            {
                if (memcmp(q.data + 4, "DMD1", 4) != 0 || q.len != 43 || q.data[8] != 0x18U)
                    continue;

                /* as the modem host delivers a burst from the radio: control byte and burst */
                memmove(q.data + 8, q.data + 9, 34);
                q.data[2] = 42;
                q.len     = 42;
            }
            else
                memcpy(q.data + 4, "SAMP", 4);

            simSendRaw(&modemC, q.data, q.len);
            simDrain(&gwC, 1, 1, grabGw, NULL);

            /* the samples arrive at the rate of the air: 96 samples are 4 ms at 24 kHz */
            usleep(kind == KIND_FRAME ? gapMs * 1000 : 4000);
        }

    simDrain(&gwC, 4000, 30000, grabGw, NULL);

    printf("  gateway got %d bursts (%d other packets)\n", gwCount, gwOther);

    int header = -1, term = -1, voice = 0, voiceOk = 0;

    if (getenv("DMR_SIM_DEBUG") != NULL)
    {
        printf("  control bytes received:");
        for (int i = 0; i < gwCount; i++)
            printf(" %02X", gwCtl[i]);
        printf("\n");
    }

    for (int i = 0; i < gwCount; i++)
    {
        if ((gwCtl[i] & 0x4FU) == 0x41U && header < 0 && sameBurst(gwBurst[i], burst[0]))
            header = i;
        else if ((gwCtl[i] & 0x4FU) == 0x42U && sameBurst(gwBurst[i], burst[FRAMES - 1]))
            term = i;
        else if ((gwCtl[i] & 0x40U) == 0U)
        {
            voice++;
            if (getenv("DMR_SIM_DEBUG") != NULL)
            {
                printf("  voice ctl %02X:", gwCtl[i]);
                for (int k = 0; k < 33; k++)
                    printf(" %02X", gwBurst[i][k]);
                printf("\n");
            }
            for (int j = 1; j <= VOICE_COUNT; j++)
                if (sameBurst(gwBurst[i], burst[j]))
                {
                    voiceOk++;
                    break;
                }
        }
    }

    simCheck(header >= 0, "the voice LC header was received");
    if (header >= 0)
    {
        unsigned int dst = ((unsigned int)gwIds[header][0] << 16) | ((unsigned int)gwIds[header][1] << 8) | gwIds[header][2];
        unsigned int src = ((unsigned int)gwIds[header][3] << 16) | ((unsigned int)gwIds[header][4] << 8) | gwIds[header][5];

        simCheck(src == SRC_ID && dst == DST_ID, "header ids: source %u destination %u (expected %u and %u)", src, dst,
                 SRC_ID, DST_ID);
        simCheck(gwIds[header][6] == 1, "the call is marked as a group call");
    }
    simCheck(voiceOk >= VOICE_COUNT - 1, "voice bursts received that match one sent: %d of %d (%d voice bursts in all)",
             voiceOk, VOICE_COUNT, voice);
    simCheck(voice <= VOICE_COUNT + 1, "no voice bursts added (%d)", voice);
    simCheck(term >= 0, "the terminator was received");

    if (kind == KIND_FRAME)
        unlinkTest();

    printf("\n%s (%d checks, %d failed)\n", simFails == 0 ? "PASS" : "FAIL", simChecks, simFails);

    return simFails == 0 ? 0 : 1;
}
