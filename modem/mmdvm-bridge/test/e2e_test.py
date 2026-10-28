#!/usr/bin/env python3
# Created by Rick KD0OSS
#
# e2e_test.py - the real Modem_Host and the real mmdvm_bridge between fake protocol services and a fake MMDVM
# modem.  Started by run_e2e.sh.  Checks that the host relays the services' frames without knowing what they are,
# that the bridge turns them into MMDVM frames (and back), keeps one service in charge of the modem and paces and
# times out the transmit side.
import os, pty, socket, subprocess, sys, threading, time, tty

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.environ.get('BUILD', HERE + '/build_e2e')
fails = 0

def check(ok, text):
    global fails
    print(('  ok: ' if ok else '  FAIL: ') + text)
    if not ok:
        fails += 1

class FakeModem:
    """A pty that answers status requests and keeps the frames it is sent."""
    def __init__(self):
        self.master, slave = pty.openpty()
        tty.setraw(self.master)
        self.slave_name = os.ttyname(slave)
        if os.path.lexists('/dev/ttyMMsim'):
            os.unlink('/dev/ttyMMsim')
        os.symlink(self.slave_name, '/dev/ttyMMsim')
        self.slave = slave
        self.frames = []
        self.lock = threading.Lock()
        self.spaces = dict(dstar=20, d1=20, d2=20, ysf=20, p25=20, nxdn=20, m17=20)
        self.buf = b''
        threading.Thread(target=self.run, daemon=True).start()
    def status(self):
        s = self.spaces
        return bytes([0xE0, 14, 1, 0, 0, 0, s['dstar'], s['d1'], s['d2'], s['ysf'], s['p25'], s['nxdn'], 0, s['m17']])
    def run(self):
        import select
        while True:
            if not select.select([self.master], [], [], 0.1)[0]:
                continue
            self.buf += os.read(self.master, 4096)
            while len(self.buf) >= 2:
                if self.buf[0] != 0xE0 or self.buf[1] < 3:
                    self.buf = self.buf[1:]
                    continue
                n = self.buf[1]
                if len(self.buf) < n:
                    break
                f = self.buf[:n]; self.buf = self.buf[n:]
                if f == bytes([0xE0, 3, 1]):
                    os.write(self.master, self.status())
                    continue
                with self.lock:
                    self.frames.append(f)
    def send(self, data):
        os.write(self.master, data)
    def take(self, secs=1.0):
        time.sleep(secs)
        with self.lock:
            out = self.frames; self.frames = []
        return out

def mode_frame(name, level):
    f = bytearray(84)
    f[0] = 0x61; f[1] = 0; f[2] = 84; f[3] = 4
    f[4:8] = b'MODE'
    f[8:8 + len(name)] = name.encode()
    f[19:25] = b'FRAME\0'
    f[25] = level
    return bytes(f)

def tlv(name, payload=b''):
    n = 8 + len(payload)
    return bytes([0x61, n >> 8, n & 255, 4]) + name + payload

def comm(t):
    return bytes([0x61, 0, 5, 1, t])

class Service:
    def __init__(self, name, level):
        self.name = name
        self.s = socket.create_connection(('127.0.0.1', 18000)); self.s.settimeout(0.2)
        self.buf = b''
        self.s.sendall(mode_frame(name, level))
    def recv(self, secs):
        end = time.time() + secs
        while time.time() < end:
            try:
                d = self.s.recv(65536)
                if not d:
                    break
                self.buf += d
            except socket.timeout:
                pass
        out = []
        while len(self.buf) >= 3 and self.buf[0] == 0x61:
            l = (self.buf[1] << 8) + self.buf[2]
            if len(self.buf) < l:
                break
            out.append(self.buf[:l]); self.buf = self.buf[l:]
        return out
    def send(self, data):
        self.s.sendall(data)

# ---------------------------------------------------------------- start everything
modem = FakeModem()
env = dict(os.environ, MH_STUB_port='ttyMMsim', MH_STUB_baud='460800', MH_STUB_board='hotspot', MH_STUB_MODEM='mmdvmhs')
log = open(BUILD + '/host.log', 'a')
bridge = subprocess.Popen([BUILD + '/mmdvm_bridge', '-l', '/dev/mmbridge'], env=env, stdout=log, stderr=log)
time.sleep(1.0)
host = subprocess.Popen([BUILD + '/Modem_Host', '-m', '1'], env=dict(env, MH_STUB_bridgePort='mmbridge'),
                        stdout=log, stderr=log)
for _ in range(60):
    try:
        socket.create_connection(('127.0.0.1', 18000)).close()
        break
    except OSError:
        time.sleep(0.25)
time.sleep(1.0)
modem.take(0.1)

try:
    # ---- two services register; the host hands the bridge their names
    a = Service('DSTAR', 45)
    got = a.recv(1.0)
    check(any(f[4:8] == b'COMM' for f in got), 'the host answers a registration with the duplex/simplex frame')
    b = Service('DMR', 55)
    b.recv(1.0)
    time.sleep(1.5)
    fr = modem.take(0.2)
    check(bytes([0xE0, 4, 3, 0]) in fr, 'the modem is put in idle when a service registers')

    # ---- service A takes the modem and transmits
    a.send(comm(2))
    fr = modem.take(0.5)
    check(bytes([0xE0, 4, 3, 1]) in fr, 'SET_MODE: the bridge puts the modem in the D-Star mode')
    hdr = bytes(range(41)); dat = bytes(range(100, 112))
    a.send(tlv(b'DSTH', hdr) + tlv(b'DSTD', dat) + tlv(b'DSTE'))
    fr = modem.take(1.0)
    exp = [bytes([0xE0, 0x2C, 0x10]) + hdr, bytes([0xE0, 0x0F, 0x11]) + dat, bytes([0xE0, 3, 0x13])]
    check(fr == exp, 'D-Star frames reach the modem in its layout: %s' % [f.hex() for f in fr])

    # ---- receive: the modem's D-Star header comes out as the service's own frame
    rx = bytes([0xE0, 46, 0x10]) + bytes(range(43))
    modem.send(rx)
    got = a.recv(1.0)
    check(got == [tlv(b'DSTH', rx[3:44])], 'D-Star header from the modem reaches the D-Star service')
    check(b.recv(0.3) == [], 'and not the DMR service')

    # ---- B cannot take the modem while A has it
    b.send(comm(2))
    check(modem.take(0.5) == [], 'SET_MODE of a second service is ignored while the first has the modem')
    b.send(tlv(b'DMD1', bytes([0x18]) + bytes(33)))
    check(modem.take(0.5) == [], 'its frames are dropped')

    # ---- A done, B takes over
    a.send(comm(3))
    fr = modem.take(0.5)
    check(fr == [bytes([0xE0, 4, 3, 0])], 'SET_IDLE: the modem is idle again')
    b.send(comm(2))
    fr = modem.take(0.5)
    check(fr == [bytes([0xE0, 4, 3, 2])], 'the DMR service gets the modem')
    pl = bytes([0x42]) + bytes(range(33))       # 0x42: this frame ends the transmission
    b.send(tlv(b'DMD1', pl))
    fr = modem.take(0.6)
    check(fr == [bytes([0xE0, len(pl) + 2]) + pl], 'DMR frame goes to the modem as E0 len payload')
    modem.send(bytes([0xE0, 0x25, 0x18]) + bytes(range(34)))
    got = b.recv(1.0)
    check(got == [tlv(b'DMD1', bytes(range(34)))], 'DMR frame from the modem reaches the DMR service')
    check(a.recv(0.3) == [], 'and not the D-Star service')

    # ---- slot 2 waits for slot 2 space
    modem.spaces['d2'] = 0
    time.sleep(0.7)
    pl2 = bytes([0x1A]) + bytes(range(33))
    pl3 = bytes([0x1B]) + bytes(range(5))
    b.send(tlv(b'DMD2', pl2) + tlv(b'DML2', pl3))
    fr = modem.take(0.8)
    check(fr == [], 'DMR slot 2 frame waits while slot 2 has no room: %s' % [f.hex() for f in fr])
    modem.spaces['d2'] = 20
    fr = modem.take(1.2)
    check(fr == [bytes([0xE0, len(pl2) + 2]) + pl2, bytes([0xE0, len(pl3) + 2]) + pl3], 'and goes when there is room: %s' % [f.hex() for f in fr])

    # ---- the bridge ends a transmission that runs too long (1.5 s in this build)
    b.send(tlv(b'DMD1', bytes([0x18]) + bytes(33)))
    modem.take(0.3)
    fr = modem.take(2.5)
    check(bytes([0xE0, 4, 3, 0]) in fr, 'transmit timeout: the modem is put in idle: %s' % [f.hex() for f in fr])
    b.send(comm(2))
    fr = modem.take(0.5)
    check(fr == [bytes([0xE0, 4, 3, 2])], 'the modem can be taken again afterwards')
    b.send(comm(3))
    modem.take(0.3)

    # ---- idle: a frame of a mode without service is dropped, one with service is delivered
    modem.send(bytes([0xE0, 0x7C, 0x20]) + bytes(range(121)))     # YSF data, nobody registered
    check(a.recv(0.4) == [] and b.recv(0.1) == [], 'a frame of a mode nobody registered is dropped')
    modem.send(bytes([0xE0, 0x25, 0x18]) + bytes(range(34)))
    check(b.recv(0.8) == [tlv(b'DMD1', bytes(range(34)))], 'while idle a DMR frame goes to the DMR service')
finally:
    host.terminate(); bridge.terminate()
    host.wait(timeout=5); bridge.wait(timeout=5)

text = open(BUILD + '/host.log').read()
for bad in ('invalid header', 'bad length', 'unknown byte', 'lost frame sync', 'out of space', 'Segmentation'):
    check(bad not in text, 'no "%s" in the logs' % bad)
print('FAILS:', fails)
sys.exit(1 if fails else 0)
