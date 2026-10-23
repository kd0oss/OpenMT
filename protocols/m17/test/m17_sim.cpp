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
 * m17_sim: plays the modem host and the M17 gateway for m17_host.
 *
 *   1. listens on the modem host port and accepts the host's connection,
 *   2. connects to the host's gateway port and links (CONN),
 *   3. network -> radio: sends a call (link setup frame, stream frames, end
 *      of transmission) the way the gateway does and captures what the host
 *      sends to the modem,
 *   4. radio -> network: sends the captured transmission back to the host as
 *      what a modem of this type delivers (frames, samples or bits) and
 *      checks the frames that come out of the gateway port.
 *
 * Run once for each modem type, with the host started with the same type
 * (see run_sim.sh):
 *
 * Usage: m17_sim [-m modem 1-10] [--modem mmdvm|openmt|openmths]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <M17/M17Datatypes.hpp>
#include <M17/M17FrameDecoder.hpp>
#include <M17/M17FrameEncoder.hpp>
#include <M17/M17LinkSetupFrame.hpp>

extern "C"
{
#include "sim_common.h"
}

#define STREAM_FRAMES 12
#define MAX_PACKETS   6000

enum { KIND_SAMP, KIND_BITS, KIND_FRAME };

static SimConn modemC;
static SimConn gwC;
static int     kind = KIND_SAMP;
static int     gapMs = 40; /* time between the frames the gateway sends; a real M17 frame is 40 ms */

/* One frame as the gateway sends it: type and the 48 bytes */
struct Frame
{
    char    type[5];
    uint8_t data[48];
};

static Frame callFrames[STREAM_FRAMES + 2];
static int   callCount = 0;

/* What the host sent to the modem */
static SimPacket* txPackets[MAX_PACKETS];
static int        txCount    = 0;
static int        txEotFlag  = 0;
static int        txCommands = 0;

/* What the host sent to the gateway */
static Frame gwFrames[200];
static int   gwCount = 0;
static int   gwOther = 0;

/*
 * buildCall()
 *
 * Makes the frames of a voice call as the M17 gateway does: a link setup
 * frame, stream frames with the last one marked, and the end of
 * transmission.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void buildCall(void)
{
    M17::M17LinkSetupFrame lsf;
    M17::M17FrameEncoder   encoder;
    M17::frame_t           frame;
    M17::payload_t         payload;

    lsf.setSource("N0CALL");
    lsf.setDestination("ALL");

    M17::streamType_t st;
    st.fields.dataMode   = 1;
    st.fields.dataType   = 2;
    st.fields.encType    = 0;
    st.fields.encSubType = 0;
    st.fields.CAN        = 0;
    lsf.setType(st);
    lsf.updateCrc();

    encoder.reset();
    callCount = 0;

    encoder.encodeLsf(lsf, frame);
    strcpy(callFrames[callCount].type, "M17L");
    memcpy(callFrames[callCount].data, frame.data(), 48);
    callCount++;

    for (int i = 0; i < STREAM_FRAMES; i++)
    {
        for (size_t j = 0; j < payload.size(); j++)
            payload[j] = (uint8_t)(i * 29 + j * 7 + 3);

        encoder.encodeStreamFrame(payload, frame, i == STREAM_FRAMES - 1);
        strcpy(callFrames[callCount].type, "M17S");
        memcpy(callFrames[callCount].data, frame.data(), 48);
        callCount++;
    }

    encoder.encodeEotFrame(frame);
    strcpy(callFrames[callCount].type, "M17E");
    memcpy(callFrames[callCount].data, frame.data(), 48);
    callCount++;
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
        txPackets[txCount] = (SimPacket*)malloc(sizeof(SimPacket));
        memcpy(txPackets[txCount], p, sizeof(SimPacket));
        txCount++;
    }

    if (memcmp(p->data + 4, "SEOT", 4) == 0)
        txEotFlag++;
}

/*
 * grabGw()
 *
 * Keeps an M17 frame the host sent to the gateway.
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

    if (memcmp(p->data + 4, "M17", 3) == 0 && p->len == 56 && gwCount < 200)
    {
        memcpy(gwFrames[gwCount].type, p->data + 4, 4);
        gwFrames[gwCount].type[4] = '\0';
        memcpy(gwFrames[gwCount].data, p->data + 8, 48);
        gwCount++;
    }
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
    const uint8_t name[8] = {'M', '1', '7', 'R', 'E', 'F', ' ', 'A'};

    for (int variant = 0; variant < 2; variant++)
    {
        printf("\nTest %d: the call from the network is cut off (%s)\n", 3 + variant,
               variant == 0 ? "the gateway is unlinked" : "the gateway goes quiet");

        for (int i = 0; i < txCount; i++)
            free(txPackets[i]);
        txCount = 0;
        simDrain(&modemC, 300, 1000, NULL, NULL);

        for (int i = 0; i < 6 && i < callCount - 1; i++)
        {
            simSend(&gwC, callFrames[i].type, callFrames[i].data, 48);
            simDrain(&modemC, gapMs, gapMs * 2, grabTx, NULL);
        }

        int before = txCount;

        if (variant == 0)
            simSend(&gwC, "DISC", name, 8);

        simDrain(&modemC, variant == 0 ? 1500 : 2500, 8000, grabTx, NULL);

        bool eot = false;

        for (int i = before; i < txCount; i++)
            if (memcmp(txPackets[i]->data + 4, "M17E", 4) == 0)
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
            fprintf(stderr, "Usage: m17_sim [-m modem] [--modem mmdvm|openmt|openmths] [--gap ms]\n");
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
        printf("m17_sim: cannot listen on the modem port\n");
        return 2;
    }

    printf("m17_sim: waiting for m17_host on port %d (modem type %s)...\n", 18000 + modem - 1, kindName);

    int mfd = simAccept(lfd, 30000);

    if (mfd < 0)
    {
        printf("m17_sim: host did not connect\n");
        return 2;
    }

    simConnInit(&modemC, mfd);

    SimPacket reg;

    if (simRead(&modemC, &reg, 10000) != 1)
    {
        printf("m17_sim: no registration from the host\n");
        return 2;
    }

    simCheck(memcmp(reg.data + 4, "MODE", 4) == 0 && memcmp(reg.data + 8, "M17", 3) == 0,
             "MODE registration for M17 (length %d)", reg.len);

    /* 2. play the gateway */
    int gfd = simConnect((uint16_t)(18200 + modem - 1), 50);

    if (gfd < 0)
    {
        printf("m17_sim: could not connect to the gateway port\n");
        return 2;
    }

    simConnInit(&gwC, gfd);

    uint8_t conn[8] = {'M', '1', '7', 'R', 'E', 'F', ' ', 'A'};

    simSend(&gwC, "CONN", conn, 8);
    usleep(500000);
    simDrain(&modemC, 300, 1000, NULL, NULL);

    /* 3. network -> radio */
    buildCall();
    printf("\nTest 1: call from the network to the radio (%d frames)\n", callCount);

    for (int i = 0; i < callCount; i++)
    {
        simSend(&gwC, callFrames[i].type, callFrames[i].data, 48);
        simDrain(&modemC, gapMs, gapMs * 2, grabTx, NULL);
    }

    simDrain(&modemC, 2500, 40000, grabTx, NULL);

    printf("  host sent %d packets and %d commands to the modem\n", txCount, txCommands);

    if (kind == KIND_FRAME)
    {
        int lsfs = 0, streams = 0, eots = 0;

        for (int i = 0; i < txCount; i++)
        {
            if (memcmp(txPackets[i]->data + 4, "M17L", 4) == 0) lsfs++;
            else if (memcmp(txPackets[i]->data + 4, "M17S", 4) == 0) streams++;
            else if (memcmp(txPackets[i]->data + 4, "M17E", 4) == 0) eots++;
        }

        {
            M17::M17FrameDecoder dec;
            printf("  stream frame numbers sent:    ");
            for (int i = 0; i < callCount; i++)
            {
                if (strcmp(callFrames[i].type, "M17S") != 0)
                    continue;
                M17::frame_t f;
                memcpy(f.data(), callFrames[i].data, 48);
                dec.decodeFrame(f);
                { M17::M17StreamFrame sf = dec.getStreamFrame(); printf(" %04X", sf.getFrameNumber()); }
            }
            printf("\n  stream frame numbers at modem:");
            for (int i = 0; i < txCount; i++)
            {
                if (memcmp(txPackets[i]->data + 4, "M17S", 4) != 0)
                    continue;
                M17::frame_t f;
                memcpy(f.data(), txPackets[i]->data + 8, 48);
                dec.decodeFrame(f);
                { M17::M17StreamFrame sf = dec.getStreamFrame(); printf(" %04X", sf.getFrameNumber()); }
            }
            printf("\n");
        }
        simCheck(lsfs == 1, "frame mode: one link setup frame reached the modem (%d)", lsfs);
        simCheck(streams == STREAM_FRAMES, "frame mode: %d of %d stream frames reached the modem", streams, STREAM_FRAMES);
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

        /* the 48 byte frames are 192 symbols of 2 bits, 5 samples of 2 bytes each */
        size_t need = (size_t)(STREAM_FRAMES + 1) * 48U * (kind == KIND_SAMP ? 40U : 1U);

        simCheck(wrongType == 0, "every packet is a %s packet (%d other)", want, wrongType);
        simCheck(bytes >= need, "%zu payload bytes sent, at least %zu needed for the frames", bytes, need);
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
            if (memcmp(q.data + 4, "M17", 3) != 0)
                continue;
        }
        else if (kind == KIND_SAMP)
            memcpy(q.data + 4, "SAMP", 4);

        simSendRaw(&modemC, q.data, q.len);
        simDrain(&gwC, 1, 1, grabGw, NULL);
        usleep(kind == KIND_FRAME ? 40000 : 1500);
    }

    simDrain(&gwC, 3000, 25000, grabGw, NULL);

    int lsfs = 0, streams = 0, eots = 0;
    bool callOk = false;
    M17::M17FrameDecoder dec;

    for (int i = 0; i < gwCount; i++)
    {
        M17::frame_t frame;

        memcpy(frame.data(), gwFrames[i].data, 48);

        if (strcmp(gwFrames[i].type, "M17L") == 0)
        {
            lsfs++;
            dec.decodeFrame(frame);
            M17::M17LinkSetupFrame l = dec.getLsf();

            if (l.valid() && l.getSource() == "N0CALL")
                callOk = true;
        }
        else if (strcmp(gwFrames[i].type, "M17S") == 0)
            streams++;
        else if (strcmp(gwFrames[i].type, "M17E") == 0)
            eots++;
    }

    printf("  gateway got %d frames: %d link setup, %d stream, %d end (%d other packets)\n", gwCount, lsfs, streams, eots, gwOther);

    simCheck(lsfs >= 1, "a link setup frame was received (%d)", lsfs);
    simCheck(callOk, "the link setup frame decodes with the source N0CALL");
    simCheck(streams >= STREAM_FRAMES - 2, "stream frames received: %d of %d", streams, STREAM_FRAMES);
    simCheck(streams == STREAM_FRAMES, "no stream frame lost");

    if (kind == KIND_FRAME)
        unlinkTest();

    printf("\n%s (%d checks, %d failed)\n", simFails == 0 ? "PASS" : "FAIL", simChecks, simFails);

    return simFails == 0 ? 0 : 1;
}
