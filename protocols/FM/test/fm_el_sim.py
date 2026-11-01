#!/usr/bin/env python3
# Created by Rick KD0OSS
#
# fm_el_sim.py - end to end test of the FM EchoLink link without a radio or the
# EchoLink network.  It plays three parts so the real fm_host and fm_gateway
# programs can be run against them:
#
#   modem host      TCP 18000, sends receiver samples (24 kSa/s) to fm_host and
#                   records the transmit samples fm_host sends back
#   directory       TCP, the EchoLink directory server (login and station list)
#   remote station  UDP, an EchoLink station on 127.0.0.2 (audio and control
#                   ports), using the real GSM codec through libgsm
#
# fm_host and fm_gateway must be started by the caller (run_sim.sh) with the
# stubbed database; this script drives them through the dashboard command file
# and the radio side, and prints PASS or FAIL.

import ctypes, math, os, random, socket, struct, sys, threading, time

APORT = 7198
CPORT = 7199
DIRPORT = 7200
STUB = "/tmp/fm_stub_"
LOG = os.environ.get("FM_SIM_LOGDIR", "/tmp")

gsm = ctypes.CDLL("libgsm.so.1")
gsm.gsm_create.restype = ctypes.c_void_p
gsm.gsm_encode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
gsm.gsm_decode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
G_ENC = gsm.gsm_create()
G_DEC = gsm.gsm_create()

fails = 0


def check(cond, text):
    global fails
    print(("  ok   " if cond else "  FAIL ") + text, flush=True)
    if not cond:
        fails += 1


def goertzel(samples, freq, fs):
    """Amplitude of one frequency in a block of samples."""
    n = len(samples)
    if n == 0:
        return 0.0
    w = 2 * math.pi * freq / fs
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + c * s1 - s2
        s2 = s1
        s1 = s0
    p = s1 * s1 + s2 * s2 - c * s1 * s2
    return 2 * math.sqrt(max(p, 0.0)) / n


# ---------------------------------------------------------------- modem host
class ModemHost(threading.Thread):
    """Accepts fm_host, feeds it receiver samples and records what it transmits."""

    def __init__(self):
        super().__init__(daemon=True)
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 18000))
        self.srv.listen(1)
        self.conn = None
        self.lock = threading.Lock()
        self.signal = ("noise",)       # what the receiver hears
        self.tx = []                   # transmit samples, in order
        self.tx_frames = 0
        self.eot = 0
        self.setmode = 0
        self.setidle = 0
        self.keyed = False
        self.registered = False
        self.running = True
        self.phase = {}

    def set_signal(self, *sig):
        with self.lock:
            self.signal = sig

    def make_block(self, n):
        kind = self.signal[0]
        out = []
        for _ in range(n):
            self.t = getattr(self, "t", 0) + 1
            t = self.t / 24000.0
            if kind == "noise":
                v = random.uniform(-30000, 30000)
            elif kind == "tone":
                v = self.signal[2] * math.sin(2 * math.pi * self.signal[1] * t)
            elif kind == "dtmf":
                v = self.signal[3] * (math.sin(2 * math.pi * self.signal[1] * t) + math.sin(2 * math.pi * self.signal[2] * t))
            else:
                v = 0.0
            out.append(int(v))
        return out

    def reader(self):
        buf = b""
        while self.running:
            try:
                d = self.conn.recv(4096)
            except OSError:
                break
            if not d:
                break
            buf += d
            while len(buf) >= 4:
                if buf[0] != 0x61:
                    buf = buf[1:]
                    continue
                ln = (buf[1] << 8) | buf[2]
                if len(buf) < ln:
                    break
                pkt = buf[:ln]
                buf = buf[ln:]
                typ = pkt[4:8]
                if typ == b"MODE":
                    self.registered = True
                elif typ == b"SAMP":
                    n = (ln - 8) // 2
                    self.tx.extend(struct.unpack("<%dh" % n, pkt[8:8 + n * 2]))
                    self.tx_frames += 1
                    self.keyed = True
                elif typ == b"SEOT":
                    self.eot += 1
                    self.keyed = False
                elif ln == 5 and pkt[4] == 2:
                    self.setmode += 1
                elif ln == 5 and pkt[4] == 3:
                    self.setidle += 1

    def run(self):
        self.conn, _ = self.srv.accept()
        threading.Thread(target=self.reader, daemon=True).start()
        nxt = time.monotonic()
        while self.running:
            nxt += 0.004
            with self.lock:
                blk = self.make_block(96)
            pkt = b"\x61" + struct.pack(">H", 8 + 192) + b"\x04SAMP" + struct.pack("<96h", *blk)
            try:
                self.conn.sendall(pkt)
            except OSError:
                break
            d = nxt - time.monotonic()
            if d > 0:
                time.sleep(d)

    def mark(self):
        return len(self.tx)

    def tone_since(self, mark, freq):
        s = self.tx[mark:]
        return goertzel(s, freq, 24000) if s else 0.0


# ---------------------------------------------------------------- directory
class Directory(threading.Thread):
    """The EchoLink directory: logins ("OK") and the station list."""

    LIST = [("W1AW-L", "ARRL Newington CT   [ON 12:00]", 12345, "127.0.0.2"),
            ("N0TEST-R", "Test repeater   [ON 12:00]", 23456, "127.0.0.4"),
            ("K1XYZ", "A station   [BUSY 12:00]", 34567, "127.0.0.5")]

    def __init__(self):
        super().__init__(daemon=True)
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", DIRPORT))
        self.srv.listen(5)
        self.logins = []
        self.lists = 0

    def run(self):
        while True:
            c, _ = self.srv.accept()
            c.settimeout(2)
            try:
                d = c.recv(1024)
            except OSError:
                d = b""
            if d.startswith(b"l"):
                self.logins.append(d.decode("latin1"))
                c.sendall(b"OK")
            elif d.startswith(b"s"):
                self.lists += 1
                txt = "@@@\n%d\n" % len(self.LIST)
                for call, data, ident, ip in self.LIST:
                    txt += "%s\n%s\n%d\n%s\n" % (call, data, ident, ip)
                txt += "+++"
                c.sendall(txt.encode())
            c.close()


# ------------------------------------------------------------ remote station
def rtcp_sdes(call, name="Test"):
    def item(t, s):
        return bytes([t, len(s)]) + s
    items = item(1, b"CALLSIGN") + item(2, ("%-15s%s" % (call, name)).encode()) + item(3, b"CALLSIGN") + item(4, b"08:30") + b"\x00\x00"
    while len(items) % 4:
        items += b"\x00"
    sdes = bytes([0xc1, 202]) + struct.pack(">H", (8 + len(items)) // 4 - 1) + b"\x00\x00\x00\x00" + items
    return bytes([0xc0, 201, 0, 1, 0, 0, 0, 0]) + sdes


def rtcp_bye():
    txt = b"\x07jan2002"
    while len(txt) % 4:
        txt += b"\x00"
    bye = bytes([0xc1, 203]) + struct.pack(">H", (8 + len(txt)) // 4 - 1) + b"\x00\x00\x00\x00" + txt
    return bytes([0xc0, 201, 0, 1, 0, 0, 0, 0]) + bye


class Remote:
    """An EchoLink station: listens on its own address, plays voice and checks what arrives."""

    def __init__(self, ip, call, answer=True):
        self.ip, self.call, self.answer = ip, call, answer
        self.a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.a.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.c.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.a.bind((ip, APORT))
        self.c.bind((ip, CPORT))
        self.sdes_seen = 0
        self.bye_seen = 0
        self.info = []
        self.rx = []                  # decoded audio from the node
        self.rx_packets = 0
        self.seq = 0
        self.running = True
        threading.Thread(target=self.ctrl_loop, daemon=True).start()
        threading.Thread(target=self.audio_loop, daemon=True).start()

    def ctrl_loop(self):
        self.c.settimeout(0.2)
        while self.running:
            try:
                d, addr = self.c.recvfrom(2048)
            except OSError:
                continue
            if len(d) > 9 and d[9] == 203:
                self.bye_seen += 1
            elif len(d) > 9 and d[9] == 202:
                self.sdes_seen += 1
                if self.answer:
                    self.c.sendto(rtcp_sdes(self.call), ("127.0.0.1", CPORT))

    def audio_loop(self):
        self.a.settimeout(0.2)
        while self.running:
            try:
                d, addr = self.a.recvfrom(2048)
            except OSError:
                continue
            if d[1:6] == b"NDATA":
                self.info.append(d)
            elif len(d) >= 12 + 132 and d[1] == 3:
                out = ctypes.create_string_buffer(640 * 2)
                for i in range(4):
                    frame = ctypes.create_string_buffer(d[12 + i * 33:12 + i * 33 + 33], 33)
                    gsm.gsm_decode(G_DEC, frame, ctypes.byref(out, i * 320))
                self.rx.extend(struct.unpack("<640h", out.raw))
                self.rx_packets += 1

    def send_sdes(self):
        self.c.sendto(rtcp_sdes(self.call), ("127.0.0.1", CPORT))

    def send_bye(self):
        self.c.sendto(rtcp_bye(), ("127.0.0.1", CPORT))

    def talk(self, freq, seconds, amp=8000):
        """Sends a tone as 80 ms GSM packets in real time."""
        t0 = time.monotonic()
        n = int(seconds / 0.08)
        phase = 0
        for k in range(n):
            pcm = [int(amp * math.sin(2 * math.pi * freq * (phase + i) / 8000.0)) for i in range(640)]
            phase += 640
            raw = struct.pack("<640h", *pcm)
            hdr = bytes([0xc0, 0x03]) + struct.pack(">H", self.seq & 0xffff) + b"\x00" * 8
            self.seq += 1
            frames = b""
            for i in range(4):
                src = ctypes.create_string_buffer(raw[i * 320:(i + 1) * 320], 320)
                dst = ctypes.create_string_buffer(33)
                gsm.gsm_encode(G_ENC, src, dst)
                frames += dst.raw
            self.a.sendto(hdr + frames, ("127.0.0.1", APORT))
            d = t0 + (k + 1) * 0.08 - time.monotonic()
            if d > 0:
                time.sleep(d)

    def close(self):
        self.running = False


# ---------------------------------------------------------------- helpers
def dash(cmd, param):
    with open(STUB + "cmd_" + cmd, "w") as f:
        f.write(param + "\n")


def wait(cond, secs, step=0.05):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(step)
    return cond()


def db_lines():
    out = []
    for name in ("host.log", "gateway.log"):
        try:
            out += open(os.path.join(LOG, name), errors="replace").read().splitlines()
        except OSError:
            pass
    return out


def db_has(text):
    return any(text in l for l in db_lines())


def db_count(text):
    return sum(1 for l in db_lines() if text in l)


DTMF = {"1": (697, 1209), "2": (697, 1336), "3": (697, 1477), "4": (770, 1209), "5": (770, 1336), "6": (770, 1477),
        "7": (852, 1209), "8": (852, 1336), "9": (852, 1477), "*": (941, 1209), "0": (941, 1336), "#": (941, 1477)}


def send_dtmf(modem, digits):
    for d in digits:
        lo, hi = DTMF[d]
        modem.set_signal("dtmf", lo, hi, 4000)
        time.sleep(0.12)
        modem.set_signal("tone", 1000, 0)   # carrier stays up between digits (silence of audio, squelch quiet)
        time.sleep(0.12)


def main():
    random.seed(7)
    open(STUB + "mode", "w").write("FM\n")
    modem = ModemHost()
    modem.start()
    dirsrv = Directory()
    dirsrv.start()
    remote = Remote("127.0.0.2", "W1AW-L")
    other = Remote("127.0.0.3", "W1AW-L")       # same callsign, not the address in the directory
    print("Waiting for fm_host and fm_gateway")
    ok = wait(lambda: modem.registered and db_has("gateway added FM"), 25)
    check(ok, "fm_host registered with the modem host and the gateway connected")
    if not ok:
        print("FAIL")
        return 1
    check(wait(lambda: len(dirsrv.logins) > 0, 10), "gateway registered with the directory")
    check("ONLINE" in dirsrv.logins[0] and "N0LINK-L" in dirsrv.logins[0], "login text has callsign and ONLINE state")
    check(wait(lambda: dirsrv.lists > 0, 10), "gateway fetched the station list")

    print("Test 1: link from the dashboard, outgoing")
    modem.set_signal("noise")
    dash("reflLinkFM", "link,W1AW-L,")
    check(wait(lambda: db_has("ack reflLinkFM success"), 12), "dashboard link acknowledged as success")
    check(db_has("reflector W1AW-L linked"), "reflector status set to W1AW-L linked")
    check(remote.sdes_seen >= 1, "remote station received our description")
    check(len(remote.info) >= 1, "remote station received the information text")
    check(wait(lambda: any("BUSY" in l for l in dirsrv.logins), 8), "directory told BUSY while connected")

    print("Test 2: the remote station talks, the repeater transmits it")
    mark = modem.mark()
    keyed_before = modem.setmode
    remote.talk(1000, 1.2, 8000)
    time.sleep(0.4)
    check(modem.setmode > keyed_before, "transmitter took the modem for the link audio")
    amp = modem.tone_since(mark + 24000 // 2, 1000)
    check(amp > 500, "1 kHz from the remote station is in the transmit audio (amplitude %.0f)" % amp)
    check(wait(lambda: modem.eot > 0, 8), "transmission ended after the hang time")

    print("Test 3: the radio receives a signal, the remote station hears it")
    n0 = len(remote.rx)
    modem.set_signal("tone", 1000, 9000)
    time.sleep(1.5)
    modem.set_signal("noise")
    time.sleep(1.0)
    got = remote.rx[n0:]
    check(len(got) >= 640 * 10, "remote station received %d samples of audio" % len(got))
    a = goertzel(got[640 * 4:640 * 12], 1000, 8000) if len(got) >= 640 * 12 else 0
    check(a > 1500, "the audio is the 1 kHz tone (amplitude %.0f)" % a)

    print("Test 4: DTMF # disconnects")
    b0 = remote.bye_seen
    modem.set_signal("tone", 1000, 0)
    time.sleep(0.5)
    mark = modem.mark()
    n0 = len(remote.rx)
    send_dtmf(modem, "#")
    modem.set_signal("noise")
    time.sleep(0.3)
    peak_tx = max([abs(x) for x in modem.tx[mark:]] or [0])
    peak_rx = max([abs(x) for x in remote.rx[n0:]] or [0])
    print("       DTMF leak: transmit peak %d, remote peak %d (tone amplitude 4000 + 4000)" % (peak_tx, peak_rx))
    check(peak_tx < 1500, "DTMF tones are muted in the repeater transmit audio")
    check(peak_rx < 500, "DTMF tones are muted in the audio to the remote station")
    check(wait(lambda: db_has("ack reflLinkFM unlinked"), 6), "link ended by DTMF #")
    check(remote.bye_seen > b0, "remote station received BYE")
    check(db_has("reflector W1AW-L unlinked"), "reflector status cleared")

    print("Test 5: DTMF node number connects")
    s0 = remote.sdes_seen
    modem.set_signal("tone", 1000, 0)
    time.sleep(0.5)
    send_dtmf(modem, "12345#")
    modem.set_signal("noise")
    check(wait(lambda: db_count("reflector W1AW-L linked") >= 2, 12), "DTMF 12345# linked to W1AW-L")
    check(remote.sdes_seen > s0, "remote station received our description")

    print("Test 6: busy node turns another station away")
    b1 = other.bye_seen
    other.send_sdes()
    check(wait(lambda: other.bye_seen > b1, 4), "second station got BYE while connected")

    print("Test 7: the remote station says goodbye")
    remote.send_bye()
    check(wait(lambda: db_count("ack reflLinkFM unlinked") >= 2, 6), "link dropped on BYE from the station")

    print("Test 8: incoming connection")
    n_before = db_count("reflector W1AW-L linked")
    remote.send_sdes()
    check(wait(lambda: db_count("reflector W1AW-L linked") > n_before, 6), "listed station W1AW-L accepted")
    check(remote.sdes_seen >= 3, "we answered with our description")

    print("Test 9: remote talks on the incoming link")
    mark = modem.mark()
    remote.talk(1000, 1.0, 8000)
    time.sleep(0.3)
    amp = modem.tone_since(mark + 24000 // 2, 1000)
    check(amp > 500, "audio from the incoming station is transmitted (amplitude %.0f)" % amp)
    remote.send_bye()
    check(wait(lambda: db_count("ack reflLinkFM unlinked") >= 3, 6), "incoming link ended by BYE")

    print("Test 10: address that is not the directory's is refused")
    b2 = other.bye_seen
    other.send_sdes()
    check(wait(lambda: other.bye_seen > b2, 4), "W1AW-L from the wrong address got BYE")
    check(db_count("reflector W1AW-L linked") == n_before + 1, "no link made")

    print("Test 11: unknown node number")
    dash("reflLinkFM", "link,99999,")
    check(wait(lambda: db_has("ack reflLinkFM failed"), 14), "unknown node answered failed")

    print("Test 12: station that does not answer")
    remote.answer = False
    dash("reflLinkFM", "link,12345,")
    check(wait(lambda: db_count("ack reflLinkFM failed") >= 2, 14), "silent station answered failed")
    remote.answer = True

    print("PASS" if fails == 0 else "FAIL (%d)" % fails)
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    rc = main()
    sys.stdout.flush()
    os._exit(rc)
