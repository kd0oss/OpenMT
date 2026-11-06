#!/usr/bin/env python3
# Copyright (C) 2026 by Rick KD0OSS - GPL v3 or later
"""
End-to-end test of the bridge against the software HackRF.

This script plays the part of Modem_Host.c: it opens the bridge's PTY and
speaks the OpenMT serial protocol to it, while the "radio" is the bridge's
simulator build (cmake -DHRF_SIM=ON), fed from a file of synthetic FM
and writing what it would transmit to another file.

  RX  a 4FSK-style FM signal (offset from the LO as a real HackRF capture
      would be) comes back as Q15 SAMP_DATA frames with the right shape,
      scale and polarity
  TX  SET_MODE + SAMP_DATA frames come out as a constant-envelope FM
      signal whose deviation is what was asked for; the end is flushed
      before the radio goes back to RX
  half duplex: RX stops before TX starts, TX stops before RX restarts, and
      RX works again afterwards
  config: an OPENMTSX SET_CONFIG reaches the (simulated) hardware

Usage: e2e.py /path/to/bridge-built-with-HRF_SIM
"""
import os, sys, time, struct, subprocess, tempfile, threading, termios, tty, signal
import numpy as np
from scipy import signal as sps

FS = 2_000_000
OFFSET = 250_000
RX_AMP = 100
DEV_PER_UNIT = 950.0          # Hz per baseband unit (bridge --deviation-hz)
TX_SYMBOL_SCALE = 494.0       # Q15 counts per baseband unit (bridge --tx-symbol-scale)
RX_G = 1.24 * 2 * np.pi / 12500.0 * 0.41 * 32767.0   # Q15 per Hz out of the RX chain (--rx-gain default 0.41 at rxLevel 50)

fails = 0
def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails += 1

def pattern(nsym, seed):
    """4FSK-like deviation (Hz) at 24 kSa/s: random +-1/+-3 symbols at 4800 baud, smoothed."""
    r = np.random.RandomState(seed)
    sym = r.choice([-3, -1, 1, 3], nsym).astype(float)
    f = np.repeat(sym * DEV_PER_UNIT, 5)
    w = np.hanning(7); w /= w.sum()
    return np.convolve(f, w, mode="same")

def fm_iq(dev24, offset=OFFSET, amp=RX_AMP, noise=1.0, seed=1):
    n = int(len(dev24) * FS / 24000)
    f = np.interp(np.arange(n) * 24000.0 / FS, np.arange(len(dev24)), dev24)
    ph = 2 * np.pi * np.cumsum(offset + f) / FS
    x = amp * np.exp(1j * ph)
    r = np.random.RandomState(seed)
    x = x + noise * (r.randn(n) + 1j * r.randn(n))
    out = np.empty(2 * n, np.int8)
    out[0::2] = np.clip(np.round(x.real), -127, 127)
    out[1::2] = np.clip(np.round(x.imag), -127, 127)
    return out

def best_fit(received, expected, up=8):
    """Align expected to received (to 1/up sample); returns (corr, slope, lag in samples)."""
    r = sps.resample_poly(np.asarray(received, float), up, 1)
    e = sps.resample_poly(np.asarray(expected, float), up, 1)
    pad = 400 * up
    r = np.concatenate([np.zeros(pad), r, np.zeros(max(0, len(e) - len(r)) + pad)])
    c = sps.correlate(r, e, mode="valid", method="fft")
    lag = int(np.argmax(c))
    seg = r[lag:lag + len(e)]
    corr = np.corrcoef(seg, e)[0, 1]
    slope = np.dot(seg, e) / np.dot(e, e)
    return corr, slope, (lag - pad) / up

class Host:
    """The Modem_Host end of the PTY."""
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        self.lock = threading.Lock()
        self.samples = []          # (monotonic time, np.int16 array)
        self.other = []            # (type, payload)
        self.run = True
        self.buf = b""
        self.t = threading.Thread(target=self.reader, daemon=True)
        self.t.start()
    def reader(self):
        import select
        while self.run:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if not r:
                continue
            try:
                d = os.read(self.fd, 65536)
            except OSError:
                break
            self.buf += d
            while len(self.buf) >= 3:
                if self.buf[0] != 0x61:
                    self.buf = self.buf[1:]
                    continue
                n = self.buf[1]
                if n < 3 or len(self.buf) < n:
                    break
                typ, pay = self.buf[2], self.buf[3:n]
                self.buf = self.buf[n:]
                with self.lock:
                    if typ & 0xF0 == 0x70:
                        self.samples.append((time.monotonic(), np.frombuffer(pay, "<i2").copy()))
                    else:
                        self.other.append((typ, pay))
    def send(self, b):
        os.write(self.fd, bytes(b))
    def stream(self):
        with self.lock:
            return np.concatenate([s for _, s in self.samples]) if self.samples else np.zeros(0, np.int16)
    def close(self):
        self.run = False

def log_text(p):
    return open(p).read()

def run(bridge):
    d = tempfile.mkdtemp(prefix="hrfe2e")
    rxf, txf, logf, pty = (os.path.join(d, n) for n in ("rx.cs8", "tx.cs8", "bridge.log", "pty"))

    # RX file: pattern A, quiet carrier, pattern B, quiet carrier
    A, B = pattern(4800, 11), pattern(4800, 22)
    quiet = np.zeros(24000 * 2 + 24000 // 2)           # 2.5 s of noise
    # (the gaps are band noise, as on a real idle channel: an unmodulated
    # carrier sitting exactly on the LO offset would be learned by the
    # chain's DC remover and subtracted from the start of the next burst)
    parts = [fm_iq(np.zeros(24000 * 3 // 4), amp=0, seed=5), fm_iq(A, seed=1), fm_iq(quiet, amp=0, seed=2),
             fm_iq(B, seed=3), fm_iq(np.zeros(24000 * 3), amp=0, seed=4)]
    np.concatenate(parts).tofile(rxf)

    lf = open(logf, "w")
    env = dict(os.environ, HRF_SIM_SPEED="1")
    p = subprocess.Popen([bridge, "--serial", rxf + "," + txf, "--pty-symlink", pty,
                          "--rx-freq", "434000000", "--tx-freq", "434000000"],
                         stderr=lf, stdout=lf, env=env)
    for _ in range(100):
        if os.path.exists(pty) and "running" in log_text(logf):
            break
        time.sleep(0.1)
    host = Host(pty)

    # OPENMTSX configuration: rx 435.000 / tx 435.600 MHz, lna 16, vga 30, dac 25
    print("config")
    cfg = bytes([1]) + struct.pack(">II", 435_000_000, 435_600_000) + bytes([16, 30, 0, 25]) + \
        struct.pack(">HH", 300, 300) + bytes([0])
    host.send(bytes([0x61, 3 + len(cfg), 0x20]) + cfg)
    time.sleep(0.6)
    lt = log_text(logf)
    check("freq 434750000" in lt, "RX LO = 435.000 MHz - 250 kHz after SET_CONFIG")
    check("rx gain lna=16 vga=30" in lt, "RX gains applied (lna 16, vga 30)")

    # ---- RX: pattern A -------------------------------------------------
    print("RX")
    t0 = time.monotonic()
    time.sleep(2.4)                                    # 0.75 s lead-in, A is 1.0 s, then quiet
    sa = host.stream()
    corr, slope, lag = best_fit(sa.astype(float), A * RX_G)
    print(f"       corr {corr:.4f}  slope {slope:.3f}  (Q15 per Hz {RX_G:.4f})")
    check(corr > 0.97, "pattern A recovered from IQ (correlation %.3f)" % corr)
    check(0.85 < slope < 1.05, "RX level and polarity right (slope %.3f; dsp.c gives ~0.91 on this signal, <0 would be inverted)" % slope)
    nrx_before = len(sa)

    # ---- SET_MODE on its own (a host locking the modem for a RECEIVED call) ----
    print("SET_MODE alone")
    n_before = len(host.stream())
    host.send([0x61, 3, 0x33])
    time.sleep(0.5)
    lt = log_text(logf)
    check("TX started" not in lt, "SET_MODE without audio does not key the transmitter")
    check(len(host.stream()) > n_before + 24000 * 0.3, "and RX keeps flowing (%d new samples in 0.5 s)" % (len(host.stream()) - n_before))
    host.send([0x61, 3, 0x3F])
    time.sleep(0.2)
    check("TX started" not in log_text(logf), "SET_MODE idle changes nothing either")
    nrx_before = len(host.stream())

    # ---- TX --------------------------------------------------------------
    print("TX")
    nsym = 9600                                        # 2 s of audio
    tx_dev = pattern(nsym, 33)                         # Hz
    q15 = np.round(tx_dev / DEV_PER_UNIT * TX_SYMBOL_SCALE).astype("<i2")
    host.send([0x61, 3, 0x33])                         # SET_MODE | conn 3 (does not key by itself)
    t_next = time.monotonic()
    for i in range(0, len(q15), 96):
        chunk = q15[i:i + 96]
        if len(chunk) < 96:
            chunk = np.concatenate([chunk, np.zeros(96 - len(chunk), "<i2")])
        host.send(bytes([0x61, 0xC4, 0x70, 50]) + chunk.tobytes())
        t_next += 96 / 24000.0
        time.sleep(max(0, t_next - time.monotonic()))
    host.send([0x61, 3, 0x00])                         # SAMP_EOT
    host.send([0x61, 3, 0x3F])                         # SET_MODE idle
    for _ in range(100):
        if "TX unkeyed" in log_text(logf):
            break
        time.sleep(0.1)
    check("TX unkeyed" in log_text(logf), "bridge unkeyed after SAMP_EOT")

    # ---- RX again: pattern B ----------------------------------------------
    print("RX after TX")
    time.sleep(3.2)
    s_all = host.stream()
    sb = s_all[nrx_before:].astype(float)
    corr, slope, lag = best_fit(sb, B * RX_G)
    print(f"       corr {corr:.4f}  slope {slope:.3f}")
    check(corr > 0.97 and 0.85 < slope < 1.05, "RX works again after TX (corr %.3f, slope %.3f)" % (corr, slope))

    host.close()
    p.send_signal(signal.SIGINT)
    try:
        p.wait(8)
    except subprocess.TimeoutExpired:
        p.kill()
        check(False, "bridge exits on SIGINT")
    lf.close()
    lt = log_text(logf)

    # ---- the transmitted signal ----------------------------------------------
    raw = np.fromfile(txf, np.int8).astype(float)
    x = raw[0::2] + 1j * raw[1::2]
    act = np.abs(x) > 20
    check(act.any(), "something was transmitted")
    idx = np.where(act)[0]
    first, last = idx[0], idx[-1]
    seg = x[first + 4000:last - 4000]
    mag = np.abs(seg)
    print(f"       on-air {(last - first) / FS:.2f} s, level {mag.mean():.1f}, ripple {100 * mag.std() / mag.mean():.2f} %")
    check(abs(mag.mean() / (0.9 * 127) - 1) < 0.03, "TX level = 0.9 of full scale (%.1f)" % mag.mean())
    check(mag.std() / mag.mean() < 0.03, "TX envelope constant (ripple %.2f %%)" % (100 * mag.std() / mag.mean()))
    ph = np.unwrap(np.angle(x[first:last]))
    ph125 = ph[:len(ph) // 16 * 16].reshape(-1, 16).mean(axis=1)       # 2 Msps -> 125 kSa/s
    f125 = np.diff(ph125) * 125000.0 / (2 * np.pi)
    f24 = sps.resample_poly(f125, 24, 125)
    corr, slope, lag = best_fit(f24, tx_dev)
    print(f"       corr {corr:.4f}  slope {slope:.3f}")
    check(corr > 0.98, "TX deviation follows the samples sent (corr %.3f)" % corr)
    check(0.95 < slope < 1.05, "TX deviation = %.0f Hz per unit (slope %.3f)" % (DEV_PER_UNIT, slope))
    check(abs(np.median(f125)) < 100, "TX centred on the LO (median offset %.0f Hz)" % np.median(f125))
    # nothing on the air after the end except zeros; and the whole tail is zero
    check(np.abs(x[last + 1:]).max(initial=0) <= 20, "TX ends with nothing on the air")

    # ---- ordering, errors -----------------------------------------------------
    print("log")
    L = lt.splitlines()
    def pos(s, start=0):
        for i in range(start, len(L)):
            if s in L[i]:
                return i
        return -1
    i_rxstop = pos("RX stopped"); i_txstart = pos("TX started"); i_txstop = pos("TX stopped")
    i_rxrestart = pos("RX started", i_txstop)
    check(0 <= i_rxstop < i_txstart < i_txstop < i_rxrestart, "half duplex: RX stop -> TX start -> TX stop -> RX start")
    check("freq 435600000" in lt, "TX LO = 435.600 MHz from SET_CONFIG")
    check("tx gain txvga=25" in lt, "TX VGA 25 dB from SET_CONFIG")
    for bad in ("underrun buffer(s)", "ring overflowed", "queue full", "ring full", "failed", "BUSY"):
        check(bad not in lt.replace("(0 underrun buffers)", ""), "no '%s' in log" % bad)
    check("0 underrun buffers" in lt, "TX ran with no USB underruns")
    print("RESULT:", "PASSED" if not fails else "FAILED (%d)" % fails)
    if fails:
        print("---- bridge log ----\n" + lt[-4000:])
    return fails

def tx_edge_cases(bridge):
    """A gap in the host's audio, unkeying by timeout, Ctrl-C while keyed."""
    print("TX edge cases")
    d = tempfile.mkdtemp(prefix="hrfe2e")
    txf, logf, pty = (os.path.join(d, n) for n in ("tx.cs8", "bridge.log", "pty"))
    lf = open(logf, "w")
    p = subprocess.Popen([bridge, "--serial", "," + txf, "--pty-symlink", pty], stderr=lf, stdout=lf,
                         env=dict(os.environ, HRF_SIM_SPEED="1"))
    for _ in range(100):
        if os.path.exists(pty) and "running" in log_text(logf):
            break
        time.sleep(0.1)
    host = Host(pty)

    def send_audio(n_frames, t_next):
        q = np.round(pattern(n_frames * 96 // 5 + 1, 44)[:n_frames * 96] / DEV_PER_UNIT * TX_SYMBOL_SCALE).astype("<i2")
        for i in range(n_frames):
            host.send(bytes([0x61, 0xC4, 0x70, 50]) + q[i * 96:(i + 1) * 96].tobytes())
            t_next += 96 / 24000.0
            time.sleep(max(0, t_next - time.monotonic()))
        return t_next

    host.send([0x61, 3, 0x33])
    t = send_audio(150, time.monotonic())            # 0.6 s
    time.sleep(0.45)                                 # the host stalls
    send_audio(150, time.monotonic())                # 0.6 s more, then no EOT at all
    for _ in range(60):
        if "no samples for" in log_text(logf):
            break
        time.sleep(0.1)
    lt = log_text(logf)
    check("no samples for 1000ms, unkeying" in lt, "bridge unkeys by timeout when the host never sends EOT")
    time.sleep(0.9)                                  # let the flush finish
    lt = log_text(logf)
    x = np.fromfile(txf, np.int8).astype(float)
    x = x[0::2] + 1j * x[1::2]
    idx = np.where(np.abs(x) > 20)[0]
    inner = np.abs(x[idx[0] + 20000:idx[-1] - 20000])
    check(inner.min() > 60, "carrier held through the gap, never silent mid-transmission (min level %.0f)" % inner.min())
    check("USB buffer underrun" in lt, "the gap was reported as an underrun")
    check("TX unkeyed" in lt, "and the radio went back to receive")

    # Ctrl-C while keyed
    host.send([0x61, 3, 0x33])
    send_audio(100, time.monotonic())
    t0 = time.monotonic()
    p.send_signal(signal.SIGINT)
    try:
        p.wait(10)
        ok = True
    except subprocess.TimeoutExpired:
        p.kill()
        ok = False
    lt = log_text(logf)
    check(ok and time.monotonic() - t0 < 8, "bridge exits promptly on Ctrl-C while transmitting (%.1f s)" % (time.monotonic() - t0))
    check(lt.rstrip().endswith("hrf_sim: RX stopped") or "hrf_sim: TX stopped" in lt.split("shutting down")[-1],
          "transmitter stopped at shutdown")
    host.close()
    lf.close()

def startup_checks(bridge):
    """--ppm moves the LO; bad settings are refused."""
    print("startup")
    d = tempfile.mkdtemp(prefix="hrfe2e")
    def go(args):
        p = subprocess.Popen([bridge, "--serial", ",", "--pty-symlink", os.path.join(d, "p")] + args,
                             stderr=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        time.sleep(1.0)
        p.send_signal(signal.SIGINT)
        try:
            out = p.communicate(timeout=8)[1]
        except subprocess.TimeoutExpired:
            p.kill(); out = p.communicate()[1]
        return p.returncode, out
    rc, out = go(["--rx-freq", "435000000", "--ppm", "10"])
    # (435e6 - 250e3) / (1 + 10e-6) = 434745652.5 -> 434745653
    check("freq 434745653" in out, "--ppm 10 corrects the LO (434745653 Hz)")
    rc, out = go(["--sample-rate", "1234567"])
    check(rc != 0 and "not usable" in out, "bad --sample-rate refused")
    rc, out = go(["--tx-amplitude", "2"])
    check(rc != 0, "bad --tx-amplitude refused")

if __name__ == "__main__":
    bridge = sys.argv[1]
    startup_checks(bridge)
    run(bridge)
    tx_edge_cases(bridge)
    print("OVERALL:", "PASSED" if not fails else "FAILED (%d)" % fails)
    sys.exit(1 if fails else 0)
