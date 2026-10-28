/* Created by Rick KD0OSS */
/*
 *  hw_f4f7.c - STM32F446 and STM32F722 hardware layer.
 *
 *  Direct register access, no vendor library needed.  The two chips are
 *  close enough to share one file; the differences (USART registers,
 *  power control names, flash accelerator) are handled with #if.
 *
 *  F722 note: RAM is placed in SRAM1 (the DMA can reach it); the DTCM at
 *  0x20000000 is not used.  The data cache is left off, so the DMA
 *  buffers need no cache maintenance.
 */

#if defined(MCU_F446)
#include "stm32f4xx.h"
#else
#include "stm32f7xx.h"
#endif
#include "config.h"
#include "hw.h"
#include "clockcalc.h"

/* ------------------------------------------------------------------ */
/* Chip differences                                                   */
/* ------------------------------------------------------------------ */
#if defined(MCU_F446)
#define MAX_SYS_HZ          180000000UL
#define MAX_APB1_HZ         42000000UL      /* without over-drive        */
#define MAX_APB2_HZ         84000000UL
#define MAX_APB1_OD_HZ      45000000UL      /* with over-drive           */
#define MAX_APB2_OD_HZ      90000000UL
#define MIN_VCO_HZ          100000000UL
#define OVERDRIVE_ABOVE_HZ  168000000UL
#define PWR_CTRL            PWR->CR
#define PWR_STAT            PWR->CSR
#define PWR_CTRL_ODEN       PWR_CR_ODEN
#define PWR_CTRL_ODSWEN     PWR_CR_ODSWEN
#define PWR_STAT_ODRDY      PWR_CSR_ODRDY
#define PWR_STAT_ODSWRDY    PWR_CSR_ODSWRDY
#define PWR_CTRL_VOS        PWR_CR_VOS
#define FLASH_ACCEL         (FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN)
#define IWDG_RELOAD         500U                /* LSI about 32 kHz / 64 */
#define TIM1_UP_ISR         TIM1_UP_TIM10_IRQHandler
#else
#define MAX_SYS_HZ          216000000UL
#define MAX_APB1_HZ         45000000UL      /* without over-drive        */
#define MAX_APB2_HZ         90000000UL
#define MAX_APB1_OD_HZ      54000000UL      /* with over-drive           */
#define MAX_APB2_OD_HZ      108000000UL
#define MIN_VCO_HZ          192000000UL
#define OVERDRIVE_ABOVE_HZ  180000000UL
#define PWR_CTRL            PWR->CR1
#define PWR_STAT            PWR->CSR1
#define PWR_CTRL_ODEN       PWR_CR1_ODEN
#define PWR_CTRL_ODSWEN     PWR_CR1_ODSWEN
#define PWR_STAT_ODRDY      PWR_CSR1_ODRDY
#define PWR_STAT_ODSWRDY    PWR_CSR1_ODSWRDY
#define PWR_CTRL_VOS        PWR_CR1_VOS
#define FLASH_ACCEL         (FLASH_ACR_PRFTEN | FLASH_ACR_ARTEN)
#define IWDG_RELOAD         500U
#define TIM1_UP_ISR         TIM1_UP_TIM10_IRQHandler
#endif

/* ------------------------------------------------------------------ */
/* GPIO                                                               */
/* ------------------------------------------------------------------ */
#define GPIO_IN             0U
#define GPIO_OUT            1U
#define GPIO_AF             2U
#define GPIO_ANALOG         3U

#define PULL_NONE           0U
#define PULL_UP             1U
#define PULL_DOWN           2U

/*
 * gpioOf()
 *
 * Gets the GPIO port register block for a pin number (port number in the
 * high nibble: 0 = A, 1 = B, ...).
 *
 * Parameters:
 *   pin - Pin number encoded as (port << 4) | bit.
 *
 * Returns: Pointer to the port's registers.
 */
static GPIO_TypeDef *gpioOf(uint8_t pin)
{
    return (GPIO_TypeDef *)(GPIOA_BASE + ((uint32_t)(pin >> 4) * 0x400U));
}

/*
 * gpioClock()
 *
 * Turns on the clock of the pin's GPIO port and waits for it to take effect.
 * Does nothing for PIN_NONE.
 *
 * Parameters:
 *   pin - Pin number encoded as (port << 4) | bit.
 *
 * Returns: nothing.
 */
static void gpioClock(uint8_t pin)
{
    if (pin != PIN_NONE)
    {
        RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN << (pin >> 4));
        (void)RCC->AHB1ENR;                         /* let the clock settle */
    }
}

/*
 * gpioConfig()
 *
 * Configures a pin: mode, alternate function (for alternate function mode),
 * push-pull output type, fast speed and pull resistor.  The port clock is
 * enabled first.  Does nothing for PIN_NONE.
 *
 * Parameters:
 *   pin  - Pin number encoded as (port << 4) | bit.
 *   mode - Pin mode (input, output, alternate function or analog).
 *   af   - Alternate function number, used only when mode is the alternate
 *          function mode.
 *   pull - Pull resistor setting.
 *
 * Returns: nothing.
 */
static void gpioConfig(uint8_t pin, uint32_t mode, uint32_t af, uint32_t pull)
{
    GPIO_TypeDef *g = gpioOf(pin);
    uint32_t n = pin & 0x0FU;

    if (pin == PIN_NONE)
    {
        return;
    }
    gpioClock(pin);

    if (mode == GPIO_AF)
    {
        if (n < 8U)
        {
            g->AFR[0] = (g->AFR[0] & ~(0xFUL << (n * 4U))) | (af << (n * 4U));
        }
        else
        {
            g->AFR[1] = (g->AFR[1] & ~(0xFUL << ((n - 8U) * 4U))) | (af << ((n - 8U) * 4U));
        }
    }
    g->OTYPER &= ~(1UL << n);                       /* push-pull         */
    g->OSPEEDR = (g->OSPEEDR & ~(3UL << (n * 2U))) | (2UL << (n * 2U));   /* fast */
    g->PUPDR = (g->PUPDR & ~(3UL << (n * 2U))) | (pull << (n * 2U));
    g->MODER = (g->MODER & ~(3UL << (n * 2U))) | (mode << (n * 2U));
}

/*
 * gpioWrite()
 *
 * Drives a pin high or low using the atomic set/reset register.  Does
 * nothing for PIN_NONE.
 *
 * Parameters:
 *   pin   - Pin number encoded as (port << 4) | bit.
 *   level - true for high, false for low.
 *
 * Returns: nothing.
 */
static void gpioWrite(uint8_t pin, bool level)
{
    GPIO_TypeDef *g = gpioOf(pin);
    uint32_t n = pin & 0x0FU;

    if (pin == PIN_NONE)
    {
        return;
    }
    g->BSRR = level ? (1UL << n) : (1UL << (n + 16U));
}

/*
 * gpioRead()
 *
 * Reads the level of a pin.
 *
 * Parameters:
 *   pin - Pin number encoded as (port << 4) | bit.
 *
 * Returns: true if the pin is high.
 */
static bool gpioRead(uint8_t pin)
{
    return (gpioOf(pin)->IDR & (1UL << (pin & 0x0FU))) != 0U;
}

/* ------------------------------------------------------------------ */
/* Clocks                                                             */
/* ------------------------------------------------------------------ */
static uint32_t sysclkHz;
static uint32_t pclk1Hz;
static uint32_t pclk2Hz;
static uint32_t timClk1Hz;                  /* TIM2-4 input clock        */
static uint32_t timClk2Hz;                  /* TIM1 input clock          */

static volatile uint32_t msTicks;

/*
 * SysTick_Handler()
 *
 * 1 ms system tick interrupt; counts milliseconds for hw_millis().
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void SysTick_Handler(void)
{
    msTicks++;
}

/*
 * hw_chip_id()
 *
 * Reads the chip's debug ID code (DBGMCU_IDCODE).
 *
 * Parameters: none.
 *
 * Returns: The ID code.
 */
uint32_t hw_chip_id(void)
{
    return *(volatile uint32_t *)0xE0042000UL;      /* DBGMCU_IDCODE */
}

/*
 * hw_millis()
 *
 * Gets the time since start-up.
 *
 * Parameters: none.
 *
 * Returns: Milliseconds (wraps after about 49 days).
 */
uint32_t hw_millis(void)
{
    return msTicks;
}

/*
 * waitFlag()
 *
 * Waits, for a limited time, for bits in a register to be set or cleared.
 *
 * Parameters:
 *   reg  - Register to watch.
 *   mask - Bits to test.
 *   set  - true to wait until any masked bit is set, false until all are
 *          clear.
 *
 * Returns: true if the condition was met, false on timeout.
 */
static bool waitFlag(volatile uint32_t *reg, uint32_t mask, bool set)
{
    uint32_t t = 400000U;

    while (t--)
    {
        if (((*reg & mask) != 0U) == set)
        {
            return true;
        }
    }
    return false;
}

/*
 * ppreBits()
 *
 * Converts an APB divider to the value of the PPRE register field.
 *
 * Parameters:
 *   div - Divider (1, 2, 4, 8 or 16).
 *
 * Returns: Field value; 0 for a divider of 1.
 */
static uint32_t ppreBits(uint32_t div)
{
    switch (div)
    {
        case 2:  return 0x4U;
        case 4:  return 0x5U;
        case 8:  return 0x6U;
        case 16: return 0x7U;
        default: return 0x0U;
    }
}

static const clk_pll_limits_t pllLimits =
{
    MAX_SYS_HZ, MAX_APB1_HZ, MAX_APB2_HZ, MAX_APB1_OD_HZ, MAX_APB2_OD_HZ,
    OVERDRIVE_ABOVE_HZ, MIN_VCO_HZ, 432000000UL
};

/*
 * pllOff()
 *
 * Goes back to the internal oscillator as system clock with the PLL stopped
 * and all bus dividers at 1.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void pllOff(void)
{
    RCC->CR |= RCC_CR_HSION;
    waitFlag(&RCC->CR, RCC_CR_HSIRDY, true);
    RCC->CFGR &= ~RCC_CFGR_SW;
    waitFlag(&RCC->CFGR, RCC_CFGR_SWS, false);
    RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);
    RCC->CR &= ~RCC_CR_PLLON;
    waitFlag(&RCC->CR, RCC_CR_PLLRDY, false);
}

/*
 * runOnPll()
 *
 * Programs the PLL from a plan, sets the bus dividers, starts the PLL,
 * switches on over-drive if the plan needs it, sets the flash wait states
 * and finally switches SYSCLK to the PLL.
 *
 * Parameters:
 *   plan    - PLL plan from clk_plan_pll().
 *   fromHse - true if the PLL source is the crystal, false for the internal
 *             oscillator.
 *
 * Returns: true on success, false if the PLL, over-drive or flash setting
 *   was not accepted in time.
 */
static bool runOnPll(const clk_pll_plan_t *plan, bool fromHse)
{
    uint32_t keep = 0x3FFFFUL | (1UL << 22) | (0xFUL << 24);
    uint32_t pllcfgr = (plan->m) | (plan->n << 6) | (((plan->p / 2U) - 1U) << 16) |
        (fromHse ? RCC_PLLCFGR_PLLSRC_HSE : 0U) | (plan->q << 24);
    uint32_t ws = (plan->sysHz - 1U) / 30000000UL;  /* 2.7 .. 3.6 V      */

    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    PWR_CTRL |= PWR_CTRL_VOS;                       /* scale 1           */

    RCC->PLLCFGR = (RCC->PLLCFGR & ~keep) | pllcfgr;
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2)) |
        (ppreBits(plan->ppre1) << 10) | (ppreBits(plan->ppre2) << 13);

    RCC->CR |= RCC_CR_PLLON;
    if (!waitFlag(&RCC->CR, RCC_CR_PLLRDY, true))
    {
        return false;
    }

    if (plan->sysHz > OVERDRIVE_ABOVE_HZ)
    {
        PWR_CTRL |= PWR_CTRL_ODEN;
        if (!waitFlag(&PWR_STAT, PWR_STAT_ODRDY, true))
        {
            return false;
        }
        PWR_CTRL |= PWR_CTRL_ODSWEN;
        if (!waitFlag(&PWR_STAT, PWR_STAT_ODSWRDY, true))
        {
            return false;
        }
    }

    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | ws | FLASH_ACCEL;
    if ((FLASH->ACR & FLASH_ACR_LATENCY) != ws)
    {
        return false;
    }

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    return waitFlag(&RCC->CFGR, RCC_CFGR_SWS_PLL, true);
}

/*
 * takePlan()
 *
 * Stores the system, bus and timer clock frequencies of a plan.
 *
 * Parameters:
 *   plan - Plan that is now running.
 *
 * Returns: nothing.
 */
static void takePlan(const clk_pll_plan_t *plan)
{
    sysclkHz = plan->sysHz;
    pclk1Hz = plan->pclk1Hz;
    pclk2Hz = plan->pclk2Hz;
    timClk1Hz = plan->timClk1Hz;
    timClk2Hz = plan->timClk2Hz;
}

/*
 * sampleTimerOnApb2()
 *
 * Tells whether the sample timer is on the APB2 bus (TIM1 is; the others are
 * on APB1).
 *
 * Parameters: none.
 *
 * Returns: true if the sample timer is on APB2.
 */
static bool sampleTimerOnApb2(void)
{
    return SAMPLE_TIM_N == 1;
}

/*
 * clockRun()
 *
 * Plans and starts the PLL from the given source.
 *
 * Parameters:
 *   srcHz   - Source frequency in Hz.
 *   fromHse - true if the source is the crystal, false for the internal
 *             oscillator.
 *
 * Returns: true if the clock now runs from the PLL, false if no plan fits or
 *   the switch failed.
 */
static bool clockRun(uint32_t srcHz, bool fromHse)
{
    clk_pll_plan_t plan;

    if (!clk_plan_pll(&pllLimits, srcHz, UART_BAUD, UART_NUM, SAMPLE_RATE_HZ, EXT_CLK_HZ == 0UL,
            sampleTimerOnApb2(), &plan))
    {
        return false;
    }
    if (!runOnPll(&plan, fromHse))
    {
        return false;
    }
    takePlan(&plan);
    return true;
}

/*
 * clockInit()
 *
 * Sets up the system clock: tries the crystal first (when configured), then
 * the internal oscillator with the PLL, and if both fail runs directly from
 * the internal 16 MHz oscillator.  Finally starts the 1 ms SysTick.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void clockInit(void)
{
    bool ok = false;

    /* start from a known state: HSI, PLL off */
    pllOff();

#if CLOCK_SOURCE == CLOCK_SOURCE_HSE
    RCC->CR |= RCC_CR_HSEON | (HSE_BYPASS ? RCC_CR_HSEBYP : 0U);
    if (waitFlag(&RCC->CR, RCC_CR_HSERDY, true))
    {
        ok = clockRun(HSE_HZ, true);
    }
    if (!ok)
    {
        /* crystal did not start or no valid PLL setting: use the HSI so
         * the board still works */
        pllOff();
        RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_HSEBYP);
    }
#endif
    if (!ok)
    {
        ok = clockRun(16000000UL, false);
    }
    if (!ok)
    {
        /* PLL refused to start: stay on the 16 MHz HSI without a PLL */
        pllOff();
        sysclkHz = 16000000UL;
        pclk1Hz = 16000000UL;
        pclk2Hz = 16000000UL;
        timClk1Hz = 16000000UL;
        timClk2Hz = 16000000UL;
    }

    SysTick_Config(sysclkHz / 1000U);
}

/* ------------------------------------------------------------------ */
/* PTT, inhibit and LEDs                                              */
/* ------------------------------------------------------------------ */
/*
 * irqSave()
 *
 * Starts a short critical section by disabling interrupts, because the
 * sample interrupt also touches PTT.
 *
 * Parameters: none.
 *
 * Returns: The previous interrupt mask, to pass to irqRestore().
 */
static inline uint32_t irqSave(void)
{
    uint32_t primask;

    __asm volatile ("mrs %0, primask\n\tcpsid i" : "=r" (primask) : : "memory");
    return primask;
}

/*
 * irqRestore()
 *
 * Ends a critical section started by irqSave().
 *
 * Parameters:
 *   primask - Value returned by irqSave().
 *
 * Returns: nothing.
 */
static inline void irqRestore(uint32_t primask)
{
    __asm volatile ("msr primask, %0" : : "r" (primask) : "memory");
}

static volatile bool pttWanted;
static volatile bool pttInvert;

/*
 * inhibited()
 *
 * Tells whether the PTT inhibit input is active.  An inhibit pin is
 * optional.
 *
 * Parameters: none.
 *
 * Returns: true if the inhibit pin is configured and at its active level.
 */
static bool inhibited(void)
{
    if (INHIBIT_PIN == PIN_NONE)
    {
        return false;
    }
    return gpioRead(INHIBIT_PIN) == (INHIBIT_ACTIVE_HIGH != 0);
}

/*
 * pttApply()
 *
 * Drives the PTT pin from the wanted state, the inhibit input, the
 * configured polarity and the host's PTT invert flag.  The state is read and
 * the pin written inside one critical section so the sample interrupt, which
 * un-keys at the end of a transmission, can never be overtaken by a stale
 * write from the main loop.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void pttApply(void)
{
    uint32_t primask = irqSave();
    bool on = pttWanted && !inhibited();
    bool level = on ? (PTT_ACTIVE_HIGH != 0) : (PTT_ACTIVE_HIGH == 0);

    if (pttInvert)
    {
        level = !level;
    }
    gpioWrite(PTT_PIN, level);
    irqRestore(primask);
}

/*
 * hw_ptt()
 *
 * Keys or un-keys the transmitter.  The request is remembered, but the pin
 * stays un-keyed while the inhibit input is active.
 *
 * Parameters:
 *   on - true to transmit.
 *
 * Returns: nothing.
 */
void hw_ptt(bool on)
{
    pttWanted = on;
    pttApply();
}

/*
 * hw_set_ptt_invert()
 *
 * Sets the PTT invert flag from the host's configuration and updates the
 * pin.
 *
 * Parameters:
 *   invert - true to invert the PTT output.
 *
 * Returns: nothing.
 */
void hw_set_ptt_invert(bool invert)
{
    pttInvert = invert;
    pttApply();
}

/*
 * hw_ptt_state()
 *
 * Reports what the PTT pin is really doing, for diagnostics.
 *
 * Parameters: none.
 *
 * Returns: Bit 0: PTT wanted, bit 1: output latch high, bit 2: pin reads
 *   high, bit 3: inhibit active, bit 4: host PTT invert set.  0 if there is no
 *   PTT pin.
 */
uint8_t hw_ptt_state(void)
{
    GPIO_TypeDef *g = gpioOf(PTT_PIN);
    uint32_t bit = 1UL << (PTT_PIN & 0x0FU);
    uint8_t s = 0;

    if (PTT_PIN == PIN_NONE)
    {
        return 0;
    }
    s |= pttWanted ? 0x01U : 0U;
    s |= (g->ODR & bit) ? 0x02U : 0U;
    s |= (g->IDR & bit) ? 0x04U : 0U;
    s |= inhibited() ? 0x08U : 0U;
    s |= pttInvert ? 0x10U : 0U;
    return s;
}

/*
 * hw_poll()
 *
 * Slow housekeeping for the main loop: when the inhibit input changes, re-
 * applies PTT so it is dropped or restored.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void hw_poll(void)
{
    static bool lastInhibit;
    bool now = inhibited();

    if (now != lastInhibit)
    {
        lastInhibit = now;
        pttApply();                                 /* drop or restore PTT */
    }
}

static const uint8_t ledPins[NUM_LEDS] =
{
    LED0_PIN, LED1_PIN, LED2_PIN, LED3_PIN, LED4_PIN, LED5_PIN, LED6_PIN, LED7_PIN
};

/*
 * hw_led()
 *
 * Switches an LED on or off, taking the LED polarity into account.  Numbers
 * outside the LED range are ignored.
 *
 * Parameters:
 *   n  - LED number (0 to NUM_LEDS - 1).
 *   on - true to light the LED.
 *
 * Returns: nothing.
 */
void hw_led(uint8_t n, bool on)
{
    if (n < NUM_LEDS)
    {
        gpioWrite(ledPins[n], LED_ACTIVE_HIGH ? on : !on);
    }
}

/* ------------------------------------------------------------------ */
/* UART with DMA                                                      */
/* ------------------------------------------------------------------ */
#define UART_RX_SIZE    2048U               /* power of two              */
#define UART_TX_SIZE    2048U               /* power of two              */

/* Pins and AF.  UART_REMAP 0 = first choice, 1 / 2 = alternatives.       */
#if UART_NUM == 1
#define USARTx          USART1
#define UART_AF         7U
#if UART_REMAP == 1
#define UART_TX_PIN     PB(6)
#define UART_RX_PIN     PB(7)
#else
#define UART_TX_PIN     PA(9)
#define UART_RX_PIN     PA(10)
#endif
#elif UART_NUM == 2
#define USARTx          USART2
#define UART_AF         7U
#if UART_REMAP == 1
#define UART_TX_PIN     PD(5)
#define UART_RX_PIN     PD(6)
#else
#define UART_TX_PIN     PA(2)
#define UART_RX_PIN     PA(3)
#endif
#elif UART_NUM == 3
#define USARTx          USART3
#define UART_AF         7U
#if UART_REMAP == 1
#define UART_TX_PIN     PC(10)
#define UART_RX_PIN     PC(11)
#elif UART_REMAP >= 2
#define UART_TX_PIN     PD(8)
#define UART_RX_PIN     PD(9)
#else
#define UART_TX_PIN     PB(10)
#define UART_RX_PIN     PB(11)
#endif
#elif UART_NUM == 4
#define USARTx          UART4
#define UART_AF         8U
#if UART_REMAP == 1
#define UART_TX_PIN     PC(10)
#define UART_RX_PIN     PC(11)
#else
#define UART_TX_PIN     PA(0)
#define UART_RX_PIN     PA(1)
#endif
#elif UART_NUM == 5
#define USARTx          UART5
#define UART_AF         8U
#define UART_TX_PIN     PC(12)
#define UART_RX_PIN     PD(2)
#elif UART_NUM == 6
#define USARTx          USART6
#define UART_AF         8U
#if UART_REMAP == 1
#define UART_TX_PIN     PG(14)
#define UART_RX_PIN     PG(9)
#else
#define UART_TX_PIN     PC(6)
#define UART_RX_PIN     PC(7)
#endif
#elif UART_NUM == 7
#define USARTx          UART7
#define UART_AF         8U
#if UART_REMAP == 1
#define UART_TX_PIN     PF(7)
#define UART_RX_PIN     PF(6)
#else
#define UART_TX_PIN     PE(8)
#define UART_RX_PIN     PE(7)
#endif
#else
#define USARTx          UART8
#define UART_AF         8U
#define UART_TX_PIN     PE(1)
#define UART_RX_PIN     PE(0)
#endif

/* DMA stream / channel for each UART (reference manual DMA request map) */
#if UART_NUM == 1
#define UART_DMA        DMA2
#define UART_DMA_BASE   DMA2_Stream0_BASE
#define UART_TX_STREAM  7U
#define UART_RX_STREAM  2U
#define UART_DMA_CH     4U
#elif UART_NUM == 2
#define UART_DMA        DMA1
#define UART_DMA_BASE   DMA1_Stream0_BASE
#define UART_TX_STREAM  6U
#define UART_RX_STREAM  5U
#define UART_DMA_CH     4U
#elif UART_NUM == 3
#define UART_DMA        DMA1
#define UART_DMA_BASE   DMA1_Stream0_BASE
#define UART_TX_STREAM  3U
#define UART_RX_STREAM  1U
#define UART_DMA_CH     4U
#elif UART_NUM == 4
#define UART_DMA        DMA1
#define UART_DMA_BASE   DMA1_Stream0_BASE
#define UART_TX_STREAM  4U
#define UART_RX_STREAM  2U
#define UART_DMA_CH     4U
#elif UART_NUM == 5
#define UART_DMA        DMA1
#define UART_DMA_BASE   DMA1_Stream0_BASE
#define UART_TX_STREAM  7U
#define UART_RX_STREAM  0U
#define UART_DMA_CH     4U
#elif UART_NUM == 6
#define UART_DMA        DMA2
#define UART_DMA_BASE   DMA2_Stream0_BASE
#define UART_TX_STREAM  6U
#define UART_RX_STREAM  1U
#define UART_DMA_CH     5U
#elif UART_NUM == 7
#define UART_DMA        DMA1
#define UART_DMA_BASE   DMA1_Stream0_BASE
#define UART_TX_STREAM  1U
#define UART_RX_STREAM  3U
#define UART_DMA_CH     5U
#else
#define UART_DMA        DMA1
#define UART_DMA_BASE   DMA1_Stream0_BASE
#define UART_TX_STREAM  0U
#define UART_RX_STREAM  6U
#define UART_DMA_CH     5U
#endif

#define DMA_STREAM(n)   ((DMA_Stream_TypeDef *)(UART_DMA_BASE + 0x18U * (n)))

/*
 * dmaShift()
 *
 * Gets the bit position of a DMA stream's flags inside the low or high
 * status register.
 *
 * Parameters:
 *   s - Stream number.
 *
 * Returns: Bit position (0, 6, 16 or 22).
 */
static uint32_t dmaShift(uint32_t s)
{
    static const uint8_t sh[4] = {0, 6, 16, 22};

    return sh[s & 3U];
}

/*
 * dmaStatus()
 *
 * Reads a DMA stream's status flags, shifted to the bottom of the value.
 *
 * Parameters:
 *   s - Stream number.
 *
 * Returns: The stream's flags.
 */
static uint32_t dmaStatus(uint32_t s)
{
    return ((s < 4U) ? UART_DMA->LISR : UART_DMA->HISR) >> dmaShift(s);
}

/*
 * dmaClear()
 *
 * Clears all of a DMA stream's flags (transfer complete, half transfer,
 * error and FIFO flags).
 *
 * Parameters:
 *   s - Stream number.
 *
 * Returns: nothing.
 */
static void dmaClear(uint32_t s)
{
    uint32_t mask = 0x3DUL << dmaShift(s);          /* TC HT TE DME FE   */

    if (s < 4U)
    {
        UART_DMA->LIFCR = mask;
    }
    else
    {
        UART_DMA->HIFCR = mask;
    }
}

#define DMA_TCIF_BIT    (1UL << 5)

#if defined(MCU_F446)
#define UART_RXREG      (&USARTx->DR)
#define UART_TXREG      (&USARTx->DR)
#else
#define UART_RXREG      (&USARTx->RDR)
#define UART_TXREG      (&USARTx->TDR)
#endif

static uint8_t           uartRx[UART_RX_SIZE] __attribute__((aligned(4)));
static uint16_t          uartRxTail;
static uint8_t           uartTx[UART_TX_SIZE] __attribute__((aligned(4)));
static uint16_t          uartTxHead;
static uint16_t          uartTxTail;
static uint16_t          uartTxInFlight;
static volatile uint32_t uartErr;

/*
 * rxHeadIndex()
 *
 * Gets the position where the DMA will write the next received UART byte.
 *
 * Parameters: none.
 *
 * Returns: Index into the UART receive ring.
 */
static uint16_t rxHeadIndex(void)
{
    return (uint16_t)((UART_RX_SIZE - DMA_STREAM(UART_RX_STREAM)->NDTR) & (UART_RX_SIZE - 1U));
}

/*
 * hw_uart_rx_available()
 *
 * Gets the number of received bytes waiting in the UART receive ring.
 *
 * Parameters: none.
 *
 * Returns: Number of bytes.
 */
uint16_t hw_uart_rx_available(void)
{
    return (uint16_t)((rxHeadIndex() - uartRxTail) & (UART_RX_SIZE - 1U));
}

/*
 * hw_uart_rx_peek()
 *
 * Reads a received byte without removing it.
 *
 * Parameters:
 *   offset - Position counted from the oldest unread byte.
 *
 * Returns: The byte.
 */
uint8_t hw_uart_rx_peek(uint16_t offset)
{
    return uartRx[(uartRxTail + offset) & (UART_RX_SIZE - 1U)];
}

/*
 * hw_uart_rx_consume()
 *
 * Removes bytes from the front of the UART receive ring.
 *
 * Parameters:
 *   count - Number of bytes to remove.
 *
 * Returns: nothing.
 */
void hw_uart_rx_consume(uint16_t count)
{
    uartRxTail = (uint16_t)((uartRxTail + count) & (UART_RX_SIZE - 1U));
}

/*
 * hw_uart_poll()
 *
 * Keeps the UART transmit DMA running and counts UART errors (an error
 * condition counts once until it clears on the F446; on the F722 each error
 * flag found is counted and cleared).  When the DMA has finished a block,
 * the sent bytes are released and the next contiguous block of the transmit
 * ring is started.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void hw_uart_poll(void)
{
    DMA_Stream_TypeDef *tx = DMA_STREAM(UART_TX_STREAM);

#if defined(MCU_F446)
    uint32_t sr = USARTx->SR;

    /* Error counting.  The flags are cleared by a read of SR followed by a
     * read of DR; the receive DMA does the DR read by itself, so most errors
     * were never seen here.  Count each flag when it appears (edge) and let
     * the DMA clear it; never read DR here (it would steal a received byte). */
    {
        static uint8_t errSeen;
        uint8_t        now = (uint8_t)((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) != 0U);

        if (now && !errSeen)
        {
            uartErr++;
        }
        errSeen = now;
    }
#else
    uint32_t isr = USARTx->ISR;

    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE))
    {
        uartErr++;
        USARTx->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF;
    }
#endif

    if (uartTxInFlight != 0U)
    {
        if (!(dmaStatus(UART_TX_STREAM) & DMA_TCIF_BIT))
        {
            return;
        }
        dmaClear(UART_TX_STREAM);
        tx->CR &= ~DMA_SxCR_EN;
        uartTxTail = (uint16_t)((uartTxTail + uartTxInFlight) & (UART_TX_SIZE - 1U));
        uartTxInFlight = 0;
    }

    if (uartTxTail != uartTxHead)
    {
        uint16_t n;

        if (uartTxHead > uartTxTail)
        {
            n = (uint16_t)(uartTxHead - uartTxTail);
        }
        else
        {
            n = (uint16_t)(UART_TX_SIZE - uartTxTail);      /* up to wrap */
        }
        while (tx->CR & DMA_SxCR_EN)                /* must be off to load */
        {
        }
        dmaClear(UART_TX_STREAM);
        tx->M0AR = (uint32_t)&uartTx[uartTxTail];
        tx->NDTR = n;
        uartTxInFlight = n;
        __DSB();                                    /* data before enable */
        tx->CR |= DMA_SxCR_EN;
    }
}

/*
 * hw_uart_tx()
 *
 * Queues bytes for sending to the host.  Either all the bytes are queued or
 * none.
 *
 * Parameters:
 *   data - Bytes to send.
 *   len  - Number of bytes.
 *
 * Returns: true if the bytes were queued, false if there was not enough
 *   room.
 */
bool hw_uart_tx(const uint8_t *data, uint16_t len)
{
    uint16_t freeBytes = (uint16_t)((uartTxTail - uartTxHead - 1U) & (UART_TX_SIZE - 1U));
    uint16_t i;

    if (len > freeBytes)
    {
        return false;
    }
    for (i = 0; i < len; i++)
    {
        uartTx[uartTxHead] = data[i];
        uartTxHead = (uint16_t)((uartTxHead + 1U) & (UART_TX_SIZE - 1U));
    }
    hw_uart_poll();
    return true;
}

/*
 * hw_uart_errors()
 *
 * Gets the number of UART overrun, framing and noise errors seen.
 *
 * Parameters: none.
 *
 * Returns: Error count.
 */
uint32_t hw_uart_errors(void)
{
    return uartErr;
}

/*
 * uartInit()
 *
 * Sets up the UART at the configured baud rate with DMA: a circular DMA
 * stream receives into the receive ring and a second stream sends from the
 * transmit ring.  The UART and DMA clocks and the TX and RX pins (with a
 * pull-up on RX) are set up as well.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void uartInit(void)
{
    DMA_Stream_TypeDef *rx = DMA_STREAM(UART_RX_STREAM);
    DMA_Stream_TypeDef *tx = DMA_STREAM(UART_TX_STREAM);
    uint32_t pclk = clk_uart_on_apb2(UART_NUM) ? pclk2Hz : pclk1Hz;

#if UART_NUM == 1 || UART_NUM == 6
    RCC->APB2ENR |= (UART_NUM == 1) ? RCC_APB2ENR_USART1EN : RCC_APB2ENR_USART6EN;
#elif UART_NUM == 2
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
#elif UART_NUM == 3
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
#elif UART_NUM == 4
    RCC->APB1ENR |= RCC_APB1ENR_UART4EN;
#elif UART_NUM == 5
    RCC->APB1ENR |= RCC_APB1ENR_UART5EN;
#elif UART_NUM == 7
    RCC->APB1ENR |= RCC_APB1ENR_UART7EN;
#else
    RCC->APB1ENR |= RCC_APB1ENR_UART8EN;
#endif
    RCC->AHB1ENR |= (UART_DMA == DMA2) ? RCC_AHB1ENR_DMA2EN : RCC_AHB1ENR_DMA1EN;
    (void)RCC->AHB1ENR;

    gpioConfig(UART_TX_PIN, GPIO_AF, UART_AF, PULL_NONE);
    gpioConfig(UART_RX_PIN, GPIO_AF, UART_AF, PULL_UP);

    USARTx->CR1 = 0;
    USARTx->CR2 = 0;
    USARTx->CR3 = USART_CR3_DMAT | USART_CR3_DMAR;
    USARTx->BRR = (uint16_t)((pclk + UART_BAUD / 2U) / UART_BAUD);

    /* RX: circular DMA into uartRx */
    rx->CR = 0;
    while (rx->CR & DMA_SxCR_EN)
    {
    }
    dmaClear(UART_RX_STREAM);
    rx->PAR = (uint32_t)UART_RXREG;
    rx->M0AR = (uint32_t)uartRx;
    rx->NDTR = UART_RX_SIZE;
    rx->FCR = 0;                                    /* direct mode       */
    rx->CR = (UART_DMA_CH << 25) | DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_PL_1 | DMA_SxCR_EN;

    /* TX: normal DMA, loaded by hw_uart_poll() */
    tx->CR = 0;
    while (tx->CR & DMA_SxCR_EN)
    {
    }
    dmaClear(UART_TX_STREAM);
    tx->PAR = (uint32_t)UART_TXREG;
    tx->FCR = 0;
    tx->CR = (UART_DMA_CH << 25) | DMA_SxCR_MINC | DMA_SxCR_DIR_0 | DMA_SxCR_PL_0;

    USARTx->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

/* ------------------------------------------------------------------ */
/* ADC and DAC                                                        */
/* ------------------------------------------------------------------ */
#define ADC_CR2_RUN     (ADC_CR2_ADON | ADC_CR2_SWSTART)

/*
 * adcInit()
 *
 * Sets up ADC1 for single conversions of the receive audio channel: chooses
 * the ADC clock divider to stay at or below 36 MHz and sets the sample time
 * and the channel.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void adcInit(void)
{
    uint32_t adcpre;

    gpioConfig(ADC_PIN, GPIO_ANALOG, 0U, PULL_NONE);

    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    (void)RCC->APB2ENR;

    /* ADC clock (PCLK2 / 2, 4, 6 or 8) must be <= 36 MHz */
    if (pclk2Hz / 2U <= 36000000UL)
    {
        adcpre = 0U;
    }
    else if (pclk2Hz / 4U <= 36000000UL)
    {
        adcpre = 1U;
    }
    else if (pclk2Hz / 6U <= 36000000UL)
    {
        adcpre = 2U;
    }
    else
    {
        adcpre = 3U;
    }
    ADC123_COMMON->CCR = (ADC123_COMMON->CCR & ~ADC_CCR_ADCPRE) | (adcpre << 16);

    ADC1->CR1 = 0;                                  /* 12 bit            */
    ADC1->CR2 = ADC_CR2_ADON;
    for (volatile uint32_t d = 0; d < 2000U; d++)   /* stabilisation     */
    {
    }

#if ADC_CHANNEL < 10
    ADC1->SMPR2 = (uint32_t)ADC_SAMPLE_TIME << (3U * ADC_CHANNEL);
    ADC1->SMPR1 = 0;
#else
    ADC1->SMPR1 = (uint32_t)ADC_SAMPLE_TIME << (3U * (ADC_CHANNEL - 10));
    ADC1->SMPR2 = 0;
#endif
    ADC1->SQR1 = 0;                                 /* one conversion    */
    ADC1->SQR3 = ADC_CHANNEL;

    /* software trigger, right aligned */
    ADC1->CR2 = ADC_CR2_RUN;                        /* first conversion  */
}

/*
 * dacInit()
 *
 * Enables the DAC channel for the transmit audio and sets it to mid-scale.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void dacInit(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_DACEN;
    (void)RCC->APB1ENR;
#if DAC_CHANNEL == 1
    gpioConfig(PA(4), GPIO_ANALOG, 0U, PULL_NONE);
    DAC->DHR12R1 = DAC_MID;                         /* mid level first   */
    DAC->CR = DAC_CR_EN1 | (DAC_OUTPUT_BUFFER ? 0U : DAC_CR_BOFF1);
    DAC->DHR12R1 = DAC_MID;
#else
    gpioConfig(PA(5), GPIO_ANALOG, 0U, PULL_NONE);
    DAC->DHR12R2 = DAC_MID;
    DAC->CR = DAC_CR_EN2 | (DAC_OUTPUT_BUFFER ? 0U : DAC_CR_BOFF2);
    DAC->DHR12R2 = DAC_MID;
#endif
}

/* ------------------------------------------------------------------ */
/* Sample timer                                                       */
/* ------------------------------------------------------------------ */
#if SAMPLE_TIM_N == 1
#define SAMPLE_TIM          TIM1
#define SAMPLE_TIM_IRQn     TIM1_UP_TIM10_IRQn
#define SAMPLE_ISR          TIM1_UP_ISR
#define SAMPLE_TIM_CLK()    (RCC->APB2ENR |= RCC_APB2ENR_TIM1EN)
#define SAMPLE_TIM_HZ       timClk2Hz
#define EXT_ETR_PIN         PA(12)
#define EXT_ETR_AF          1U
#elif SAMPLE_TIM_N == 2
#define SAMPLE_TIM          TIM2
#define SAMPLE_TIM_IRQn     TIM2_IRQn
#define SAMPLE_ISR          TIM2_IRQHandler
#define SAMPLE_TIM_CLK()    (RCC->APB1ENR |= RCC_APB1ENR_TIM2EN)
#define SAMPLE_TIM_HZ       timClk1Hz
#if EXT_CLK_TIM2_REMAP
#define EXT_ETR_PIN         PA(15)
#else
#define EXT_ETR_PIN         PA(0)
#endif
#define EXT_ETR_AF          1U
#elif SAMPLE_TIM_N == 3
#define SAMPLE_TIM          TIM3
#define SAMPLE_TIM_IRQn     TIM3_IRQn
#define SAMPLE_ISR          TIM3_IRQHandler
#define SAMPLE_TIM_CLK()    (RCC->APB1ENR |= RCC_APB1ENR_TIM3EN)
#define SAMPLE_TIM_HZ       timClk1Hz
#define EXT_ETR_PIN         PD(2)
#define EXT_ETR_AF          2U
#else
#define SAMPLE_TIM          TIM4
#define SAMPLE_TIM_IRQn     TIM4_IRQn
#define SAMPLE_ISR          TIM4_IRQHandler
#define SAMPLE_TIM_CLK()    (RCC->APB1ENR |= RCC_APB1ENR_TIM4EN)
#define SAMPLE_TIM_HZ       timClk1Hz
#define EXT_ETR_PIN         PE(0)
#define EXT_ETR_AF          2U
#endif

#define TIM_SMCR_ECE_BIT    (1UL << 14)
#define TIM_SMCR_ETP_BIT    (1UL << 15)

static volatile uint32_t sampleTicks;
#if SAMPLE_TRIM_PPM != 0
static uint32_t          trimPeriod;
static uint32_t          trimFrac;
static uint32_t          trimAcc;
#endif
static bool              extMissing;
static bool              usingInternal;
static uint32_t          lastTickSeen;
static uint32_t          lastTickMs;

/*
 * timerInternal()
 *
 * Programs the sample timer to run from the chip's own clock at the sample
 * rate and enables its update interrupt.  With SAMPLE_TRIM_PPM set, the
 * period is trimmed by a whole number plus a fraction of a tick (see
 * tim_plan_trim()), which the interrupt applies on every sample.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void timerInternal(void)
{
    tim_plan_t plan;

    tim_plan_internal(SAMPLE_TIM_HZ, SAMPLE_RATE_HZ, &plan);

    SAMPLE_TIM->CR1 = 0;
    SAMPLE_TIM->SMCR = 0;                           /* internal clock    */
    SAMPLE_TIM->PSC = 0;
    SAMPLE_TIM->ARR = plan.arr;
#if SAMPLE_TRIM_PPM != 0
    {
        tim_trim_t trim;

        tim_plan_trim(plan.arr, SAMPLE_TRIM_PPM, &trim);
        trimPeriod = trim.period;
        trimFrac = trim.frac;
        trimAcc = 0;
        SAMPLE_TIM->ARR = (trim.period - 1U);
    }
#endif
    SAMPLE_TIM->DIER = TIM_DIER_UIE;
    SAMPLE_TIM->EGR = TIM_EGR_UG;
    SAMPLE_TIM->SR = 0;
    usingInternal = true;
}

#if EXT_CLK_HZ > 0
/*
 * timerExternal()
 *
 * Programs the sample timer to run from an external clock input (external
 * clock mode 2) divided to the sample rate and enables its update interrupt.
 * With SAMPLE_TRIM_PPM set, the period is trimmed as in timerInternal().
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void timerExternal(void)
{
    /* External clock mode 2: ETR -> ETPS -> counter */
    tim_plan_t plan;

    tim_plan_external(EXT_CLK_HZ, SAMPLE_TIM_HZ, SAMPLE_RATE_HZ, &plan);

    SAMPLE_TIM->CR1 = 0;
    SAMPLE_TIM->SMCR = TIM_SMCR_ECE_BIT | (plan.etps << 12) | ((uint32_t)EXT_CLK_FILTER << 8) |
        (EXT_CLK_INVERT ? TIM_SMCR_ETP_BIT : 0U);
    SAMPLE_TIM->PSC = 0;
    SAMPLE_TIM->ARR = plan.arr;
#if SAMPLE_TRIM_PPM != 0
    {
        tim_trim_t trim;

        tim_plan_trim(plan.arr, SAMPLE_TRIM_PPM, &trim);
        trimPeriod = trim.period;
        trimFrac = trim.frac;
        trimAcc = 0;
        SAMPLE_TIM->ARR = (trim.period - 1U);
    }
#endif
    SAMPLE_TIM->DIER = TIM_DIER_UIE;
    SAMPLE_TIM->EGR = TIM_EGR_UG;
    SAMPLE_TIM->SR = 0;
    usingInternal = false;
}
#endif

/*
 * timerInit()
 *
 * Enables the sample timer clock and sets it up from the external clock if
 * one is configured (including its input pin), otherwise from the internal
 * clock.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void timerInit(void)
{
    SAMPLE_TIM_CLK();
    (void)RCC->APB1ENR;
    (void)RCC->APB2ENR;

#if EXT_CLK_HZ > 0
    gpioConfig(EXT_ETR_PIN, GPIO_AF, EXT_ETR_AF, PULL_NONE);
    timerExternal();
#else
    timerInternal();
#endif
}

/*
 * SAMPLE_ISR()
 *
 * Sample timer interrupt: applies the sample clock trim (if enabled), reads
 * the ADC result of the previous conversion, starts the next conversion,
 * calls engine_sample_tick() and writes its result to the DAC.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void SAMPLE_ISR(void)
{
    uint16_t raw;
    uint16_t out;

    SAMPLE_TIM->SR = 0;                             /* clear update flag */
#if SAMPLE_TRIM_PPM != 0
    /* dither the period: one extra tick whenever the fraction carries */
    trimAcc += trimFrac;
    SAMPLE_TIM->ARR = ((trimAcc < trimFrac) ? trimPeriod : (trimPeriod - 1U));
#endif

    raw = (uint16_t)ADC1->DR;                       /* previous result   */
    ADC1->CR2 = ADC_CR2_RUN;                        /* start next        */

    out = engine_sample_tick(raw);

#if DAC_CHANNEL == 1
    DAC->DHR12R1 = out;
#else
    DAC->DHR12R2 = out;
#endif
    sampleTicks++;
}

/*
 * hw_start_sampling()
 *
 * Starts the sample timer: sets the interrupt to the highest priority,
 * discards the interrupt left pending by the timer's update event, enables
 * the interrupt and starts the counter.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void hw_start_sampling(void)
{
    NVIC_SetPriority(SAMPLE_TIM_IRQn, 0);
    NVIC_ClearPendingIRQ(SAMPLE_TIM_IRQn);          /* drop the UG tick  */
    NVIC_EnableIRQ(SAMPLE_TIM_IRQn);
    lastTickMs = msTicks;
    SAMPLE_TIM->CR1 |= TIM_CR1_CEN;
}

/*
 * hw_ext_clock_missing()
 *
 * Checks that the sample interrupt is still running from the external clock.
 * If it stops for more than 250 ms, the external clock is reported missing
 * and, if EXT_CLK_FALLBACK is set, the timer is switched to the internal
 * clock.  Without an external clock configured it always returns false.
 *
 * Parameters: none.
 *
 * Returns: true if the external clock is missing or the internal clock is in
 *   use.
 */
bool hw_ext_clock_missing(void)
{
#if EXT_CLK_HZ > 0
    uint32_t now = msTicks;
    uint32_t t = sampleTicks;

    if (t != lastTickSeen)
    {
        lastTickSeen = t;
        lastTickMs = now;
        if (!usingInternal)
        {
            extMissing = false;
        }
    }
    else if ((uint32_t)(now - lastTickMs) > 250U && !usingInternal)
    {
        extMissing = true;
#if EXT_CLK_FALLBACK
        SAMPLE_TIM->CR1 &= ~TIM_CR1_CEN;
        timerInternal();
        NVIC_ClearPendingIRQ(SAMPLE_TIM_IRQn);
        SAMPLE_TIM->CR1 |= TIM_CR1_CEN;
#endif
    }
    return extMissing || usingInternal;
#else
    (void)lastTickSeen;
    (void)lastTickMs;
    (void)extMissing;
    (void)usingInternal;
    return false;
#endif
}

/* ------------------------------------------------------------------ */
/* Watchdog                                                           */
/*
 * hw_enter_bootloader()
 *
 * Restarts the chip into the ROM boot loader: switches PTT off, lets the
 * UART send the pending ACK for 20 ms, stores the boot magic value and
 * resets.  Reset_Handler() sees the magic value and jumps to the boot
 * loader.  Does not return.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void hw_enter_bootloader(void)
{
    extern uint32_t bootMagic;
    uint32_t start = hw_millis();

    hw_ptt(false);
    while ((uint32_t)(hw_millis() - start) < 20U)   /* let the ACK drain */
    {
        hw_uart_poll();
        hw_watchdog_kick();
    }
    __asm volatile ("cpsid i" : : : "memory");
    bootMagic = BOOT_MAGIC;
    NVIC_SystemReset();
    for (;;)
    {
    }
}

/*
 * hw_watchdog_kick()
 *
 * Restarts the watchdog countdown (does nothing if the watchdog is
 * disabled).
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void hw_watchdog_kick(void)
{
#if WATCHDOG_ENABLE
    IWDG->KR = 0xAAAAU;
#endif
}

/*
 * watchdogInit()
 *
 * Starts the independent watchdog with a timeout of about 1 second.  The
 * watchdog is stopped while a debugger halts the chip.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
static void watchdogInit(void)
{
#if WATCHDOG_ENABLE
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;
    IWDG->KR = 0x5555U;
    IWDG->PR = 4U;                                  /* LSI / 64          */
    IWDG->RLR = IWDG_RELOAD;                        /* about 1 s         */
    IWDG->KR = 0xCCCCU;
#endif
}

/* ------------------------------------------------------------------ */
/* Init                                                               */
/*
 * hw_init()
 *
 * Initialises the hardware: PTT, LED and inhibit pins, the system clock,
 * UART, ADC, DAC, sample timer and watchdog.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void hw_init(void)
{
    uint8_t i;

    /* PTT first, before the (slow) clock start-up: enable the port, set
     * the inactive level, then make it an output, so the pin never
     * glitches active and is not left floating for long */
    pttWanted = false;
    gpioClock(PTT_PIN);
    pttApply();
    gpioConfig(PTT_PIN, GPIO_OUT, 0U, PULL_NONE);

    for (i = 0; i < NUM_LEDS; i++)
    {
        gpioClock(ledPins[i]);
        hw_led(i, false);
        gpioConfig(ledPins[i], GPIO_OUT, 0U, PULL_NONE);
    }

    /* inhibit input: pull towards the inactive level, so a floating or
     * unconnected pin never blocks transmit */
    gpioConfig(INHIBIT_PIN, GPIO_IN, 0U, INHIBIT_ACTIVE_HIGH ? PULL_DOWN : PULL_UP);

    clockInit();

    uartInit();
    adcInit();
    dacInit();
    timerInit();
    watchdogInit();
}
