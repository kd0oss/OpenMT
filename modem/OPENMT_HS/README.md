# OPENMT_HS - OpenMT bit-stream firmware for ADF7021 hotspot boards

Firmware for hotspot boards built around an STM32F103 and one or two ADF7021
transceivers (ZUMspot, MMDVM_HS_Hat, MMDVM_HS_Dual_Hat, Nano hotSPOT, ...).  It
is the OpenMT modem type **`openmths`** (shown as "OpenMT_HS" in the dashboard's
Device Type list).

No radio protocol runs on the modem.  The protocol services on the host
(DMR, D-Star, P25, M17, NXDN, YSF) build the ADF7021 register set and the sync
pattern for their mode and send them to the modem; after that the modem only
tunes, watches for the sync, and moves raw bits in both directions.  All framing,
FEC, slot timing and so on are done on the host.

## Ported from MMDVM_HS

**This firmware is a port of [MMDVM_HS](https://github.com/juribeparada/MMDVM_HS)**
by Andy Uribe CA6JAU, which in turn is based on the MMDVM firmware by
Jonathan Naylor G4KLX and on the ZUMspot work of Jim McLaughlin KI6ZUM.  As much of
the RF protocol support as possible was removed, and an OpenMT bit-stream layer was
put in its place.  The ADF7021 driver, the board definitions, the pin and interrupt
code, the STM32 support library, the ring buffers and the build system all come from
MMDVM_HS, and the copyright notices of their authors have been kept in the source
files.  Other contributors named in those files include Colin Durbridge G4EML,
Danilo DB4PLE and Florian Wolters DF2ET.  Many thanks to all of them.

What was removed: the D-Star, DMR, YSF, P25, NXDN, M17 and POCSAG receive and
transmit code, the CW ID, the calibration modes, the MMDVM serial protocol and the
modem state machine, together with the code that only they used (the per-mode
ADF7021 register builders, the deviation and calibration code, RSSI readback, the USART2
serial repeater, the UDID feature and the matching `Config.h` options).  What is new: `OpenMT.cpp` / `OpenMT.h` (the mode table, sync
search, scanning and transmit control), a new frame layer in `SerialPort.cpp`, the
bit stream functions in `IO.cpp` and `ADF7021.cpp` (`ifConfRegs()` and friends), and
the `test/` simulator.

Because the firmware no longer speaks the MMDVM protocol, **MMDVMHost and other MMDVM
programs cannot use a board that runs it**.  Flash the original MMDVM_HS firmware
again to go back.

License: GNU General Public License.  The MMDVM_HS files are "version 2 or (at your
option) any later version", as stated in their headers, and the new firmware files carry
the same notice, so the code can be used inside the version 3 OpenMT project.  The files in
`test/` are under the version 3 notice of the rest of OpenMT.

## What the firmware does

* **Registers modes.**  Each protocol service that connects to Modem_Host has a
  connection id (0 to 14).  Modem_Host passes its 58 byte mode data on with an
  `ADD_MODE` frame: ten ADF7021 registers, a sync pattern and mask, and a flag
  byte.  Up to `OPENMT_MAX_MODES` (8) modes are kept.  `SET_CONFIG` starts a new
  session and clears the table.
* **Checks the mode.**  The register numbers, the band (VHF 144-148 and 219-225 MHz,
  UHF 420-475 and 842-950 MHz) and the banned ranges (145.8-146 and 435-438 MHz,
  satellite use) are checked, the same as `SET_FREQ` in MMDVM_HS.  A mode that fails is
  refused with a NAK (error 4).  `DISABLE_FREQ_CHECK` and `DISABLE_FREQ_BAN` in
  `Config.h` switch the checks off.
* **Scans.**  When nothing is going on and two or more modes are registered, the
  modem loads each mode in turn for `OPENMT_SCAN_TIME` clock edges times the mode's
  scan multiplier.  A few milliseconds after a mode is loaded (`OPENMT_RSSI_SETTLE_MS`)
  the received signal strength is read; if it is weaker than `OPENMT_RSSI_THRESHOLD`
  (-115 dBm by default) the channel is empty and the next mode is loaded at once.  An
  idle scan cycle is then a few milliseconds per mode instead of the whole dwell, and
  once a carrier is present the full dwell is used again.  A failed readback looks like
  a strong signal, so the scan then behaves as it does without the check.
* **Receives.**  The bits of the current mode are compared with its sync pattern.  On
  a match (up to 0 errors for a 16 bit sync, 1 for 32 bits, 2 for 48 bits) the modem
  stops scanning and sends the bits to the host in `BIT_DATA` frames, starting with
  the 64 bits before and including the sync (192 bits for DMR, because its sync is in
  the middle of the burst; the DMR voice sync counts as a match as well).  That first
  frame is sent at once, without waiting for it to fill up to `OPENMT_RX_FRAME_BYTES`.
  If the host does not lock the mode, streaming ends after the mode's lock timeout
  following the last sync.  The timeout starts at `OPENMT_LOCK_TIMEOUT_MS` and is
  learned: while the host has a mode locked the modem measures the longest gap between
  two syncs, and afterwards the timeout is 2.5 times that gap, between
  `OPENMT_LOCK_MIN_MS` and `OPENMT_LOCK_TIMEOUT_MS`.  A false sync then holds up the scan
  for a fraction of a second.  The host locks the mode with `SET_MODE | connection id` when a service
  recognises a call, and releases it with `SET_MODE | 0x0F`.  While locked the modem
  streams every bit, in frames of `OPENMT_RX_FRAME_BYTES`.
* **Transmits.**  `BIT_DATA` frames from the host go into a bit buffer
  (`OPENMT_TX_BUFFER_FRAMES` frames of 192 bytes).  The transmitter keys up when
  `OPENMT_TX_START_BITS` are buffered or after `OPENMT_TX_START_TIMEOUT_MS`, and
  drops by itself when the buffer is empty.  The modem tells the host how many frames
  fit (a status frame, 0 to 15) whenever the buffer becomes full or has room again,
  and when asked.  The host paces itself with that.
* **Lights the LEDs** by modulation: GMSK shows as D-Star, 2FSK as POCSAG and
  3/4FSK as DMR.  The modem does not know which protocol a connection carries.

Only bit-stream modes work.  The FM service needs samples and cannot use this modem;
sample frames (`SAMP_DATA`) are answered with a NAK (error 1).

## Build

You need the GNU Arm Embedded toolchain (`arm-none-eabi-gcc`).

    cd modem/OPENMT_HS
    make hs         # STM32F103, no bootloader offset (SWD / stm32flash)
    make bl         # with the 8 KB bootloader offset and USB support
    make nobl       # USB build without the bootloader offset
    make clean

The result is in `bin/`: `openmths_f1.hex` and `.bin` for `hs`, `openmths_f1bl.*` for `bl`
and `openmths_f1nobl.*` for `nobl` (MMDVM_HS called these `mmdvm_f1*`).  The firmware reports
itself as `OPENMTHS <board>-<version> ...` in the version reply.  `GitVersion.h`
is made from git by the Makefile; the one in the tree holds `0000000`.  The Makefile
also still has the F4 and F7 targets of MMDVM_HS; their libraries are not part of
this tree, so only the three STM32F103 targets above can be built.

`STM32F10X_Lib/README.md` describes the support library, the bootloader and the
flash tools in `STM32F10X_Lib/utils`.  Boards are flashed the same way as with
MMDVM_HS (SWD, or `stm32flash` through the UART boot loader).

## Configuration (Config.h)

| Setting | Notes |
|---|---|
| Board | Select exactly one: `ZUMSPOT_ADF7021`, `LIBRE_KIT_ADF7021`, `MMDVM_HS_HAT_REV12`, `MMDVM_HS_DUAL_HAT_REV10` (the default), `NANO_HOTSPOT`, `NANO_DV_REV10`, `D2RG_MMDVM_HS`, `SKYBRIDGE_HS` or `LONESTAR_USB`.  The pin definitions in `configs/` are those of MMDVM_HS. |
| `DUPLEX` | Two ADF7021: receive on the second chip while the first transmits. Only with `SET_CONFIG` bit 0 set (see below). |
| TCXO | `ADF7021_14_7456` (default) or `ADF7021_12_2880`.  **It must be the same in `tools/ADF7021.h`** because the services compute the registers from it. |
| Host link | `STM32_USART1_HOST` (default), `STM32_USB_HOST` or `STM32_I2C_HOST`. |
| `OPENMT_SERIAL_SPEED` | UART speed, default 460800.  **Must equal the modem's `baud` setting** in the dashboard / database (Modem_Host creates it as 460800). |
| `OPENMT_TX_BUFFER_FRAMES` | TX buffer in host frames, default 8 (192 bytes of bits each, so 2 x 192 bytes of RAM per frame). |
| `OPENMT_TX_START_BITS`, `OPENMT_TX_START_TIMEOUT_MS` | When to key up: default 3072 bits or 350 ms. |
| `OPENMT_RX_FRAME_BYTES` | Bytes of received bits per frame, default 24 (at most 252). |
| `OPENMT_LOCK_TIMEOUT_MS` | End of streaming after the last sync when not locked by the host, default 1500. Also the upper limit of the learned timeout. |
| `OPENMT_LOCK_LEARN`, `OPENMT_LOCK_MIN_MS`, `OPENMT_LOCK_FACTOR_PERCENT` | Learn the lock timeout per mode (default on, at least 250 ms, 250 percent of the longest sync gap). |
| `OPENMT_SYNC_FLUSH` | Send the first frame at the sync, default 1. |
| `OPENMT_RSSI_SCAN`, `OPENMT_RSSI_THRESHOLD`, `OPENMT_RSSI_SETTLE_MS` | Skip empty channels while scanning (default on, -115 dBm, 2 ms). Raise the threshold number to listen to weaker signals; lower it if the noise floor of your board is above -115 dBm and the scan never skips. |
| `OPENMT_DEBUG_EVENTS` | Short debug frames, on by default (see below). |
| `OPENMT_LOOPBACK_TEST` | Diagnostic for duplex boards, off by default (see `OpenMT.h`). |

A few LED and board options inherited from MMDVM_HS (`USE_ALTERNATE_*_LEDS`,
`CONSTANT_SRV_LED`, `QUIET_MODE_LEDS`, ...) are still honoured.  The firmware banner reads
`<board>-v<version> <TCXO>MHz <chip>` followed by the build time.

## Setting up the host

In the dashboard choose Device Type **OpenMT_HS** (`modem` = `openmths` in the `config`
section), set `port` to the board's serial device and leave `baud` at 460800 unless
`OPENMT_SERIAL_SPEED` was changed.  On start Modem_Host sends
`SET_CONFIG` with the single flag byte `0x81` and, as each service connects, an
`ADD_MODE` frame for it.  The services' `rxFrequency`, `txFrequency` and `rfPower`
settings end up in the mode's registers.

The `SET_CONFIG` flags are bit 0 duplex, bit 1 RX invert, bit 2 TX invert and bit 3
PTT invert (ignored here).  Bit 7 is set by the handshake and ignored.  RX invert and
TX invert are applied to the bit streams.

## Serial protocol

Frames: `0x61`, total length (3 to 255, counting the header), type byte, payload.  The
high nibble of the type byte is the message type, the low nibble a connection id.

| Host to modem | Type | Payload |
|---|---|---|
| `GET_VERSION` | `0x00` | none; answered with protocol version 1 and the description text |
| `GET_STATUS` | `0x10` | none; answered with `0x10 \| free frames` |
| `SET_CONFIG` | `0x20` | flags byte |
| `SET_MODE` | `0x30 \| conn` | none; `0x3F` = idle, scan again |
| `ADD_MODE` | `0x40` | connection id + 58 bytes of mode data |
| `DEL_MODE` | `0x50 \| conn` | none |
| `BIT_DATA` | `0x60 \| conn` | bits to transmit, 8 per byte (192 bytes) |
| `MEAS` | `0xD0 \| conn` | none: measure the AFC error of the mode `conn` (see Calibration); or pass, half: raw readback words |

| Modem to host | Type | Payload |
|---|---|---|
| `BIT_DATA` | `0x60 \| conn` | received bits, first bit in the top bit of the first byte; optionally 2 bytes of TX position |
| `GET_STATUS` | `0x10 \| free frames` | none |
| `ACK` | `0xE0` | the type byte acknowledged |
| `NAK` | `0xF0` | the type byte, then 1 unsupported, 2 no mode, 3 bad connection, 4 bad data, 5 busy, 6 no space, 8 transmit data waiting (MEAS only) |
| `DEBUG` | `0xC0` | text and two 16 bit values |
| `MEAS` | `0xD0 \| conn` | afc0, afc1, step, rssi, syncs (each 16 bit little endian), flags; or eight 16 bit readback words |

The mode data layout (registers, sync, flags) is described at the top of `OpenMT.h`.
`BIT_DATA` from the host is not acknowledged, only refused with a NAK.

## Calibration (RX frequency offset per mode)

Crystal error moves the receive frequency, and the error differs per band.  Modem_Host
keeps one RX offset in Hz per modem and mode in the `modem_host.config` table
(parameter `rxOffset`, limit +-20000 Hz) and adds it to the RX frequency word of the
`ADD_MODE` registers, so the firmware needs no change to use it.

To measure, the host sends `MEAS`.  The firmware (refused with NAK 5 when the transmitter is keyed and NAK 8 while transmit data is queued; a mode the host has locked is
measured too, the stream pauses for about 150 ms and the modem returns to the locked mode, so the reference can
be a radio sending frames of any mode) loads the mode with the ADF7021 internal AFC switched off.  With the AFC off the AFC readback is the
intermediate frequency itself (datasheet, Readback Format): `Hz = word * DEMOD_CLK / 2^18`, DEMOD_CLK =
XTAL / DEMOD_CLK_DIVIDE (register 3 bits 6 to 9), and 100 kHz when the receiver is exactly on the signal.  The
firmware reads it twice, `OPENMT_MEAS_SETTLE_MS` apart, with the RSSI, and answers with both words and the scale
(Hz per count in 1/64 Hz).  The host averages the words; the error is the average minus 100 kHz, positive when the
signal is above the receive frequency.  A carrier from a reference transmitter on the RX frequency, about -50 to
-100 dBm, is needed, and the readback is only valid while a FSK signal is received.

Use `tools/calibrate/openmt_cal` (`list`, `measure`, `auto`, `manual`, `raw`, `set`, `reset`) or
the dashboard **Calibration** tab.  `openmt_cal raw <mode>` and `openmt_cal sweep <mode>` are diagnostics: they
print the words register 7 can select, and the AFC word at several frequency offsets with the AFC off and on.
Results more than 25 kHz from the signal, or readings that disagree by more than 1 kHz, are refused.

## Debug events

With `OPENMT_DEBUG_EVENTS` the modem sends a short `DEBUG` frame for the events that
matter when a protocol is brought up; Modem_Host prints them as "Modem rec data":
the build stamp and `ADD conn,PA` when a mode is added (`PA=0` means the transmitter
power in the mode is zero), `RX stream on/off`, `TX key, bits,waited ms`,
`TX end, ms on air`, `RX syncs seen while TX`, `RX near-miss sync` (a sync that was
only a few bits off) and `HANG GUARD tripped` (a wait on the ADF7021 clock was
abandoned), `RSSI carrier, conn,-dBm` (at most once a second, when a scan step finds a signal) and
`lock timeout learned, conn,ms`.  Set the option to 0 for a quiet modem.

## Testing

`test/run_sim.sh` builds the firmware logic for a PC and checks it against the real
Modem_Host.  The simulator (`test/hs_sim.cpp`) is built from the real `OpenMT.cpp`,
`SerialPort.cpp`, `IO.cpp`, `ADF7021.cpp`, `BitRB.cpp` and `Utils.cpp`; only the pins,
the delays and the UART are replaced, and the ADF7021 clock interrupt runs in a
thread.  `test/hs_test.py` plays the protocol services and checks mode registration
(and the refusal of a banned frequency), reception with the sync history for a 32 bit
sync and for DMR, transmission (every bit, in order, one key up), and the flow control
with frames sent in a hurry.  It also checks the first frame at a sync, the learned lock
timeout and the skipping of empty channels (the simulator takes the signal strength from
its control socket).  The `run_sim.sh` scripts of the protocol hosts also run
each host against a simulated `openmths` modem.

The simulator does not replace the bench: the RF side and the timing on the STM32 are
not simulated.  Check a new build on the air with a dummy load or a monitor receiver first.

## Files

| File | Origin |
|---|---|
| `OpenMT.cpp`, `OpenMT.h` | New: the OpenMT mode table, sync search, scanning and TX control |
| `SerialPort.cpp`, `SerialPort.h` | MMDVM_HS, frame layer replaced |
| `IO.cpp`, `IO.h`, `ADF7021.cpp`, `ADF7021.h` | MMDVM_HS, bit stream functions added |
| `IOSTM.cpp`, `SerialSTM.cpp`, `I2CHost.*` | MMDVM_HS, STM32F103 pins, UART, USB and I2C |
| `BitRB.*`, `Utils.*`, `Debug.h`, `Globals.h`, `version.h` | MMDVM_HS |
| `Config.h`, `configs/` | MMDVM_HS board selection and pin definitions, OpenMT options added |
| `Makefile`, linker scripts, `STM32F10X_Lib/` | MMDVM_HS build system and support library |
| `test/` | New: PC simulator and tests |
