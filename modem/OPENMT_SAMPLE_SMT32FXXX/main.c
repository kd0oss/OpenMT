/* Created by Rick KD0OSS */
/*
 *  main.c - OpenMT SAMPLE-mode modem firmware for STM32F105 / F446 / F722.
 */

#include "config.h"
#include "hw.h"
#include "engine.h"

/*
 * delayMs()
 *
 * Waits for a time while keeping the watchdog fed.
 *
 * Parameters:
 *   ms - Time to wait in milliseconds.
 *
 * Returns: nothing.
 */
static void delayMs(uint32_t ms)
{
    uint32_t start = hw_millis();

    while ((uint32_t)(hw_millis() - start) < ms)
    {
        hw_watchdog_kick();
    }
}

/*
 * ledTest()
 *
 * Lights each LED in turn and then switches them off in turn, 60 ms per
 * step, as a start-up check.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
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

/*
 * main()
 *
 * Firmware entry point: starts the hardware and the engine, starts sampling,
 * and then loops forever running the engine and hardware polling and kicking
 * the watchdog.
 *
 * Parameters: none.
 *
 * Returns: Never returns.
 */
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
