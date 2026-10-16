/*
 *  startup.c - reset handler and vector table for the STM32F105/F107
 *  connectivity line (Cortex-M3) and the STM32F446 (M4) / F722 (M7).
 *
 *  Only the interrupts the firmware uses are wired up; the rest go to
 *  Default_Handler.
 */

#include <stdint.h>
#include "config.h"

#if defined(MCU_F105)
#define NUM_IRQS        68                  /* up to OTG_FS              */
#define TIM1_UP_NAME    TIM1_UP_IRQHandler
#elif defined(MCU_F446)
#define NUM_IRQS        97                  /* up to FMPI2C1_ER          */
#define TIM1_UP_NAME    TIM1_UP_TIM10_IRQHandler
#else
#define NUM_IRQS        104                 /* up to SDMMC2              */
#define TIM1_UP_NAME    TIM1_UP_TIM10_IRQHandler
#endif

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

/* Survives a software reset (not cleared at start-up, see linker.ld.in) */
uint32_t bootMagic __attribute__((section(".noinit")));

#ifndef BOOT_DIAG
#define BOOT_DIAG       0   /* 1 = light LED0 for about a second before the
                               jump (F105): shows the jump was reached.
                               Build with CFLAGS_EXTRA="-DBOOT_DIAG=1" */
#endif

#if BOOTLOADER_COMMAND && !USE_LOADER
/* Called first thing after the reset that hw_enter_bootloader() requested:
 * the chip is in its reset state (HSI, no peripherals running), exactly
 * what the ROM boot loader expects. */
static void __attribute__((noreturn)) jumpToBootloader(void)
{
    uint32_t sp = *(volatile uint32_t *)BOOTLOADER_ADDR;
    uint32_t pc = *(volatile uint32_t *)(BOOTLOADER_ADDR + 4U);

#if BOOT_DIAG && defined(MCU_F105) && (LED0_PIN != PIN_NONE)
    {
        uint32_t port = (uint32_t)LED0_PIN >> 4;
        uint32_t num = (uint32_t)LED0_PIN & 15U;
        volatile uint32_t *gpio = (volatile uint32_t *)(0x40010800UL + 0x400UL * port);
        volatile uint32_t *cr = &gpio[num >> 3];          /* CRL / CRH   */
        uint32_t shift = (num & 7U) * 4U;
        volatile uint32_t i;

        *(volatile uint32_t *)0x40021018UL |= (1UL << (2U + port));   /* clock */
        *cr = (*cr & ~(0xFUL << shift)) | (0x2UL << shift); /* 2 MHz push-pull */
#if LED_ACTIVE_HIGH
        gpio[4] = 1UL << num;                                /* BSRR: LED on */
#else
        gpio[4] = 1UL << (num + 16U);
#endif
        for (i = 0; i < 1500000UL; i++)
        {
        }
    }
#endif

#if defined(MCU_F446)
    *(volatile uint32_t *)(0x40023800UL + 0x44U) |= (1UL << 14);   /* RCC_APB2ENR SYSCFGEN */
    *(volatile uint32_t *)(0x40013800UL) = 1U;                     /* SYSCFG_MEMRMP: system flash */
#endif
    /* The ROM boot loader uses interrupts (SysTick for the baud rate
     * detection) and, when it starts from system memory by the BOOT pins,
     * finds its vector table at address 0.  Here address 0 is our flash, so
     * point VTOR at the boot loader's own table; otherwise its first
     * interrupt lands in our Default_Handler and it hangs. */
    *(volatile uint32_t *)0xE000ED08UL = BOOTLOADER_ADDR;
    __asm volatile ("dsb\n\tisb" : : : "memory");
    __asm volatile ("msr msp, %0\n\tbx %1" : : "r" (sp), "r" (pc) : "memory");
    for (;;)
    {
    }
}
#endif

extern int main(void);
void Reset_Handler(void);
void Default_Handler(void);

void NMI_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void TIM1_UP_NAME(void)       __attribute__((weak, alias("Default_Handler")));
void TIM2_IRQHandler(void)    __attribute__((weak, alias("Default_Handler")));
void TIM3_IRQHandler(void)    __attribute__((weak, alias("Default_Handler")));
void TIM4_IRQHandler(void)    __attribute__((weak, alias("Default_Handler")));

typedef void (*vector_t)(void);

/* 16 core vectors + the chip's interrupts */
__attribute__((section(".isr_vector"), used))
const vector_t vectorTable[16 + NUM_IRQS] =
{
    [0 ... 16 + NUM_IRQS - 1] = Default_Handler,
    [0]  = (vector_t)&_estack,
    [1]  = Reset_Handler,
    [2]  = NMI_Handler,
    [3]  = HardFault_Handler,
    [4]  = MemManage_Handler,
    [5]  = BusFault_Handler,
    [6]  = UsageFault_Handler,
    [11] = SVC_Handler,
    [12] = DebugMon_Handler,
    [14] = PendSV_Handler,
    [15] = SysTick_Handler,
    [16 + 25] = TIM1_UP_NAME,
    [16 + 28] = TIM2_IRQHandler,
    [16 + 29] = TIM3_IRQHandler,
    [16 + 30] = TIM4_IRQHandler,
};

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;

#if BOOTLOADER_COMMAND && !USE_LOADER
    if (bootMagic == BOOT_MAGIC)
    {
        bootMagic = 0;
        jumpToBootloader();
    }
#endif

    /* Vector table where it really is (matters with APP_OFFSET, i.e. a
     * boot loader, and on chips that alias flash at address 0) */
    *(volatile uint32_t *)0xE000ED08UL = (uint32_t)vectorTable;

    while (dst < &_edata)
    {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss; dst++)
    {
        *dst = 0;
    }
    main();
    for (;;)
    {
    }
}

void Default_Handler(void)
{
    for (;;)
    {
    }
}
