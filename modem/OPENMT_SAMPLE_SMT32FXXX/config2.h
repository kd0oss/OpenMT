/*
 *  OpenMT SAMPLE-mode firmware for STM32F105, F446 and F722
 *
 *  config.h - every board specific setting lives in this file.
 *
 *  The chip is chosen on the make command line (make MCU=f105, f446 or
 *  f722), which defines MCU_F105, MCU_F446 or MCU_F722.  Defaults below
 *  follow the chip; set anything you need to different values here.
 *
 *  Pins are written PA(n) .. PE(n).  Use PIN_NONE for "not fitted".
 *  Anything that can be overridden from the command line is wrapped in
 *  #ifndef, so  make CFLAGS_EXTRA="-DHSE_HZ=12000000"  also works.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Pin helpers - do not edit                                          */
/* ------------------------------------------------------------------ */
#define PIN_ID(port, num)   (((port) << 4) | ((num) & 0x0F))
#define PA(n)               PIN_ID(0, n)
#define PB(n)               PIN_ID(1, n)
#define PC(n)               PIN_ID(2, n)
#define PD(n)               PIN_ID(3, n)
#define PE(n)               PIN_ID(4, n)
#define PF(n)               PIN_ID(5, n)        /* F4 / F7 only */
#define PG(n)               PIN_ID(6, n)
#define PH(n)               PIN_ID(7, n)
#define PIN_NONE            0xFF

/* ------------------------------------------------------------------ */
/* Chip selection and chip dependent defaults - do not edit           */
/* ------------------------------------------------------------------ */
#if !defined(MCU_F105) && !defined(MCU_F446) && !defined(MCU_F722)
#define MCU_F105
#endif

#if defined(MCU_F105)
#define MCU_NAME            "F105"
#define MCU_FAMILY_F1       1
#define DEF_FLASH_KB        128
#define DEF_PTT_PIN         PA(8)
#define DEF_ADC_PIN         PA(1)
#define DEF_ADC_SAMPLE_TIME 5               /* 55.5 cycles               */
#define DEF_TX_BUF_SAMPLES  8192
#define DEF_INHIBIT_PIN     PIN_NONE
#define MAX_UART_NUM        3
#define DEF_BOOTLOADER_CMD  1               /* no BOOT0 / NRST wires     */
#define BOOTLOADER_ADDR     0x1FFFB000UL    /* ROM bootloader (AN2606)   */
#elif defined(MCU_F446)
#define MCU_NAME            "F446"
#define MCU_FAMILY_F4       1
#define DEF_PTT_PIN         PB(12)
#define DEF_ADC_PIN         PB(0)
#define DEF_ADC_SAMPLE_TIME 4               /* 84 cycles                 */
#define DEF_TX_BUF_SAMPLES  16384
#define DEF_INHIBIT_PIN     PB(13)
#define MAX_UART_NUM        6
#define DEF_BOOTLOADER_CMD  0               /* BOOT0 / NRST on the Pi    */
#define BOOTLOADER_ADDR     0x1FFF0000UL
#elif defined(MCU_F722)
#define MCU_NAME            "F722"
#define MCU_FAMILY_F7       1
#define DEF_PTT_PIN         PB(12)
#define DEF_ADC_PIN         PB(0)
#define DEF_ADC_SAMPLE_TIME 4               /* 84 cycles                 */
#define DEF_TX_BUF_SAMPLES  16384
#define DEF_INHIBIT_PIN     PIN_NONE
#define MAX_UART_NUM        8
#define DEF_BOOTLOADER_CMD  0
#define BOOTLOADER_ADDR     0x1FF00000UL
#endif

/* ------------------------------------------------------------------ */
/* Sample rate                                                        */
/* ------------------------------------------------------------------ */
#define SAMPLE_RATE_HZ      24000           /* what OpenMT expects      */
#define SAMPLES_PER_FRAME   96              /* 192 byte q15 payload;
                                               matches 196 byte frames  */

/* ------------------------------------------------------------------ */
/* MCU clock                                                          */
/*                                                                    */
/*   CLOCK_SOURCE_HSE : crystal (or TCXO with HSE_BYPASS 1) on OSC_IN */
/*   CLOCK_SOURCE_HSI : no crystal, internal RC oscillator            */
/*                                                                    */
/* With HSE the firmware searches the PLL dividers for the fastest    */
/* SYSCLK (F105 72 MHz, F446 180 MHz, F722 216 MHz) that keeps the    */
/* sample timer and the UART baud rate accurate, so any crystal can   */
/* be used by just changing HSE_HZ (F105 3..25 MHz, F4/F7 4..26 MHz). */
/* With HSI the F105 runs at 36 MHz and the F4/F7 use their 16 MHz    */
/* RC oscillator through the same PLL search.  HSI is only about +-1% */
/* accurate, so the 24 kHz sample rate and baud rate are then +-1%    */
/* too; use an external clock (see below) or a crystal when you can.  */
/* ------------------------------------------------------------------ */
#define CLOCK_SOURCE_HSE    1
#define CLOCK_SOURCE_HSI    2

#ifndef CLOCK_SOURCE
#define CLOCK_SOURCE        CLOCK_SOURCE_HSE
#endif

#ifndef HSE_HZ
#define HSE_HZ              8000000UL       /* 3 MHz .. 25 MHz          */
#endif

#ifndef HSE_BYPASS
#define HSE_BYPASS          0               /* 1 = external clock on
                                               OSC_IN, crystal absent   */
#endif

/* ------------------------------------------------------------------ */
/* Sample interrupt clock                                             */
/*                                                                    */
/* EXT_CLK_HZ = 0     : sample interrupt from the MCU's own clock     */
/*                      (timer INT_SAMPLE_TIM, 24 kHz derived from    */
/*                      SYSCLK).                                      */
/* EXT_CLK_HZ > 0     : an external clock of that frequency is wired  */
/*                      to the ETR pin of timer EXT_CLK_TIM.  The     */
/*                      timer divides it down to 24 kHz and fires the */
/*                      sample interrupt, so ADC and DAC are locked   */
/*                      to that clock.  The frequency does not need   */
/*                      to be an exact multiple of 24 kHz (the        */
/*                      divider is rounded) but 12.288 MHz, 6.144 MHz */
/*                      12 MHz, 24 MHz, 9.6 MHz, 19.2 MHz etc. are    */
/*                      exact.                                        */
/*                                                                    */
/* ETR input pins are fixed by the chip (same on F105, F446 and F722): */
/*     EXT_CLK_TIM 1 : PA12                                           */
/*     EXT_CLK_TIM 2 : PA0  (EXT_CLK_TIM2_REMAP 1 -> PA15)            */
/*     EXT_CLK_TIM 3 : PD2                                            */
/*     EXT_CLK_TIM 4 : PE0                                            */
/* ------------------------------------------------------------------ */
#ifndef EXT_CLK_HZ
#define EXT_CLK_HZ          0UL
#endif

#ifndef EXT_CLK_TIM
#define EXT_CLK_TIM         2
#endif

#ifndef EXT_CLK_TIM2_REMAP
#define EXT_CLK_TIM2_REMAP  0               /* 1 = ETR on PA15          */
#endif

#ifndef EXT_CLK_INVERT
#define EXT_CLK_INVERT      0               /* 1 = count falling edges  */
#endif

#ifndef EXT_CLK_FILTER
#define EXT_CLK_FILTER      0               /* 0..15 input filter (see
                                               TIMx_SMCR.ETF); 0 = off,
                                               use >0 for noisy clocks
                                               below ~1 MHz only        */
#endif

#ifndef EXT_CLK_FALLBACK
#define EXT_CLK_FALLBACK    1               /* 1 = if no external clock
                                               edges are seen within
                                               250 ms of start, switch
                                               to the internal timer    */
#endif

#ifndef INT_SAMPLE_TIM
#define INT_SAMPLE_TIM      2               /* 2, 3 or 4; used when
                                               EXT_CLK_HZ is 0.  (The
                                               fallback reuses the
                                               EXT_CLK_TIM timer.)      */
#endif

/* ------------------------------------------------------------------ */
/* Raspberry Pi UART                                                  */
/*                                                                    */
/* UART_NUM 1 : TX PA9  RX PA10   (UART_REMAP 1 : TX PB6  RX PB7)     */
/* UART_NUM 2 : TX PA2  RX PA3    (UART_REMAP 1 : TX PD5  RX PD6)     */
/* UART_NUM 3 : TX PB10 RX PB11   (UART_REMAP 1 : TX PC10 RX PC11,    */
/*                                 UART_REMAP 3 : TX PD8  RX PD9)     */
/*                                                                    */
/* F446 / F722 only (UART_REMAP 0 = first pins, 1 / 2 = alternatives, */
/* USART3 remap 2 or 3 = PD8 / PD9):                                  */
/* UART_NUM 4 : TX PA0  RX PA1    (UART_REMAP 1 : TX PC10 RX PC11)    */
/* UART_NUM 5 : TX PC12 RX PD2                                        */
/* UART_NUM 6 : TX PC6  RX PC7    (UART_REMAP 1 : TX PG14 RX PG9)     */
/* UART_NUM 7 : TX PE8  RX PE7    (F722; UART_REMAP 1 : PF7 / PF6)    */
/* UART_NUM 8 : TX PE1  RX PE0    (F722)                              */
/*                                                                    */
/* 24 kHz x 16 bit = 48 kbyte/s needs MORE than 460800 baud, so the   */
/* link runs at 921600 baud.  Set "baud=921600" in the OpenMT modem   */
/* config on the Pi.                                                  */
/* ------------------------------------------------------------------ */
#ifndef UART_NUM
#define UART_NUM            1
#endif

#ifndef UART_REMAP
#define UART_REMAP          0
#endif

#ifndef UART_BAUD
#define UART_BAUD           921600UL
#endif

/* ------------------------------------------------------------------ */
/* PTT                                                                */
/*                                                                    */
/* The "pttInvert" flag sent by the host is XORed with PTT_ACTIVE_HIGH */
/* so leave PTT_ACTIVE_HIGH at the real hardware polarity.            */
/* ------------------------------------------------------------------ */
#ifndef PTT_PIN
#define PTT_PIN             DEF_PTT_PIN
#endif

#ifndef PTT_ACTIVE_HIGH
#define PTT_ACTIVE_HIGH     1
#endif

/* ------------------------------------------------------------------ */
/* TX inhibit input (F446 / F722; PIN_NONE = not used)                 */
/*                                                                    */
/* While the pin is at its active level PTT is held off and keying is */
/* refused.  Configured as an input with the internal pull resistor   */
/* set to the inactive level.  PB2 (BOOT1) is never touched.          */
/* ------------------------------------------------------------------ */
#ifndef INHIBIT_PIN
#define INHIBIT_PIN         DEF_INHIBIT_PIN
#endif

#ifndef INHIBIT_ACTIVE_HIGH
#define INHIBIT_ACTIVE_HIGH 1
#endif

/* ------------------------------------------------------------------ */
/* ADC input (RX audio / discriminator)                               */
/*                                                                    */
/* Any ADC1 capable pin: PA0..PA7, PB0, PB1, PC0..PC5                 */
/* ------------------------------------------------------------------ */
#ifndef ADC_PIN
#define ADC_PIN             DEF_ADC_PIN
#endif

#ifndef ADC_SAMPLE_TIME
#define ADC_SAMPLE_TIME     DEF_ADC_SAMPLE_TIME
                                            /* F105 SMPx: 0=1.5 1=7.5
                                               2=13.5 3=28.5 4=41.5
                                               5=55.5 6=71.5 7=239.5
                                               cycles.
                                               F446 / F722 SMPx: 0=3
                                               1=15 2=28 3=56 4=84
                                               5=112 6=144 7=480
                                               cycles.  Use more for
                                               high impedance sources   */
#endif

/* debug report flags RX as too hot when the ADC hits a rail, or the peak
 * (after gain) reaches this q15 level in a 5 s window */
#ifndef RX_HOT_PEAK
#define RX_HOT_PEAK         32000U
#endif

#ifndef ADC_MID
#define ADC_MID             2048            /* ADC count for 0 signal   */
#endif

/* ------------------------------------------------------------------ */
/* DAC output (TX audio / modulation)                                 */
/*                                                                    */
/* DAC_CHANNEL 1 : PA4        DAC_CHANNEL 2 : PA5                     */
/* ------------------------------------------------------------------ */
#ifndef DAC_CHANNEL
#define DAC_CHANNEL         1
#endif

#ifndef DAC_OUTPUT_BUFFER
#define DAC_OUTPUT_BUFFER   1               /* 1 = on-chip output buffer
                                               enabled (lower drive
                                               impedance, but it cannot
                                               swing rail to rail)      */
#endif

#ifndef DAC_MID
#define DAC_MID             2048            /* DAC count for 0 signal   */
#endif

/* Added to every DAC value (12-bit counts, may be negative).              */
/* A DC error on a DC-coupled modulator shifts the carrier frequency; a   */
/* strict radio may then refuse the downlink.  1 count = 0.8 mV at 3.3 V. */
#ifndef TX_DC_OFFSET
#define TX_DC_OFFSET        0
#endif

/* ------------------------------------------------------------------ */
/* TX test pattern (measurement aid, off by default)                  */
/*                                                                    */
/* TX_TEST_PATTERN 1 : while the host is transmitting (any mode, e.g. */
/* DMR downlink after a radio wakes the repeater) the DAC outputs a   */
/* fixed 4FSK test pattern instead of the host's samples.  The host   */
/* stream still paces PTT and timing.  The pattern repeats every      */
/* 2.4 s in six 400 ms steps, 5 samples (one DMR symbol) per symbol:  */
/*   1  constant +3 level          4  constant -3 level               */
/*   2  constant +1 level          5  constant 0 (centre)             */
/*   3  constant -1 level          6  alternating +3 / -3 (2400 Hz)   */
/* TX_TEST_AMPL is the +3 level in q15 (the DMR host's +3 symbol is   */
/* 1362 and the default host txLevel 75 gives 1021).  +1/-1 are a     */
/* third of it.  The output goes through TX_DAC_SHIFT / DAC_MID /     */
/* TX_DC_OFFSET like normal audio.  Compare the F105 and F446 DAC     */
/* pin (scope) or the RF (SDR / discriminator) with the same pattern. */
/* ------------------------------------------------------------------ */
#ifndef TX_TEST_PATTERN
#define TX_TEST_PATTERN     0
#endif
#ifndef TX_TEST_AMPL
#define TX_TEST_AMPL        1021
#endif

/* ------------------------------------------------------------------ */
/* LEDs (up to 8).  PIN_NONE for any that are not fitted.             */
/*                                                                    */
/*   LED0  heartbeat: 1 Hz blink while waiting for the host, short    */
/*         flash once a second after the host has configured it       */
/*   LED1  frame received from host                                   */
/*   LED2  sample frame sent to host                                  */
/*   LED3  PTT keyed                                                  */
/*   LED4  TX audio playing                                           */
/*   LED5  RX audio activity (level above threshold)                  */
/*   LED6  ADC clipping                                               */
/*   LED7  error (TX underrun / buffer overflow / UART error /        */
/*         external clock missing)                                    */
/* ------------------------------------------------------------------ */
#ifndef LED0_PIN
#define LED0_PIN            PC(0)
#endif
#ifndef LED1_PIN
#define LED1_PIN            PC(1)
#endif
#ifndef LED2_PIN
#define LED2_PIN            PC(2)
#endif
#ifndef LED3_PIN
#define LED3_PIN            PC(3)
#endif
#ifndef LED4_PIN
#define LED4_PIN            PC(4)
#endif
#ifndef LED5_PIN
#define LED5_PIN            PC(5)
#endif
#ifndef LED6_PIN
#define LED6_PIN            PC(6)
#endif
#ifndef LED7_PIN
#define LED7_PIN            PC(7)
#endif

#ifndef LED_ACTIVE_HIGH
#define LED_ACTIVE_HIGH     1
#endif

#ifndef LED_TEST_AT_BOOT
#define LED_TEST_AT_BOOT    1               /* chase pattern at power up */
#endif

/* ------------------------------------------------------------------ */
/* JTAG / SWD  (F105 only; on F4 / F7 PA15, PB3, PB4 are free as soon  */
/* as they are configured as GPIO)                                    */
/*                                                                    */
/* 1 = JTAG disabled, SWD kept.  Frees PA15, PB3 and PB4 for use as   */
/*     LEDs etc.  Programming through SWD still works.               */
/* ------------------------------------------------------------------ */
#ifndef FREE_JTAG_PINS
#define FREE_JTAG_PINS      1
#endif

/* ------------------------------------------------------------------ */
/* Levels                                                             */
/*                                                                    */
/* The host sends rxLevel (SET_CONFIG) and txLevel (every TX frame).  */
/*   gain = level / UNITY * GAIN_Q8 / 256                             */
/* Set *_USE_HOST_LEVEL 0 to ignore the host value and use GAIN_Q8    */
/* only (256 = x1.0).                                                 */
/* ------------------------------------------------------------------ */
#ifndef RX_USE_HOST_LEVEL
#define RX_USE_HOST_LEVEL   1
#endif
#ifndef RX_LEVEL_UNITY
#define RX_LEVEL_UNITY      50              /* host rxLevel for gain x1 */
#endif
#ifndef RX_GAIN_Q8
#define RX_GAIN_Q8          256
#endif

#ifndef TX_USE_HOST_LEVEL
#define TX_USE_HOST_LEVEL   1
#endif
#ifndef TX_LEVEL_UNITY
#define TX_LEVEL_UNITY      100             /* host txLevel for gain x1 */
#endif
#ifndef TX_GAIN_Q8
#define TX_GAIN_Q8          256
#endif

/* Sample scaling.  OpenMT sample values are q15 (+-32767 = full scale).
 * RX_ADC_SHIFT 4 maps the 12-bit ADC span to full q15 and TX_DAC_SHIFT 4
 * maps full q15 to the 12-bit DAC span.  Use 0 for both to treat sample
 * values as raw ADC / DAC counts (what some older firmware did).       */
#ifndef RX_ADC_SHIFT
#define RX_ADC_SHIFT        4
#endif
#ifndef TX_DAC_SHIFT
#define TX_DAC_SHIFT        4
#endif

/* Slow DC removal on the RX path (removes ADC / bias offset).  The   */
/* time constant is 2^shift samples (14 -> about 0.7 s at 24 kHz).    */
#ifndef RX_DC_BLOCK
#define RX_DC_BLOCK         1
#endif
#ifndef RX_DC_SHIFT
#define RX_DC_SHIFT         14
#endif

/* RX LED threshold, q15 units (32767 = full scale) */
#ifndef RX_ACTIVITY_LEVEL
#define RX_ACTIVITY_LEVEL   800
#endif

/* ------------------------------------------------------------------ */
/* TX buffering                                                       */
/*                                                                    */
/* TX_BUF_SAMPLES must be a power of two.  The host may send a whole  */
/* P25 LDU (4320 samples) in one burst, so 8192 is recommended on a   */
/* 32 KB RAM part.  On a 20 KB part (F105R8) use 4096.  The F446 and  */
/* F722 have plenty of RAM and default to 16384.                      */
/* ------------------------------------------------------------------ */
#ifndef TX_BUF_SAMPLES
#define TX_BUF_SAMPLES      DEF_TX_BUF_SAMPLES
#endif

#ifndef TX_PRIME_SAMPLES
/* Samples buffered before playback starts, and so the cushion against  */
/* host timing jitter.  A DMR host writes each 60 ms cycle just in time, */
/* so a small value underruns on every late scheduler tick (gaps in the  */
/* downlink).  M17 captures showed the host feeding the first second of  */
/* a transmission with almost no lead (lw= as low as 102 samples), so    */
/* 10 frames = 960 samples = 40 ms (was 5 frames, 20 ms).                */
#define TX_PRIME_SAMPLES    (SAMPLES_PER_FRAME * 10)
#endif

#ifndef TX_UNDERRUN_TIMEOUT_MS
#define TX_UNDERRUN_TIMEOUT_MS 1500         /* unkey if the host stops
                                               sending without EOT      */
#endif

/* TX/RX sample alignment report for the DMR host (type 0xB0, "TXTM").
 * Lets dmr_host place every RX sample in its TDMA cycle exactly, so the
 * timeslot of a received burst is exact instead of estimated from packet
 * arrival times.  Needs the latest Modem_Host.c, which forwards it to the
 * DMR host only; harmless otherwise.  Fine-tune with [DMR] SlotDelay. */
#ifndef TX_TIMING_REPORT
#define TX_TIMING_REPORT    1
#endif

/* After a TX underrun the audio restarts only once the silent gap is a
 * whole number of DMR TDMA cycles (1440 samples = 60 ms).  Any other gap
 * shifts the downlink against the radios' slot timing, which swaps their
 * timeslot.  Set 0 for the old restart-as-soon-as-primed behaviour. */
#ifndef TX_STALL_CYCLE_ALIGN
#define TX_STALL_CYCLE_ALIGN 1
#endif
#define TX_CYCLE_SAMPLES    1440U

/* While transmitting, repeat the report every this many TX samples
 * (7200 = 0.3 s) so RX frames lost on the way to the host cannot leave
 * the host's slot reference stale. */
#ifndef TX_TIMING_REFRESH
#define TX_TIMING_REFRESH   7200U
#endif

/* Simplex operation (duplex flag 0 from host): RX samples are sent as
 * zeros while PTT is keyed.  Duplex streams the real ADC. */

/* ------------------------------------------------------------------ */
/* Misc                                                               */
/* ------------------------------------------------------------------ */
#ifndef WATCHDOG_ENABLE
#define WATCHDOG_ENABLE     1               /* ~1 s IWDG; PTT is
                                               released on reset        */
#endif

/* Firmware identification, returned by the version query (frame type  */
/* 0xD0, see README) and announced once when the host first configures  */
/* the modem.  Bump FW_VERSION when you change the firmware.            */
#ifndef FW_NAME
#define FW_NAME             "OpenMT-SAMP-" MCU_NAME
#endif
#ifndef FW_VERSION
#define FW_VERSION          "1.5.9-uartdiag"
#endif

/* "Reboot into the ROM bootloader" command, for boards where the Pi has */
/* no BOOT0 / NRST wires.  Host frame:  61 07 D1 42 4F 4F 54  (D1,"BOOT") */
/* The modem ACKs, resets and starts the chip's built-in serial          */
/* bootloader on its normal Pi UART pins (see the README).  Default on   */
/* for the F105, off for the F446 / F722 (untested path there).          */
#ifndef BOOTLOADER_COMMAND
#define BOOTLOADER_COMMAND  DEF_BOOTLOADER_CMD
#endif

/* 1 = the firmware is built to run behind the serial loader (F105, see
 * the README, "Serial loader"): make LOADER=1 sets this.  The BOOT command
 * then restarts into the loader instead of the chip's ROM boot loader. */
#ifndef USE_LOADER
#define USE_LOADER          0
#endif
#define BOOT_MAGIC          0xB007B007UL

#ifndef DEBUG_FRAMES
#define DEBUG_FRAMES        0               /* 1 = send a DEBUG (0xC0)
                                               text frame with counters
                                               every 5 s                */
#endif

/* ------------------------------------------------------------------ */
/* Sanity checks                                                      */
/* ------------------------------------------------------------------ */
#if (TX_BUF_SAMPLES & (TX_BUF_SAMPLES - 1)) != 0
#error TX_BUF_SAMPLES must be a power of two
#endif

#if (SAMPLE_RATE_HZ != 24000)
#warning OpenMT SAMPLE mode runs at 24 kHz
#endif

#if (DAC_CHANNEL != 1) && (DAC_CHANNEL != 2)
#error DAC_CHANNEL must be 1 (PA4) or 2 (PA5)
#endif

#if (UART_NUM < 1) || (UART_NUM > MAX_UART_NUM)
#error UART_NUM out of range for this chip
#endif

/* ADC channel for ADC_PIN */
#if (ADC_PIN) <= PA(7)
#define ADC_CHANNEL         ((ADC_PIN) - PA(0))
#elif (ADC_PIN) == PB(0)
#define ADC_CHANNEL         8
#elif (ADC_PIN) == PB(1)
#define ADC_CHANNEL         9
#elif ((ADC_PIN) >= PC(0)) && ((ADC_PIN) <= PC(5))
#define ADC_CHANNEL         (10 + (ADC_PIN) - PC(0))
#else
#error ADC_PIN must be PA0-PA7, PB0, PB1 or PC0-PC5
#endif

#if (INHIBIT_PIN == PB(2))
#error PB2 is BOOT1 on these chips and cannot be the inhibit pin
#endif

#if (EXT_CLK_HZ > 0)
#define SAMPLE_TIM_N        EXT_CLK_TIM
#if (EXT_CLK_TIM < 1) || (EXT_CLK_TIM > 4)
#error EXT_CLK_TIM must be 1..4
#endif
#else
#define SAMPLE_TIM_N        INT_SAMPLE_TIM
#endif

#if (INT_SAMPLE_TIM < 2) || (INT_SAMPLE_TIM > 4)
#error INT_SAMPLE_TIM must be 2, 3 or 4
#endif

#endif /* CONFIG_H */
