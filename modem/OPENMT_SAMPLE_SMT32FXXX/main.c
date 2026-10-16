/*
 *  main.c - OpenMT SAMPLE-mode modem firmware for STM32F105 / F446 / F722.
 */

#include "config.h"
#include "hw.h"
#include "engine.h"

static void delayMs(uint32_t ms)
{
    uint32_t start = hw_millis();

    while ((uint32_t)(hw_millis() - start) < ms)
    {
        hw_watchdog_kick();
    }
}

static void ledTest(void)
{
    uint8_t i;

    for (i = 0; i < NUM_LEDS; i++)
    {
        hw_led(i, true);
        delayMs(60);
    }
    for (i = 0; i < NUM_LEDS; i++)
    {
        hw_led(i, false);
        delayMs(60);
    }
}

int main(void)
{
    hw_init();

#if LED_TEST_AT_BOOT
    ledTest();
#endif

    engine_init();
    hw_start_sampling();

    for (;;)
    {
        engine_poll();
        hw_poll();
        hw_watchdog_kick();
    }
}
