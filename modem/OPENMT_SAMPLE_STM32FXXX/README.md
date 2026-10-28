# OpenMT SAMPLE-mode firmware for STM32F105, F446 and F722

Firmware for an STM32F105, STM32F446 or STM32F722 based MMDVM-style modem
on a Raspberry Pi GPIO board.  One source tree, one `config.h`; the chip
is picked with `make MCU=f105`, `f446` or `f722`.  It streams 24 kHz, 16-bit (q15) samples between the Pi and the
modem's ADC / DAC using the OpenMT serial protocol, and drives PTT and
8 status LEDs.  Set the modem type to `openmt` on the Pi.

No radio protocol runs on the modem; everything (DMR, P25, ...) is decoded
and encoded on the Pi.

## Build

    export PATH=/path/to/arm-none-eabi-gcc/bin:$PATH
    make                         # F105RB: 128K flash / 32K RAM (default)
    make MCU=f446                # F446RE: 512K flash / 128K RAM
    make MCU=f722                # F722RE: 512K flash / 192K RAM (SRAM1+2)
    make MCU=f446 FLASH_KB=256   # F446RC
    make FLASH_KB=64 RAM_KB=20 CFLAGS_EXTRA="-DTX_BUF_SAMPLES=4096"   # F105R8
    make APP_OFFSET=0x2000       # if a bootloader occupies the first 8 KB

Output: `build/<mcu>/openmt_sample_<mcu>.bin` and `.hex` (5-7 KB of flash).
Flash with SWD (ST-Link / OpenOCD) or the UART boot loader (BOOT0 high).
`make clean` removes every chip's build directory.

| | F105 | F446 | F722 |
|---|---|---|---|
| Core | Cortex-M3 | Cortex-M4 | Cortex-M7 |
| Max SYSCLK | 72 MHz | 180 MHz (over-drive) | 216 MHz (over-drive) |
| Crystal | 3-25 MHz | 4-26 MHz | 4-26 MHz |
| HSI (no crystal) | 36 MHz via PLL | 16 MHz RC through the PLL search | same |
| UARTs | USART1-3 | USART1-3, UART4-5, USART6 | + UART7-8 |
| Default PTT / ADC | PA8 / PA1 | PB12 / PB0 | PB12 / PB0 |
| TX buffer default | 8192 samples | 16384 | 16384 |
| ADC sample time default | 55.5 cycles | 84 cycles | 84 cycles |
| RAM used | 32 KB part | 128 KB | SRAM1+SRAM2 (DTCM unused: DMA cannot reach it) |

The F4/F7 pin and DMA assignments follow the reference manuals, and the
register code was checked field by field against ST's CMSIS headers, but
like the F105 layer it has **not been run on real silicon**.  Expect to
check pins, clocks and the UART on the first board.

Every setting is in **`config.h`** (or can be passed with `CFLAGS_EXTRA`).
Pins are written `PA(8)`, `PC(0)` and so on; `PIN_NONE` means "not fitted".

## Configuration

| Setting | What it does |
|---|---|
| `PTT_PIN`, `PTT_ACTIVE_HIGH` | PTT output and its real polarity |
| `INHIBIT_PIN`, `INHIBIT_ACTIVE_HIGH` | F446 / F722: TX inhibit input (default PB13 on the F446, none on the F722). While it is at its active level PTT is held off and keying is refused. Internal pull resistor to the inactive level, so a floating pin never blocks TX. `PIN_NONE` disables it. PB2 (BOOT1) is never touched |
| `ADC_PIN` | RX audio input. Any of PA0-PA7, PB0, PB1, PC0-PC5 (ADC1) |
| `ADC_SAMPLE_TIME` | per chip: F105 0..7 = 1.5 .. 239.5 cycles, F446 / F722 0..7 = 3 .. 480 cycles (see config.h) |
| `DAC_CHANNEL` | TX audio output. 1 = PA4, 2 = PA5 (fixed by the chip) |
| `ADC_MID`, `DAC_MID` | Counts that mean "zero signal" (default 2048) |
| `LED0_PIN` ... `LED7_PIN`, `LED_ACTIVE_HIGH` | 8 LEDs, any pins |
| `UART_NUM`, `UART_REMAP`, `UART_BAUD` | Link to the Pi (default USART1 PA9/PA10, 921600). F446 / F722 also USART6, UART4, UART5 (UART7 / UART8 on the F722); pins listed in config.h |
| `CLOCK_SOURCE` | `CLOCK_SOURCE_HSE` (crystal) or `CLOCK_SOURCE_HSI` (no crystal) |
| `HSE_HZ`, `HSE_BYPASS` | Crystal frequency (F105 3-25 MHz, F4/F7 4-26 MHz), or a TCXO / clock on OSC_IN |
| `EXT_CLK_HZ` | 0 = sample interrupt from the MCU clock. Otherwise the frequency of an external clock that drives the sample interrupt |
| `EXT_CLK_TIM`, `EXT_CLK_TIM2_REMAP`, `EXT_CLK_INVERT`, `EXT_CLK_FILTER`, `EXT_CLK_FALLBACK` | Which timer's ETR pin takes the external clock (TIM1 PA12, TIM2 PA0 or PA15, TIM3 PD2, TIM4 PE0), edge, input filter, and fallback to the internal clock if the external one is missing |
| `RX_*`, `TX_*` levels | Gain mapping, DC blocker (see below) |
| `FREE_JTAG_PINS` | F105: 1 = JTAG off, SWD kept; frees PA15, PB3, PB4. (On F4/F7 those pins are free once configured as GPIO) |

### Clocks

* **Crystal:** set `HSE_HZ`.  At start-up the firmware searches the PLL
  dividers for the fastest SYSCLK (at most 72 MHz) with an accurate
  921600 baud rate and, when no external clock is used, an accurate
  24 kHz sample rate.  Any crystal from 3 to 25 MHz works.  Examples
  (checked in `tests/`): 8 MHz gives 72 MHz, 12 MHz gives 72 MHz,
  12.288 MHz gives 55.296 MHz (exactly 24000 Hz), 14.7456 MHz gives
  66.36 MHz (23998 Hz), 25 MHz gives 56.25 MHz.
* **F446 / F722:** the same idea with the F4/F7 PLL (f = HSE / M x N / P).
  The search keeps the PLL input in 1-2 MHz (2 MHz where possible), the
  VCO in range, the APB clocks within their limits (over-drive limits
  only when SYSCLK is above 168 MHz on the F446 and above 180 MHz on the
  F722) and picks the fastest SYSCLK whose UART baud and 24 kHz timer
  errors are within 2000 ppm of the best.  Examples from the tests: 8 MHz
  gives 179 MHz on the F446 (baud 922680, 0.1% off) and 214 MHz on the
  F722; 14.7456 MHz gives 213 MHz with exactly 921600 baud on the F722;
  26 MHz uses M13 (2 MHz) on both.  Above 168 MHz (F446) / 180 MHz (F722)
  the firmware enables over-drive first.
* **No crystal:** `CLOCK_SOURCE_HSI` runs at 36 MHz from the internal
  oscillator (F105) or through the same PLL search from the 16 MHz RC
  oscillator (F446 / F722).  It is only about 1% accurate, so the sample rate and baud
  rate move with it.  A crystal or an external sample clock is better.
* **External sample clock:** set `EXT_CLK_HZ` to its frequency, e.g.
  `-DEXT_CLK_HZ=12288000`.  The timer divides it by `EXT_CLK_HZ / 24000`
  (12.288 MHz divides by 512, 12 MHz by 500, 24 MHz by 1000 with the
  input prescaler). The ADC and DAC are then locked to that clock.  If the clock does
  not toggle for 250 ms, the firmware falls back to the internal timer and
  LED7 stays on.  If the MCU itself has no crystal, use `HSI` for the
  MCU and the external clock for the samples.

## Raspberry Pi side

24 kHz x 16 bit is 48 kbyte/s, which does **not** fit in 460800 baud, the
host default.  Set the modem config to:

    modem   = openmt
    baud    = 921600
    mode    = simplex | duplex
    rxInvert / txInvert / pttInvert = true | false
    rxLevel = 50      (host default; see levels)

Use the PL011 UART (`/dev/ttyAMA0`) with Bluetooth moved off it.

## LEDs

| LED | Meaning |
|---|---|
| 0 | Heartbeat. Even 1 Hz blink = waiting for host, short flash each second = host linked |
| 1 | Frame received from host |
| 2 | Sample frame sent to host |
| 3 | PTT keyed |
| 4 | TX audio playing |
| 5 | RX audio activity |
| 6 | ADC clipping |
| 7 | Error: TX underrun, TX buffer overflow, UART error, RX data dropped, external clock missing |

A chase pattern runs at power-up (`LED_TEST_AT_BOOT`).

## F446 / F722 notes

* **Board defaults** (F446): DAC PA4, ADC PB0, PTT PB12, inhibit PB13,
  BOOT1 PB2 left alone.  Crystal, UART, LEDs and the external clock
  default to the F105 values (8 MHz, USART1, PC0-PC7, none): set them in
  `config.h` for your board.  The F722 defaults (PTT PB12, ADC PB0) are
  a guess until the Zum board's wiring is known.
* The sample timer, ADC and DAC work the same way as on the F105 (ADC1
  software start, DAC channel 1 = PA4 / 2 = PA5, TIM1-4 with ETR for the
  external clock).  External clock pins are the same chips' ETR pins:
  TIM1 PA12, TIM2 PA0 or PA15, TIM3 PD2, TIM4 PE0.
* UART RX uses circular DMA, TX normal DMA, as on the F105.  The F446 /
  F722 stream and channel for each UART comes from the DMA request map
  (e.g. USART1: DMA2 RX stream 2 / TX stream 7, channel 4).
* PTT is configured before the clock start-up so it never floats while
  the crystal starts, and the PTT write is a critical section against
  the sample interrupt (this also fixed a rare race in the F105 build in
  which PTT could stay on after a transmission).

## Firmware version

`FW_NAME` and `FW_VERSION` in `config.h` identify the build.  The firmware
reports them, with the build date, as a DEBUG (0xC0) text frame, for example
`OpenMT-SAMP-F446 v1.2.0 Oct  6 2026` (the name follows the chip):

* automatically, once, when the host first configures the modem (every
  host start, and again after a link timeout), and
* on request: send `61 03 D0`.

`Modem_Host` logs it as `Modem rec data::` with the text in the hex dump.
Frame type 0xD0 is used because `0x00` is already the host's SAMP_EOT.
To query by hand with the host stopped (the host owns the port otherwise):

    stty -F /dev/ttyAMA0 921600 raw -echo
    printf '\x61\x03\xd0' > /dev/ttyAMA0 & timeout 1 cat /dev/ttyAMA0 | strings

## Updating the firmware over the serial port

The chip's built-in ROM boot loader (ST AN3155) accepts a new image over a
USART: 8 data bits, even parity, 1 stop bit, auto-baud (use 115200).
`tools/openmt_flash.sh` does it from the Pi with `stm32flash`
(`sudo apt install stm32flash`); stop the OpenMT / MMDVM host first, it
owns the port.  Pi header pins 8 / 10 (GPIO14 / 15, `/dev/ttyAMA0`) are the
UART on all boards.

| Chip | How the boot loader is started | Command |
|---|---|---|
| F105 (USART1, PA9/PA10; no BOOT0 / NRST wires) | software: the firmware reboots itself into the boot loader when it gets the frame `61 07 D1 42 4F 4F 54` (`D1` + "BOOT"; ACKed first) | `tools/openmt_flash.sh -m soft openmt_sample_f105.bin` |
| F446 (Pi pin 38 = GPIO20 = BOOT0, pin 40 = GPIO21 = NRST) | the script drives BOOT0 high and pulses NRST, flashes, then BOOT0 low and pulses NRST again | `tools/openmt_flash.sh -m gpio openmt_sample_f446.bin` |

Notes:
* The boot loader only listens on fixed pins (F105: USART1 PA9/PA10 or
  USART2 remapped PD5/PD6; F446 / F722: USART1 PA9/PA10, USART3
  PB10/PB11 or PC10/PC11, see AN2606), so the Pi link must be on one of
  them.  With `UART_NUM 1` (the default) the F105 is fine.
* F446: BOOT1 (PB2) must be low at reset, which is why the firmware never
  touches PB2.
* `BOOTLOADER_COMMAND` (config.h) enables the software command.  It is on
  for the F105 and off for the F446 / F722, which use the GPIO method.  The
  command is a protocol extension: frame type 0xD, low nibble 1 = boot,
  0 = version query.  A wrong key is answered with a NAK.
* The firmware does not need to be healthy for the GPIO method to work, but
  the software command only works if the running firmware is 1.3.0 or
  newer.  The first time an F105 gets this firmware it must be flashed with
  SWD or the BOOT0 pin.
* `-a ADDR` flashes at an offset (for example `0x08002000` with
  `make APP_OFFSET=0x2000`).  `-n` shows what would be done without
  touching anything.
* On the Raspberry Pi the script drives the GPIO pins with `pinctrl`
  (Raspberry Pi OS Bookworm) or `raspi-gpio`.
* On the F105 the software jump into the ROM boot loader did **not** work on
  the board it was tried on (the chip is a genuine F105 rev Z, the Pi's
  0x7F reaches PA10, but the ROM boot loader never answers).  Use the serial
  loader below instead.

## Serial loader (F105 without BOOT0 / NRST wires)

A small loader of our own speaks the same protocol as the ST boot loader
(AN3155: the 0x7F handshake with auto-baud, Get, Get Version, Get ID, Read
Memory, Write Memory, Erase 0x43, Go), so `stm32flash` and
`tools/openmt_flash.sh` work unchanged.  It lives in `loader/`.

Flash layout (2 KB pages):

| Address | Content |
|---|---|
| `0x08000000` | loader code (4 KB, pages 0 and 1) |
| `0x08001000` | marker page (page 2): first half word `FFFF` = application complete, `0000` = update in progress |
| `0x08001800` | application (page 3 and up) |

The last 16 bytes of RAM carry the "start the loader" flag across a reset,
so the application's RAM is 16 bytes smaller in every build.

**One-time setup (SWD / ST-Link)**

    make LOADER=1                     # builds the loader and the application
    # flash build/f105/openmt_sample_f105_full.bin at 0x08000000

(`make LOADER=1` also writes `openmt_sample_f105_full.hex`.)  Optional status LED
for the loader, for example on PB9: `make LOADER=1 LOADER_LED_PORT=1
LOADER_LED_NUM=9` (port A=0, B=1, C=2 ...; active high).  It is **on while the
loader waits for the host and goes off as soon as the host's 0x7F has been
recognised**, which shows how far a failed update got.  If the loader
is over 4 KB the link fails; `make -C loader` shows its size.

Diagnosis: `LOADER_DEBUG=1` makes the loader report every received byte that is
not a clean 0x7F: three bytes on the UART (the byte, the USART status register,
0xA6) and LED blinks (1 = framing error, 2 = parity error, 3 = overrun,
4 = a good byte with another value).  While waiting, the debug build also sends a 0x55 beacon about every 33 ms while waiting (only with LD_FIXED_BAUD set), and in auto-baud mode two bytes with the measured BRR once the host has been heard.  Leave it at 0 for normal use.

**Every later update, from the Pi (no wires needed)**

    tools/openmt_flash.sh -m soft -L -p /dev/ttyAMA0 build/f105/openmt_sample_f105.bin

`-L` selects the loader address (`0x08001800`) and erases only as many pages
as the image needs.  Use the application `.bin`, not the `_full` one.  The
script sends the `BOOT` frame; the firmware restarts into the loader, which
measures the baud rate from the 0x7F, accepts the new image and, on the Go
command at the end, marks it complete and starts it.

How it behaves:
* Boot: if the flag is set, or the marker is not `FFFF`, or the vector table
  at `0x08001800` is not plausible, the loader stays in update mode.
  Otherwise it jumps straight to the application, with every peripheral
  still in its reset state.
* The marker is set to `0000` by the first Erase and cleared again only by the
  final Go.  So a failed or interrupted update leaves the loader waiting for
  a new image after a reset or power cycle: the board cannot be bricked
  that way, and the loader code itself is never written or erased.
* **The `-g` (Go) step is what commits the image.**  Do not flash without it.
  `openmt_flash.sh` always includes it.
* Pages past the end of this chip's flash in an Erase command are skipped
  (stm32flash assumes the largest chip of the family).
* The loader runs on the internal oscillator and measures the baud rate from
  the host's 0x7F (8 bit times counted on the clock the USART itself runs
  from, so the real oscillator frequency does not matter; this board's
  measured about 25 % below 8 MHz).  Use `stm32flash -b 115200`.
  `LD_FIXED_BAUD` (top of `loader/loader_hw.c`) selects a fixed rate instead,
  which assumes an 8 MHz oscillator.
* If the loader is started but no host sends the 0x7F within about 10 s and a
  complete application is present, it restarts the application.
* `FLASH_KB` / `RAM_KB` must be the same when building the loader and the
  application (`make LOADER=1 FLASH_KB=256 RAM_KB=64` handles both).
* The loader has been tested on a PC against the real `stm32flash` (write,
  verify and Go through a simulated chip, see `tests/loader_sim.c`) but has
  **not yet been run on real silicon**: the baud-rate measurement, the flash
  programming and the reset hand-over are what to watch on the first try.

## Behaviour

* RX samples stream to the host (96 per frame, 4 ms) after the host's
  `SET_CONFIG`.  In simplex mode zeros are sent while PTT is keyed.
* TX: PTT keys on the first sample frame.  Playback starts once 192
  samples are buffered.  At `SAMP_EOT` the buffered audio is played out
  and then PTT drops.  If the host disappears without EOT, PTT drops
  after `TX_UNDERRUN_TIMEOUT_MS` (1.5 s).  The independent watchdog resets
  the MCU (releasing PTT) if the firmware ever hangs.
* The TX buffer holds 8192 samples (341 ms), enough for a whole P25 LDU
  burst.  `GET_STATUS` returns the free space in frames (0-15); overflow
  returns a NAK with error 6.
* The PTT pin is high-impedance until the MCU starts; fit a pull
  resistor to the inactive level.

## DMR slot timing

With `TX_TIMING_REPORT 1` (default) the firmware sends the DMR host a
`TXTM` frame (type 0xB0) each time a downlink starts playing: which RX
sample was being captured when TX sample 0 went on air.  The host then
places every received burst in its TDMA cycle exactly, so the timeslot
no longer depends on packet arrival jitter and `[DMR] SlotPhase` is not
used.  Requires the Modem_Host.c that forwards `OPENMT_TX_TIMING`.  If
slots are still swapped, shift the windows with `[DMR] SlotDelay`
(samples; 720 swaps slot 1 and 2, small values nudge).

## Matching an older CubeMX / HAL firmware

Older firmware treated sample values as raw ADC/DAC counts (no x16 / /16
scaling).  To get identical levels set `RX_ADC_SHIFT 0`, `TX_DAC_SHIFT 0`,
`RX_LEVEL_UNITY 256`, `TX_LEVEL_UNITY 256`, `RX_DC_BLOCK 0` and
`RX_ACTIVITY_LEVEL 50` (checked by `make -C tests legacy`).

## Things to check on the bench

1. **Levels.** The firmware scales RX by `rxLevel / 50` and TX by
   `txLevel / 100` (`*_USE_HOST_LEVEL`, `*_LEVEL_UNITY`, `*_GAIN_Q8`).
   That mapping is an assumption; if your signal is too hot or cold, set
   `TX_USE_HOST_LEVEL 0` and tune `TX_GAIN_Q8`, or change the unity points.
2. **DC offset.** A slow DC blocker (about 0.7 s) is on; turn it off with
   `RX_DC_BLOCK 0` if your host side already removes DC.
3. **Idle noise.** If the ADC input is not biased to mid-rail,
   change `ADC_MID`.

## Tests

`make -C tests loader` runs the loader protocol tests (refused writes and
erases, line errors, commit and invalidate of the marker, read-back).
`make -C tests run` runs the protocol, RX stream, TX/PTT/EOT, overflow,
resync, simplex and clock-divider maths on a PC with a simulated sample
interrupt.  The hardware layers (`hw_f1.c`, `hw_f4f7.c`) have been compiled for many
configurations on all three chips, but have not been run on real silicon.

## Files

| File | Purpose |
|---|---|
| `config.h` | all pins and options |
| `engine.c/.h` | OpenMT protocol, sample path, PTT logic, LEDs (portable) |
| `hw.h` | hardware interface used by the engine |
| `hw_f1.c` | STM32F105 clocks, GPIO, ADC, DAC, timer, UART + DMA, watchdog |
| `hw_f4f7.c` | the same for STM32F446 and F722 (PLL M/N/P, over-drive, GPIO alternate functions, DMA streams) |
| `clockcalc.c/.h` | PLL and timer divider calculations for all three chips (unit tested) |
| `startup.c`, `linker.ld.in`, `Makefile` | start-up, memory map, build |
| `cmsis/` | ST device headers (F105 in `cmsis/`, F446 in `cmsis/f4`, F722 in `cmsis/f7`) and the ARM CMSIS core headers (`cmsis/core`), under their own licences |
| `tests/` | PC unit tests |
| `tools/openmt_flash.sh` | Pi side script for flashing over the serial port |
| `loader/` | the F105 serial loader (`loader_core.c` protocol, `loader_hw.c` hardware and start-up, `loader.ld.in`, `Makefile`) |
