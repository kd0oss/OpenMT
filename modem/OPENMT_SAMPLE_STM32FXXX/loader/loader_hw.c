/* Created by Rick KD0OSS */
/*
 *  loader_hw.c - STM32F105 hardware layer and start-up of the serial loader.
 *
 *  Runs from the reset state: HSI 8 MHz, no PLL, nothing else enabled.
 *  USART1 on PA9 (TX) / PA10 (RX), as the ROM boot loader would use.
 */

#include "stm32f10x.h"
#include "loader.h"

#define HCLK_HZ         8000000UL
#define RX_BIT          (1UL << 10)

/* ------------------------------------------------------------------ */
/* Watchdog                                                           */
/*
 * ld_kick()
 *
 * Feeds the independent watchdog (harmless if it is not running).
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void ld_kick(void)
{
    IWDG->KR = 0xAAAAU;                 /* harmless if the IWDG is not running */
}

/* ------------------------------------------------------------------ */
/* UART                                                               */
/* ------------------------------------------------------------------ */
/* LD_FIXED_BAUD != 0: the host (stm32flash -b 115200, as openmt_flash.sh
 * does) is expected at exactly that rate; the 0x7F is received normally.
 * LD_FIXED_BAUD 0: measure the baud rate from the 0x7F (auto-baud). */
#ifndef LD_FIXED_BAUD
#define LD_FIXED_BAUD   0UL
#endif

/* Optional status LED (build with LD_LED_PORT / LD_LED_NUM, active high):
 * on while the loader waits for the host, off once the host has been heard. */
#ifndef LD_LED_PORT
#define LD_LED_PORT     (-1)
#endif
#ifndef LD_LED_NUM
#define LD_LED_NUM      0
#endif

/* LDR_DEBUG 1: every received byte that is not a clean 0x7F is reported
 * (LED blinks: 1 = framing error, 2 = parity error, 3 = overrun, 4 = a good
 * byte with another value; and 3 bytes on the UART: the byte, the USART
 * status register and 0xA5).  For finding out why the host is not heard. */
#ifndef LDR_DEBUG
#define LDR_DEBUG        0
#endif

#if LD_LED_PORT >= 0
#define LED_GPIO        ((GPIO_TypeDef *)(GPIOA_BASE + 0x400UL * (uint32_t)LD_LED_PORT))
/*
 * ledInit()
 *
 * Sets up the optional status LED pin as an output and turns the LED on.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void ledInit(void)
{
    volatile uint32_t *cr = (volatile uint32_t *)&LED_GPIO->CRL + (LD_LED_NUM >> 3);

    RCC->APB2ENR |= (RCC_APB2ENR_IOPAEN << LD_LED_PORT);
    (void)RCC->APB2ENR;
    *cr = (*cr & ~(0xFUL << ((LD_LED_NUM & 7) * 4))) | (0x2UL << ((LD_LED_NUM & 7) * 4));
    LED_GPIO->BSRR = 1UL << LD_LED_NUM;
}
/*
 * ledOff()
 *
 * Turns the status LED off.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void ledOff(void)
{
    LED_GPIO->BSRR = 1UL << (LD_LED_NUM + 16);
}
/*
 * ledOn()
 *
 * Turns the status LED on.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
__attribute__((unused)) static void ledOn(void)
{
    LED_GPIO->BSRR = 1UL << LD_LED_NUM;
}
#else
#define ledInit()       ((void)0)
#define ledOff()        ((void)0)
#define ledOn()         ((void)0)
#endif

#if LDR_DEBUG && (LD_LED_PORT >= 0)
/*
 * pause()
 *
 * Busy-waits for roughly n * 15 ms while feeding the watchdog (debug aid).
 *
 * Parameters:
 *   n - Number of 15 ms units.
 *
 * Returns: nothing.
 */
static void pause(uint32_t n)
{
    volatile uint32_t i;

    for (i = 0; i < n * 20000UL; i++)               /* about n * 15 ms */
    {
        if ((i & 0x3FFFUL) == 0U)
        {
            ld_kick();
        }
    }
}

/*
 * ledBlinks()
 *
 * Debug aid: after a pause, blinks the status LED n times to report why a
 * received byte was not accepted as the host's 0x7F.
 *
 * Parameters:
 *   n - Number of blinks.
 *
 * Returns: nothing.
 */
__attribute__((unused)) static void ledBlinks(uint32_t n)
{
    pause(30);
    while (n--)
    {
        ledOff();
        pause(5);
        ledOn();
        pause(10);
    }
}
#else
#define ledBlinks(n)    ((void)0)
#endif

#if LD_FIXED_BAUD != 0
/*
 * ld_autobaud()
 *
 * Waits for the host's first byte (0x7F) on USART1 at the fixed baud rate
 * the loader was built for (LD_FIXED_BAUD), 8 data bits, even parity.  A
 * byte with a framing or parity error is ignored.  If the host does not
 * appear within about 10 s and a valid application exists, the application
 * is started.  The status LED goes off once the host has been heard.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void ld_autobaud(void)
{
    uint32_t sr;
    uint32_t wraps = 0;
    uint8_t d;

    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;
    ledInit();
    GPIOA->ODR |= RX_BIT;                                   /* PA10 pull-up   */
    GPIOA->CRH = (GPIOA->CRH & ~0x0FF0UL) | (0x8UL << 8) | (0xBUL << 4);
    SysTick->LOAD = 0x00FFFFFFUL;
    SysTick->VAL = 0;
    SysTick->CTRL = 5U;

    USART1->BRR = (uint16_t)((HCLK_HZ + LD_FIXED_BAUD / 2U) / LD_FIXED_BAUD);
    USART1->CR2 = 0;
    USART1->CR3 = 0;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE |
                  USART_CR1_M | USART_CR1_PCE;              /* 8E1            */

    sr = USART1->SR;                                        /* drop anything stale */
    (void)USART1->DR;

    for (;;)                                                /* wait for 0x7F  */
    {
        sr = USART1->SR;
        if ((sr & (USART_SR_RXNE | USART_SR_ORE)) != 0U)
        {
            d = (uint8_t)USART1->DR;
            /* an overrun flag alone does not matter here: the byte itself is checked */
            if (d == 0x7FU && (sr & (USART_SR_FE | USART_SR_PE)) == 0U)
            {
                break;
            }
#if LDR_DEBUG
            ld_putc(d);
            ld_putc((uint8_t)sr);
            ld_putc(0xA6U);
            ledBlinks(((sr & USART_SR_FE) != 0U) ? 1U :
                      ((sr & USART_SR_PE) != 0U) ? 2U :
                      ((sr & USART_SR_ORE) != 0U) ? 3U : 4U);
#endif
            continue;
        }
        ld_kick();
#if LDR_DEBUG
        {   /* beacon: a 'U' (0x55) about every 33 ms, so the host can see
             * what the UART really sends and at which speed */
            static uint32_t lastTick;
            uint32_t t = SysTick->VAL >> 18;

            if (t != lastTick)
            {
                lastTick = t;
                ld_putc(0x55U);
            }
        }
#endif
        /* asked for update mode, but nobody comes: after about 10 s go back
         * to the application, if there is one */
        if ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) != 0U)
        {
            if (++wraps >= 5U && ld_app_valid())
            {
                ld_launch();
            }
        }
    }
    ledOff();
}
#else
/*
 * ld_autobaud()
 *
 * Measures the host's baud rate from its first byte (0x7F) and sets up
 * USART1 for it (8 data bits, even parity).  The pin is watched directly:
 * the time from the falling edge of the start bit to the next falling edge
 * is 8 bit times, and the first low period must be one eighth of that.
 * Edges are examined as a sliding window, so a glitch ahead of the real
 * byte is rejected.  If no host appears within about 10 s and a valid
 * application exists, the application is started.  The status LED goes off
 * once the host has been heard.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
/* The host sends 0x7F: with 8E1 that is start(0) 1111111 0 parity(1) stop(1),
 * so the line goes low for one bit, high for seven, low again for one:
 * falling edge to the next falling edge is exactly 8 bit times and the first
 * low is exactly 1/8 of it.  Edges are examined as a sliding window, so a
 * glitch ahead of the real byte (a serial port being re-configured) is
 * rejected and the very next edges are tried as the start of the byte. */
void ld_autobaud(void)
{
    uint32_t t1;
    uint32_t tr;
    uint32_t t2;
    uint32_t cyc;
    uint32_t low;
    uint32_t dev;
    uint32_t wraps = 0;

    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;                                     /* let the clock settle */
    ledInit();
    GPIOA->ODR |= RX_BIT;                                   /* PA10 pull-up   */
    GPIOA->CRH = (GPIOA->CRH & ~0x0FF0UL) | (0x8UL << 8) | (0xBUL << 4);
                                                            /* PA10 in PU, PA9 AF PP */
    SysTick->LOAD = 0x00FFFFFFUL;
    SysTick->VAL = 0;
    SysTick->CTRL = 5U;                                     /* HCLK, running  */

    while ((GPIOA->IDR & RX_BIT) == 0U)                     /* line idle      */
    {
        ld_kick();
    }
    while ((GPIOA->IDR & RX_BIT) != 0U)                     /* first edge     */
    {
        ld_kick();
        /* asked for update mode, but nobody comes: after about 10 s go
         * back to the application, if there is one */
        if ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) != 0U)
        {
            if (++wraps >= 5U && ld_app_valid())
            {
                ld_launch();
            }
        }
    }
    t1 = SysTick->VAL;

    for (;;)
    {
        while ((GPIOA->IDR & RX_BIT) == 0U)                 /* low phase      */
        {
        }
        tr = SysTick->VAL;
        while ((GPIOA->IDR & RX_BIT) != 0U)                 /* high phase     */
        {
            if ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) != 0U)
            {
                ld_kick();                                  /* idle a long time */
            }
        }
        t2 = SysTick->VAL;
        cyc = (t1 - t2) & 0x00FFFFFFUL;                     /* 8 bit times    */
        low = (t1 - tr) & 0x00FFFFFFUL;                     /* 1 bit time     */
        dev = (8UL * low > cyc) ? (8UL * low - cyc) : (cyc - 8UL * low);

        if (cyc >= 128UL && cyc <= 20000UL && dev * 8UL <= cyc * 3UL)
        {
            break;                                          /* looks like 0x7F */
        }
        t1 = t2;                                            /* slide the window */
    }

    /* let the rest of that byte (d7, parity, stop) pass */
    while (((t2 - SysTick->VAL) & 0x00FFFFFFUL) < (cyc / 2U))
    {
    }

    /* 8 bit times took cyc ticks of the very clock the USART runs from, so
     * BRR (= clock / baud) is cyc / 8: no assumption about the real clock
     * frequency (the internal oscillator is not necessarily 8 MHz here) or
     * about the baud rate is needed. */
    USART1->BRR = (uint16_t)((cyc + 4U) / 8U);
    USART1->CR2 = 0;
    USART1->CR3 = 0;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE |
                  USART_CR1_M | USART_CR1_PCE;              /* 8E1            */
    ledOff();
#if LDR_DEBUG
    ld_putc((uint8_t)(USART1->BRR >> 8));                   /* what was measured */
    ld_putc((uint8_t)USART1->BRR);
#endif
}
#endif

/*
 * ld_getc()
 *
 * Waits for one byte from the host on the loader's UART, feeding the
 * watchdog meanwhile.
 *
 * Parameters: none.
 *
 * Returns: The byte (0 to 255), or -1 if it arrived with an overrun, framing
 *   or parity error.
 */
int ld_getc(void)
{
    uint32_t sr;
    uint8_t d;

    for (;;)
    {
        sr = USART1->SR;
        if ((sr & (USART_SR_RXNE | USART_SR_ORE)) != 0U)
        {
            d = (uint8_t)USART1->DR;                        /* clears the flags */
            /* NE (noise) is ignored: the byte is usually still right, and
             * parity plus the protocol checksums guard the data */
            if ((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_PE)) != 0U)
            {
                return -1;
            }
            return d;
        }
        ld_kick();
    }
}

/*
 * ld_putc()
 *
 * Sends one byte to the host, waiting until the transmit register is free.
 *
 * Parameters:
 *   c - Byte to send.
 *
 * Returns: nothing.
 */
void ld_putc(uint8_t c)
{
    while ((USART1->SR & USART_SR_TXE) == 0U)
    {
        ld_kick();
    }
    USART1->DR = c;
}

/*
 * ld_flush()
 *
 * Waits until everything written has been completely sent.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void ld_flush(void)
{
    while ((USART1->SR & USART_SR_TC) == 0U)
    {
        ld_kick();
    }
}

/* ------------------------------------------------------------------ */
/* Flash                                                              */
/*
 * flashWait()
 *
 * Waits for the flash controller to finish and clears its status flags.
 *
 * Parameters: none.
 *
 * Returns: 0 if the operation succeeded, -1 on a programming or write
 *   protection error.
 */
static int flashWait(void)
{
    uint32_t s;

    while ((FLASH->SR & FLASH_SR_BSY) != 0U)
    {
        ld_kick();
    }
    s = FLASH->SR;
    FLASH->SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;
    return ((s & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) != 0U) ? -1 : 0;
}

/*
 * flashUnlock()
 *
 * Unlocks the flash controller for programming and erasing, if it is locked.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void flashUnlock(void)
{
    if ((FLASH->CR & FLASH_CR_LOCK) != 0U)
    {
        FLASH->KEYR = 0x45670123UL;
        FLASH->KEYR = 0xCDEF89ABUL;
    }
}

/*
 * flashLock()
 *
 * Locks the flash controller again.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void flashLock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

/*
 * ld_flash_erase_page()
 *
 * Erases one flash page.
 *
 * Parameters:
 *   page - Page number counted from the start of flash.
 *
 * Returns: 0 on success, -1 on error.
 */
int ld_flash_erase_page(uint32_t page)
{
    int r;

    flashUnlock();
    (void)flashWait();                                      /* also clears old flags */
    FLASH->CR |= FLASH_CR_PER;
    FLASH->AR = LD_FLASH_BASE + page * LD_PAGE_SIZE;
    FLASH->CR |= FLASH_CR_STRT;
    r = flashWait();
    FLASH->CR &= ~FLASH_CR_PER;
    flashLock();
    return r;
}

/*
 * ld_flash_program()
 *
 * Programs bytes into erased flash, one 16-bit half word at a time.
 *
 * Parameters:
 *   addr - Flash address, half-word aligned.
 *   d    - Data to program.
 *   len  - Number of bytes; must be even.
 *
 * Returns: 0 on success, -1 on error.
 */
int ld_flash_program(uint32_t addr, const uint8_t *d, uint32_t len)
{
    uint32_t i;
    int r = 0;

    flashUnlock();
    (void)flashWait();
    FLASH->CR |= FLASH_CR_PG;
    for (i = 0; i < len; i += 2U)
    {
        *(volatile uint16_t *)(addr + i) = (uint16_t)(d[i] | ((uint16_t)d[i + 1U] << 8));
        if (flashWait() != 0)
        {
            r = -1;
            break;
        }
    }
    FLASH->CR &= ~FLASH_CR_PG;
    flashLock();
    return r;
}

/*
 * ld_flash_read()
 *
 * Reads bytes from flash or RAM.
 *
 * Parameters:
 *   addr - Address to read from.
 *   d    - Receives the bytes.
 *   len  - Number of bytes.
 *
 * Returns: nothing.
 */
void ld_flash_read(uint32_t addr, uint8_t *d, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
    {
        d[i] = *(volatile uint8_t *)(addr + i);
    }
}

/*
 * ld_marker_valid()
 *
 * Tells whether the "application complete" marker is set.  The marker is the
 * first half word of its own flash page: 0xFFFF means the application is
 * complete, 0x0000 means an update is under way.
 *
 * Parameters: none.
 *
 * Returns: 1 if the application is marked complete, 0 if not.
 */
int ld_marker_valid(void)
{
    return *(volatile uint16_t *)LD_MARKER_ADDR == 0xFFFFU;
}

/*
 * ld_marker_invalidate()
 *
 * Marks the application as incomplete by programming zero over the marker
 * (this needs no erase).  Does nothing if it is already marked so.
 *
 * Parameters: none.
 *
 * Returns: 0 on success, non-zero on a flash error.
 */
int ld_marker_invalidate(void)
{
    static const uint8_t zero[2] = { 0, 0 };

    if (!ld_marker_valid())
    {
        return 0;
    }
    return ld_flash_program(LD_MARKER_ADDR, zero, 2);
}

/*
 * ld_marker_commit()
 *
 * Marks the application as complete again by erasing the marker's page.
 * Does nothing if it is already marked complete.
 *
 * Parameters: none.
 *
 * Returns: 0 on success, non-zero on a flash error.
 */
int ld_marker_commit(void)
{
    if (ld_marker_valid())
    {
        return 0;
    }
    return ld_flash_erase_page((LD_MARKER_ADDR - LD_FLASH_BASE) / LD_PAGE_SIZE);
}

/*
 * ld_launch()
 *
 * Starts the application by resetting the chip; Reset_Handler() then jumps
 * to it.  Does not return.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void ld_launch(void)
{
    __DSB();
    NVIC_SystemReset();
    for (;;)
    {
    }
}

/* ------------------------------------------------------------------ */
/* Start-up                                                           */
/* ------------------------------------------------------------------ */
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _estack;

/*
 * jumpApp()
 *
 * Starts the application directly: loads the stack pointer and reset
 * vector from the application's vector table and jumps to it.
 *
 * Parameters: none.
 *
 * Returns: nothing (does not return).
 */
static void __attribute__((noreturn)) jumpApp(void)
{
    uint32_t sp = *(volatile uint32_t *)LD_APP_START;
    uint32_t pc = *(volatile uint32_t *)(LD_APP_START + 4U);

    __asm volatile ("msr msp, %0\n\tbx %1" : : "r" (sp), "r" (pc) : "memory");
    for (;;)
    {
    }
}

void Reset_Handler(void);
void Default_Handler(void);

/*
 * Default_Handler()
 *
 * Handler for unused interrupts; loops forever.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void Default_Handler(void)
{
    for (;;)
    {
    }
}

__attribute__((section(".isr_vector"), used))
const void * const vectorTable[16] =
{
    &_estack,
    Reset_Handler,
    Default_Handler,        /* NMI        */
    Default_Handler,        /* HardFault  */
    Default_Handler,
    Default_Handler,
    Default_Handler,
    0, 0, 0, 0,
    Default_Handler,        /* SVC        */
    Default_Handler,
    0,
    Default_Handler,        /* PendSV     */
    Default_Handler         /* SysTick    */
};

/*
 * Reset_Handler()
 *
 * Loader entry point.  Jumps straight to the application if it is valid and
 * no update was requested (the boot flag holds the magic value when the
 * application asks for an update); otherwise clears the BSS and runs the
 * loader.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void Reset_Handler(void)
{
    volatile uint32_t *flag = (volatile uint32_t *)LD_BOOT_FLAG_ADDR;
    uint32_t force = (*flag == LD_BOOT_MAGIC);
    uint32_t *p;

    *flag = 0;
    if (!force && ld_app_valid())
    {
        jumpApp();                      /* peripherals untouched: still reset state */
    }

    for (p = &_sbss; p < &_ebss; p++)
    {
        *p = 0;
    }
    ld_run();
    for (;;)
    {
    }
}
