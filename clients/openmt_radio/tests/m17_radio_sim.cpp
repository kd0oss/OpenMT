/* Plays the modem host and the gateway for the repo's m17_host, like protocols/m17/test/m17_sim.cpp, but the radio is the
 * OpenMT Radio program: its transmission (samples.raw, from m17_interop_tool capture) is delivered to m17_host as the modem
 * delivers it, and the frames that come out of the gateway port are checked: the link setup frame, the stream frames and
 * the speech inside them, the end.
 *
 * Usage: m17_radio_sim <samples.raw> [--packet]   (--packet: the radio sent a text message in packet mode) */

#define MESSAGE "Testing M17 packet mode from OpenMT Radio: the quick brown fox jumps over the lazy dog, 0123456789."

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <M17/M17Datatypes.hpp>
#include <M17/M17FrameDecoder.hpp>
#include <M17/M17LinkSetupFrame.hpp>

extern "C"
{
#include "sim_common.h"
}

#define VOICE_FRAMES 10U

struct Frame
{
    char    type[5];
    uint8_t data[48];
};

static SimConn modemC, gwC;
static Frame   gwFrames[200];
static int     gwCount = 0, gwOther = 0, gwEots = 0;

static void grabGw(const SimPacket* p, void* arg)
{
    (void)arg;

    if (memcmp(p->data + 4, "M17E", 4) == 0)
        gwEots++;
    else if (memcmp(p->data + 4, "M17", 3) == 0 && p->len == 56 && gwCount < 200)
    {
        memcpy(gwFrames[gwCount].type, p->data + 4, 4);
        gwFrames[gwCount].type[4] = '\0';
        memcpy(gwFrames[gwCount].data, p->data + 8, 48);
        gwCount++;
    }
    else
        gwOther++;
}

/* block b of the call, as m17_interop_tool.cpp makes it */
static void want(unsigned int b, uint8_t* block)
{
    for (unsigned int k = 0U; k < 8U; k++)
        block[k] = (uint8_t)(0x1DU * (k + 1U) + 53U * b + 7U);
}

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
    {
        fprintf(stderr, "usage: m17_radio_sim <samples.raw> [--packet]\n");
        return 2;
    }
    bool packetMode = argc == 3 && strcmp(argv[2], "--packet") == 0;

    FILE* f = fopen(argv[1], "rb");

    if (!f)
    {
        printf("m17_radio_sim: cannot read the radio's transmission\n");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* raw = (uint8_t*)malloc((size_t)bytes + 400);
    memset(raw, 0, (size_t)bytes + 400);
    if (fread(raw, 1, (size_t)bytes, f) != (size_t)bytes)
        return 2;
    fclose(f);

    int lfd = simListen(18000);
    printf("m17_radio_sim: waiting for m17_host on port 18000...\n");
    int mfd = simAccept(lfd, 30000);
    if (mfd < 0)
    {
        printf("m17_radio_sim: host did not connect\n");
        return 2;
    }
    simConnInit(&modemC, mfd);

    SimPacket reg;
    if (simRead(&modemC, &reg, 10000) != 1 || memcmp(reg.data + 8, "M17", 3) != 0)
    {
        printf("m17_radio_sim: no registration for M17\n");
        return 2;
    }

    int gfd = simConnect(18200, 50);
    if (gfd < 0)
    {
        printf("m17_radio_sim: could not connect to the gateway port\n");
        return 2;
    }
    simConnInit(&gwC, gfd);

    uint8_t conn[8] = {'M', '1', '7', 'R', 'E', 'F', ' ', 'A'};
    simSend(&gwC, "CONN", conn, 8);
    usleep(500000);
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

    int lsfs = 0, streams = 0, eots = gwEots, right = 0, total = 0;
    bool callOk = false, canOk = false;
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

            if (l.valid() && l.getSource() == "KD0OSS" && l.getDestination() == "ALL")
                callOk = true;
            if (l.valid() && l.getType().fields.CAN == 5U)
                canOk = true;
        }
        else if (strcmp(gwFrames[i].type, "M17S") == 0)
        {
            dec.decodeFrame(frame);

            M17::M17StreamFrame sf = dec.getStreamFrame();
            uint8_t exp[16];

            want((unsigned int)streams * 2U, exp);
            want((unsigned int)streams * 2U + 1U, exp + 8);
            total++;
            if (memcmp(sf.payload().data(), exp, 16) == 0)
                right++;
            streams++;
        }
    }

    printf("gateway got %d frames: %d link setup, %d stream, %d end (%d other packets)\n", gwCount, lsfs, streams, eots, gwOther);

    if (packetMode)
    {
        /* the packet frames: 25 bytes each and a byte with the end flag and the frame number (or the byte count in the last) */
        uint8_t packet[900];
        size_t  len = 0U;
        int     pkts = 0;
        bool    done = false, srcOk = false;

        for (int i = 0; i < gwCount; i++)
        {
            M17::frame_t frame;

            memcpy(frame.data(), gwFrames[i].data, 48);
            dec.decodeFrame(frame);
            if (strcmp(gwFrames[i].type, "M17L") == 0)
            {
                M17::M17LinkSetupFrame l = dec.getLsf();

                srcOk = l.valid() && l.getSource() == "KD0OSS" && l.getDestination() == "W1AW" && l.getType().fields.dataMode == 0U;
            }
            else if (strcmp(gwFrames[i].type, "M17P") == 0 && !done)
            {
                M17::M17PacketFrame pf = dec.getPacketFrame();
                const M17::pktPayload_t& pl = pf.payload();
                unsigned int n = (unsigned int)((pl[25] >> 2) & 0x1F);

                pkts++;
                if (pl[25] & 0x80)
                {
                    memcpy(packet + len, pl.data(), n < 25U ? n : 25U);
                    len += n < 25U ? n : 25U;
                    done = true;
                }
                else
                {
                    memcpy(packet + len, pl.data(), 25U);
                    len += 25U;
                }
            }
        }

        M17::M17LinkSetupFrame helper;
        bool crcOk = len > 3U && (uint16_t)(((unsigned int)packet[len - 2U] << 8) | packet[len - 1U]) ==
                                     __builtin_bswap16(helper.m17Crc(packet, len - 2U));
        bool textOk = len > 3U && packet[0] == 0x05U && strlen((const char*)packet + 1) == strlen(MESSAGE) &&
                      memcmp(packet + 1, MESSAGE, strlen(MESSAGE)) == 0;

        simCheck(srcOk, "the link setup frame is a packet mode call from KD0OSS to W1AW");
        simCheck(pkts >= 2 && done, "packet frames reached the gateway, the last one flagged (%d)", pkts);
        simCheck(crcOk, "the CRC of the packet is right");
        simCheck(textOk, "the packet is a text message (type 05) with the text that was sent");
        simCheck(eots >= 1, "the end of the call reached the gateway");
        printf("\n%s (%d checks, %d failed)\n", simFails == 0 ? "PASS" : "FAIL", simChecks, simFails);
        return simFails == 0 ? 0 : 1;
    }

    simCheck(lsfs >= 1, "a link setup frame reached the gateway (%d)", lsfs);
    simCheck(callOk, "the link setup frame decodes with source KD0OSS and destination ALL");
    simCheck(canOk, "the link setup frame carries channel access number 5");
    simCheck(streams == (int)VOICE_FRAMES, "stream frames at the gateway: %d of %u sent", streams, VOICE_FRAMES);
    simCheck(total > 0 && right == total, "stream frame payloads identical to the ones sent, in order: %d of %d", right, total);
    simCheck(eots >= 1, "the end of the call reached the gateway");

    printf("\n%s (%d checks, %d failed)\n", simFails == 0 ? "PASS" : "FAIL", simChecks, simFails);
    return simFails == 0 ? 0 : 1;
}
