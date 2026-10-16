/*
 *  loader_core.c - the ST USART boot loader protocol (AN3155), the subset
 *  stm32flash needs: Get, Get Version, Get ID, Read Memory, Go, Write
 *  Memory and Erase (0x43).  Portable C; the hardware sits behind loader.h.
 */

#include "loader.h"

#define CMD_GET     0x00U
#define CMD_GVR     0x01U
#define CMD_GID     0x02U
#define CMD_READ    0x11U
#define CMD_GO      0x21U
#define CMD_WRITE   0x31U
#define CMD_ERASE   0x43U

static uint8_t buf[256];
static uint8_t rxErr;                       /* a byte was lost to a line error */

/* A byte with a line error still takes its place in the frame (so the
 * rest stays aligned); it is only flagged, and the command that contains
 * it is refused. */
static uint8_t rx(void)
{
    int c = ld_getc();

    if (c < 0)
    {
        rxErr = 1;
        return 0;
    }
    return (uint8_t)c;
}

static void ack(void)
{
    ld_putc(LD_ACK);
}

static void nack(void)
{
    ld_putc(LD_NACK);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int inFlash(uint32_t a, uint32_t len)
{
    return (a >= LD_FLASH_BASE) && (a < LD_FLASH_END) && (len <= LD_FLASH_END - a);
}

static int inRam(uint32_t a, uint32_t len)
{
    return (a >= LD_RAM_BASE) && (a < LD_RAM_BASE + LD_RAM_SIZE) &&
           (len <= LD_RAM_BASE + LD_RAM_SIZE - a);
}

/* 4 byte big endian address + XOR; 1 = good */
static int rxAddr(uint32_t *a)
{
    uint8_t b[5];
    uint8_t i;
    uint8_t x = 0;
    int ok;

    for (i = 0; i < 5; i++)
    {
        b[i] = rx();
    }
    for (i = 0; i < 4; i++)
    {
        x ^= b[i];
    }
    ok = (x == b[4]) && !rxErr;
    rxErr = 0;
    *a = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    return ok;
}

int ld_app_valid(void)
{
    uint8_t v[8];
    uint32_t sp;
    uint32_t pc;

    if (!ld_marker_valid())
    {
        return 0;
    }
    ld_flash_read(LD_APP_START, v, 8);
    sp = le32(v);
    pc = le32(v + 4);
    return (sp > LD_RAM_BASE) && (sp <= LD_RAM_BASE + LD_RAM_SIZE) &&
           ((pc & 1U) != 0U) && (pc > LD_APP_START) && (pc < LD_FLASH_END);
}

/* ------------------------------------------------------------------ */
static void cmdGet(void)
{
    static const uint8_t list[] =
    {
        LD_VERSION, CMD_GET, CMD_GVR, CMD_GID, CMD_READ, CMD_GO, CMD_WRITE, CMD_ERASE
    };
    uint8_t i;

    ack();
    ld_putc((uint8_t)(sizeof(list) - 1U));      /* bytes that follow, minus 1 */
    for (i = 0; i < sizeof(list); i++)
    {
        ld_putc(list[i]);
    }
    ack();
}

static void cmdGvr(void)
{
    ack();
    ld_putc(LD_VERSION);
    ld_putc(0);                                 /* option bytes: none        */
    ld_putc(0);
    ack();
}

static void cmdGid(void)
{
    ack();
    ld_putc(1);
    ld_putc((uint8_t)(LD_PID >> 8));
    ld_putc((uint8_t)(LD_PID & 0xFFU));
    ack();
}

static void cmdRead(void)
{
    uint32_t a;
    uint8_t n;
    uint8_t c;
    uint32_t len;
    uint32_t i;

    ack();
    if (!rxAddr(&a) || !(inFlash(a, 1) || inRam(a, 1)))
    {
        nack();
        return;
    }
    ack();
    n = rx();
    c = rx();
    len = (uint32_t)n + 1U;
    if (c != (uint8_t)~n || rxErr || !(inFlash(a, len) || inRam(a, len)))
    {
        rxErr = 0;
        nack();
        return;
    }
    ack();
    ld_flash_read(a, buf, len);
    for (i = 0; i < len; i++)
    {
        ld_putc(buf[i]);
    }
}

static void cmdWrite(void)
{
    uint32_t a;
    uint8_t n;
    uint8_t x;
    uint8_t cs;
    uint32_t len;
    uint32_t i;

    ack();
    if (!rxAddr(&a) || (a & 3U) || a < LD_APP_START || a >= LD_FLASH_END)
    {
        nack();
        return;
    }
    if (ld_marker_invalidate() != 0)            /* image incomplete from now on,
                                                   even without an Erase first */
    {
        nack();
        return;
    }
    ack();
    n = rx();
    x = n;
    len = (uint32_t)n + 1U;
    for (i = 0; i < len; i++)
    {
        buf[i] = rx();
        x ^= buf[i];
    }
    cs = rx();
    if (cs != x || rxErr || (len & 1U) || !inFlash(a, len))
    {
        rxErr = 0;
        nack();
        return;
    }
    if (ld_flash_program(a, buf, len) != 0)
    {
        nack();
        return;
    }
    ack();
}

static void eraseApp(void)
{
    uint32_t p;

    for (p = LD_APP_FIRST_PAGE; p < LD_TOTAL_PAGES; p++)
    {
        if (ld_flash_erase_page(p) != 0)
        {
            nack();
            return;
        }
    }
    ack();
}

static void cmdErase(void)
{
    uint8_t n;
    uint8_t x;
    uint8_t cs;
    uint32_t count;
    uint32_t i;

    ack();
    n = rx();
    if (n == 0xFFU)                             /* global erase: app area only */
    {
        cs = rx();
        if (cs != 0x00U || rxErr || ld_marker_invalidate() != 0)
        {
            rxErr = 0;
            nack();
            return;
        }
        eraseApp();
        return;
    }
    count = (uint32_t)n + 1U;
    x = n;
    for (i = 0; i < count; i++)
    {
        buf[i] = rx();
        x ^= buf[i];
    }
    cs = rx();
    if (cs != x || rxErr)
    {
        rxErr = 0;
        nack();
        return;
    }
    for (i = 0; i < count; i++)                 /* never the loader or marker */
    {
        if (buf[i] < LD_APP_FIRST_PAGE)
        {
            nack();
            return;
        }
    }
    if (ld_marker_invalidate() != 0)            /* app is incomplete from now on */
    {
        nack();
        return;
    }
    for (i = 0; i < count; i++)
    {
        /* stm32flash assumes the largest flash of the family and may list
         * pages this chip does not have: those are simply skipped */
        if (buf[i] < LD_TOTAL_PAGES && ld_flash_erase_page(buf[i]) != 0)
        {
            nack();
            return;
        }
    }
    ack();
}

/* A refused Go.  With LDR_DEBUG the reason follows the NACK on the wire
 * ('G', reason, then address, sp and pc as little endian words): 1 = bad
 * address or checksum, 2 = start vectors out of range, 3 = marker commit. */
static void goFail(uint8_t why, uint32_t a, uint32_t sp, uint32_t pc)
{
    nack();
#if LDR_DEBUG
    {
        uint32_t w[3];
        uint8_t i;
        uint8_t j;

        w[0] = a;
        w[1] = sp;
        w[2] = pc;
        ld_putc('G');
        ld_putc(why);
        for (i = 0; i < 3; i++)
        {
            for (j = 0; j < 4; j++)
            {
                ld_putc((uint8_t)(w[i] >> (8 * j)));
            }
        }
    }
#else
    (void)why;
    (void)a;
    (void)sp;
    (void)pc;
#endif
}

/* "Go" to the application start commits the image and restarts. */
static void cmdGo(void)
{
    uint32_t a;
    uint8_t v[8];
    uint32_t sp = 0;
    uint32_t pc = 0;

    ack();
    if (!rxAddr(&a) || a != LD_APP_START)
    {
        goFail(1, a, sp, pc);
        return;
    }
    ld_flash_read(LD_APP_START, v, 8);
    sp = le32(v);
    pc = le32(v + 4);
    if (!((sp > LD_RAM_BASE) && (sp <= LD_RAM_BASE + LD_RAM_SIZE) &&
          (pc & 1U) && pc > LD_APP_START && pc < LD_FLASH_END))
    {
        goFail(2, a, sp, pc);
        return;
    }
    if (ld_marker_commit() != 0)
    {
        goFail(3, a, sp, pc);
        return;
    }
    ack();
    ld_flush();
    ld_launch();
}

void ld_run(void)
{
    uint8_t c;
    uint8_t n;

    ld_autobaud();
    rxErr = 0;
    ack();

    for (;;)
    {
        c = rx();
        if (c == 0x7FU)                         /* host re-syncing */
        {
            nack();
            continue;
        }
        n = rx();
        if (n != (uint8_t)~c || rxErr)
        {
            rxErr = 0;
            nack();
            continue;
        }
        switch (c)
        {
        case CMD_GET:   cmdGet();   break;
        case CMD_GVR:   cmdGvr();   break;
        case CMD_GID:   cmdGid();   break;
        case CMD_READ:  cmdRead();  break;
        case CMD_GO:    cmdGo();    break;
        case CMD_WRITE: cmdWrite(); break;
        case CMD_ERASE: cmdErase(); break;
        default:        nack();     break;
        }
    }
}
