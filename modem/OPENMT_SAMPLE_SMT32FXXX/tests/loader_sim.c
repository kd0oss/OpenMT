/*
 *  loader_sim.c - runs loader/loader_core.c on the PC.
 *
 *    loader_sim selftest          scripted protocol tests (error cases etc.)
 *    loader_sim pty [file]        serves the loader on a pseudo terminal so the
 *                                 real stm32flash can talk to it; prints the
 *                                 pty name on stdout.  On "Go" it writes the
 *                                 flash image to [file] and exits.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <setjmp.h>
#include <termios.h>
#include "loader.h"

static uint8_t flash[LD_FLASH_KB * 1024];
static uint8_t ram[LD_RAM_KB * 1024];
static int mode_pty;
static int mfd = -1;
static const char *dumpFile;

/* scripted input / captured output for the self test */
static uint8_t inq[4096];
static unsigned inLen, inPos;
static uint8_t outq[4096];
static unsigned outLen;
static jmp_buf endOfInput;
static int launched;
static int failures;
static int lineErrorAt = -1;                /* input index to flag as a line error */

#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

void ld_autobaud(void)
{
    if (mode_pty)
    {
        uint8_t b;

        do
        {
            if (read(mfd, &b, 1) != 1)
            {
                exit(1);
            }
        } while (b != 0x7F);
    }
    else
    {
        inPos++;                            /* the 0x7F is the first input byte */
    }
}

int ld_getc(void)
{
    uint8_t b;

    if (mode_pty)
    {
        if (read(mfd, &b, 1) != 1)
        {
            exit(1);
        }
        return b;
    }
    if (inPos >= inLen)
    {
        longjmp(endOfInput, 1);
    }
    if ((int)inPos == lineErrorAt)
    {
        inPos++;
        return -1;
    }
    return inq[inPos++];
}

void ld_putc(uint8_t c)
{
    if (mode_pty)
    {
        if (write(mfd, &c, 1) != 1)
        {
            exit(1);
        }
    }
    else if (outLen < sizeof(outq))
    {
        outq[outLen++] = c;
    }
}

void ld_flush(void) {}
void ld_kick(void) {}

int ld_flash_erase_page(uint32_t page)
{
    if (page >= LD_TOTAL_PAGES)
    {
        return -1;
    }
    memset(flash + page * LD_PAGE_SIZE, 0xFF, LD_PAGE_SIZE);
    return 0;
}

int ld_flash_program(uint32_t addr, const uint8_t *d, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
    {
        uint8_t *p = &flash[addr - LD_FLASH_BASE + i];

        if ((*p & d[i]) != d[i])             /* can only clear bits */
        {
            return -1;
        }
        *p &= d[i];
    }
    return 0;
}

void ld_flash_read(uint32_t addr, uint8_t *d, uint32_t len)
{
    if (addr >= LD_RAM_BASE)
    {
        memcpy(d, ram + (addr - LD_RAM_BASE), len);
    }
    else
    {
        memcpy(d, flash + (addr - LD_FLASH_BASE), len);
    }
}

int ld_marker_valid(void)
{
    return flash[LD_MARKER_ADDR - LD_FLASH_BASE] == 0xFF &&
           flash[LD_MARKER_ADDR - LD_FLASH_BASE + 1] == 0xFF;
}

int ld_marker_invalidate(void)
{
    static const uint8_t zero[2] = { 0, 0 };

    return ld_marker_valid() ? ld_flash_program(LD_MARKER_ADDR, zero, 2) : 0;
}

int ld_marker_commit(void)
{
    return ld_marker_valid() ? 0 : ld_flash_erase_page((LD_MARKER_ADDR - LD_FLASH_BASE) / LD_PAGE_SIZE);
}

void ld_launch(void)
{
    if (mode_pty)
    {
        if (dumpFile)
        {
            FILE *f = fopen(dumpFile, "wb");

            if (f)
            {
                fwrite(flash, 1, sizeof(flash), f);
                fclose(f);
            }
        }
        fprintf(stderr, "sim: launch, app valid = %d\n", ld_app_valid());
        usleep(300000);                     /* let the last ACK reach the reader */
        exit(0);
    }
    launched = 1;
    longjmp(endOfInput, 2);
}

/* ------------------------------------------------------------------ */
/* Self test                                                          */
/* ------------------------------------------------------------------ */
static void put(const void *p, unsigned n)
{
    memcpy(inq + inLen, p, n);
    inLen += n;
}

static void putb(uint8_t b)
{
    put(&b, 1);
}

static void cmd(uint8_t c)
{
    putb(c);
    putb((uint8_t)~c);
}

static void addr(uint32_t a)
{
    uint8_t b[5] = { (uint8_t)(a >> 24), (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a, 0 };

    b[4] = b[0] ^ b[1] ^ b[2] ^ b[3];
    put(b, 5);
}

static void writeBlock(uint32_t a, const uint8_t *d, unsigned len, int badCs)
{
    uint8_t x = (uint8_t)(len - 1);
    unsigned i;

    cmd(0x31);
    addr(a);
    putb((uint8_t)(len - 1));
    for (i = 0; i < len; i++)
    {
        putb(d[i]);
        x ^= d[i];
    }
    putb(badCs ? (uint8_t)(x ^ 0x55) : x);
}

static void erasePages(const uint8_t *pg, unsigned n)
{
    uint8_t x = (uint8_t)(n - 1);
    unsigned i;

    cmd(0x43);
    putb((uint8_t)(n - 1));
    for (i = 0; i < n; i++)
    {
        putb(pg[i]);
        x ^= pg[i];
    }
    putb(x);
}

static void begin(void)
{
    inLen = 0;
    inPos = 0;
    outLen = 0;
    launched = 0;
    lineErrorAt = -1;
    putb(0x7F);
}

static int run(void)
{
    int r = setjmp(endOfInput);

    if (r == 0)
    {
        ld_run();
    }
    return r;
}

static int outIs(const uint8_t *want, unsigned n)
{
    return outLen == n && memcmp(outq, want, n) == 0;
}

static void freshFlash(void)
{
    memset(flash, 0xFF, sizeof(flash));
    memset(ram, 0, sizeof(ram));
    /* "loader" content below the marker page, to prove it is protected */
    memset(flash, 0xA5, 16);
}

static void selftest(void)
{
    uint8_t d[8] = { 0x00, 0x80, 0x00, 0x20, 0x01, 0x19, 0x00, 0x08 };  /* sp, pc */
    uint8_t d2[16];
    uint8_t pg[4];
    unsigned i;

    /* init + Get */
    freshFlash();
    begin();
    cmd(0x00);
    run();
    {
        const uint8_t want[] = { 0x79, 0x79, 7, LD_VERSION, 0x00, 0x01, 0x02, 0x11, 0x21, 0x31, 0x43, 0x79 };

        CHECK(outIs(want, sizeof(want)), "Get reply");
    }

    /* Get Version, Get ID */
    begin();
    cmd(0x01);
    cmd(0x02);
    run();
    {
        const uint8_t want[] = { 0x79, 0x79, LD_VERSION, 0, 0, 0x79, 0x79, 1, 0x04, 0x18, 0x79 };

        CHECK(outIs(want, sizeof(want)), "GVR / GID reply");
    }

    /* bad complement and unknown command -> NACK; 0x7F resync -> NACK */
    begin();
    putb(0x00);
    putb(0x00);
    putb(0x7F);
    cmd(0x99);
    run();
    {
        const uint8_t want[] = { 0x79, 0x1F, 0x1F, 0x1F };

        CHECK(outIs(want, sizeof(want)), "bad command handling");
    }

    /* a good write works on erased flash; each refusal is its own session,
     * because after a NACK the host stops (the rest of its frame would be
     * read as commands, as with the ST loader) */
    freshFlash();
    begin();
    writeBlock(LD_APP_START, d, 8, 0);
    run();
    {
        const uint8_t want[] = { 0x79, 0x79, 0x79, 0x79 };

        CHECK(outIs(want, sizeof(want)), "good write replies");
    }
    CHECK(memcmp(flash + (LD_APP_START - LD_FLASH_BASE), d, 8) == 0, "write content");
    CHECK(!ld_marker_valid(), "write did not invalidate the marker");

    {
        static const struct { uint32_t a; int badCs; const char *what; } bad[] =
        {
            { LD_APP_START - 4UL, 0, "below the app" },
            { 0x08000000UL,       0, "loader" },
            { LD_MARKER_ADDR,     0, "marker page" },
            { LD_APP_START + 2UL, 0, "unaligned" },
            { LD_FLASH_END,       0, "past the end" },
        };

        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        {
            freshFlash();
            begin();
            writeBlock(bad[i].a, d, 8, 0);
            inLen = 1 + 2 + 5;                      /* the host stops at the NACK */
            run();
            CHECK(outLen == 3 && outq[1] == 0x79 && outq[2] == 0x1F, "write %s not refused", bad[i].what);
            CHECK(flash[0] == 0xA5 && flash[LD_APP_START - LD_FLASH_BASE] == 0xFF, "write %s changed flash", bad[i].what);
        }
    }

    freshFlash();
    begin();
    writeBlock(LD_APP_START, d, 8, 1);              /* bad checksum */
    run();
    CHECK(outLen == 4 && outq[3] == 0x1F, "bad checksum not NACKed");
    CHECK(flash[LD_APP_START - LD_FLASH_BASE] == 0xFF, "bad checksum block written");

    /* erase protects the loader and the marker page */
    {
        static const uint8_t refused[] = { 0, 1, 2 };

        for (i = 0; i < sizeof(refused); i++)
        {
            freshFlash();
            begin();
            pg[0] = refused[i];
            erasePages(pg, 1);
            run();
            CHECK(outLen == 3 && outq[2] == 0x1F, "erase of page %u not refused", refused[i]);
            CHECK(flash[0] == 0xA5 && ld_marker_valid(), "refused erase changed flash");
        }
    }

    /* pages past the end of this chip's flash are skipped, not an error */
    freshFlash();
    begin();
    pg[0] = 3;
    pg[1] = (uint8_t)LD_TOTAL_PAGES;
    pg[2] = 127;
    erasePages(pg, 3);
    run();
    {
        const uint8_t want[] = { 0x79, 0x79, 0x79 };

        CHECK(outIs(want, sizeof(want)), "erase past the end replies");
    }

    /* a full update: erase pages, write, Go -> committed; marker invalid in between */
    freshFlash();
    begin();
    pg[0] = 3;
    pg[1] = 4;
    erasePages(pg, 2);
    run();
    CHECK(!ld_marker_valid(), "marker not invalidated by erase");
    CHECK(!ld_app_valid(), "app valid during update");
    begin();
    writeBlock(LD_APP_START, d, 8, 0);
    run();
    CHECK(!ld_app_valid(), "half written app accepted before Go");
    begin();
    cmd(0x21);
    addr(LD_APP_START);
    run();
    CHECK(launched, "Go did not launch");
    CHECK(ld_marker_valid() && ld_app_valid(), "Go did not commit");
    {
        const uint8_t want[] = { 0x79, 0x79, 0x79 };

        CHECK(outIs(want, sizeof(want)), "Go replies");
    }

    /* Go with a bad vector table or a wrong address is refused */
    freshFlash();
    begin();
    cmd(0x21);
    addr(LD_APP_START);                             /* blank flash */
    cmd(0x21);
    addr(0x08000000UL);
    run();
    CHECK(!launched, "Go launched a blank app");
    {
        const uint8_t want[] = { 0x79, 0x79, 0x1F, 0x79, 0x1F };

        CHECK(outIs(want, sizeof(want)), "Go refusal replies");
    }

    /* global erase only clears the application area */
    freshFlash();
    memset(flash + LD_APP_START - LD_FLASH_BASE, 0x00, 64);
    begin();
    cmd(0x43);
    putb(0xFF);
    putb(0x00);
    run();
    CHECK(flash[0] == 0xA5, "mass erase hit the loader");
    CHECK(flash[LD_APP_START - LD_FLASH_BASE] == 0xFF && flash[LD_FLASH_KB * 1024 - 1] == 0xFF, "mass erase missed the app");
    CHECK(!ld_marker_valid(), "mass erase did not invalidate");

    /* read memory: flash, then refused outside memory */
    freshFlash();
    for (i = 0; i < 16; i++)
    {
        flash[LD_APP_START - LD_FLASH_BASE + i] = (uint8_t)(i + 1);
    }
    begin();
    cmd(0x11);
    addr(LD_APP_START);
    cmd(15);                                        /* 16 bytes */
    cmd(0x11);
    addr(0x40000000UL);
    run();
    memcpy(d2, outq + 4, 16);
    CHECK(outLen == 4 + 16 + 2 && outq[0] == 0x79 && outq[1] == 0x79 && outq[2] == 0x79 && outq[3] == 0x79, "read replies");
    for (i = 0; i < 16; i++)
    {
        CHECK(d2[i] == i + 1, "read data %u", i);
    }
    CHECK(outq[20] == 0x79 && outq[21] == 0x1F, "read outside memory not refused");

    /* a line error inside a write block makes it NACK and nothing is written */
    freshFlash();
    begin();
    writeBlock(LD_APP_START, d, 8, 0);
    lineErrorAt = 1 + 2 + 5 + 3;                    /* inside the data bytes */
    {
        int r = setjmp(endOfInput);

        if (r == 0)
        {
            ld_run();
        }
    }
    CHECK(flash[LD_APP_START - LD_FLASH_BASE] == 0xFF, "block with a line error was written");
    CHECK(outLen >= 4 && outq[3] == 0x1F, "line error not NACKed");

    /* boot decision helpers */
    freshFlash();
    CHECK(!ld_app_valid(), "blank flash counts as an app");
    memcpy(flash + (LD_APP_START - LD_FLASH_BASE), d, 8);
    CHECK(ld_app_valid(), "good vectors not accepted");
    d[3] = 0x40;                                    /* sp outside RAM */
    memcpy(flash + (LD_APP_START - LD_FLASH_BASE), d, 8);
    CHECK(!ld_app_valid(), "sp outside RAM accepted");
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "pty") == 0)
    {
        struct termios t;

        mode_pty = 1;
        dumpFile = (argc >= 3) ? argv[2] : NULL;
        memset(flash, 0xFF, sizeof(flash));
        mfd = posix_openpt(O_RDWR | O_NOCTTY);
        if (mfd < 0 || grantpt(mfd) || unlockpt(mfd))
        {
            perror("pty");
            return 1;
        }
        tcgetattr(mfd, &t);
        cfmakeraw(&t);
        tcsetattr(mfd, TCSANOW, &t);
        printf("%s\n", ptsname(mfd));
        fflush(stdout);
        ld_run();
        return 0;
    }

    selftest();
    if (failures)
    {
        printf("\n%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("LOADER TESTS PASSED\n");
    return 0;
}
