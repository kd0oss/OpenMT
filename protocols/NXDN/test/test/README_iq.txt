nxdn_iq: NXDN test signal generator / decoder for HackRF (and any SDR taking 8-bit IQ)

Build (from the NXDN directory; dsp_tools.c is the OpenMT tools/ one):
  gcc -O2 -I. -I../../tools -o nxdn_iq test/nxdn_iq.c nxdn_func.c nxdn_dsp.c nxdn_gen.c ../../tools/dsp_tools.c -lm

Generate and transmit (only on a frequency you are licensed for, low power / dummy load):
  ./nxdn_iq gen -o call.cs8 --voice 100 --ran 1 --src 1234 --dst 9 --repeat 5
  hackrf_transfer -t call.cs8 -f 446000000 -s 2400000 -x 0 -R

Capture and decode (a radio's transmission, or the generated file over the air):
  hackrf_transfer -r cap.cs8 -f 446000000 -s 2400000 -n 24000000
  ./nxdn_iq dec cap.cs8 [--offset hz] [--invert]

Loopback with no hardware:  ./nxdn_iq gen -o a.cs8 && ./nxdn_iq dec a.cs8
Options: --data N (data call), --individual, --level pct (deviation), --offset hz, --shape rrc|rc, --invert, -r rate
