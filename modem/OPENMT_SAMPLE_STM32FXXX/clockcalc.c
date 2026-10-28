/* Created by Rick KD0OSS */
/*
 *  clockcalc.c - see clockcalc.h
 */

#include "clockcalc.h"

/*
 * absDiff()
 *
 * Calculates the difference between two unsigned numbers without wrapping.
 *
 * Parameters:
 *   a - First value.
 *   b - Second value.
 *
 * Returns: The larger value minus the smaller one.
 */
static uint32_t absDiff(uint32_t a, uint32_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

/*
 * clk_uart_pclk()
 *
 * Gets the input clock of a UART on an STM32F1 for a given system clock.
 * USART1 is on APB2 (full speed); the others are on APB1, which runs at half
 * speed once SYSCLK is above 36 MHz.
 *
 * Parameters:
 *   sysHz   - System clock in Hz.
 *   uartNum - UART number (1, 2, 3, ...).
 *
 * Returns: UART peripheral clock in Hz.
 */
uint32_t clk_uart_pclk(uint32_t sysHz, uint8_t uartNum)
{
    if (uartNum == 1U)
    {
        return sysHz;                               /* APB2: no divider  */
    }
    return (sysHz > 36000000UL) ? (sysHz / 2U) : sysHz;
}

/*
 * planCost()
 *
 * Rates how well a system clock suits the wanted baud rate and, when the
 * sample timer is divided from the system clock, the wanted sample rate.
 * The cost is the baud rate error plus (for the internal tick) the sample
 * rate error, each in parts per million.
 *
 * Parameters:
 *   sys          - System clock in Hz.
 *   baud         - Wanted baud rate.
 *   uartNum      - UART number.
 *   sampleRate   - Wanted sample rate in Hz.
 *   internalTick - true if the sample timer is divided from the system
 *                  clock, so its error counts too.
 *
 * Returns: The error in ppm (lower is better), or 0xFFFFFFFF if the baud
 *   divider would be zero.
 */
static uint32_t planCost(uint32_t sys, uint32_t baud, uint8_t uartNum,
    uint32_t sampleRate, bool internalTick)
{
    uint32_t pclk = clk_uart_pclk(sys, uartNum);
    uint32_t brr = (pclk + baud / 2U) / baud;
    uint32_t cost;

    if (brr == 0U)
    {
        return 0xFFFFFFFFUL;
    }
    cost = (uint32_t)(((uint64_t)absDiff(pclk / brr, baud) * 1000000ULL) / baud);

    if (internalTick)
    {
        uint32_t div = (sys + sampleRate / 2U) / sampleRate;

        cost += (uint32_t)(((uint64_t)absDiff(sys / div, sampleRate) * 1000000ULL) / sampleRate);
    }
    return cost;
}

/* PLL multiplier x 2 and RCC_CFGR PLLMUL bits for the connectivity line */
static const struct
{
    uint8_t  mulX2;
    uint32_t bits;
} pllMul[] =
{
    {  8, 0x2UL << 18 }, { 10, 0x3UL << 18 }, { 12, 0x4UL << 18 },
    { 14, 0x5UL << 18 }, { 16, 0x6UL << 18 }, { 18, 0x7UL << 18 },
    { 13, 0xDUL << 18 }
};

/*
 * clk_plan_hse()
 *
 * Finds a PLL setting for an STM32F1 with an external crystal: it tries
 * every PLL input divider and multiplier that keeps the PLL input at 3 to 12
 * MHz and SYSCLK from 24 to 72 MHz, and keeps the one with the lowest baud
 * (and sample rate) error.  A candidate within 2000 ppm of the best is
 * preferred if it gives a faster SYSCLK.
 *
 * Parameters:
 *   hseHz        - Crystal frequency in Hz.
 *   baud         - Wanted baud rate.
 *   uartNum      - UART number.
 *   sampleRate   - Wanted sample rate in Hz.
 *   internalTick - true if the sample timer is divided from the system
 *                  clock.
 *   out          - Receives the chosen divider, multiplier, register bits,
 *                  SYSCLK and cost.
 *
 * Returns: true if a setting was found, false if nothing fits.
 */
bool clk_plan_hse(uint32_t hseHz, uint32_t baud, uint8_t uartNum,
    uint32_t sampleRate, bool internalTick, clk_plan_t *out)
{
    bool found = false;
    uint32_t p1;
    uint32_t i;

    for (p1 = 1; p1 <= 16U; p1++)
    {
        uint32_t fin = hseHz / p1;

        if (fin < 3000000UL || fin > 12000000UL)
        {
            continue;                               /* PLL input range   */
        }
        for (i = 0; i < sizeof(pllMul) / sizeof(pllMul[0]); i++)
        {
            uint32_t sys = (uint32_t)(((uint64_t)hseHz * pllMul[i].mulX2) / (2U * p1));
            uint32_t cost;

            if (sys > 72000000UL || sys < 24000000UL)
            {
                continue;
            }
            cost = planCost(sys, baud, uartNum, sampleRate, internalTick);

            /* Lower cost wins.  Costs within 2000 ppm count as equal and
             * then the faster clock wins. */
            if (!found || cost + 2000U < out->costPpm ||
                (cost <= out->costPpm + 2000U && sys > out->sysHz))
            {
                out->sysHz = sys;
                out->prediv1 = p1;
                out->mulX2 = pllMul[i].mulX2;
                out->pllBits = pllMul[i].bits;
                out->costPpm = cost;
                found = true;
            }
        }
    }
    return found;
}

/*
 * tim_plan_internal()
 *
 * Works out the timer reload value that divides the timer input clock down
 * to the sample rate.  The divider is limited to 1..65536.
 *
 * Parameters:
 *   timClkHz   - Timer input clock in Hz.
 *   sampleRate - Wanted sample rate in Hz.
 *   out        - Receives etps (0), the reload value and the sample rate
 *                actually obtained.
 *
 * Returns: nothing.
 */
void tim_plan_internal(uint32_t timClkHz, uint32_t sampleRate, tim_plan_t *out)
{
    uint32_t div = (timClkHz + sampleRate / 2U) / sampleRate;

    if (div < 1U)
    {
        div = 1U;
    }
    if (div > 65536U)
    {
        div = 65536U;
    }
    out->etps = 0;
    out->arr = div - 1U;
    out->actualHz = timClkHz / div;
}

/*
 * tim_plan_external()
 *
 * Works out the external clock prescaler and timer reload value that divide
 * an external clock down to the sample rate (external clock mode 2).  The
 * prescaler (1, 2, 4 or 8) is raised until the external clock is at most a
 * quarter of the timer clock.
 *
 * Parameters:
 *   extHz      - External clock frequency in Hz.
 *   timClkHz   - Timer input clock in Hz.
 *   sampleRate - Wanted sample rate in Hz.
 *   out        - Receives the ETPS field, the reload value and the sample
 *                rate actually obtained.
 *
 * Returns: nothing.
 */
void tim_plan_external(uint32_t extHz, uint32_t timClkHz, uint32_t sampleRate, tim_plan_t *out)
{
    uint32_t ps = 1;
    uint32_t etps = 0;
    uint32_t div;

    /* the pre-divided ETR frequency must stay below TIMCLK / 4 */
    while (ps < 8U && (extHz / ps) > (timClkHz / 4U))
    {
        ps <<= 1;
        etps++;
    }
    div = (extHz / ps + sampleRate / 2U) / sampleRate;
    if (div < 1U)
    {
        div = 1U;
    }
    if (div > 65536U)
    {
        div = 65536U;
    }
    out->etps = etps;
    out->arr = div - 1U;
    out->actualHz = (extHz / ps) / div;
}

/*
 * tim_plan_trim()
 *
 * Turns a reload value and a trim in ppm into a whole period plus a Q32
 * fraction, so the sample interrupt can average out a clock that is slightly
 * off.  The trim is limited to +/-20000 ppm and the period is never less
 * than one tick.
 *
 * Parameters:
 *   arr - Planned auto reload value (the period is arr + 1 ticks).
 *   ppm - Trim in parts per million; positive makes the sample clock slower.
 *   out - Receives the whole ticks per sample and the fraction.
 *
 * Returns: nothing.
 */
void tim_plan_trim(uint32_t arr, int32_t ppm, tim_trim_t *out)
{
    int64_t t = (int64_t)((uint64_t)(arr + 1U) << 32);

    if (ppm > 20000)
    {
        ppm = 20000;
    }
    if (ppm < -20000)
    {
        ppm = -20000;
    }
    t += (t * (int64_t)ppm) / 1000000LL;
    if (t < ((int64_t)1 << 32))
    {
        t = (int64_t)1 << 32;
    }
    out->period = (uint32_t)((uint64_t)t >> 32);
    out->frac = (uint32_t)((uint64_t)t & 0xFFFFFFFFULL);
}

/* ------------------------------------------------------------------ */
/* STM32F4 / F7 PLL                                                   */
/*
 * clk_uart_on_apb2()
 *
 * Tells whether a UART is on the APB2 bus (USART1 and USART6) rather than
 * APB1.
 *
 * Parameters:
 *   uartNum - UART number.
 *
 * Returns: true for USART1 and USART6.
 */
bool clk_uart_on_apb2(uint8_t uartNum)
{
    return uartNum == 1U || uartNum == 6U;
}

/*
 * apbDivFor()
 *
 * Finds the smallest APB divider (1, 2, 4, 8 or 16) that keeps the bus clock
 * at or below its maximum.
 *
 * Parameters:
 *   sysHz - System clock in Hz.
 *   maxHz - Highest allowed bus clock in Hz.
 *
 * Returns: The divider.
 */
static uint32_t apbDivFor(uint32_t sysHz, uint32_t maxHz)
{
    uint32_t div = 1;

    while (div < 16U && (sysHz / div) > maxHz)
    {
        div <<= 1;
    }
    return div;
}

/*
 * clk_plan_pll()
 *
 * Searches the STM32F4/F7 PLL settings (M, N, P, plus the APB dividers and
 * Q) for the best clock.  The first pass finds the lowest baud (and sample
 * rate) error.  The second pass picks, among plans within 2000 ppm of that,
 * the fastest SYSCLK in whole MHz, then the lowest M (highest PLL input
 * frequency), then the lowest error.
 *
 * Parameters:
 *   lim          - Limits of the chip: maximum clocks and VCO range.
 *   srcHz        - PLL source frequency in Hz (the crystal, or 16 MHz for
 *                  the internal oscillator).
 *   baud         - Wanted baud rate.
 *   uartNum      - UART number, which decides which APB bus its clock comes
 *                  from.
 *   sampleRate   - Wanted sample rate in Hz.
 *   internalTick - true if the sample timer is divided from the system
 *                  clock.
 *   timOnApb2    - true if the sample timer is on APB2, false for APB1.
 *   out          - Receives the chosen setting.
 *
 * Returns: true if a plan was found, false if nothing valid exists.
 */
bool clk_plan_pll(const clk_pll_limits_t *lim, uint32_t srcHz, uint32_t baud,
    uint8_t uartNum, uint32_t sampleRate, bool internalTick, bool timOnApb2,
    clk_pll_plan_t *out)
{
    bool found = false;
    uint32_t minCost = 0xFFFFFFFFUL;
    uint32_t pass;

    /* Pass 0 finds the lowest cost.  Pass 1 takes, among the plans whose
     * cost is within 2000 ppm of it, the fastest SYSCLK (compared in whole
     * MHz), then the highest PLL input frequency (lowest jitter), then
     * the lowest cost. */
    for (pass = 0; pass < 2U; pass++)
    {
        uint32_t m;

        for (m = 2; m <= 63U; m++)
        {
            uint32_t fin = srcHz / m;
            uint32_t n;

            if (fin < 1000000UL || fin > 2000000UL)
            {
                continue;                           /* VCO input 1..2 MHz */
            }
            for (n = 50; n <= 432U; n++)
            {
                uint32_t vco = (uint32_t)(((uint64_t)srcHz * n) / m);
                uint32_t p;

                if (vco < lim->minVcoHz || vco > lim->maxVcoHz)
                {
                    continue;
                }
                for (p = 2; p <= 8U; p += 2)
                {
                    uint32_t sys = vco / p;
                    uint32_t pre1, pre2, pclk1, pclk2, tim1, tim2, uartClk;
                    uint32_t brr, cost;

                    if (sys > lim->maxSysHz || sys < 24000000UL)
                    {
                        continue;
                    }
                    pre1 = apbDivFor(sys, (sys > lim->odAboveHz) ? lim->maxApb1OdHz : lim->maxApb1Hz);
                    pre2 = apbDivFor(sys, (sys > lim->odAboveHz) ? lim->maxApb2OdHz : lim->maxApb2Hz);
                    pclk1 = sys / pre1;
                    pclk2 = sys / pre2;
                    tim1 = (pre1 == 1U) ? pclk1 : pclk1 * 2U;
                    tim2 = (pre2 == 1U) ? pclk2 : pclk2 * 2U;

                    uartClk = clk_uart_on_apb2(uartNum) ? pclk2 : pclk1;
                    brr = (uartClk + baud / 2U) / baud;
                    if (brr == 0U)
                    {
                        continue;
                    }
                    cost = (uint32_t)(((uint64_t)absDiff(uartClk / brr, baud) * 1000000ULL) / baud);
                    if (internalTick)
                    {
                        uint32_t timClk = timOnApb2 ? tim2 : tim1;
                        uint32_t div = (timClk + sampleRate / 2U) / sampleRate;

                        cost += (uint32_t)(((uint64_t)absDiff(timClk / div, sampleRate) * 1000000ULL) /
                            sampleRate);
                    }

                    if (pass == 0U)
                    {
                        if (cost < minCost)
                        {
                            minCost = cost;
                        }
                        continue;
                    }
                    if (cost > minCost + 2000U)
                    {
                        continue;
                    }
                    if (!found || sys / 1000000UL > out->sysHz / 1000000UL ||
                        (sys / 1000000UL == out->sysHz / 1000000UL &&
                         (m < out->m || (m == out->m && cost < out->costPpm))))
                    {
                        uint32_t q = (vco + 47999999UL) / 48000000UL;

                        if (q < 2U)
                        {
                            q = 2U;
                        }
                        out->sysHz = sys;
                        out->m = m;
                        out->n = n;
                        out->p = p;
                        out->q = q;
                        out->ppre1 = pre1;
                        out->ppre2 = pre2;
                        out->pclk1Hz = pclk1;
                        out->pclk2Hz = pclk2;
                        out->timClk1Hz = tim1;
                        out->timClk2Hz = tim2;
                        out->costPpm = cost;
                        found = true;
                    }
                }
            }
        }
        if (minCost == 0xFFFFFFFFUL)
        {
            return false;                           /* nothing valid     */
        }
    }
    return found;
}
