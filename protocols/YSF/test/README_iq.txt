ysf_iq: YSF (C4FM, 9600 bit/s) test signal generator / decoder for HackRF and any SDR recording

Build (from the YSF directory; fec_tools.c is the OpenMT tools/ one; nxdn_func.c and ambe3000.c are the NXDN ones,
for the dongle):
  gcc -O2 -I. -I../nxdn -I../../tools -o ysf_iq test/ysf_iq.c ysf_func.c ysf_dsp.c ysf_gen.c ../nxdn/nxdn_func.c ../nxdn/ambe3000.c ../../tools/dsp_tools.c ../../tools/fec_tools.c -lm

Generate and transmit (only on a frequency you are licensed for, low power / dummy load):
  ./ysf_iq gen -o call.cs8 --src KD0OSS --dst CQCQCQ --voice 100 --dgid 0 --repeat 5
  hackrf_transfer -t call.cs8 -f 446500000 -s 2400000 -x 0 -R

Capture and decode:
  hackrf_transfer -r cap.cs8 -f 446500000 -s 2400000 -n 24000000
  ./ysf_iq dec cap.cs8 [--offset hz | --auto] [--invert] [-v]

Loopback with no hardware:  ./ysf_iq gen -o a.cs8 && ./ysf_iq dec a.cs8

gen options: --src/--dst callsigns, --dgid n, --voice frames (V/D mode 2, 100 ms per frame, random voice bits
  with the required triple repetition), --data frames (data FR mode with valid channels), --repeat n, --gap ms,
  --offset hz, --level pct (100 = 2.7 kHz outer deviation), --low (1.35 kHz), --shape rrc|rc, --invert,
  -r rate (a multiple of 24000), --raw24 file (the 24 kHz deviation samples).
  No real vocoder is involved: the voice channels carry random bits, so a radio will decode the call and
  play noise.  The call is a header, the frames and a terminator, with the callsigns in the header and in
  frames 0 and 1 of the voice frames, as a radio sends them.

Finding the right frequency offset / polarity against a real receiver (e.g. a hotspot):
  ./ysf_iq gen --sweep 8000 --step 2000 -o sweep.cs8
  prints a table: source SWEEP000, SWEEP001, ... = offset + polarity
  hackrf_transfer -t sweep.cs8 -f <Hz> -s 2400000 -x 0 -R
  The receiver's log shows which source callsigns it decoded; use that offset (and --invert if only the
  inverted ones worked).

Decoding a gqrx (or other SDR program) recording
  gqrx saves float32 IQ at its input rate, named gqrx_DATE_TIME_FREQ_RATE_fc.raw.
  ysf_iq dec reads that as cf32 and takes the rate from the file name:
      ./ysf_iq dec --auto -v gqrx_..._fc.raw
  --auto finds the strongest narrow signal in the recording and mixes it to the centre (prints
  "carrier search: ... Hz"); or give --offset hz.  Other files: --format cs8|cs16|cf32 and -r rate.

What the decoder prints
  Each frame (-v): FICH frame type, data type, frame number, DG-ID.
  Each call: callsigns, DG-ID and data type at the start; at the end the length and the BER of the voice
  channels (the repetition code's corrected bits, counted as the host does) -- 0.00% on a clean signal.
  The summary line: frames decoded, frames with a bad FICH, sync losses, calls, the sync search statistics
  and the carrier offset that was tracked.  Then the measured outer symbol level in Hz (nominal 2.7 kHz
  at the normal setting, 1.35 kHz at low deviation; the figure reads about 3% low because of the
  decimation filter).
  A wrong polarity decodes nothing: try --invert.

Voice through an AMBE3000R dongle (ThumbDV, DVstick 30 and similar)
  The chip runs the same settings as nxdn_iq (AMBE+2 2450 + 1150 bit/s, 72 bit blocks, layout detected at start-up;
  check the dongle first with:  nxdn_iq ambe-test /dev/ttyUSB0 --baud 460800).  All the AMBE options of nxdn_iq work
  here: --baud, --layout, --ambe-rate, --ambe-index, --gain, --out-gain, --ambe-hwreset, --ambe-init, --ambe-chunk,
  --ambe-gap, --ambe-flow, --ambe-debug.
  Transmit speech (a WAV file: mono, 8000 Hz, 16 bit):
      ./ysf_iq gen --speech speech.wav --ambe /dev/ttyUSB0 -o call.cs8 --src KD0OSS --dst CQCQCQ
  Decode a recording to audio (V/D mode 2 voice):
      ./ysf_iq dec --auto --invert --ambe /dev/ttyUSB0 --wav heard.wav gqrx_..._fc.raw
  A V/D mode 2 voice channel is 104 bits: the first 27 bits sent three times each (81 bits), then 23 bits sent once.
  The 49 bits of the vocoder are taken as the first 27 followed by the next 22, in the order of the chip's 49 bit
  parameter set, and the last bit is not used.  This arrangement was worked out from a recording of a real radio's
  traffic, not checked against another program: the first positions of the 27 are the ones that stay the same from
  one 20 ms frame to the next (pitch and gain), the last bit is nearly always 0.  If the decoded audio is garbled,
  "dec --dump-vch file" writes the 104 bit voice channels (de-whitened, repeated bits voted, 13 bytes of hex and the
  number of corrected triplets per line) so the arrangement can be checked.
