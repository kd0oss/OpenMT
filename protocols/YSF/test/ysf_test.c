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
 * Scenario tests of the YSF control: RF to network, late entry, access
 * control, network to RF, the network watchdog and signal loss.
 *
 *   gcc -I.. -I../.. ysf_test.c ../ysf_func.c ../../dsp_tools.c -lm -o ysf_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dsp_tools.h>

#include "ysf_func.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t WHITE[] = {0x93U, 0xD7U, 0x51U, 0x21U, 0x9CU, 0x2FU, 0x6CU, 0xD0U, 0xEFU, 0x0FU, 0xF8U, 0x3DU, 0xF1U};

typedef struct
{
    uint8_t body[300][YSF_NET_BODY_LENGTH];
    int nNet;
    int netEnd;
    uint8_t rf[300][YSF_FRAME_LENGTH_BYTES];
    int nRf;
    int rfEot;
    YsfCall ev[40];
    int nEv;
} Sink;

static void onNet(void* ctx, const uint8_t* body, bool end)
{
    Sink* s = (Sink*)ctx;
    memcpy(s->body[s->nNet++], body, YSF_NET_BODY_LENGTH);
    if (end) s->netEnd++;
}

static void onRf(void* ctx, const uint8_t* frame, bool eot)
{
    Sink* s = (Sink*)ctx;
    memcpy(s->rf[s->nRf++], frame, YSF_FRAME_LENGTH_BYTES);
    if (eot) s->rfEot++;
}

static void onEvent(void* ctx, const YsfCall* c)
{
    Sink* s = (Sink*)ctx;
    s->ev[s->nEv++] = *c;
}

static void setup(YsfCtrl* c, Sink* s, const char* call)
{
    memset(s, 0, sizeof(*s));
    ysfCtrlInit(c);
    ysfCtrlSetCallsign(c, call);
    c->sendNet = onNet;
    c->sendRf = onRf;
    c->event = onEvent;
    c->ctx = s;
    c->localRepeat = false;
}

static void pad(uint8_t* d, const char* s)
{
    memset(d, ' ', 10);
    memcpy(d, s, strlen(s));
}

/* header / terminator frame */
static void mkHeader(uint8_t* f, uint8_t fi, uint8_t cm, uint8_t dt, const char* dest, const char* src)
{
    uint8_t csd1[20], csd2[20];
    YsfFich h;

    memset(f, 0, YSF_FRAME_LENGTH_BYTES);
    ysfAddSync(f);
    ysfFichInit(&h);
    ysfFichSetFI(&h, fi);
    ysfFichSetCM(&h, cm);
    ysfFichSetFT(&h, 7);
    ysfFichSetDT(&h, dt);
    ysfFichSetDGId(&h, 0);
    ysfFichEncode(&h, f);
    pad(csd1, dest);
    pad(csd1 + 10, src);
    pad(csd2, "        ");
    pad(csd2 + 10, "        ");
    ysfPayloadWriteHeader(f, csd1, csd2);
}

/* V/D mode 2 data channel: 10 bytes, same code as the payload code uses */
static void mkShort(uint8_t* frame, const uint8_t* ten)
{
    uint8_t out[13] = {0}, conv[25], bytes[25];

    memset(bytes, 0, sizeof(bytes));
    for (int i = 0; i < 10; i++) out[i] = ten[i] ^ WHITE[i];
    CRC_addCCITT162(out, 12);
    out[12] = 0;
    ysfConvEncode(out, conv, 100);

    unsigned j = 0;
    for (unsigned i = 0; i < 100; i++)
    {
        unsigned n = 2 * (i / 5) + 40 * (i % 5);
        int s0 = (conv[j >> 3] >> (7 - (j & 7))) & 1; j++;
        int s1 = (conv[j >> 3] >> (7 - (j & 7))) & 1; j++;
        if (s0) bytes[n >> 3] |= 0x80 >> (n & 7);
        n++;
        if (s1) bytes[n >> 3] |= 0x80 >> (n & 7);
    }
    for (int i = 0; i < 5; i++) memcpy(frame + 30 + i * 18, bytes + i * 5, 5);
}

/* A V/D mode 2 frame carrying the callsign for frame number fn */
static void mkVd2(uint8_t* f, uint8_t fn, uint8_t cm, const char* dest, const char* src)
{
    YsfFich h;
    uint8_t ten[10];

    memset(f, 0, YSF_FRAME_LENGTH_BYTES);
    for (int i = 0; i < 120; i++) f[i] = (uint8_t)rand();
    ysfAddSync(f);
    ysfFichInit(&h);
    ysfFichSetFI(&h, YSF_FI_COMMUNICATIONS);
    ysfFichSetCM(&h, cm);
    ysfFichSetFN(&h, fn);
    ysfFichSetFT(&h, 6);
    ysfFichSetDT(&h, YSF_DT_VD_MODE2);
    ysfFichEncode(&h, f);

    memset(ten, ' ', 10);
    if (fn == 0) pad(ten, dest);
    else if (fn == 1) pad(ten, src);
    mkShort(f, ten);

    /* voice channels: every bit three times, so the vote finds no errors */
    for (int j = 0; j < 5; j++)
    {
        unsigned offset = 40 + 144 * j;
        uint8_t vch[13] = {0};
        for (int i = 0; i < 81; i += 3)
        {
            int b = rand() & 1;
            for (int k = 0; k < 3; k++)
                if (b) vch[(i + k) >> 3] |= 0x80 >> ((i + k) & 7);
        }
        for (int i = 0; i < 13; i++) vch[i] ^= WHITE[i];
        for (int i = 0; i < 104; i++)
        {
            unsigned n = (i / 26) + 4 * (i % 26);
            unsigned p = offset + n;
            int s = (vch[i >> 3] >> (7 - (i & 7))) & 1;
            if (s) f[30 + (p >> 3)] |= 0x80 >> (p & 7); else f[30 + (p >> 3)] &= ~(0x80 >> (p & 7));
        }
    }
}

static void rfCall(YsfCtrl* c, const char* dest, const char* src, int nFrames, bool header)
{
    uint8_t f[YSF_FRAME_LENGTH_BYTES];

    if (header)
    {
        mkHeader(f, YSF_FI_HEADER, 3, YSF_DT_VD_MODE2, dest, src);
        ysfCtrlRf(c, f, false);
    }
    for (int i = 0; i < nFrames; i++)
    {
        mkVd2(f, (uint8_t)(i % 7), 3, dest, src);
        ysfCtrlRf(c, f, false);
    }
    if (header)
    {
        mkHeader(f, YSF_FI_TERMINATOR, 3, YSF_DT_VD_MODE2, dest, src);
        ysfCtrlRf(c, f, false);
    }
}

static void testRfToNet(void)
{
    YsfCtrl c;
    Sink s;

    setup(&c, &s, "N0CALL");
    rfCall(&c, "CQCQCQ", "W1ABC", 10, true);

    CHECK(s.nNet == 12, "net frames %d", s.nNet);
    CHECK(s.netEnd == 1, "one end flag, got %d", s.netEnd);
    CHECK(s.nEv == 2 && s.ev[0].event == YSF_EV_RF_START && s.ev[1].event == YSF_EV_RF_END, "events %d", s.nEv);
    CHECK(strncmp(s.ev[0].source, "W1ABC     ", 10) == 0, "source '%s'", s.ev[0].source);
    CHECK(strncmp(s.ev[0].dest, "CQCQCQ", 6) == 0, "dest '%s'", s.ev[0].dest);
    CHECK(memcmp(s.body[0], "N0CALL    ", 10) == 0, "gateway field");
    CHECK(memcmp(s.body[0] + 10, "W1ABC     ", 10) == 0, "body source");
    CHECK((s.body[0][30] >> 1) == 0 && (s.body[5][30] >> 1) == 5 && (s.body[11][30] & 1) == 1, "counter bytes");
    CHECK(c.rfState == YSF_RF_LISTENING, "back to listening");
    CHECK(s.ev[1].seconds > 1.19F && s.ev[1].seconds < 1.21F, "duration %.2f", s.ev[1].seconds);

    /* every frame to the network must still carry a valid FICH and the sync */
    for (int i = 0; i < s.nNet; i++)
    {
        YsfFich f; ysfFichInit(&f);
        CHECK(ysfHasSync(s.body[i] + 31, 0) && ysfFichDecode(&f, s.body[i] + 31), "frame %d fich", i);
    }
}

static void testLocalRepeat(void)
{
    YsfCtrl c;
    Sink s;

    setup(&c, &s, "N0CALL");
    c.localRepeat = true;
    c.lowDeviation = true;
    rfCall(&c, "CQCQCQ", "W1ABC", 4, true);

    CHECK(s.nRf == 6 && s.rfEot == 1, "repeated %d eot %d", s.nRf, s.rfEot);

    YsfFich f; ysfFichInit(&f);
    CHECK(ysfFichDecode(&f, s.rf[2]), "repeat fich");
    CHECK(ysfFichGetMR(&f) == YSF_MR_BUSY && ysfFichGetDev(&f), "MR %u dev %d", ysfFichGetMR(&f), ysfFichGetDev(&f));
}

static void testSelfOnly(void)
{
    YsfCtrl c;
    Sink s;

    setup(&c, &s, "N0CALL");
    c.selfOnly = true;
    rfCall(&c, "CQCQCQ", "W1ABC", 5, true);

    CHECK(s.nNet == 0, "nothing sent, got %d", s.nNet);
    CHECK(s.nEv == 1 && s.ev[0].event == YSF_EV_RF_REJECTED, "rejected event");
    CHECK(c.rfState == YSF_RF_LISTENING, "released after terminator");

    setup(&c, &s, "N0CALL");
    c.selfOnly = true;
    rfCall(&c, "CQCQCQ", "N0CALLX", 5, true);
    CHECK(s.nNet == 7, "own call passes, got %d", s.nNet);
}

static void testLateEntry(void)
{
    YsfCtrl c;
    Sink s;
    uint8_t f[YSF_FRAME_LENGTH_BYTES];

    setup(&c, &s, "N0CALL");

    for (int i = 0; i < 3; i++) { mkVd2(f, 3 + i, 3, "CQCQCQ", "W1ABC"); ysfCtrlRf(&c, f, false); }
    CHECK(s.nNet == 0 && c.rfState == YSF_RF_LISTENING, "nothing until callsigns are known");

    mkVd2(f, 0, 3, "CQCQCQ", "W1ABC"); ysfCtrlRf(&c, f, false);
    CHECK(s.nNet == 0, "dest alone is not enough");
    mkVd2(f, 1, 3, "CQCQCQ", "W1ABC"); ysfCtrlRf(&c, f, false);

    CHECK(s.nNet == 2, "synthetic header + frame, got %d", s.nNet);
    CHECK(s.nEv == 1 && s.ev[0].event == YSF_EV_RF_LATE, "late entry event");
    CHECK(strncmp(s.ev[0].source, "W1ABC", 5) == 0, "late source");

    YsfFich h; ysfFichInit(&h);
    CHECK(ysfFichDecode(&h, s.body[0] + 31) && ysfFichGetFI(&h) == YSF_FI_HEADER, "synthetic header FICH");

    for (int i = 0; i < 4; i++) { mkVd2(f, 2 + i, 3, "CQCQCQ", "W1ABC"); ysfCtrlRf(&c, f, false); }
    ysfCtrlRf(&c, NULL, true);
    CHECK(s.ev[s.nEv - 1].event == YSF_EV_RF_LOST, "lost event");
    CHECK(c.rfState == YSF_RF_LISTENING, "ready again");
}

static void testNetToRf(void)
{
    YsfCtrl a, b;
    Sink sa, sb;

    setup(&a, &sa, "N0CALL");
    rfCall(&a, "CQCQCQ", "W1ABC", 8, true);

    setup(&b, &sb, "K9XYZ");
    b.remoteGateway = false;
    for (int i = 0; i < sa.nNet; i++)
    {
        uint8_t body[YSF_NET_BODY_LENGTH];
        memcpy(body, sa.body[i], sizeof(body));
        memcpy(body, "REFLECTOR ", 10);
        ysfCtrlNet(&b, body);
        ysfCtrlClock(&b, 100);
    }

    CHECK(sb.nRf == 10, "frames to RF %d", sb.nRf);
    CHECK(sb.rfEot == 1, "one eot");
    CHECK(sb.nEv == 2 && sb.ev[0].event == YSF_EV_NET_START && sb.ev[1].event == YSF_EV_NET_END, "net events %d", sb.nEv);
    CHECK(strncmp(sb.ev[0].source, "W1ABC", 5) == 0 && strncmp(sb.ev[0].gateway, "REFLECTOR", 9) == 0, "net start fields");
    CHECK(!ysfCtrlBusy(&b), "idle after end");

    YsfFich f; ysfFichInit(&f);
    CHECK(ysfFichDecode(&f, sb.rf[3]), "net fich");
    CHECK(ysfFichGetMR(&f) == YSF_MR_BUSY, "MR busy for a hotspot");
}

static void testWatchdog(void)
{
    YsfCtrl c;
    Sink s;
    Sink sa;
    YsfCtrl a;

    setup(&a, &sa, "N0CALL");
    rfCall(&a, "CQCQCQ", "W1ABC", 8, true);
    setup(&c, &s, "K9XYZ");

    for (int i = 0; i < 4; i++) ysfCtrlNet(&c, sa.body[i]);
    CHECK(c.netState == YSF_NET_AUDIO, "net active");
    ysfCtrlClock(&c, 1000);
    CHECK(c.netState == YSF_NET_AUDIO, "not yet");
    ysfCtrlClock(&c, 600);
    CHECK(c.netState == YSF_NET_IDLE, "watchdog fired");
    CHECK(s.ev[s.nEv - 1].event == YSF_EV_NET_LOST, "lost event");
    CHECK(s.rfEot == 1, "a terminator was sent to the transmitter");

    /* and a duplicate counter is ignored */
    setup(&c, &s, "K9XYZ");
    ysfCtrlNet(&c, sa.body[0]);
    ysfCtrlNet(&c, sa.body[0]);
    CHECK(s.nRf == 1, "duplicate dropped, got %d", s.nRf);

    /* RF blocks the network */
    setup(&c, &s, "K9XYZ");
    uint8_t f[YSF_FRAME_LENGTH_BYTES];
    mkHeader(f, YSF_FI_HEADER, 3, YSF_DT_VD_MODE2, "CQCQCQ", "W1ABC");
    ysfCtrlRf(&c, f, false);
    ysfCtrlNet(&c, sa.body[0]);
    CHECK(s.nRf == 0, "network blocked while RF is active");
}

int main(void)
{
    srand(5);
    testRfToNet();
    testLocalRepeat();
    testSelfOnly();
    testLateEntry();
    testNetToRf();
    testWatchdog();

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "all control tests passed", failures);
    return failures != 0;
}
