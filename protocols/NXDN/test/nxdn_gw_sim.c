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
 * nxdn_gw_sim - plays nxdn_host and an NXDN reflector for nxdn_gateway.
 *
 *   echo "10200 127.0.0.1 34010" > /tmp/nxdn_hosts.txt
 *   ./nxdn_gw_sim &
 *   NXDN_STUB_hostsFile=/tmp/nxdn_hosts.txt ./nxdn_gateway -m 1
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define HOST_PORT   18500
#define REFL_PORT   34010
#define TG          10200
#define TG2         10300

static int curTG = TG;

static int tcpFd = -1;
static int udpFd = -1;
static struct sockaddr_in gwAddr;
static int failures = 0;

static void check(int ok, const char* what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

static int waitFd(int fd, int ms)
{
    struct pollfd p = { fd, POLLIN, 0 };
    return poll(&p, 1, ms) > 0;
}

/* One packet from the gateway on the TCP link */
static int hostRead(uint8_t* buf, int ms)
{
    if (!waitFd(tcpFd, ms))
        return 0;
    if (read(tcpFd, buf, 4) != 4)
        return 0;
    int len = (buf[1] << 8) | buf[2];
    int got = 4;
    while (got < len)
    {
        int n = read(tcpFd, buf + got, len - got);
        if (n <= 0)
            return 0;
        got += n;
    }
    return len;
}

static void hostWriteNxdn(uint8_t type, const uint8_t* d33)
{
    uint8_t p[49];
    memset(p, 0, sizeof(p));
    p[0] = 0x61; p[2] = 49; p[3] = 4;
    memcpy(p + 4, "NXDN", 4);
    p[8] = type;
    memcpy(p + 9, d33, 33);
    write(tcpFd, p, sizeof(p));
}

static void hostWriteConn(const char* cs, const char* name)
{
    uint8_t p[20];
    memset(p, 0, sizeof(p));
    p[0] = 0x61; p[2] = 20; p[3] = 4;
    memcpy(p + 4, "CONN", 4);
    memcpy(p + 8, cs, strlen(cs));
    memcpy(p + 14, name, strlen(name));
    write(tcpFd, p, sizeof(p));
}

static void hostWriteSimple(const char* type)
{
    uint8_t p[8] = { 0x61, 0, 8, 4 };
    memcpy(p + 4, type, 4);
    write(tcpFd, p, sizeof(p));
}

/* Next packet at the reflector. Returns the length, 0 on timeout. */
static int reflRead(uint8_t* buf, int ms)
{
    if (!waitFd(udpFd, ms))
        return 0;
    socklen_t l = sizeof(gwAddr);
    int n = recvfrom(udpFd, buf, 200, 0, (struct sockaddr*)&gwAddr, &l);
    return n > 0 ? n : 0;
}

static void reflWrite(const uint8_t* buf, int len)
{
    sendto(udpFd, buf, len, 0, (struct sockaddr*)&gwAddr, sizeof(gwAddr));
}

static void reflPoll(void)
{
    uint8_t p[17];
    memcpy(p, "NXDNP", 5);
    memcpy(p + 5, "REFLECTOR ", 10);
    p[15] = curTG >> 8; p[16] = curTG & 0xFF;
    reflWrite(p, 17);
}

/* Count the polls ('P') or unlinks ('U') for talk group tg that arrive within 600 ms of the first */
static int countKind(char kind, int tg, int firstMs)
{
    uint8_t x[200];
    int count = 0;
    int ms = firstMs;
    int n;

    while ((n = reflRead(x, ms)) > 0)
    {
        ms = 600;
        if (n == 17 && memcmp(x, "NXDN", 4) == 0 && x[4] == (uint8_t)kind && ((x[15] << 8) | x[16]) == tg)
            count++;
        if (count == 3)
            break;
    }
    return count;
}

static void reflData(uint16_t src, uint16_t dst, uint8_t flags, const uint8_t* d33)
{
    uint8_t p[43];
    memcpy(p, "NXDND", 5);
    p[5] = src >> 8; p[6] = src & 0xFF;
    p[7] = dst >> 8; p[8] = dst & 0xFF;
    p[9] = flags;
    memcpy(p + 10, d33, 33);
    reflWrite(p, 43);
}

/* A frame as the radio side produces it */
static void voiceFrame(uint8_t* d, uint8_t lich, uint8_t type, uint16_t src, uint16_t dst, uint8_t seed)
{
    memset(d, 0, 33);
    for (int i = 0; i < 33; i++)
        d[i] = (uint8_t)(seed + i * 7);
    d[0] = lich;
    d[5] = type;
    d[7] = 0x20;
    d[8] = src >> 8; d[9] = src & 0xFF;
    d[10] = dst >> 8; d[11] = dst & 0xFF;
}

static void dataFrame(uint8_t* d, uint8_t lich, uint8_t type, uint16_t src, uint16_t dst, uint8_t seed)
{
    for (int i = 0; i < 33; i++)
        d[i] = (uint8_t)(seed + i * 5);
    d[0] = lich;
    d[2] = type;
    d[4] = 0x20;
    d[5] = src >> 8; d[6] = src & 0xFF;
    d[7] = dst >> 8; d[8] = dst & 0xFF;
}

static void expectReflData(const char* what, const uint8_t* d33, uint8_t flags, uint16_t src)
{
    uint8_t r[200];
    int n = reflRead(r, 2000);
    int ok = n == 43 && memcmp(r, "NXDND", 5) == 0 && r[9] == flags && ((r[5] << 8) | r[6]) == src &&
             ((r[7] << 8) | r[8]) == curTG && memcmp(r + 10, d33, 33) == 0;
    char msg[120];
    snprintf(msg, sizeof(msg), "to reflector: %s (flags 0x%02X)", what, n == 43 ? r[9] : 0);
    check(ok, msg);
}

static void expectHostData(const char* what, const uint8_t* d33, uint8_t type)
{
    uint8_t r[200];
    int n = hostRead(r, 2000);
    int ok = n == 49 && memcmp(r + 4, "NXDN", 4) == 0 && r[8] == type && memcmp(r + 9, d33, 33) == 0 &&
             r[42] == 0 && r[48] == 0;
    char msg[120];
    snprintf(msg, sizeof(msg), "to host: %s (type %d)", what, n == 49 ? r[8] : -1);
    check(ok, msg);
}

/* Second run: NXDN_STUB_staticTG=10200 NXDN_STUB_hangTime=3 */
static int runStatic(int lfd)
{
    uint8_t r[200];
    uint8_t v[33];
    int n;

    tcpFd = accept(lfd, NULL, NULL);
    printf("static TG test: gateway connected\n");

    check(countKind('P', TG, 3000) == 3, "static talk group linked at start (3 NXDNP)");
    reflPoll();
    n = hostRead(r, 3000);
    check(n == 14 && memcmp(r + 4, "CONN", 4) == 0 && memcmp(r + 8, "10200", 5) == 0, "host told CONN 10200");

    curTG = TG2;
    voiceFrame(v, 0x81, 0x01, 1234, TG2, 5);
    hostWriteNxdn(0, v);
    check(countKind('U', TG, 3000) == 3, "RF call to another talk group unlinks the static one");
    n = hostRead(r, 1000);
    check(n == 14 && memcmp(r + 4, "DISC", 4) == 0, "host told DISC");
    check(countKind('P', TG2, 1000) == 3, "and links the new one");
    reflPoll();
    n = hostRead(r, 3000);
    check(n == 14 && memcmp(r + 4, "CONN", 4) == 0 && memcmp(r + 8, "10300", 5) == 0, "host told CONN 10300");

    check(countKind('U', TG2, 6500) == 3, "hang time expires, RF link dropped");
    n = hostRead(r, 1000);
    check(n == 14 && memcmp(r + 4, "DISC", 4) == 0, "host told DISC");
    curTG = TG;
    check(countKind('P', TG, 3000) == 3, "back on the static talk group");
    reflPoll();
    n = hostRead(r, 3000);
    check(n == 14 && memcmp(r + 4, "CONN", 4) == 0 && memcmp(r + 8, "10200", 5) == 0, "host told CONN 10200");

    printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
    int one = 1;
    struct sockaddr_in a;
    uint8_t r[200];
    int n;

    /* TCP server for the gateway */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(HOST_PORT);
    if (bind(lfd, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(lfd, 1) < 0)
    {
        perror("tcp");
        return 1;
    }

    /* UDP reflector */
    udpFd = socket(AF_INET, SOCK_DGRAM, 0);
    a.sin_port = htons(REFL_PORT);
    if (bind(udpFd, (struct sockaddr*)&a, sizeof(a)) < 0)
    {
        perror("udp");
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "static") == 0)
        return runStatic(lfd);

    printf("waiting for nxdn_gateway...\n");
    fflush(stdout);
    tcpFd = accept(lfd, NULL, NULL);
    printf("gateway connected\n");

    /* 1. link */
    printf("link to reflector %d\n", TG);
    hostWriteConn("KD0OSS", "10200");
    n = reflRead(r, 3000);
    check(n == 17 && memcmp(r, "NXDNP", 5) == 0, "gateway polls the reflector");
    check(n == 17 && memcmp(r + 5, "N0CALL    ", 10) == 0, "poll carries the callsign padded to 10");
    check(n == 17 && ((r[15] << 8) | r[16]) == TG, "poll carries the talk group");
    {
        int polls = 1;
        uint8_t x[200];
        while (reflRead(x, 200) == 17)
            polls++;
        check(polls == 3, "three polls sent when linking");
    }
    reflPoll();
    n = hostRead(r, 3000);
    check(n == 14 && memcmp(r + 4, "CONN", 4) == 0 && memcmp(r + 8, "10200", 5) == 0, "host told CONN 10200");

    /* 2. voice call from the radio */
    printf("voice call, radio to reflector\n");
    uint8_t h[33], b[33], t[33];
    voiceFrame(h, 0x81, 0x01, 1234, TG, 10);
    voiceFrame(b, 0xA3, 0x00, 1234, TG, 20);
    voiceFrame(t, 0x81, 0x08, 1234, TG, 30);
    hostWriteNxdn(0, h); expectReflData("voice header", h, 0x05, 1234);
    hostWriteNxdn(1, b); expectReflData("voice body", b, 0x01, 1234);
    hostWriteNxdn(2, t); expectReflData("voice trailer", t, 0x09, 1234);

    /* 3. data call from the radio */
    printf("data call, radio to reflector\n");
    uint8_t dh[33], db[33], dt[33];
    dataFrame(dh, 0x90, 0x09, 4321, TG, 40);
    dataFrame(db, 0x90, 0x0B, 4321, TG, 50);
    dataFrame(dt, 0x90, 0x08, 4321, TG, 60);
    hostWriteNxdn(3, dh); expectReflData("data header", dh, 0x07, 4321);
    hostWriteNxdn(4, db); expectReflData("data body", db, 0x03, 4321);
    hostWriteNxdn(5, dt); expectReflData("data trailer", dt, 0x0B, 4321);

    /* 4. calls from the reflector */
    printf("voice call, reflector to radio\n");
    voiceFrame(h, 0x81, 0x01, 777, TG, 70);
    voiceFrame(b, 0xA3, 0x00, 777, TG, 80);
    voiceFrame(t, 0x81, 0x08, 777, TG, 90);
    reflData(777, TG, 0x05, h); expectHostData("voice header", h, 0);
    reflData(777, TG, 0x01, b); expectHostData("voice body", b, 1);
    reflData(777, TG, 0x09, t); expectHostData("voice trailer", t, 2);

    printf("data call, reflector to radio\n");
    dataFrame(dh, 0x90, 0x09, 777, TG, 100);
    dataFrame(db, 0x90, 0x0B, 777, TG, 110);
    dataFrame(dt, 0x90, 0x08, 777, TG, 120);
    reflData(777, TG, 0x07, dh); expectHostData("data header", dh, 3);
    reflData(777, TG, 0x03, db); expectHostData("data body", db, 4);
    reflData(777, TG, 0x0B, dt); expectHostData("data trailer", dt, 5);

    printf("filtering\n");
    reflData(777, 5, 0x01, b);              /* other talk group */
    reflData(777, TG, 0x00, b);             /* not a group call */
    check(hostRead(r, 500) == 0, "other talk group and individual calls are not passed on");

    /* 5. polling */
    printf("keep-alive (a link from the dashboard is not dropped by the hang timer)\n");
    n = reflRead(r, 6500);
    check(n == 17 && memcmp(r, "NXDNP", 5) == 0, "gateway polls again after 5 s");
    reflPoll();

    /* 6. unlink */
    printf("unlink\n");
    hostWriteSimple("DISC");
    int unlinks = 0;
    while ((n = reflRead(r, 1000)) > 0)
        if (n == 17 && memcmp(r, "NXDNU", 5) == 0)
            unlinks++;
    check(unlinks == 3, "three NXDNU sent to the reflector");
    n = hostRead(r, 1000);
    check(n == 14 && memcmp(r + 4, "DISC", 4) == 0, "host told DISC");

    /* 7. unknown reflector */
    printf("unknown reflector\n");
    hostWriteConn("KD0OSS", "99999");
    n = hostRead(r, 3000);
    check(n == 14 && memcmp(r + 4, "NACK", 4) == 0, "NACK for a reflector that is not in the list");

    /* 8. no answer from the reflector */
    printf("link without an answer\n");
    hostWriteConn("KD0OSS", "10200");
    n = hostRead(r, 6000);
    check(n == 14 && memcmp(r + 4, "NACK", 4) == 0, "NACK when the reflector does not reply");
    while (reflRead(r, 300) > 0) { }

    /* 9. configuration */
    printf("configuration\n");
    {
        uint8_t c[9] = { 0x61, 0, 9, 4, 'C', 'O', 'M', 'M', 0x04 };
        write(tcpFd, c, 9);
        n = hostRead(r, 2000);
        check(n == 8 && memcmp(r + 4, "COMM", 4) == 0, "COMM acknowledged");
    }

    /* 9b. linking from the radio */
    printf("RF-driven linking\n");
    {
        uint8_t v[33];
        int c;

        curTG = TG2;
        voiceFrame(v, 0x81, 0x01, 1234, TG2, 5);
        hostWriteNxdn(0, v);
        c = countKind('P', TG2, 3000);
        check(c == 3, "voice header to a new talk group sends 3 polls for it");
        reflPoll();
        n = hostRead(r, 3000);
        check(n == 14 && memcmp(r + 4, "CONN", 4) == 0 && memcmp(r + 8, "10300", 5) == 0, "host told CONN 10300");

        voiceFrame(v, 0xA3, 0x00, 1234, TG2, 6);
        hostWriteNxdn(1, v);
        expectReflData("voice body goes to the new link", v, 0x01, 1234);

        voiceFrame(v, 0x81, 0x08, 1234, TG2, 7);
        hostWriteNxdn(2, v);
        expectReflData("trailer for the same talk group does not relink", v, 0x09, 1234);

        voiceFrame(v, 0x81, 0x01, 1234, TG, 8);
        hostWriteNxdn(0, v);
        c = countKind('U', TG2, 3000);
        check(c == 3, "switching talk group unlinks the old one (3 NXDNU)");
        n = hostRead(r, 1000);
        check(n == 14 && memcmp(r + 4, "DISC", 4) == 0 && memcmp(r + 8, "10300", 5) == 0, "host told DISC 10300");
        curTG = TG;
        c = countKind('P', TG, 3000);
        check(c == 3, "and links the new one (3 NXDNP)");
        reflPoll();
        n = hostRead(r, 3000);
        check(n == 14 && memcmp(r + 4, "CONN", 4) == 0 && memcmp(r + 8, "10200", 5) == 0, "host told CONN 10200");

        voiceFrame(v, 0x81, 0x01, 1234, 555, 9);
        hostWriteNxdn(0, v);
        c = countKind('U', TG, 3000);
        check(c == 3, "talk group with no reflector unlinks the current one");
        n = hostRead(r, 1000);
        check(n == 14 && memcmp(r + 4, "DISC", 4) == 0, "host told DISC");
        n = hostRead(r, 1000);
        check(n == 14 && memcmp(r + 4, "NACK", 4) == 0 && memcmp(r + 8, "555", 3) == 0, "host told NACK 555");

        voiceFrame(v, 0x81, 0x01, 1234, TG2, 10);
        v[7] = 0x80;                    /* individual call */
        hostWriteNxdn(0, v);
        check(countKind('P', TG2, 700) == 0, "individual call does not link");

        voiceFrame(v, 0x81, 0x01, 1234, TG2, 12);
        v[7] = 0x00;                    /* call type 0, sent by some radios for a talk group */
        hostWriteNxdn(0, v);
        curTG = TG2;
        check(countKind('P', TG2, 3000) == 3, "call type 0 links like a talk group");
        reflPoll();
        n = hostRead(r, 3000);
        check(n == 14 && memcmp(r + 4, "CONN", 4) == 0, "linked");
        voiceFrame(v, 0x81, 0x01, 1234, 555, 13);
        hostWriteNxdn(0, v);
        check(countKind('U', TG2, 3000) == 3, "unlinked again");
        n = hostRead(r, 1000);
        check(n == 14 && memcmp(r + 4, "DISC", 4) == 0, "host told DISC");
        n = hostRead(r, 1000);
        check(n == 14 && memcmp(r + 4, "NACK", 4) == 0, "host told NACK 555");

        voiceFrame(v, 0x81, 0x01, 1234, TG2, 11);
        hostWriteNxdn(0, v);
        curTG = TG2;
        check(countKind('P', TG2, 3000) == 3, "link again for the hang timer test");
        reflPoll();
        n = hostRead(r, 3000);
        check(n == 14 && memcmp(r + 4, "CONN", 4) == 0, "linked");
        printf("hang timer (3 s)\n");
        c = countKind('U', TG2, 6500);
        check(c == 3, "RF link dropped after the hang time");
        n = hostRead(r, 2000);
        check(n == 14 && memcmp(r + 4, "DISC", 4) == 0, "host told DISC");
        curTG = TG;
    }

    /* 10. host goes away while linked */
    printf("host disconnect\n");
    hostWriteConn("KD0OSS", "10200");
    n = reflRead(r, 3000);
    while (reflRead(r, 200) == 17) { }
    reflPoll();
    n = hostRead(r, 3000);
    check(n == 14 && memcmp(r + 4, "CONN", 4) == 0, "linked again");
    close(tcpFd);
    unlinks = 0;
    while ((n = reflRead(r, 1500)) > 0)
        if (n == 17 && memcmp(r, "NXDNU", 5) == 0)
            unlinks++;
    check(unlinks == 3, "reflector unlinked when the host disconnects");

    printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
