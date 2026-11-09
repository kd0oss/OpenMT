# hackrf-openmt-bridge

Presents a HackRF to [OpenMT](https://github.com/) as a virtual `OPENMTHS` /
`OPENMTSX` modem over a PTY, the same way `sx1255-openmt-bridge` presents the
SX1255 HAT. **No changes to `Modem_Host.c` or any protocol service** (D-Star,
DMR, YSF, P25, NXDN, M17). The bridge does the radio and the generic FM
front end; the services do their own symbol filtering, sync and FEC on the
24 kSa/s Q15 baseband it hands them.

```
HackRF --USB--> int8 IQ @ 2 Msps
   -> shift 250 kHz down -> decimate 4 -> decimate 4 -> 125 kSa/s complex
   -> dsp.c (DC removal, 12.5 kHz channel filter, FM demod, baseband AFC,
             resample 12.5k -> 24k)                    <- shared with the SX1255 bridge, unchanged
   -> Q15 SAMP_DATA frames -> PTY -> Modem_Host.c -> protocol service

protocol service -> Modem_Host.c -> PTY -> Q15 samples
   -> dsp.c (resample 24k -> 125k, FM modulate)
   -> interpolate 4 -> interpolate 4 -> (shift up) -> int8 IQ @ 2 Msps -> HackRF
```

## Status: read this first

Built and tested **in simulation only**. A software HackRF (`hw/hrf_sim.c`)
stands in for the radio, and a Python script plays `Modem_Host.c` over the
real PTY protocol. What that does and does not establish:

| Established (automated tests, all passing) | Not established (needs your hardware) |
|---|---|
| Front end: unity gain across the channel, aliases/images ≥ 63/79 dB down, shift directions correct, round trip returns the input | `hw/hrf_dev.c` has **never run against a real HackRF**. It is written from the documented libhackrf API and was only syntax-checked against an API stub |
| Front end is transparent to `dsp.c` (0.9998 correlation with feeding `dsp.c` directly) | RX/TX polarity on the air (defaults `--no-rx-invert --no-tx-invert` come from the HackRF's normal I/Q convention, not a measurement) |
| RX: 4FSK-style FM comes out as the right Q15 stream; works again after a transmission | Spectral purity of the transmitted signal (8-bit DAC, harmonics, I/Q image) |
| TX: constant-envelope FM, deviation exactly as sent, the whole transmission on the air, nothing after it | CPU load on a Raspberry Pi (42× real time measured on a 2.1 GHz x86 core; Pi not measured) |
| Half duplex ordering, USB-queue flush, carrier held through a host gap, timeout unkey, Ctrl-C while keyed | DMR (see Limitations) |

Treat the first on-air session as bring-up, not as a finished product.

## Build

```
sudo apt install build-essential cmake pkg-config libhackrf-dev
cmake -B build && cmake --build build
```

USB access: install the HackRF udev rules (the `libhackrf-dev`/`hackrf`
packages ship `53-hackrf.rules`) and put the account that runs the bridge in
`plugdev`. Check with `hackrf_info`.

Simulator build and tests (no hardware, no libhackrf needed):

```
cmake -B build-sim -DHRF_SIM=ON && cmake --build build-sim
./build-sim/test_frontend                          # filter/shift unit test
gcc -O2 -Idsp test/test_chain.c dsp/hrf_frontend.c dsp/dsp.c -lm -o /tmp/test_chain && /tmp/test_chain
python3 test/e2e.py build-sim/bridge               # needs numpy, scipy
```

## Run

```
./build/bridge --rx-freq 434000000 --tx-freq 434000000 --ppm -4.5
```

It prints the PTY it created and symlinks it at `/dev/hackrf-modem`
(`--pty-symlink` to change). Start the bridge **before** `Modem_Host`.

**Settings.** Like the other bridges, this one keeps its settings in the OpenMT
config database (module `config` of the modem, `--modem-name`, default
`modem1`): `rxFrequency`, `txFrequency`, `rxLnaGain` (0-40, steps of 8),
`rxPgaGain` (the RX VGA, 0-62, steps of 2), `txDacGain` (the TX VGA, 0-47; 0 or
less keeps the current value), `rxLevel` (RX gain: 50 is the `--rx-gain` value, 100 is twice that), `rxInvert`, `txInvert`, `freqOffsetPpm` (reference clock correction, -100 to 100, same as `--ppm`; positive when the HackRF's actual frequency is too high, see the calibration steps below; applies to RX and TX). The command line options
(`--rx-freq`, `--tx-freq`, ...) are only the defaults of settings that are not
saved yet; what is saved wins. The bridge reads them at start and again when
`Modem_Host` sends a RELOAD or HOST_UP control frame (a change in the
dashboard, or the host restarting), and retunes the receiver at once; the
transmitter uses the new values at its next key-up. The bridge publishes its
stream type (SAMPLE) there too, so there is no `modem=` type to choose.
Check with the log: `bridge: settings changed -- rx=... tx=...`.

Build without MariaDB (simulator tests only): `cmake -DHRF_DB=OFF`. The
old 18-byte `OPENMTSX` SET_CONFIG packet is still understood.

systemd, so the symlink exists when `Modem_Host` starts:

```
[Unit]
Description=HackRF OpenMT bridge
Before=modem-host.service

[Service]
ExecStart=/opt/openmt/hackrf-bridge --rx-freq 434000000 --tx-freq 434000000 --ppm -4.5
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

## Things that are different about a HackRF

**Half duplex.** It receives or transmits, never both. Keying up stops RX;
unkeying restarts it, about a third of a second later (see TX latency). While
transmitting, nothing is received. That suits a simplex hotspot and not a
duplex repeater.

**Frequency accuracy.** The reference clock is not good enough for 12.5 kHz
channels unless you correct it: tens of ppm is several kilohertz at 435 MHz.
Set `--ppm` once:

1. tune a receiver you trust (or this one in gqrx) to a signal of known frequency, with the HackRF's LO commanded to a round number *F*;
2. read where it appears, *f_shown*;
3. `ppm = (f_true − f_shown) / F × 1e6`.

(Meaning: when asked for *f* the HackRF actually tunes to *f·(1+ppm/10⁶)*, and the bridge asks it for *f/(1+ppm/10⁶)*.) The software AFC in `dsp.c` removes only small residual offsets.

**Offset tuning.** The HackRF's centre spike (ADC offset, LO leakage) would
sit in the channel. RX tunes the LO `--rx-offset` (250 kHz) below the wanted
frequency and shifts digitally; at 2 Msps that is exactly fs/8. Avoid strong
signals at 2 × the offset below your channel: the I/Q image of the HackRF
(typically 30–40 dB down) lands on the channel from there. TX defaults to no
offset (`--tx-offset 0`): a constant-envelope FM signal is not hurt by LO
leakage on its own carrier, and shifting it would place the mirror image
500 kHz away.

**TX latency.** libhackrf keeps four 262144-byte buffers in flight (65 ms each
at 2 Msps), so audio reaches the antenna roughly 0.2–0.3 s after the bridge
produces it, and the end of a transmission is flushed through that queue
before the radio switches back to RX. Normal; the protocol services do not
care. It matters if you compare timing with another modem.

**Output power and cleanliness.** The HackRF is a milliwatt-class transmitter
with an 8-bit DAC. Set power with `--tx-vga-gain` (default 20 dB); the 14 dB
amplifier is off unless you pass `--tx-amp`. Digital images in this design are
suppressed to ≥ 79 dBc in simulation, but the analogue side (harmonics, I/Q
image, LO leakage) is the HackRF's own: filter before any antenna or external
amplifier, and check what you are radiating. That is your responsibility as
the licensee; nothing in this program can check it.

**Underruns.** If the USB buffer runs dry mid-transmission the last sample is
repeated, i.e. an unmodulated carrier for the length of the gap, rather than
silence or garbage. It is counted and logged ("USB buffer underrun").

**Unkeying on EOT.** On `SAMP_EOT` / `SET_MODE idle` the bridge first plays
out the audio the host has already sent, then flushes and unkeys. (Hosts send
EOT behind their last audio frame, a couple of hundred milliseconds before it
is due on the air. The SX1255 bridge unkeys at once on EOT and drops whatever
was still queued; if you hear clipped endings there, that's where to look.)

## Options

| Option | Default | |
|---|---|---|
| `--serial S` | first HackRF | pick one of several |
| `--sample-rate HZ` | 2000000 | multiple of 500 kHz, 1–8 Msps (125 kSa/s × 4k) |
| `--rx-freq HZ`, `--tx-freq HZ` | 434000000 | |
| `--rx-offset HZ`, `--tx-offset HZ` | 250000, 0 | LO is this far *below* the frequency |
| `--ppm P` | 0 | clock error, see above |
| `--lna-gain DB` | 24 | RX IF gain, 0–40 in 8 dB steps |
| `--vga-gain DB` | 20 | RX baseband gain, 0–62 in 2 dB steps |
| `--tx-vga-gain DB` | 20 | 0–47 |
| `--rx-amp`, `--tx-amp` | off | 14 dB RF amplifier while receiving / transmitting |
| `--bias-tee` | off | antenna port power |
| `--tx-amplitude X` | 0.9 | fraction of int8 full scale; set power with the gains, not this |
| `--rx-invert`/`--no-rx-invert` | off | negate RX baseband (see below) |
| `--tx-invert`/`--no-tx-invert` | off | negate TX baseband |
| `--rx-gain M` | 0.41 | scale before Q15 at `rxLevel` 50; the Modem tab's `rxLevel` then scales it by `rxLevel/50` (the SAMPLE firmware's convention). 0.41 makes one DMR symbol step (648 Hz) about 5250 counts, the level of a capture from the real OpenMT modem that decodes well (fitted to the DMR sync pattern); the SX1255 bridge's 0.04 does not suit the HackRF's front end |
| `--tx-symbol-scale N` | 494 | divisor for incoming Q15 |
| `--deviation-hz N` | 950 | FM deviation per baseband unit |
| `--tx-prime-ms N` | 250 | audio queued before playback starts |
| `--tx-key-timeout-ms N` | 1000 | unkey after this long without TX samples |
| `--tx-key-settle-ms N` | 20 | wait after starting the transmitter |
| `--key-on-set-mode` | off | also key up on `SET_MODE` (the SX1255 bridge's behaviour); see below |
| `--pty-symlink PATH` | `/dev/hackrf-modem` | |

`--rx-gain`, `--tx-symbol-scale`, `--deviation-hz` and the Q15 conventions
belong to the 125 kSa/s chain and to what the services expect, not to the
radio, so they are the values tuned for the SX1255 bridge. The SX1255 bridge
defaults both invert flags **on** (its HAT's I/Q wiring is reversed); the
HackRF uses the ordinary convention, so they default **off**. If a decoder
finds sync but every frame fails FEC, or a known-good radio hears your
transmission with the symbols swapped, flip the flag.

**Keying: why `SET_MODE` does not key up here.** The SX1255 bridge keys the
transmitter on `SET_MODE` with a connection number, which gives it a head
start before the first sample. But the hosts do not send `SET_MODE` only when
they are about to transmit: the M17, D-Star and P25 hosts also send it when
they decode the header of a call they are *receiving*, to hold the modem for
that call (the YSF and NXDN hosts send it at the start of a transmission).
With a full-duplex radio a spurious key-up is merely a stray carrier; with a
HackRF it would stop the receiver in the middle of the call. So this bridge
keys up when the first TX audio arrives, which the 250 ms priming delay
absorbs, and ignores `SET_MODE` with a connection number. `SET_MODE` idle and
`SAMP_EOT` still unkey. Pass `--key-on-set-mode` to get the SX1255 behaviour.

### `modem=OPENMTSX`

Same 18-byte `SET_CONFIG` payload as the SX1255 bridge, so the one
`Modem_Host.c` serves both. Field mapping:

| Config key | HackRF meaning |
|---|---|
| `rxFrequency`, `txFrequency` | frequencies (0 keeps the command line's) |
| `rxLnaGain` | LNA gain, rounded down to 8 dB steps, max 40 (0 keeps the command line's) |
| `rxPgaGain` | RX VGA gain, 2 dB steps, max 62 (0 keeps the command line's) |
| `txDacGain` | TX VGA gain 0–47 dB (**0 or negative keeps the command line's**, so the SX1255's 0/−3/−6/−9 never raises power) |
| `txMixerGain`, `rxPllBw`, `txPllBw` | unused |
| `rxInvert`, `txInvert` | the invert flags |

A new RX frequency or gain is applied immediately when idle; while
transmitting it is applied when RX restarts.

## Layout

```
app/bridge.c            PTY protocol, key-up/down, TX/RX threads (framing and TX queue as in the SX1255 bridge)
dsp/dsp.[ch]            the shared FM chain, byte-identical to sx1255-openmt-bridge
dsp/hrf_frontend.[ch]   2 Msps <-> 125 kSa/s: frequency shift, decimators/interpolators
hw/hrf_dev.h            what the bridge needs from a HackRF
hw/hrf_dev.c            ... on libhackrf
hw/hrf_sim.c            ... as a software model (reads/writes .cs8 files), for tests
test/                   test_frontend.c, test_chain.c, e2e.py, stubs/libhackrf (compile check only)
```

## Limitations

- **DMR.** The SX1255 bridge sends `OPENMT_TX_TIMING` so the DMR host can tell
  timeslots apart; it relies on RX and TX sharing a sample clock. A HackRF has
  no such relation and this bridge sends no timing report. DMR through it is
  unsupported and untested.
- **No duplex.** See above.
- **One HackRF, one channel.** No scanning or multi-channel receive.
- 8-bit samples: the usable dynamic range is modest, so set the gains so the
  wanted signal sits well clear of the noise floor but a strong neighbour does
  not clip (watch for saturated samples at ±127).

## Setting the RX level (HackRF gains)

Start with `--rx-stats` and key the radio: the log shows the level of the HackRF's 8-bit samples every
2 seconds. With the signal present you want an rms of roughly 15-40 and no clipping; an rms near 90 with tens of
percent clipped means the HackRF is overloaded, and the demodulated signal is full of clicks and cannot decode.
Lower `rxLnaGain` (0 is the lowest; steps of 8) and `rxPgaGain` (steps of 2) on the Modem tab, or in the config
table. **Once a setting is saved in the database the command line (`--lna-gain`, ...) no longer changes it**; the
log line `bridge: RX gains lna=.. vga=..` at start and `settings changed -- ... lna=.. vga=..` after a reload show
the values in use. If the gains are at 0 and it still clips, the signal is simply too strong for a receiver
this close to a transmitter: move the antenna away or put an attenuator in line. Do not put the HackRF
on a transmitter's antenna connector.

## RF spectrum for monitors

The bridge can serve the spectrum of the received channel to a monitor program, for example the Monitor tab of OpenMT Radio.
It is a side channel over UDP and does not touch Modem_Host or the other services.  The server is `tools/rf_spectrum.c`, shared with the SX1255 bridge (which uses port 18051 by default).  On this bridge the frequency axis is turned round when RX invert is on, because that is what negating the demodulated signal does (the HackRF's I/Q has the ordinary convention).  The SX1255 bridge does not do that: its spectrum is not tied to RX invert (`--spectrum-flip` turns it round).

- `--spectrum-port N` (default 18050, 0 = off) and `--spectrum-bind ADDR` (default: all interfaces).
- A monitor sends the 4 bytes `SPEC` at least every 5 seconds; the bridge answers with about 8 frames a second of 1024 bins over
  the 125 kHz around the channel, plus the HackRF's converter state (peak, share of samples at full scale, overload flag).
  Nothing is computed or sent while no monitor asks.
- The frame layout is described in `tools/rf_spectrum.h`.  The spectrum is that of the baseband the DSP chain sees (the channel
  shifted to 0 Hz, 125 kHz wide), not of the full 2 MHz the HackRF receives.
- The port has no password: it only gives out the spectrum, but bind it to a trusted address (`--spectrum-bind`) on an exposed network.
- Tests: `test_spectrum` and `test_iq` (the server alone) and `test/e2e.py` (the bridge with the software HackRF).

### IQ of the channel

On the same port the monitor can ask for the channel itself with the request `IQST` (every second, like `SPEC`): the 125 kS/s
baseband is low passed at 22 kHz and decimated by 2, and sent as about 200 datagrams a second of 300 complex 16 bit samples
(62.5 kS/s, about 250 KB/s, only while a monitor asks).  Each frame has a gain shift (a weak signal is scaled up before it is made
16 bit; the monitor divides it out), a sequence number so losses show, and the converter's overload flag.  The frequency axis is the
true one (RX invert is taken out, as for the spectrum).  The layout is in `tools/rf_spectrum.h`.  OpenMT Radio's Monitor tab uses
it for the true deviation in Hz, the carrier's frequency error and an eye diagram at 13 samples a symbol.  The SX1255 bridge serves
the same, from the same code.
