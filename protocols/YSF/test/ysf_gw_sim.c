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
 * ysf_gw_sim - plays ysf_host and a YSF reflector for ysf_gateway.
 *
 *   ./ysf_gw_sim &                 (uses /tmp/ysf_hosts.json and YSFHosts.txt, made here)
 *   YSF_STUB_hostsFile=/tmp/ysf_hosts.json ./ysf_gateway -m 1
 *   ./ysf_gw_sim static            (YSF_STUB_staticReflector=Test-One)
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define HOST_PORT   18500
#define REFL_PORT   42000

static int tcpFd = -1;
static int udpFd = -1;
static struct sockaddr_in gwAddr;
static int failures = 0;

/*
 * check()
 *
 * Prints the result of one check and counts the failures.
 *
 * Parameters:
 *   ok   - Non-zero if the check passed.
 *   what - Description of the check.
 *
 * Returns: nothing.
 */
static void check(int ok, const char* what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

/*
 * waitFd()
 *
 * Waits until a socket has data to read.
 *
 * Parameters:
 *   fd - The socket.
 *   ms - Longest wait, in milliseconds.
 *
 * Returns: Non-zero if data is waiting, 0 on time out.
 */
static int waitFd(int fd, int ms)
{
    struct pollfd p = { fd, POLLIN, 0 };
    return poll(&p, 1, ms) > 0;
}

/*
 * hostRead()
 *
 * Reads one packet that the gateway sent to the (simulated) host.
 *
 * Parameters:
 *   buf - Receives the packet.
 *   ms  - Longest wait for the packet, in milliseconds.
 *
 * Returns: The length of the packet, or 0 if none arrived.
 */
static int hostRead(uint8_t* buf, int ms)
{
    int len, got;

    if (!waitFd(tcpFd, ms) || read(tcpFd, buf, 4) != 4)
        return 0;

    len = (buf[1] << 8) | buf[2];
    got = 4;

    while (got < len)
    {
        int n = read(tcpFd, buf + got, len - got);
        if (n <= 0)
            return 0;
        got += n;
    }

    return len;
}

/*
 * hostWriteConn()
 *
 * Sends a CONN packet to the gateway, as the host does to link a reflector.
 *
 * Parameters:
 *   cs   - The callsign of the station.
 *   name - The reflector name or id.
 *
 * Returns: nothing.
 */
static void hostWriteConn(const char* cs, const char* name)
{
    uint8_t p[34];

    memset(p, 0, sizeof(p));
    p[0] = 0x61; p[2] = 34; p[3] = 4;
    memcpy(p + 4, "CONN", 4);
    memcpy(p + 8, cs, strlen(cs));
    memcpy(p + 18, name, strlen(name));
    write(tcpFd, p, sizeof(p));
}

/*
 * hostWriteSimple()
 *
 * Sends a packet that has only a type (DISC, for example) to the gateway.
 *
 * Parameters:
 *   type - The four character type.
 *
 * Returns: nothing.
 */
static void hostWriteSimple(const char* type)
{
    uint8_t p[8] = { 0x61, 0, 8, 4 };
    memcpy(p + 4, type, 4);
    write(tcpFd, p, sizeof(p));
}

/*
 * hostWriteYsf()
 *
 * Sends a YSFG packet with a frame of the radio to the gateway.
 *
 * Parameters:
 *   end  - true for the last frame of the call.
 *   body - The 151 byte body.
 *
 * Returns: nothing.
 */
static void hostWriteYsf(bool end, const uint8_t* body)
{
    uint8_t p[160];

    memset(p, 0, sizeof(p));
    p[0] = 0x61; p[2] = 160; p[3] = 4;
    memcpy(p + 4, "YSFG", 4);
    p[8] = end ? 1 : 0;
    memcpy(p + 9, body, 151);
    write(tcpFd, p, sizeof(p));
}

/*
 * reflRead()
 *
 * Reads one packet that the gateway sent to the (simulated) reflector, and
 * remembers the address it came from.
 *
 * Parameters:
 *   buf - Receives the packet; at least 300 bytes.
 *   ms  - Longest wait for the packet, in milliseconds.
 *
 * Returns: The length of the packet, or 0 if none arrived.
 */
static int reflRead(uint8_t* buf, int ms)
{
    socklen_t l = sizeof(gwAddr);
    int n;

    if (!waitFd(udpFd, ms))
        return 0;

    n = recvfrom(udpFd, buf, 300, 0, (struct sockaddr*)&gwAddr, &l);
    return n > 0 ? n : 0;
}

/*
 * reflWrite()
 *
 * Sends a packet from the (simulated) reflector to the gateway.
 *
 * Parameters:
 *   buf - The packet.
 *   len - Number of bytes in buf.
 *
 * Returns: nothing.
 */
static void reflWrite(const uint8_t* buf, int len)
{
    sendto(udpFd, buf, len, 0, (struct sockaddr*)&gwAddr, sizeof(gwAddr));
}

/*
 * reflPoll()
 *
 * Sends a poll answer (YSFP) from the reflector.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void reflPoll(void)
{
    uint8_t p[14];
    memcpy(p, "YSFP", 4);
    memcpy(p + 4, "TESTREFL  ", 10);
    reflWrite(p, 14);
}

/*
 * countKind()
 *
 * Counts the polls ('P') or unlinks ('U') of this station that arrive at the
 * reflector, each within 600 ms of the one before, up to three.
 *
 * Parameters:
 *   kind    - 'P' or 'U'.
 *   firstMs - Longest wait for the first packet, in milliseconds.
 *
 * Returns: The number of packets counted.
 */
static int countKind(char kind, int firstMs)
{
    uint8_t x[300];
    int count = 0, ms = firstMs, n;

    while ((n = reflRead(x, ms)) > 0)
    {
        ms = 600;
        if (n == 14 && memcmp(x, "YSF", 3) == 0 && x[3] == (uint8_t)kind && memcmp(x + 4, "N0CALL    ", 10) == 0)
            count++;
        if (count == 3)
            break;
    }

    return count;
}

/*
 * makeBody()
 *
 * Makes a 151 byte body: the gateway, source and destination callsigns, the
 * counter and a frame of known content.
 *
 * Parameters:
 *   body    - Receives the body.
 *   gw      - The gateway callsign.
 *   src     - The source callsign.
 *   dst     - The destination callsign.
 *   counter - The frame counter byte.
 *   seed    - Starting value of the frame content.
 *
 * Returns: nothing.
 */
static void makeBody(uint8_t* body, const char* gw, const char* src, const char* dst, uint8_t counter, uint8_t seed)
{
    memset(body, ' ', 30);
    memcpy(body, gw, strlen(gw));
    memcpy(body + 10, src, strlen(src));
    memcpy(body + 20, dst, strlen(dst));
    body[30] = counter;

    for (int i = 0; i < 120; i++)
        body[31 + i] = (uint8_t)(seed + i * 3);
}

/*
 * reflData()
 *
 * Sends a YSFD packet with a body from the reflector to the gateway.
 *
 * Parameters:
 *   body - The 151 byte body.
 *
 * Returns: nothing.
 */
static void reflData(const uint8_t* body)
{
    uint8_t p[155];
    memcpy(p, "YSFD", 4);
    memcpy(p + 4, body, 151);
    reflWrite(p, 155);
}

/*
 * expectReflData()
 *
 * Checks that the reflector receives a YSFD packet with the given body.
 *
 * Parameters:
 *   what - Description of the check.
 *   body - The expected body.
 *
 * Returns: nothing.
 */
static void expectReflData(const char* what, const uint8_t* body)
{
    uint8_t r[300];
    int n = reflRead(r, 2000);
    check(n == 155 && memcmp(r, "YSFD", 4) == 0 && memcmp(r + 4, body, 151) == 0, what);
}

/*
 * expectHostData()
 *
 * Checks that the host receives a YSFG packet with the given body.
 *
 * Parameters:
 *   what - Description of the check.
 *   body - The expected body.
 *   end  - The expected end flag.
 *
 * Returns: nothing.
 */
static void expectHostData(const char* what, const uint8_t* body, bool end)
{
    uint8_t r[300];
    int n = hostRead(r, 2000);
    check(n == 160 && memcmp(r + 4, "YSFG", 4) == 0 && r[8] == (end ? 1 : 0) && memcmp(r + 9, body, 151) == 0, what);
}

/*
 * expectName()
 *
 * Checks that the host receives a status packet (CONN, NACK or DISC) with
 * the given reflector name.
 *
 * Parameters:
 *   what - Description of the check.
 *   type - The expected type.
 *   name - The expected reflector name.
 *
 * Returns: nothing.
 */
static void expectName(const char* what, const char* type, const char* name)
{
    uint8_t r[300];
    char text[120];
    int n = hostRead(r, 6000);
    int ok = n == 24 && memcmp(r + 4, type, 4) == 0 && strncmp((char*)r + 8, name, strlen(name)) == 0 &&
             r[8 + strlen(name)] == 0;

    snprintf(text, sizeof(text), "%s (%s %s)", what, type, name);
    check(ok, text);
}

/*
 * runStatic()
 *
 * Runs the test of the static reflector: the gateway must link at start
 * without a request, and tell the host.
 *
 * Parameters:
 *   lfd - The listening socket that the gateway connects to.
 *
 * Returns: 0 if it passes, 1 if it fails.
 */
static int runStatic(int lfd)
{
    tcpFd = accept(lfd, NULL, NULL);
    printf("static reflector test: gateway connected\n");

    check(countKind('P', 3000) == 3, "static reflector linked at start (3 YSFP)");
    reflPoll();
    expectName("host told CONN", "CONN", "Test-One");

    printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}

/*
 * main()
 *
 * Plays ysf_host and a YSF reflector for ysf_gateway.  It makes a JSON and a
 * text reflector list, then tests linking (by id, by name, by country and
 * name, XX prefix, from the text list, unknown, no answer), voice in both
 * directions, filtering of packets, polls, unlinking and the COMM packet.
 * With the argument "static" only the static reflector is tested.
 *
 * Parameters:
 *   argc - Number of command line arguments.
 *   argv - Command line arguments.
 *
 * Returns: 0 if all checks pass, 1 if any fails or on a setup error.
 */
int main(int argc, char** argv)
{
    int one = 1;
    struct sockaddr_in a;
    uint8_t r[300];
    uint8_t body[151], b2[151];
    int n;
    FILE* fp;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);

    /* reflector lists: the JSON file of hostfiles.refcheck.radio and the text format */
    fp = fopen("/tmp/ysf_hosts.json", "w");
    fprintf(fp, "{\"reflectors\":[\n"
        "{\"designator\":\"00001\",\"country\":\"US\",\"name\":\"Test-One\",\"use_xx_prefix\":false,"
        "\"user_count\":3,\"description\":\"Simulated\",\"ipv4\":\"127.0.0.1\",\"ipv6\":null,\"port\":%d},\n"
        "{\"designator\":\"00002\",\"country\":\"DE\",\"name\":\"Test-Two\",\"use_xx_prefix\":true,"
        "\"user_count\":0,\"description\":\"Elsewhere\",\"ipv4\":\"127.0.0.1\",\"ipv6\":null,\"port\":%d}\n]}\n",
        REFL_PORT, REFL_PORT + 1);
    fclose(fp);
    fp = fopen("YSFHosts.txt", "w");   /* the gateway reads this one from its working directory */
    fprintf(fp, "# comment\n00003;Text-Three;Old format;127.0.0.1;%d;5\n", REFL_PORT);
    fclose(fp);

    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(HOST_PORT);
    if (bind(lfd, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(lfd, 1) < 0)
    {
        perror("tcp");
        return 1;
    }

    udpFd = socket(AF_INET, SOCK_DGRAM, 0);
    a.sin_port = htons(REFL_PORT);
    if (bind(udpFd, (struct sockaddr*)&a, sizeof(a)) < 0)
    {
        perror("udp");
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "static") == 0)
        return runStatic(lfd);

    printf("waiting for ysf_gateway...\n");
    fflush(stdout);
    tcpFd = accept(lfd, NULL, NULL);
    printf("gateway connected\n");

    printf("link by id (JSON list)\n");
    hostWriteConn("W1ABC", "00001");
    n = reflRead(r, 3000);
    check(n == 14 && memcmp(r, "YSFP", 4) == 0, "gateway polls the reflector");
    check(n == 14 && memcmp(r + 4, "N0CALL    ", 10) == 0, "poll carries the callsign padded to 10");
    {
        int polls = 1;
        uint8_t x[300];
        while (reflRead(x, 200) == 14)
            polls++;
        check(polls == 3, "three polls sent when linking");
    }
    reflPoll();
    expectName("host told CONN", "CONN", "00001");

    printf("voice, radio to reflector\n");
    makeBody(body, "N0CALL", "W1ABC", "ALL", 0x00, 10);
    hostWriteYsf(false, body); expectReflData("first frame", body);
    makeBody(body, "N0CALL", "W1ABC", "ALL", 0x02, 20);
    hostWriteYsf(false, body); expectReflData("second frame", body);
    makeBody(body, "N0CALL", "W1ABC", "ALL", 0x05, 30);
    hostWriteYsf(true, body); expectReflData("last frame (end flag)", body);

    printf("voice, reflector to radio\n");
    makeBody(b2, "REMOTE", "K1XYZ", "ALL", 0x00, 40);
    reflData(b2); expectHostData("first frame", b2, false);
    makeBody(b2, "REMOTE", "K1XYZ", "ALL", 0x02, 50);
    reflData(b2); expectHostData("second frame", b2, false);
    makeBody(b2, "REMOTE", "K1XYZ", "ALL", 0x05, 60);
    reflData(b2); expectHostData("last frame (end flag)", b2, true);

    printf("filtering\n");
    {
        struct sockaddr_in other;
        int ofd = socket(AF_INET, SOCK_DGRAM, 0);
        uint8_t p[155];

        memset(&other, 0, sizeof(other));
        other.sin_family = AF_INET; other.sin_addr.s_addr = htonl(INADDR_LOOPBACK); other.sin_port = htons(REFL_PORT);
        memcpy(p, "YSFD", 4);
        memcpy(p + 4, b2, 151);
        /* the gateway socket address is the one we last heard from */
        sendto(ofd, p, 155, 0, (struct sockaddr*)&gwAddr, sizeof(gwAddr));
        close(ofd);
        check(hostRead(r, 500) == 0, "data from a source other than the reflector is dropped");

        reflWrite(p, 100);
        check(hostRead(r, 500) == 0, "a short YSFD is dropped");
    }

    printf("keep-alive\n");
    n = reflRead(r, 6500);
    check(n == 14 && memcmp(r, "YSFP", 4) == 0, "gateway polls again after 5 s");
    reflPoll();

    printf("unlink\n");
    hostWriteSimple("DISC");
    check(countKind('U', 2000) == 3, "three YSFU sent to the reflector");
    expectName("host told DISC", "DISC", "00001");

    printf("link by name and by country-name\n");
    hostWriteConn("W1ABC", "Test-One");
    check(countKind('P', 3000) >= 1, "name Test-One found");
    reflPoll();
    expectName("host told CONN", "CONN", "Test-One");
    hostWriteConn("W1ABC", "us-test-one");
    n = reflRead(r, 3000);
    check(n == 14 && memcmp(r, "YSFU", 4) == 0, "switching reflector unlinks the old one first");
    while (reflRead(r, 700) > 0) { }
    reflPoll();
    expectName("host told DISC for the old link", "DISC", "Test-One");

    hostWriteSimple("DISC");
    while (hostRead(r, 800) > 0) { }
    while (reflRead(r, 300) > 0) { }

    printf("text list and XX prefix\n");
    hostWriteConn("W1ABC", "Text-Three");
    check(countKind('P', 3000) >= 1, "Text-Three found in the text list");
    reflPoll();
    expectName("host told CONN", "CONN", "Text-Three");
    hostWriteSimple("DISC");
    while (hostRead(r, 800) > 0) { }
    while (reflRead(r, 300) > 0) { }
    {
        int fd2 = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in b2a;

        memset(&b2a, 0, sizeof(b2a));
        b2a.sin_family = AF_INET; b2a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); b2a.sin_port = htons(REFL_PORT + 1);
        bind(fd2, (struct sockaddr*)&b2a, sizeof(b2a));
        hostWriteConn("W1ABC", "XX-Test-Two");
        check(waitFd(fd2, 2000), "XX-Test-Two found at its own port (use_xx_prefix)");
        close(fd2);
    }
    hostWriteSimple("DISC");
    while (hostRead(r, 800) > 0) { }
    while (reflRead(r, 300) > 0) { }

    printf("unknown reflector\n");
    hostWriteConn("W1ABC", "99999");
    expectName("NACK for a reflector that is not in the lists", "NACK", "99999");

    printf("link without an answer\n");
    hostWriteConn("W1ABC", "00001");
    expectName("NACK when the reflector does not reply", "NACK", "00001");
    while (reflRead(r, 300) > 0) { }

    printf("configuration\n");
    {
        uint8_t c[9] = { 0x61, 0, 9, 4, 'C', 'O', 'M', 'M', 0x04 };
        write(tcpFd, c, 9);
        n = hostRead(r, 2000);
        check(n == 8 && memcmp(r + 4, "COMM", 4) == 0, "COMM acknowledged");
    }

    printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
