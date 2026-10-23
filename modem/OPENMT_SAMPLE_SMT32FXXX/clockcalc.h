/* Created by Rick KD0OSS */
/*
 *  clockcalc.h - pure calculations for the clock tree and sample timer
 *  (no register access, so they can be unit tested on a PC).
 */

#ifndef CLOCKCALC_H
#define CLOCKCALC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint32_t sysHz;         /* resulting SYSCLK                          */
    uint32_t prediv1;       /* 1..16                                     */
    uint32_t mulX2;         /* PLL multiplier x 2 (13 = x6.5)            */
    uint32_t pllBits;       /* RCC_CFGR PLLMUL field value, in place     */
    uint32_t costPpm;       /* baud (+ sample rate) error                */
} clk_plan_t;

/* UART clock for a given SYSCLK (USART1 on APB2, 2/3 on APB1) */
uint32_t clk_uart_pclk(uint32_t sysHz, uint8_t uartNum);

/* Find the PLL setting for a crystal.  Returns false if nothing fits. */
bool clk_plan_hse(uint32_t hseHz, uint32_t baud, uint8_t uartNum,
    uint32_t sampleRate, bool internalTick, clk_plan_t *out);

typedef struct
{
    uint32_t etps;          /* ETPS register field 0..3                  */
    uint32_t arr;           /* auto reload value                         */
    uint32_t actualHz;      /* resulting sample rate                     */
} tim_plan_t;

/* Divide an external clock down to the sample rate (external clock
 * mode 2).  timClkHz is the timer input clock. */
void tim_plan_external(uint32_t extHz, uint32_t timClkHz, uint32_t sampleRate, tim_plan_t *out);

/* Divide the timer input clock down to the sample rate */
void tim_plan_internal(uint32_t timClkHz, uint32_t sampleRate, tim_plan_t *out);


/* Sample clock trim.  The timer divides its input clock by a whole number
 * of ticks, and the clock itself (crystal, PLL) is never exact, so the real
 * sample rate is off by tens to hundreds of ppm.  A radio's M17 demodulator
 * tolerates a symbol clock that is slow but not one that is fast by more
 * than about 100 ppm.  tim_plan_trim() turns the planned ARR and a trim in
 * ppm (positive = slower) into a whole period plus a fraction (Q32); the
 * sample interrupt adds the fraction every time and lengthens the period by
 * one tick whenever it carries, so the average period is exact. */
typedef struct
{
    uint32_t period;        /* whole ticks per sample (>= 1)            */
    uint32_t frac;          /* fraction of a tick, Q32                  */
} tim_trim_t;

void tim_plan_trim(uint32_t arr, int32_t ppm, tim_trim_t *out);

/* ------------------------------------------------------------------ */
/* STM32F4 / F7 style PLL: f = src / M * N / P, APB1 / APB2 dividers   */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint32_t maxSysHz;      /* 180 MHz (F446), 216 MHz (F722)            */
    uint32_t maxApb1Hz;     /* without over-drive: 42 / 45 MHz           */
    uint32_t maxApb2Hz;     /*                     84 / 90 MHz           */
    uint32_t maxApb1OdHz;   /* with over-drive:    45 / 54 MHz           */
    uint32_t maxApb2OdHz;   /*                     90 / 108 MHz          */
    uint32_t odAboveHz;     /* SYSCLK above this needs over-drive        */
    uint32_t minVcoHz;      /* 100 MHz (F446), 192 MHz (F722)            */
    uint32_t maxVcoHz;      /* 432 MHz                                   */
} clk_pll_limits_t;

typedef struct
{
    uint32_t sysHz;         /* SYSCLK = HCLK (AHB divider is always 1)   */
    uint32_t m, n, p, q;    /* PLLM, PLLN, PLLP (2/4/6/8), PLLQ          */
    uint32_t ppre1;         /* APB1 divider 1,2,4,8,16                   */
    uint32_t ppre2;         /* APB2 divider                              */
    uint32_t pclk1Hz;
    uint32_t pclk2Hz;
    uint32_t timClk1Hz;     /* TIM2-7, 12-14 input clock                 */
    uint32_t timClk2Hz;     /* TIM1, 8-11 input clock                    */
    uint32_t costPpm;       /* baud (+ sample rate) error                */
} clk_pll_plan_t;

/* USART1 and USART6 are on APB2, all other U(S)ARTs on APB1 */
bool clk_uart_on_apb2(uint8_t uartNum);

/* Search M / N / P and the APB dividers for the fastest SYSCLK that keeps
 * the baud rate (and, with internalTick, the 24 kHz timer divider)
 * accurate.  srcHz is the HSE frequency, or 16 MHz for the HSI.
 * timOnApb2 says which bus the sample timer is on. */
bool clk_plan_pll(const clk_pll_limits_t *lim, uint32_t srcHz, uint32_t baud,
    uint8_t uartNum, uint32_t sampleRate, bool internalTick, bool timOnApb2,
    clk_pll_plan_t *out);

#endif /* CLOCKCALC_H */
