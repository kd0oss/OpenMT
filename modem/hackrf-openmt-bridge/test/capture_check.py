#!/usr/bin/env python3
# Copyright (C) 2026 by Rick KD0OSS - GPL v3 or later
"""
Quick quality check of an OpenMT Radio capture (openmt_rx.raw: 16-bit little endian samples, 24 kSa/s) of a
4FSK signal such as DMR.  It finds the part with a signal, and reports

  - the four symbol levels (they should be about equally spaced, and a gap of 1/3 of the outer spread)
  - the centre offset (a frequency error: 12.25 counts per Hz at the default rx gain)
  - the discriminator clicks per second: full-scale jumps that appear when the signal is too weak for the
    FM receiver.  A good signal has none or a few; dozens a second mean too little signal (raise the
    HackRF gains / improve the antenna) or an overloaded HackRF (see the bridge's --rx-stats).

Usage: capture_check.py capture.raw
"""
import sys
import numpy as np
from scipy import signal as sg

FS = 24000
x = np.fromfile(sys.argv[1], dtype='<i2').astype(float)
w = 2400
env = np.array([x[i:i + w].std() for i in range(0, len(x) - w, w)])
noise = np.median(env)                       # idle: the receiver's noise, much louder than a signal
act = [i for i, e in enumerate(env) if e < 0.75 * noise]
if len(act) < 5:
    sys.exit("no signal found (the level never drops below the noise)")
s = x[act[0] * w:(act[-1] + 1) * w]
print("signal %.1f s, noise rms %.0f, signal rms %.0f, clipped %.1f%%" %
      (len(s) / FS, noise, s.std(), 100 * np.mean(np.abs(s) >= 32700)))
b, a = sg.butter(4, 2600 / (FS / 2))
y = sg.filtfilt(b, a, s)
lo, hi = np.percentile(y, [2, 98])
print("outer levels %.0f .. %.0f, centre %.0f (%.0f Hz), spread %.0f" % (lo, hi, (lo + hi) / 2, (lo + hi) / 2 / 12.25, hi - lo))
d = np.abs(np.diff(s))
print("clicks (full-scale jumps): %d  = %.1f per second" % (np.sum(d > 50000), np.sum(d > 50000) / (len(s) / FS)))
