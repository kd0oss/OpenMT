Unit tests (host-side, no hardware):
 difftest.cpp  bit-exact comparison of nxdn_func.c against the NXDN_Archive C++ code (needs the archive sources + shim/)
 t_ctrl.c      call-control logic (RF->net, net->RF, late entry, RAN filter, watchdog)
 t_dsp.c       modulate->demodulate loopback (offsets, drift, noise, false-lock check)
 run1.sh       host + nxdn_sim end-to-end:  ./run1.sh [extra sim args, e.g. --noise 150 | --frame]
