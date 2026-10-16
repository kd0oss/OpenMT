/*
 *  hw.c - STM32F105 (connectivity line) hardware layer.
 *
 *  Direct register access, no vendor library needed.
 */

#include "stm32f10x.h"
#include "config.h"
#include "hw.h"
#include "clockcalc.h"

/* ------------------------------------------------------------------ */
/* GPIO                                                               */
/* ------------------------------------------------------------------ */
#define GPIO_IN_FLOAT   0x4U
#define GPIO_IN_PUPD    0x8U
#define GPIO_OUT_PP     0x3U
#define GPIO_AF_PP      0xBU
#define GPIO_ANALOG     0x0U

static GPIO_TypeDef *gpioOf(uint8_t pin)
{
    return (GPIO_TypeDef *)(GPIOA_BASE + ((uint32_t)(pin >> 4) * 0x400U));
}

static void gpioClock(uint8_t pin)
{
    if (pin != PIN_NONE)
    {
        RCC->APB2ENR |= (RCC_APB2ENR_IOPAEN << (pin >> 4));
    }
}

static void gpioMode(uint8_t pin, uint32_t cfg)
{
    GPIO_TypeDef *g = gpioOf(pin);
    uint32_t n = pin & 0x0FU;
    uint32_t sh;

    if (pin == PIN_NONE)
    {
        return;
    }
    gpioClock(pin);

    if (n < 8U)
    {
        sh = n * 4U;
        g->CRL = (g->CRL & ~(0xFUL << sh)) | (cfg << sh);
    }
    else
    {
        sh = (n - 8U) * 4U;
        g->CRH = (g->CRH & ~(0xFUL << sh)) | (cfg << sh);
    }
}

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

/* ------------------------------------------------------------------ */
/* Clocks                                                             */
/* ------------------------------------------------------------------ */
static uint32_t sysclkHz;
static uint32_t pclk1Hz;
static uint32_t pclk2Hz;
static uint32_t timClk1Hz;                  /* TIM2-4 input clock        */
static uint32_t timClk2Hz;                  /* TIM1 input clock          */

static volatile uint32_t msTicks;

void SysTick_Handler(void)
{
    msTicks++;
}

uint32_t hw_chip_id(void)
{
    return *(volatile uint32_t *)0xE0042000UL;      /* DBGMCU_IDCODE */
}

uint32_t hw_millis(void)
{
    return msTicks;
}

static bool waitFlag(volatile uint32_t *reg, uint32_t mask, bool set)
{
    uint32_t t = 200000U;

    while (t--)
    {
        if (((*reg & mask) != 0U) == set)
        {
            return true;
        }
    }
    return false;
}

static void deriveClocks(uint32_t sys)
{
    sysclkHz = sys;
    pclk2Hz = sys;
    if (sys > 36000000UL)
    {
        pclk1Hz = sys / 2U;
        timClk1Hz = pclk1Hz * 2U;
    }
    else
    {
        pclk1Hz = sys;
        timClk1Hz = sys;
    }
    timClk2Hz = pclk2Hz;
}

static void setFlashLatency(uint32_t sys)
{
    uint32_t lat = (sys <= 24000000UL) ? 0U : ((sys <= 48000000UL) ? 1U : 2U);

    FLASH->ACR = (FLASH->ACR & ~0x7UL) | lat | FLASH_ACR_PRFTBE;
}

/* Switch to the PLL, given the PLL bits already programmed in CFGR */
static bool runOnPll(uint32_t sys)
{
    RCC->CR |= RCC_CR_PLLON;
    if (!waitFlag(&RCC->CR, RCC_CR_PLLRDY, true))
    {
        return false;
    }
    setFlashLatency(sys);
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2 | RCC_CFGR_SW))
        | ((sys > 36000000UL) ? RCC_CFGR_PPRE1_DIV2 : 0U) | RCC_CFGR_SW_PLL;
    return waitFlag(&RCC->CFGR, RCC_CFGR_SWS_1, true);
}

static bool clockHsi(void)
{
    /* HSI/2 = 4 MHz, x9 = 36 MHz (F105 PLL limit with the HSI input) */
    RCC->CR |= RCC_CR_HSION;
    waitFlag(&RCC->CR, RCC_CR_HSIRDY, true);
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL)) | (0x7UL << 18);
    if (!runOnPll(36000000UL))
    {
        return false;
    }
    deriveClocks(36000000UL);
    return true;
}

#if CLOCK_SOURCE == CLOCK_SOURCE_HSE
static bool clockHse(void)
{
    clk_plan_t plan;

    RCC->CR |= RCC_CR_HSEON | (HSE_BYPASS ? RCC_CR_HSEBYP : 0U);
    if (!waitFlag(&RCC->CR, RCC_CR_HSERDY, true))
    {
        RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_HSEBYP);
        return false;
    }

    if (!clk_plan_hse(HSE_HZ, UART_BAUD, UART_NUM, SAMPLE_RATE_HZ, EXT_CLK_HZ == 0UL, &plan))
    {
        /* no valid PLL setting (crystal outside the 3..25 MHz range) */
        RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_HSEBYP);
        return false;
    }

    RCC->CFGR2 = (RCC->CFGR2 & ~(RCC_CFGR2_PREDIV1 | RCC_CFGR2_PREDIV1SRC)) | (plan.prediv1 - 1U);
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL)) | RCC_CFGR_PLLSRC_PREDIV1 |
        plan.pllBits;
    if (!runOnPll(plan.sysHz))
    {
        return false;
    }
    deriveClocks(plan.sysHz);
    return true;
}
#endif

static void clockInit(void)
{
    /* start from a known state: HSI, PLL off */
    RCC->CR |= RCC_CR_HSION;
    waitFlag(&RCC->CR, RCC_CR_HSIRDY, true);
    RCC->CFGR &= ~RCC_CFGR_SW;
    waitFlag(&RCC->CFGR, RCC_CFGR_SWS, false);
    RCC->CR &= ~RCC_CR_PLLON;

#if CLOCK_SOURCE == CLOCK_SOURCE_HSE
    if (!clockHse())
    {
        /* crystal did not start: run from HSI so the board still works */
        clockHsi();
    }
#else
    clockHsi();
#endif

    SysTick_Config(sysclkHz / 1000U);
}

/* ------------------------------------------------------------------ */
/* PTT and LEDs                                                       */
/* ------------------------------------------------------------------ */
/* Short critical section (PRIMASK) - the sample interrupt also touches PTT */
static inline uint32_t irqSave(void)
{
    uint32_t primask;

    __asm volatile ("mrs %0, primask\n\tcpsid i" : "=r" (primask) : : "memory");
    return primask;
}

static inline void irqRestore(uint32_t primask)
{
    __asm volatile ("msr primask, %0" : : "r" (primask) : "memory");
}

static volatile bool pttWanted;
static volatile bool pttInvert;

/* Reads the wanted state and writes the pin in one critical section, so
 * the sample interrupt (which unkeys at the end of a transmission) can
 * never be overtaken by a stale write from the main loop. */
static void pttApply(void)
{
    uint32_t primask = irqSave();
    bool level = pttWanted ? (PTT_ACTIVE_HIGH != 0) : (PTT_ACTIVE_HIGH == 0);

    if (pttInvert)
    {
        level = !level;
    }
    gpioWrite(PTT_PIN, level);
    irqRestore(primask);
}

void hw_ptt(bool on)
{
    pttWanted = on;
    pttApply();
}

void hw_set_ptt_invert(bool invert)
{
    pttInvert = invert;
    pttApply();
}

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
    s |= pttInvert ? 0x10U : 0U;
    return s;
}

static const uint8_t ledPins[NUM_LEDS] =
{
    LED0_PIN, LED1_PIN, LED2_PIN, LED3_PIN, LED4_PIN, LED5_PIN, LED6_PIN, LED7_PIN
};

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

#define DMACH(n)        ((DMA_Channel_TypeDef *)(DMA1_Channel1_BASE + 0x14U * ((n) - 1U)))
#define DMA_TCIF(n)     (0x2UL << (4U * ((n) - 1U)))
#define DMA_CLRALL(n)   (0xFUL << (4U * ((n) - 1U)))

#if UART_NUM == 1
#define USARTx          USART1
#define UART_TX_DMA     4U
#define UART_RX_DMA     5U
#if UART_REMAP
#define UART_TX_PIN     PB(6)
#define UART_RX_PIN     PB(7)
#define UART_MAPR       AFIO_MAPR_USART1_REMAP
#else
#define UART_TX_PIN     PA(9)
#define UART_RX_PIN     PA(10)
#define UART_MAPR       0U
#endif
#elif UART_NUM == 2
#define USARTx          USART2
#define UART_TX_DMA     7U
#define UART_RX_DMA     6U
#if UART_REMAP
#define UART_TX_PIN     PD(5)
#define UART_RX_PIN     PD(6)
#define UART_MAPR       AFIO_MAPR_USART2_REMAP
#else
#define UART_TX_PIN     PA(2)
#define UART_RX_PIN     PA(3)
#define UART_MAPR       0U
#endif
#else
#define USARTx          USART3
#define UART_TX_DMA     2U
#define UART_RX_DMA     3U
#if UART_REMAP == 3
#define UART_TX_PIN     PD(8)
#define UART_RX_PIN     PD(9)
#define UART_MAPR       AFIO_MAPR_USART3_REMAP_FULLREMAP
#elif UART_REMAP == 1
#define UART_TX_PIN     PC(10)
#define UART_RX_PIN     PC(11)
#define UART_MAPR       AFIO_MAPR_USART3_REMAP_PARTIALREMAP
#else
#define UART_TX_PIN     PB(10)
#define UART_RX_PIN     PB(11)
#define UART_MAPR       0U
#endif
#endif

static uint8_t           uartRx[UART_RX_SIZE];
static uint16_t          uartRxTail;
static uint8_t           uartTx[UART_TX_SIZE];
static uint16_t          uartTxHead;
static uint16_t          uartTxTail;
static uint16_t          uartTxInFlight;
static volatile uint32_t uartErr;

static uint16_t rxHeadIndex(void)
{
    return (uint16_t)((UART_RX_SIZE - DMACH(UART_RX_DMA)->CNDTR) & (UART_RX_SIZE - 1U));
}

uint16_t hw_uart_rx_available(void)
{
    return (uint16_t)((rxHeadIndex() - uartRxTail) & (UART_RX_SIZE - 1U));
}

uint8_t hw_uart_rx_peek(uint16_t offset)
{
    return uartRx[(uartRxTail + offset) & (UART_RX_SIZE - 1U)];
}

void hw_uart_rx_consume(uint16_t count)
{
    uartRxTail = (uint16_t)((uartRxTail + count) & (UART_RX_SIZE - 1U));
}

void hw_uart_poll(void)
{
    DMA_Channel_TypeDef *ch = DMACH(UART_TX_DMA);
    uint32_t sr = USARTx->SR;

    /* error counting: only clear when no received byte is pending, so
     * the DMA never loses one */
    if ((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) && !(sr & USART_SR_RXNE))
    {
        uartErr++;
        (void)USARTx->DR;
    }

    if (uartTxInFlight != 0U)
    {
        if (!(DMA1->ISR & DMA_TCIF(UART_TX_DMA)))
        {
            return;
        }
        DMA1->IFCR = DMA_CLRALL(UART_TX_DMA);
        ch->CCR &= ~DMA_CCR1_EN;
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
        ch->CMAR = (uint32_t)&uartTx[uartTxTail];
        ch->CNDTR = n;
        uartTxInFlight = n;
        ch->CCR |= DMA_CCR1_EN;
    }
}

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

uint32_t hw_uart_errors(void)
{
    return uartErr;
}

static void uartInit(void)
{
    DMA_Channel_TypeDef *rx = DMACH(UART_RX_DMA);
    DMA_Channel_TypeDef *tx = DMACH(UART_TX_DMA);
    uint32_t pclk = clk_uart_pclk(sysclkHz, UART_NUM);

    RCC->AHBENR |= RCC_AHBENR_DMA1EN;
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
#if UART_NUM == 1
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
#elif UART_NUM == 2
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
#else
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
#endif

    if (UART_MAPR != 0U)
    {
        AFIO->MAPR |= UART_MAPR;
    }

    gpioMode(UART_TX_PIN, GPIO_AF_PP | 0x3U);       /* AF push-pull 50 MHz */
    gpioMode(UART_RX_PIN, GPIO_IN_PUPD);
    gpioWrite(UART_RX_PIN, true);                   /* pull-up           */

    USARTx->CR1 = 0;
    USARTx->CR2 = 0;
    USARTx->CR3 = USART_CR3_DMAT | USART_CR3_DMAR;
    USARTx->BRR = (uint16_t)((pclk + UART_BAUD / 2U) / UART_BAUD);

    /* RX: circular DMA into uartRx */
    rx->CCR = 0;
    rx->CPAR = (uint32_t)&USARTx->DR;
    rx->CMAR = (uint32_t)uartRx;
    rx->CNDTR = UART_RX_SIZE;
    rx->CCR = DMA_CCR1_MINC | DMA_CCR1_CIRC | DMA_CCR1_PL_1 | DMA_CCR1_EN;

    /* TX: normal DMA, loaded by hw_uart_poll() */
    tx->CCR = 0;
    tx->CPAR = (uint32_t)&USARTx->DR;
    tx->CCR = DMA_CCR1_MINC | DMA_CCR1_DIR | DMA_CCR1_PL_0;

    USARTx->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

/* ------------------------------------------------------------------ */
/* ADC and DAC                                                        */
/* ------------------------------------------------------------------ */
#define ADC_CR2_RUN     (ADC_CR2_ADON | ADC_CR2_EXTTRIG | ADC_CR2_EXTSEL | ADC_CR2_SWSTART)

static void adcInit(void)
{
    uint32_t adcpre;

    gpioMode(ADC_PIN, GPIO_ANALOG);

    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

    /* ADC clock must be <= 14 MHz */
    if (pclk2Hz / 2U <= 14000000UL)
    {
        adcpre = 0U;
    }
    else if (pclk2Hz / 4U <= 14000000UL)
    {
        adcpre = 1U;
    }
    else if (pclk2Hz / 6U <= 14000000UL)
    {
        adcpre = 2U;
    }
    else
    {
        adcpre = 3U;
    }
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_ADCPRE) | (adcpre << 14);

    ADC1->CR1 = 0;
    ADC1->CR2 = ADC_CR2_ADON;
    for (volatile uint32_t d = 0; d < 2000U; d++)
    {
    }
    ADC1->CR2 |= ADC_CR2_RSTCAL;
    while (ADC1->CR2 & ADC_CR2_RSTCAL)
    {
    }
    ADC1->CR2 |= ADC_CR2_CAL;
    while (ADC1->CR2 & ADC_CR2_CAL)
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

static void dacInit(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_DACEN;
#if DAC_CHANNEL == 1
    gpioMode(PA(4), GPIO_ANALOG);
    DAC->CR = DAC_CR_EN1 | (DAC_OUTPUT_BUFFER ? 0U : DAC_CR_BOFF1);
    DAC->DHR12R1 = DAC_MID;
#else
    gpioMode(PA(5), GPIO_ANALOG);
    DAC->CR = DAC_CR_EN2 | (DAC_OUTPUT_BUFFER ? 0U : DAC_CR_BOFF2);
    DAC->DHR12R2 = DAC_MID;
#endif
}

/* ------------------------------------------------------------------ */
/* Sample timer                                                       */
/* ------------------------------------------------------------------ */
#if SAMPLE_TIM_N == 1
#define SAMPLE_TIM          TIM1
#define SAMPLE_TIM_IRQn     TIM1_UP_IRQn
#define SAMPLE_ISR          TIM1_UP_IRQHandler
#define SAMPLE_TIM_CLK()    (RCC->APB2ENR |= RCC_APB2ENR_TIM1EN)
#define SAMPLE_TIM_HZ       timClk2Hz
#define EXT_ETR_PIN         PA(12)
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
#elif SAMPLE_TIM_N == 3
#define SAMPLE_TIM          TIM3
#define SAMPLE_TIM_IRQn     TIM3_IRQn
#define SAMPLE_ISR          TIM3_IRQHandler
#define SAMPLE_TIM_CLK()    (RCC->APB1ENR |= RCC_APB1ENR_TIM3EN)
#define SAMPLE_TIM_HZ       timClk1Hz
#define EXT_ETR_PIN         PD(2)
#else
#define SAMPLE_TIM          TIM4
#define SAMPLE_TIM_IRQn     TIM4_IRQn
#define SAMPLE_ISR          TIM4_IRQHandler
#define SAMPLE_TIM_CLK()    (RCC->APB1ENR |= RCC_APB1ENR_TIM4EN)
#define SAMPLE_TIM_HZ       timClk1Hz
#define EXT_ETR_PIN         PE(0)
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

static void timerInternal(void)
{
    tim_plan_t plan;

    tim_plan_internal(SAMPLE_TIM_HZ, SAMPLE_RATE_HZ, &plan);

    SAMPLE_TIM->CR1 = 0;
    SAMPLE_TIM->SMCR = 0;                           /* internal clock    */
    SAMPLE_TIM->PSC = 0;
    SAMPLE_TIM->ARR = (uint16_t)plan.arr;
#if SAMPLE_TRIM_PPM != 0
    {
        tim_trim_t trim;

        tim_plan_trim(plan.arr, SAMPLE_TRIM_PPM, &trim);
        trimPeriod = trim.period;
        trimFrac = trim.frac;
        trimAcc = 0;
        SAMPLE_TIM->ARR = (uint16_t)(trim.period - 1U);
    }
#endif
    SAMPLE_TIM->DIER = TIM_DIER_UIE;
    SAMPLE_TIM->EGR = TIM_EGR_UG;
    SAMPLE_TIM->SR = 0;
    usingInternal = true;
}

#if EXT_CLK_HZ > 0
static void timerExternal(void)
{
    /* External clock mode 2: ETR -> ETPS -> counter */
    tim_plan_t plan;

    tim_plan_external(EXT_CLK_HZ, SAMPLE_TIM_HZ, SAMPLE_RATE_HZ, &plan);

    SAMPLE_TIM->CR1 = 0;
    SAMPLE_TIM->SMCR = TIM_SMCR_ECE_BIT | (plan.etps << 12) | ((uint32_t)EXT_CLK_FILTER << 8) |
        (EXT_CLK_INVERT ? TIM_SMCR_ETP_BIT : 0U);
    SAMPLE_TIM->PSC = 0;
    SAMPLE_TIM->ARR = (uint16_t)plan.arr;
#if SAMPLE_TRIM_PPM != 0
    {
        tim_trim_t trim;

        tim_plan_trim(plan.arr, SAMPLE_TRIM_PPM, &trim);
        trimPeriod = trim.period;
        trimFrac = trim.frac;
        trimAcc = 0;
        SAMPLE_TIM->ARR = (uint16_t)(trim.period - 1U);
    }
#endif
    SAMPLE_TIM->DIER = TIM_DIER_UIE;
    SAMPLE_TIM->EGR = TIM_EGR_UG;
    SAMPLE_TIM->SR = 0;
    usingInternal = false;
}
#endif

static void timerInit(void)
{
    SAMPLE_TIM_CLK();

#if EXT_CLK_HZ > 0
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
#if SAMPLE_TIM_N == 2 && EXT_CLK_TIM2_REMAP
    AFIO->MAPR = (AFIO->MAPR & ~AFIO_MAPR_TIM2_REMAP) | AFIO_MAPR_TIM2_REMAP_PARTIALREMAP1;
#endif
    gpioMode(EXT_ETR_PIN, GPIO_IN_FLOAT);
    timerExternal();
#else
    timerInternal();
#endif
}

void SAMPLE_ISR(void)
{
    uint16_t raw;
    uint16_t out;

    SAMPLE_TIM->SR = 0;                             /* clear update flag */
#if SAMPLE_TRIM_PPM != 0
    /* dither the period: one extra tick whenever the fraction carries */
    trimAcc += trimFrac;
    SAMPLE_TIM->ARR = (uint16_t)((trimAcc < trimFrac) ? trimPeriod : (trimPeriod - 1U));
#endif

    raw = ADC1->DR;                                 /* previous result   */
    ADC1->CR2 = ADC_CR2_RUN;                        /* start next        */

    out = engine_sample_tick(raw);

#if DAC_CHANNEL == 1
    DAC->DHR12R1 = out;
#else
    DAC->DHR12R2 = out;
#endif
    sampleTicks++;
}

void hw_start_sampling(void)
{
    NVIC_SetPriority(SAMPLE_TIM_IRQn, 0);
    NVIC_ClearPendingIRQ(SAMPLE_TIM_IRQn);          /* drop the UG tick  */
    NVIC_EnableIRQ(SAMPLE_TIM_IRQn);
    lastTickMs = msTicks;
    SAMPLE_TIM->CR1 |= TIM_CR1_CEN;
}

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
/* ------------------------------------------------------------------ */
void hw_poll(void)
{
}

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

void hw_watchdog_kick(void)
{
#if WATCHDOG_ENABLE
    IWDG->KR = 0xAAAAU;
#endif
}

static void watchdogInit(void)
{
#if WATCHDOG_ENABLE
    DBGMCU->CR |= DBGMCU_CR_DBG_IWDG_STOP;
    IWDG->KR = 0x5555U;
    IWDG->PR = 4U;                                  /* LSI / 64          */
    IWDG->RLR = 625U;                               /* about 1 s         */
    IWDG->KR = 0xCCCCU;
#endif
}

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */
void hw_init(void)
{
    uint8_t i;

    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
#if FREE_JTAG_PINS
    AFIO->MAPR = (AFIO->MAPR & ~AFIO_MAPR_SWJ_CFG) | AFIO_MAPR_SWJ_CFG_JTAGDISABLE;
#endif

    /* PTT first, before the (slow) clock start-up: enable the port, set
     * the inactive level, then make it an output, so the pin never
     * glitches active and is not left floating for long */
    pttWanted = false;
    gpioClock(PTT_PIN);
    pttApply();
    gpioMode(PTT_PIN, GPIO_OUT_PP);

    for (i = 0; i < NUM_LEDS; i++)
    {
        gpioClock(ledPins[i]);
        hw_led(i, false);
        gpioMode(ledPins[i], GPIO_OUT_PP);
    }

    clockInit();

    uartInit();
    adcInit();
    dacInit();
    timerInit();
    watchdogInit();
}
