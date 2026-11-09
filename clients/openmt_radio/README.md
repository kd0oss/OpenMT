# OpenMT Radio

A Qt desktop program that acts as a radio on an OpenMT modem. It is a client of Modem_Host (like
`nxdn_host`, `dmr_host`, ...): it registers a mode on TCP port 18000 + modem - 1 and exchanges 24 kHz
sample packets with the modem. Speech comes from the computer's microphone and goes to its speaker; an
AMBE3000R dongle (USB serial) does the vocoding.

## Status

| Mode   | State |
|--------|-------|
| NXDN   | done: transmit and receive, verified in a loopback test against the repo's own decoder |
| DMR    | done: direct (simplex) mode, and repeater / duplex hotspot mode (slot timed, wake-up, receives while keyed); both checked against the repo's own `dmr_host` (see below) |
| P25    | done in direct (simplex) mode: C4FM, IMBE, NAC / radio id / talk group or private call; checked against the repo's own `p25_host` (see below). Speech is coded in software (the chip has no IMBE); not yet tried on air |
| YSF    | done for V/D mode 2 (AMBE+2 speech): C4FM 9600 bit/s, callsigns, DG-ID; checked against the repo's own `ysf_host` (see below); not yet tried on air |
| D-STAR | done (AMBE speech, header, text message): GMSK 4800 bit/s; checked against the repo's own `dstar_host` (see below); not yet tried on air; the chip's D-STAR rate setting is not yet verified on hardware |
| M17 | done for unencrypted voice (Codec2 3200): 4FSK 4800 baud, callsigns, CAN, late entry; checked against the repo's own `m17_host` (see below); not yet tried on air. No dongle is used; Codec2 comes from `libcodec2` |
| FM | done: voice on a repeater with CTCSS (tone sent with the speech, tone squelch and noise squelch on receive); checked against the repo's own `fm_host`, as a radio and a repeater (see below); not yet tried on air. No dongle |

Not tested here: the Qt window (no Qt development files were available) and a real AMBE3000R dongle or
Modem_Host. The radio core (`core/`) is plain C++ and is tested with `core_test`.

### DMR

The radio works like a handheld in direct mode: a 27.5 ms burst and fill every 60 ms, MS sourced sync, a run of 0x5F
bytes before the call, a voice LC header, voice bursts A-F with the embedded link control, and a terminator with LC.
The burst coding is the repo's `dmr_func.c`; the pulse shape and levels are `dmr_host`'s. It is meant for a simplex
hotspot (for example the OpenMT modem running `dmr_host` in simplex mode): set the same colour code on both. The
time slot does not matter in direct mode, so there is none to choose. Each voice burst carries three 20 ms frames
from the AMBE3000R, and the block layout of the chip (coding order or as on the air) is detected on connecting.

#### Repeater or duplex hotspot

Choose "Repeater or duplex hotspot" under Operation and a time slot. The modem has to be duplex (Modem_Host says so
in the log; with a simplex modem the receiver is given silence while the radio transmits). The radio then behaves like a
subscriber radio of a repeater:

* It receives the continuous two slot downlink (base station sync, CACH) and decodes the call in its own slot only; the
  slot of each burst comes from the TC bit in the CACH, voted over many bursts. It keeps receiving while it transmits.
* If it hears no downlink when you press Push To Talk it wakes the repeater with the wake-up CSBK (BS outbound
  activation), repeated until the downlink appears. `dmr_host` ignores everything else while its repeater is not keyed.
* Its transmission is slot timed: nothing but a burst of 27.5 ms in each 60 ms, in its slot, placed from the downlink
  (the sync of its burst 31 samples after the sync of the downlink burst of that slot, which is where `dmr_host`
  expects a TS1 sync at cycle sample 495, TS2 at 1215). A tone of at least 360 ms goes first, then the LC headers, the
  voice and the terminator.

How the slot timing is found depends on the modem. If it sends the TX timing report (`TXTM`, the modem firmware
`OPENMT_SAMPLE_SMT32FXXX` with `openmt_bridge`; the report goes to the connection named DMR, which is this program), the
radio knows exactly which receive sample is on the air as each transmit sample and the placement is exact. **The
`sx1255_bridge` does not send it.** Without it the radio estimates the placement from the clocks and one number you
tune, "TX slot timing": the delay from handing a sample to Modem_Host until it is heard on the receive side
(default 100 ms). Everything else (the other fixed delays of your chain) is in that one number. To set it, key up
with `dmr_host -d` running on the repeater: it prints `DMR RX slot phase: ... assigned slotN, phase says slotM` for every burst.
When the call is in the wrong slot, or the phase wanders towards the edge of a slot (a TS1 burst is expected about 54.5 ms
into the repeater's cycle unless `[DMR] SlotPhase` says otherwise), change TX slot timing in 10 ms steps and key again.
If the repeater puts a call in the other slot, the radio notices when the repeater sends the call back there (it hears
its own ids in the wrong slot), logs it, and moves its timing by half a slot (30 ms) for the next call; the first call
after connecting may therefore be in the wrong slot. This needs a repeater that repeats the call; otherwise tune the
number by hand (30 ms moves the call to the other slot).
The delay is stable within one transmission; it may differ a few ms from one key up to the next because of the bridge's
transmit priming.

`tests/run_dmr_interop.sh <build dir>` runs the mode against the real `dmr_host`: `dmr_host` transmits a call and the
radio receives it (ids, bit errors, all voice blocks), then the radio transmits a call and `dmr_host` passes it to the
gateway port (header ids, group flag, every voice burst, terminator). Then the repeater mode: `dmr_host` in duplex
sends its two slot downlink with a call in slot 1 and in slot 2 and the radio must decode its own slot and ignore the
other; the radio keys up against that downlink in a model of the modem and the position of its bursts in the cycle is
measured (with and without the TX timing report); and finally the radio and the real `dmr_host` are the two ends of an
idle repeater (wake-up, downlink, timed call), the call must reach the gateway in the radio's slot. It needs the
MariaDB headers or a stub (`SHIM=<dir with mariadb/mysql.h>`).

### P25

Phase 1 (4800 baud C4FM, 24 kHz samples), direct mode, like a mobile talking to a simplex hotspot. A call is the start sync,
a header (HDU), pairs of LDU1 / LDU2 (nine 20 ms IMBE frames each; LDU1 carries the source id and destination, LDU2 the
message indicator) and a terminator. Set the NAC (hex), your radio id, the talk group (or radio called) and group / private
in the window. The frame coding is the repo's own `protocols/P25/p25_func.c`, so what `p25_host` expects is what is sent.
Calls whose algorithm id says encrypted are reported but not decoded.

The AMBE3000R has no IMBE codec (its manual lists AMBE+2 rates only; "rate 33" is the P25 *half rate*, which is phase 2), so
P25 speech is coded in software with Pavel Yazev's fixed point IMBE vocoder from OP25 (`third_party/imbe_vocoder`, GPL v3, see
the README there). The dongle is still used for DMR, NXDN and the other AMBE modes. `audip25encode` / `audip25decode` put the
88 bit IMBE frames into the P25 frames with their error correction. IMBE is a DVSI codec: check patent and licence terms for
your use.

`tests/run_p25_interop.sh <build dir>` runs the mode against the real `p25_host`: the host transmits a call that the radio
must decode (ids and all 54 speech frames), and the radio transmits a call that the host must receive and pass to its
gateway with the same ids and speech. The same samples are decoded at 30% of the signal level or a DC offset;
noise of about 150 (rms, on a 1260 outer symbol) starts to cost speech frames.

### YSF

C4FM at 4800 baud (9600 bit/s, 24 kHz samples), V/D mode 2: the digital voice mode of Yaesu radios with the callsigns in the
short data channel. A call is a preamble, a header frame, voice frames of 100 ms (five 20 ms AMBE+2 blocks, the first 27 of the
49 bits of each sent three times) and a terminator. Set your callsign, the station called (CQCQCQ calls everyone) and the DG-ID in
the window. The coding and modulation are the repo's own `protocols/YSF/ysf_func.c` / `ysf_dsp.c`. The chip is used as for
NXDN (the 49 bits are the same, with the same block arrangement setting). V/D mode 1, data FR and voice FR calls are recognised
and logged but their speech is not decoded; there is no Wires-X, GPS or text data. The mode registers with Modem_Host as
module `YSF`, type `C4FM`, like `ysf_host`.

`tests/run_ysf_interop.sh <build dir>` runs the mode against the real `ysf_host` (built with the stubbed database): the host
transmits a call (from `ysf_sim`) that the radio must decode (callsigns, end, all 100 speech blocks), and the radio transmits
a call that the host must pass to its gateway port (10 voice frames, callsigns, every 49 bit speech block right). A group call
is reported by the host with the destination `ALL`, as MMDVMHost does.

### D-STAR

GMSK at 4800 bit/s (24 kHz samples), voice with the AMBE chip switched to its D-STAR rate (2400 bit/s speech and 1200 bit/s
error correction, as the dongle is used by D-STAR hotspots). A call is the bit sync, the header (flags, RPT2, RPT1, UR, MY and
suffix, CRC, convolutional coding, interleaving and scrambling), voice frames of 20 ms (9 bytes of voice and 3 of slow data;
the frame sync in every 21st) and the end pattern. Set your callsign (with an optional suffix), the station called (UR; CQCQCQ
calls everyone), RPT1, RPT2 and a 20 character text message in the window. **RPT1 and RPT2 must be the callsigns the host is set
to** (callsign and module B, callsign and G): the host drops a header that is addressed to other repeaters. The header coding is
cut out of `protocols/DSTAR/dstar_host.c` by `tools/gen_dstar_codec.py` (`core/dstar_codec.c`), so the radio and the host cannot
disagree. The receiver takes the sync upside down as well as the right way up, so the invert setting is not needed, follows the
bit clock through the signal, corrects the voice bits with the host's own FEC check before they go to the chip, and reports the
bit error rate of the call. Only the text message of the slow data is sent and shown; there is no GPS, data, DPRS or call sign
squelch, and a call that is joined late (no header) is not received. The mode registers with Modem_Host as module `DSTAR`, type
`GMSK`, like `dstar_host`.

`tests/run_dstar_interop.sh <build dir>` runs the mode against the real `dstar_host` (built with the stubbed database): the host
transmits a call (from `dstar_sim`, N0CALL/TEST to CQCQCQ, 42 frames) that the radio must decode (callsigns, end, all 42 speech
blocks), and the radio transmits a call that the host must pass to its gateway port (header with all four callsigns, 50 voice
frames identical and in order, end). The same samples are decoded with noise of 200 (rms, on a symbol of 841), a clock error of
1000 ppm, a DC offset, 30% of the level and the polarity reversed; at noise of 300 the header is lost.

### M17

4FSK at 4800 baud (24 kHz samples, 5 samples per symbol), speech coded in software with Codec2 3200 (two 8 byte blocks in each
40 ms stream frame); the AMBE dongle is not used. A call is the preamble, the link setup frame (source, destination,
type, CAN), stream frames (frame number, 16 bytes of speech, and a piece of the link setup in every one so a call can be joined
late) and the end frame. Set your callsign, the destination (ALL calls everyone) and the channel access number in the window.
**Codec2 is loaded when the mode starts**: install the library (`sudo apt install libcodec2-1.2` on Debian or Ubuntu) or the
mode says so and does not start. Only unencrypted voice stream calls are received and sent, plus text (below): no encryption,
no other packet data. **Receive filter**: the output of a real modem (checked on a capture from an ADF7021 modem) is already filtered, and the host's root raised cosine on top of it blurs the symbols together and turns the speech into loud noise; the default `Light` filter (two samples averaged) decodes such a capture. `Root raised cosine` is the host's own filter and tolerates more noise on a clean signal (it is the one the noise figures below were measured with). The sampling instant is refined for every frame (quarter sample steps up to two and a half samples either side of the sync word, keeping the one at which the symbols lie closest to the four levels), which took a captured call from 15 of 129 frames lost to 2. Stream frames whose number does not follow the one before are not played, so a signal too poor to decode gives silence instead of noise. The mode registers with Modem_Host as module `M17`, like `m17_host`. The receiver does not find a reversed signal by
itself (the link setup and stream syncs are each other's opposite), so use the invert setting if one is needed.

#### Text: metadata with voice and messages in packet mode

**Text with voice** (up to 52 characters, UTF-8) goes into the link setup frame of every voice transmission as metadata text: four
blocks of 13 bytes at most, one block in each superframe (6 stream frames, 240 ms) in turn, so a receiver that listens for a second
has all of it. The first byte of a block is the number of blocks as a bit field (0001, 0011, 0111, 1111) and the number of this
block as one bit; the text is padded with spaces. The text can be changed while connected (the field applies when you leave it) and
is used from the next transmission on. A received call that carries such text shows it in the "Last heard" line and in the log
(`M17: text with the call from KD0OSS: ...`) as soon as all its blocks have been heard.

**Message** sends a text message (SMS) in packet mode to the callsign in **Message to** (ALL for everyone). Type the text, then press
Enter or **Send** (connected only). The radio sends the link setup frame (packet mode, your callsign, the destination, the CAN), the
packet in frames of 25 bytes (the type byte 05 for SMS, the UTF-8 text, a zero byte, the CRC; the last byte of a frame has the end flag, and
the frame number or in the last frame the byte count) and the end of transmission. A message is at most 796 bytes of text (800 bytes in
all); four can wait to be sent, and each goes out as soon as the transmitter is free (not while the key is down). The log says when it
was sent. A received packet is put together, its CRC is checked, and a text message is written in the log (`M17: message from
KD0OSS to W1AW: ...`); other packet types are named in the log and not shown. A packet whose start was not heard, or from which a frame was lost, is dropped.

The receiver also reads the link setup frame of a call (stream or packet) and starts the call from it, which needed the sampling
instant of the first frame after the preamble to be searched wider than before: with the narrower search that frame often failed its
CRC, which a stream call never showed (the pieces of the link setup in the stream frames start it late) but a packet call, with no
such pieces, depends on. The bit error figures in the end-of-call log line used the difference between two frames' counts and now use the
count of each frame.

`tests/run_m17_interop.sh <build dir>` runs the mode against the real `m17_host` (built with the stubbed database): the host
transmits a call (from `m17_sim`, N0CALL to ALL, 12 stream frames) that the radio must decode (callsigns, end, all 24 speech
blocks), and the radio transmits a call that the host must pass to its gateway port (link setup frame with KD0OSS, ALL and
CAN 5, 10 stream frames with the speech identical and in order, end), and the radio sends a text message that the host must pass to its
gateway port as a packet-mode call (link setup frame, packet frames, end; the CRC and the text are checked, and the host's own log says it detected an SMS packet). With the root raised cosine filter the host's call is also decoded with noise of 400
(rms, on a symbol peak of about 1900), a clock error of 1000 ppm, a DC offset and 30% of the level; at noise of 600 it is lost.

### FM

Analogue FM voice for a repeater run by `fm_host` (protocols/FM), through a sample modem (OpenMT); it is not available with the
MMDVM modem, where the firmware does the repeating, as for `fm_host`. Transmit: the microphone speech is band limited to 300 Hz -
3 kHz, raised to 24 kHz, and the sub-audible CTCSS tone is added (default 88.5 Hz at 1.5% of full scale, about 1 kHz of deviation on the SX1255 bridge; the repeater
sends 3%, a level that suits its own modem); the samples are the modulation the modem puts on the carrier. The tone starts when the key goes down and runs alone
for the **key-up delay** (default 400 ms) before the speech: `fm_host` needs about that long to know the tone, and the start of
the speech is lost with less. Receive: the demodulated samples go through the DC blocker, the 300 Hz - 3 kHz band filter (which
also removes the CTCSS tone), and the **squelch**, the repeater's own code (`fm_dsp.c` is used as it is): noise (a carrier quiets
the receiver noise above 6 kHz, so it needs no tone), tone (the CTCSS tone of the repeater, in the receive tone field), both, or
open. The audio goes through a delay of 100 ms so the start that came before the squelch opened is kept and the burst of noise
when the carrier goes away is cut. A call is shown from the opening of the squelch to 300 ms after it closed. The mode registers
with Modem_Host as module `FM`, type `FM`, like `fm_host`. No EchoLink, DTMF, CW ID or timeout (those are the repeater's). **Receive level**: a discriminator can give very little, so `Automatic receive level` (on by default) brings the received speech to a steady level (gain 0.5 - 40, slow up, fast down, not changed while there is only the noise floor). **Setting the squelch**: the log shows, every 3 seconds while anything is received, the peak and rms of the samples, the noise level the squelch sees (it opens below the `Noise squelch` setting and closes at 1.75 times it) and, with a receive tone, whether the tone is detected, the strongest tone and how much of the low band it holds; a transmission logs the tone and level it sends. Set the noise squelch between the noise level with and without a carrier.

`tests/run_fm_interop.sh <build dir>` runs the mode against the real `fm_host` (built with the stubbed database, access by
CTCSS 88.5 Hz): the radio transmits 1.5 s of a 1 kHz tone as speech, `fm_host` opens on the tone and repeats it (all 1.5 s of
it came back with the repeater's own tone), and the radio then receives what the repeater transmitted, between noise as a
receiver gives without a carrier (one call, 1 kHz received, the CTCSS tone filtered off).

### Releasing the modem

Modem_Host gives all samples to one mode at a time, and keeps that mode until the client that asked for it hands it back.
If the radio closes or crashes while it holds the mode, or a message is lost, the mode stays held and no host hears anything. The
radio now hands the modem back whenever it connects and whenever it disconnects, and the `Release modem` button does it on
request. Modem_Host only acts on this when the mode it holds is the radio's own, so the other protocol hosts are never
affected; a mode held by one of them (a crashed `dmr_host`, say) can only be cleared by restarting that host.

### The dongle stays open

Opening the serial port of the AMBE3000R can reset it (DTR) and it then needs seconds to answer, so OpenMT Radio opens it once when the
program starts and keeps the port open until the program ends, whatever mode runs and whether or not that mode uses the dongle
(FM and M17 do not). Connect and Disconnect do not touch it. Pressing Connect opens it if it was not there at the start (it was
unplugged, say) or if the device or baud rate in the window was changed; a mode that needs it and cannot open it does not start.
At the end of a run the chip is put back to its usual AMBE+2 rate, so D-STAR (which changes it) does not leave it in the wrong one.

A dongle that stops answering is reset without closing the port: if a speech frame gets no answer, the radio sends filler zeros (they
finish a packet the chip may be waiting for), throws away what the chip sent meanwhile, resets the chip with its reset packet,
waits for READY, sets the rate again and repeats the frame. The log says `the dongle stopped answering: it was reset and is
answering again`. The same check, a product id request, is made each time Connect finds the port already open. A slow dongle
If the reset does not help either (the Linux log, `dmesg`, then shows `ftdi_sio ttyUSB..: usb_serial_generic_read_bulk_callback - urb
stopped: -32`: the USB serial driver has stopped reading from the dongle, and only a new open starts it again), the radio closes the
port, opens it again at the same baud rate with the same rate settings, and says `its port was closed and opened again`. That takes
a few seconds. A dongle that does this often has a USB problem: look at the cable, the power and USB autosuspend.
(`dongle decode ... ms per 20 ms frame`, over 18 ms is too slow) is a separate problem: try the other baud rate, or a different
`Write chunk` and `Gap between chunks`.

### The end of a transmission

A modem un-keys the moment it gets the end of transmission and drops what it has not sent yet; the SX1255 bridge holds about
250 ms, and the radio sends 200 ms ahead of real time, so the end of every call was being cut off. The radio now waits until
the last sample has been played, plus `txDrainMs` (250 ms), before it sends the end. This is in the link (`modem_link.cpp`),
so it applies to every mode.

### DTMF keypad

The **DTMF** button (next to the push to talk button) shows a keypad (0-9, `*`, `#`, A-D) as a panel over the window, under the button; the
button is only available while connected in a mode whose host listens for DTMF, which is **FM** only (`fm_host` decodes the
digits from the audio for the EchoLink gateway; the other hosts have no DTMF handling, and a vocoder would mangle the tones).
A digit is 100 ms of the two tones (each at the `DTMF level`, default 2.5% of full scale, about 1.5 kHz of deviation on the SX1255 bridge) and 80 ms of silence, what the repeater's decoder needs. When the key is not down, a digit
puts it down itself: the CTCSS tone alone for the key-up delay, so the repeater has opened before the first digit, then the
digits, and the key is let go after the **DTMF hang time** (default 2 s) without a digit, so a number typed at the keypad goes out as one transmission. While
you hold push to talk, the digits are added to your speech.

### Transmit and receive level

The protocol hosts take `txLevel` and `rxLevel` from the database; OpenMT Radio does not read the database and has its own
**Transmit level** and **Receive level** boxes (both default 50, as the hosts do). The transmit level is sent to Modem_Host
with the mode, which hands it to the modem with every block of samples. The receive level scales the samples coming from
Modem_Host by the level divided by 50, as the hosts do, for every mode (the noise squelch of FM sees the scaled samples).
Set them to the values the host in the database uses when the radio should behave like it.

### When a received signal is not decoded

The log says what the receiver sees after a signal has been there for a second without a call: its level, how well the
best DMR sync matches, how many bursts were rejected, and whether a colour code differs. Colour code and polarity are
the usual causes (`Invert the received signal` turns the polarity round). `Record received samples` writes everything
Modem_Host delivers to `~/openmt_rx.raw` (16 bit little endian, 24 kHz) so the signal can be looked at offline.
The receiver accepts the base station, mobile and direct mode slot 1 and 2 sync patterns.

## Build

    sudo apt install cmake g++ qt6-base-dev qt6-multimedia-dev     # or the Qt 5.15 equivalents
    cd clients/openmt_radio
    cmake -S . -B build
    cmake --build build -j
    ./build/core_test          # the core without Qt: link, framing, NXDN loopback
    ./build/openmt_radio

`cmake -DBUILD_GUI=OFF` builds only the core and its test.

## Use

1. Make sure no `nxdn_host` is running for the same modem number: this program registers as the NXDN service.
2. Enter the Modem_Host address and modem number, your RAN, unit id and the call target.
3. Enter the dongle's serial device and baud rate (460800 is the AMBE3000R default), or tick "No dongle" to test the link only.
4. Pick the sound card devices, press Connect, then hold Push To Talk (or the space bar, which works wherever the cursor is except in a text or number box). The log is cleared each time you press Connect.

The block layout is detected when connecting; set it by hand only if reception or transmission sounds wrong.

## Monitor tab

The Monitor tab is a virtual service monitor.  From the samples received from Modem_Host it shows the eye diagram, the histogram of
the symbol values, the deviation of the outer symbols (in Hz and as a share of the mode's nominal), the symbol levels and how much
they blur into each other, and a spectrum of the demodulated signal.  A clipping warning appears when the samples reach the limit
of the modem.

**Source of the eye and the deviation.**  The box at the top right of the Monitor tab chooses where the eye, the histogram, the
levels and the deviation come from:

- *IQ from the bridge*: the HackRF and SX1255 bridges can send the received channel itself.  The radio demodulates it on its own, so
  the deviation is in real Hz (no assumption about the modem's scaling), the carrier's frequency error is shown, and the eye has
  13 samples a symbol.  It is the transmitter that is measured, like a service monitor does.  Only these two bridges send IQ.
- *Modem samples*: what Modem_Host delivers, i.e. what the decoder sees (0.7 counts per Hz).  Works with every modem.
- *Automatic* (default): the IQ when the bridge sends it and there is a signal in it, otherwise the modem samples.

For P25 and DMR the monitor also looks for the frame syncs.  The sync symbols are all +3 or -3, so their mean is the outer level
without the bias that noise puts on a blind reading: with syncs found the deviation is exact (the readout says so and how many syncs
were found) and an inverted transmitter is recognised.  A reading needs a few syncs (P25 sends about 5 a second), so it settles
after a moment and jumps a little with weak signals.  Without syncs (other modes, or none in the last half second) the deviation is
the earlier blind estimate and is marked rough when the levels overlap.

With the HackRF bridge it also shows the RF spectrum of the 125 kHz around the channel with a waterfall, the channel power, the
noise floor, the signal-to-noise ratio, the frequency error of the signal and the overload state of the HackRF's converter.  The
tab asks the bridge for these over UDP (the "RF spectrum port" setting, default 18050, on the same host as Modem_Host) only while it
is shown.  The HackRF bridge answers on port 18050 and the SX1255 bridge on 18051 by default: set the port of the modem in use.
Other modems do not answer and the RF panels stay empty.

The Hz figures assume the modem scales its samples at 0.7 counts per Hz, as the protocol hosts do.  When the levels overlap
(blur above about 0.25) the deviation is only a rough figure (marked with ~).
