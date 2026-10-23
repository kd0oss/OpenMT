/* Created by Rick KD0OSS */
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

/*
 * rx()
 *
 * Reads one byte from the loader's serial line.  A byte with a line error
 * still takes its place in the frame so the rest stays aligned; the error is
 * only flagged and the command that contains it is refused.
 *
 * Parameters: none.
 *
 * Returns: The byte, or 0 if there was a line error (the error flag is set).
 */
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

/*
 * ack()
 *
 * Sends the ACK byte to the host.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void ack(void)
{
    ld_putc(LD_ACK);
}

/*
 * nack()
 *
 * Sends the NACK byte to the host.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void nack(void)
{
    ld_putc(LD_NACK);
}

/*
 * le32()
 *
 * Reads a 32-bit little-endian value.
 *
 * Parameters:
 *   p - Pointer to 4 bytes.
 *
 * Returns: The value.
 */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/*
 * inFlash()
 *
 * Tells whether an address range lies inside the chip's flash.
 *
 * Parameters:
 *   a   - Start address.
 *   len - Length in bytes.
 *
 * Returns: 1 if the whole range is in flash, 0 otherwise.
 */
static int inFlash(uint32_t a, uint32_t len)
{
    return (a >= LD_FLASH_BASE) && (a < LD_FLASH_END) && (len <= LD_FLASH_END - a);
}

/*
 * inRam()
 *
 * Tells whether an address range lies inside the chip's RAM.
 *
 * Parameters:
 *   a   - Start address.
 *   len - Length in bytes.
 *
 * Returns: 1 if the whole range is in RAM, 0 otherwise.
 */
static int inRam(uint32_t a, uint32_t len)
{
    return (a >= LD_RAM_BASE) && (a < LD_RAM_BASE + LD_RAM_SIZE) &&
           (len <= LD_RAM_BASE + LD_RAM_SIZE - a);
}

/*
 * rxAddr()
 *
 * Reads a 4-byte big-endian address followed by its XOR checksum byte and
 * clears the line error flag.
 *
 * Parameters:
 *   a - Receives the address.
 *
 * Returns: 1 if the checksum is right and there was no line error, 0
 *   otherwise.
 */
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

/*
 * ld_app_valid()
 *
 * Checks that a complete application is installed: the completion marker is
 * valid and the initial stack pointer and reset vector in the application's
 * vector table are plausible (stack in RAM, odd Thumb address inside the
 * flash after the loader).
 *
 * Parameters: none.
 *
 * Returns: 1 if the application can be started, 0 otherwise.
 */
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

/*
 * cmdGet()
 *
 * Handles the Get command: replies with the loader version and the list of
 * commands supported.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
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

/*
 * cmdGvr()
 *
 * Handles the Get Version command: replies with the loader version and two
 * zero option bytes.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void cmdGvr(void)
{
    ack();
    ld_putc(LD_VERSION);
    ld_putc(0);                                 /* option bytes: none        */
    ld_putc(0);
    ack();
}

/*
 * cmdGid()
 *
 * Handles the Get ID command: replies with the product ID of the chip.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void cmdGid(void)
{
    ack();
    ld_putc(1);
    ld_putc((uint8_t)(LD_PID >> 8));
    ld_putc((uint8_t)(LD_PID & 0xFFU));
    ack();
}

/*
 * cmdRead()
 *
 * Handles Read Memory: receives an address and a length (with checks) and
 * sends back the bytes, from flash or RAM.  Refused with NACK if the range
 * is not in flash or RAM.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
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

/*
 * cmdWrite()
 *
 * Handles Write Memory: receives an address (word aligned, inside the
 * application area), marks the installed image as incomplete, receives up to
 * 256 bytes with their checksum and programs them into flash.  Refused with
 * NACK if the checksum or range is wrong or the length is odd.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
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

/*
 * eraseApp()
 *
 * Erases every page of the application area, then sends ACK (or NACK if a
 * page fails).
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
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

/*
 * cmdErase()
 *
 * Handles the Erase command.  A global erase (0xFF) erases only the
 * application area, never the loader.  Otherwise a list of pages is received
 * and erased; a list that names a loader page is refused and pages the chip
 * does not have are skipped.  The installed image is marked incomplete
 * first.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
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

/*
 * goFail()
 *
 * Sends the NACK for a refused Go command.  With LDR_DEBUG, the reason
 * follows on the wire: 'G', the reason, then the address, stack pointer and
 * reset vector as little-endian words.
 *
 * Parameters:
 *   why - Reason: 1 = bad address or checksum, 2 = start vectors out of
 *         range, 3 = the completion marker could not be written.
 *   a   - Address the host sent.
 *   sp  - Stack pointer read from the application.
 *   pc  - Reset vector read from the application.
 *
 * Returns: nothing.
 */
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

/*
 * cmdGo()
 *
 * Handles the Go command: the address must be the application start and its
 * vector table plausible.  The image is then marked complete (committed),
 * ACK is sent and the chip is reset into the application.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
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

/*
 * ld_run()
 *
 * Main loop of the loader: sets the baud rate from the host's first byte,
 * sends ACK and then handles commands (each sent as a code and its
 * complement) until the host starts the application.  A 0x7F byte from the
 * host is answered with NACK; a bad command or a line error also gets NACK.
 * Does not return.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
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
