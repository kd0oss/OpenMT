#!/usr/bin/env python3
"""Tests mmdvm_bridge against a fake modem (a pty) and compares its frames
with the ones the old host code (test/legacy/mmdvm.c) builds from the same settings."""
import os, pty, subprocess, sys, time, select, tempfile, tty
import faulthandler; faulthandler.dump_traceback_later(100, exit=True)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
STUB = os.environ.get('MYSQL_STUB', '/tmp/fakemysql')
BUILD = os.environ.get('MB_BUILD', os.path.join(tempfile.gettempdir(), 'mb_build'))
os.makedirs(BUILD, exist_ok=True)
fails = 0

def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print('FAIL:', msg)

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr); sys.exit('build failed')

inc = ['-I' + STUB, '-I' + ROOT + '/tools', '-I' + ROOT + '/modem', '-w']
SRC = [HERE + '/../mmdvm_bridge.c', HERE + '/../mmdvm_frames.c', HERE + '/../mode_space.c', ROOT + '/tools/bridge_common.c', HERE + '/stubdb.c']
run(['gcc', *inc, '-I' + HERE + '/..', *SRC, '-lutil', '-o', BUILD + '/bridge'])
run(['gcc', *inc, '-I' + HERE + '/..', '-DTX_TIMEOUT_MS=1000U', *SRC, '-lutil', '-o', BUILD + '/bridge_to'])
run(['gcc', *inc, HERE + '/legacy_dump.c', HERE + '/legacy/mmdvm.c', ROOT + '/tools/RingBuffer.c', HERE + '/stubdb.c', '-o', BUILD + '/legacy'])

def cfgfile(rows):
    p = os.path.join(BUILD, 'cfg.txt')
    with open(p, 'w') as f:
        for r in rows:
            f.write('|'.join(r) + '\n')
    return p

def rows(modem, extra=()):
    base = [('config', 'modem', modem), ('config', 'mode', 'duplex'), ('config', 'rxInvert', 'true'),
            ('config', 'txInvert', 'true'), ('config', 'pttInvert', 'false'), ('config', 'useCOSAsLockout', 'true'),
            ('config', 'debug', 'true'), ('config', 'trace', 'false'), ('config', 'txdelay', '120'),
            ('config', 'rxLevel', '60'), ('config', 'rxDCOffset', '3'), ('config', 'txDCOffset', '-4'),
            ('config', 'cwIdTxLevel', '40'), ('config', 'rxFrequency', '449900000'), ('config', 'txFrequency', '444900000'),
            ('DMR', 'txLevel', '55'), ('DSTAR', 'txLevel', '45'), ('P25', 'txLevel', '35'), ('M17', 'txLevel', '65'),
            ('FM', 'txLevel', '30'), ('YSF', 'txLevel', '52'), ('NXDN', 'txLevel', '48'), ('YSF', 'loDev', 'true')]
    return base + list(extra)

def legacy(kind, flags, cfgpath):
    env = dict(os.environ, STUBCFG=cfgpath)
    out = subprocess.run([BUILD + '/legacy', kind, flags], capture_output=True, text=True, env=env).stdout.split()
    return [bytes.fromhex(x) for x in out]

def read_frames(fd, want_bytes, timeout=3.0):
    data = b''
    end = time.time() + timeout
    while len(data) < want_bytes and time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            data += os.read(fd, 512)
    return data

def drain(fd, quiet=0.3):
    data = b''
    while True:
        r, _, _ = select.select([fd], [], [], quiet)
        if not r:
            return data
        data += os.read(fd, 512)

class Bridge:
    def __init__(self, modem, extra=(), exe='bridge'):
        self.cfgpath = cfgfile(rows(modem, extra))
        self.out = os.path.join(BUILD, 'out.txt')
        open(self.out, 'w').close()
        self.master, slave = pty.openpty()
        tty.setraw(self.master)
        self.slave_name = os.ttyname(slave)
        self.link = os.path.join(BUILD, 'hostport')
        env = dict(os.environ, STUBCFG=self.cfgpath, STUBOUT=self.out)
        # point the bridge's real port at the fake modem
        rowsx = rows(modem, extra) + [('config', 'port', self.slave_name[5:]), ('config', 'baud', '460800')]
        cfgfile(rowsx)
        self.pend = b''
        self.p = subprocess.Popen([BUILD + '/' + exe, '-l', self.link], env=env, stderr=subprocess.PIPE)
        time.sleep(0.5)
        self.host = os.open(self.link, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.host)
    def modem_read(self, n, t=3.0):
        # the bridge asks the modem for its status now and then; those requests are not what is tested here
        end = time.time() + t
        while len(self.pend) < n and time.time() < end:
            self.pend += read_frames(self.master, 1, 0.1)
            frames = []
            rest = self.pend
            while len(rest) >= 2 and rest[1] >= 3 and len(rest) >= rest[1]:
                f = rest[:rest[1]]; rest = rest[rest[1]:]
                if f != bytes([0xE0, 3, 1]):
                    frames.append(f)
            self.pend = b''.join(frames) + rest
        out, self.pend = self.pend[:n], self.pend[n:]
        return out
    def stop(self):
        self.p.terminate(); self.p.wait(timeout=3)

def split(data):
    fr = []
    while data:
        n = data[1]
        fr.append(data[:n]); data = data[n:]
    return fr

print('section 1', flush=True)
# 1. start-up frames equal the old host's, hotspot board
b = Bridge('mmdvmhs')
exp = legacy('hs', '', b.cfgpath)
got = split(b.modem_read(26 + 17 + 3))
check(len(got) == 3, 'hs start-up gives 3 frames, got %d' % len(got))
if len(got) == 3:
    check(got[0] == exp[0], 'hs config frame\n got %s\n exp %s' % (got[0].hex(), exp[0].hex()))
    check(got[1] == exp[1], 'hs frequency frame\n got %s\n exp %s' % (got[1].hex(), exp[1].hex()))
    check(got[2] == bytes([0xE0, 3, 0]), 'version request')
out = open(b.out).read()
check('set|config|streamType|FRAME' in out, 'stream type published')
check('set|config|bridgePort|hostport' in out, 'bridge port published')
check('default|config|rxFrequency' not in out, 'existing settings are not defaulted')
check('default|config|fmTxLevel|50' in out, 'missing settings are defaulted')
check('|modem|' not in out, 'the modem row is not created by the bridge')

print('section 2', flush=True)
# 2. a raw modem frame from the host reaches the modem; answers of the modem stay in the bridge
os.write(b.host, bytes([0xE0, 5, 0x10, 0xAA, 0xBB]))
check(b.modem_read(5) == bytes([0xE0, 5, 0x10, 0xAA, 0xBB]), 'host frame reaches the modem')
os.write(b.master, bytes([0xE0, 4, 0x70, 0x03]))
check(drain(b.host) == b'', 'an ACK of the modem is not passed to the host')

def with_mode(frame, idx, code):
    # a bridge with exactly one mode enabled rests in that mode (a hotspot board only listens for a mode it is in)
    return frame[:idx] + bytes([code]) + frame[idx + 1:]

print('section 3', flush=True)
# 3. a frame split across writes, with a control frame in between, stays whole
os.write(b.host, bytes([0xE0, 6, 0x11]))
time.sleep(0.1)
os.write(b.host, bytes([0x01, 0x02, 0x03]))
check(b.modem_read(6) == bytes([0xE0, 6, 0x11, 0x01, 0x02, 0x03]), 'split frame forwarded whole')

print('section 4', flush=True)
# 4. mode on: config with the mode flag, then the mode command a second later
t0 = time.time()
os.write(b.host, bytes([0xFE, 6, 0x02]) + b'DMR'[:3])
got = split(b.modem_read(26 + 4, 4))
exp = [with_mode(legacy('hs', 'dmr', b.cfgpath)[0], 6, 2)]
check(len(got) == 2, 'mode on gives config + mode command, got %d' % len(got))
if len(got) == 2:
    check(got[0] == exp[0], 'config with DMR enabled\n got %s\n exp %s' % (got[0].hex(), exp[0].hex()))
    check(got[1] == bytes([0xE0, 4, 3, 2]), 'DMR mode command')
    check(time.time() - t0 >= 0.9, 'mode command waits a second')
# already enabled: only the mode command, at once
os.write(b.host, bytes([0xFE, 6, 0x02]) + b'DMR')
got = b.modem_read(4)
check(got == bytes([0xE0, 4, 3, 2]), 'second enable sends the mode command only')

print('section 5', flush=True)
# 5. M17 on top, then M17 off
os.write(b.host, bytes([0xFE, 6, 0x02]) + b'M17')
got = split(b.modem_read(26 + 4, 4))
exp = legacy('hs', 'dmr,m17', b.cfgpath)
check(len(got) == 2 and got[0] == exp[0] and got[1] == bytes([0xE0, 4, 3, 7]), 'M17 enable')
os.write(b.host, bytes([0xFE, 6, 0x03]) + b'M17')
got = split(b.modem_read(26 + 4, 4))
exp = [with_mode(legacy('hs', 'dmr', b.cfgpath)[0], 6, 2)]
check(len(got) == 2 and got[0] == exp[0] and got[1] == bytes([0xE0, 4, 3, 2]), 'M17 disable gives config + rests in the one mode left (DMR)')

print('section 6', flush=True)
# 6. unknown mode name and garbage bytes are ignored, the stream recovers
os.write(b.host, bytes([0xFE, 6, 0x02]) + b'XYZ')
os.write(b.host, bytes([0x00, 0x55, 0xE0, 3, 0x00]))
check(b.modem_read(3) == bytes([0xE0, 3, 0]), 'garbage skipped, next frame forwarded')

print('section 7', flush=True)
# 7. reload reads the settings again
rowsx = rows('mmdvmhs', [('config', 'txdelay', '200'), ('config', 'mode', 'simplex')]) + [('config', 'port', b.slave_name[5:]), ('config', 'baud', '460800')]
cfgfile(rowsx)
os.write(b.host, bytes([0xFE, 3, 0x01]))
got = split(b.modem_read(26 + 17 + 3))
exp = legacy('hs', 'dmr', b.cfgpath)
check(len(got) == 3 and got[0][5] == 20 and (got[0][3] & 0x80), 'reload applies txdelay 200 and simplex')
check(len(got) == 3 and got[0][4] & 0x02, 'reload keeps the DMR flag')
b.stop()

print('section 8', flush=True)
# 8. full board frames equal the old host's, with several modes on
for flags in ('', 'dmr', 'dstar,ysf,p25,nxdn,fm,m17'):
    b = Bridge('mmdvm')
    exp = legacy('full', flags, b.cfgpath)
    split(b.modem_read(40 + 3))                       # start-up
    for name in [f for f in flags.split(',') if f]:
        os.write(b.host, bytes([0xFE, 3 + len(name), 0x02]) + name.upper().encode())
        time.sleep(1.2)
    data = drain(b.master)
    frames = [f for f in split(data) if f[2] == 0x02]
    last = frames[-1] if frames else None
    if flags == '':
        os.write(b.host, bytes([0xFE, 3, 0x01]))
        last = split(b.modem_read(40 + 3))[0]
    if flags == 'dmr':
        exp = [with_mode(exp[0], 7, 2)]
    check(last == exp[0], 'full board config, modes [%s]\n got %s\n exp %s' % (flags, last.hex() if last else None, exp[0].hex()))
    b.stop()

print('section frames', flush=True)
def relay(conn, t):
    n = 4 + len(t)
    return bytes([0x62, n >> 8, n & 255, conn]) + t
def tlv(name, payload=b''):
    n = 8 + len(payload)
    return bytes([0x61, n >> 8, n & 255, 4]) + name + payload
def comm(t):
    return bytes([0x61, 0, 5, 1, t])
def ctl(t, payload=b''):
    return bytes([0xFE, 3 + len(payload), t]) + payload
def nostatus(data):
    return b''.join(f for f in split(data) if f != bytes([0xE0, 3, 1]))
def status(dstar=20, d1=20, d2=20, ysf=20, p25=20, nxdn=20, m17=20):
    return bytes([0xE0, 14, 1, 0, 0, 0, dstar, d1, d2, ysf, p25, nxdn, 0, m17])
ACTIVE_NONE = bytes([0xFE, 4, 7, 0xFF])
def active(c):
    return bytes([0xFE, 4, 7, c])

b = Bridge('mmdvmhs')
split(b.modem_read(26 + 17 + 3))
os.write(b.host, ctl(2, b'DSTAR') + ctl(4, bytes([0]) + b'DSTAR') + ctl(2, b'DMR') + ctl(4, bytes([1]) + b'DMR'))
time.sleep(1.4)
d = drain(b.master)
modes = [f[3] for f in split(d) if len(f) == 4 and f[:3] == bytes([0xE0, 4, 3])]
check(modes and modes[-1] == 0, 'with two modes registered the modem ends up in idle (it scans): %s' % modes)
check(bytes([0xE0, 4, 3, 2]) not in d, 'registering the second mode does not lock the modem into it')
h = drain(b.host)
check(h == ACTIVE_NONE * 2, 'host is told idle for each registration: %s' % h.hex())

os.write(b.master, status())
time.sleep(0.2)

# the service asks for the modem
os.write(b.host, relay(0, comm(2)))
check(b.modem_read(4) == bytes([0xE0, 4, 3, 1]), 'SET_MODE puts the modem in the mode')
check(read_frames(b.host, 4) == active(0), 'host is told the owner')

# a second service cannot take it
os.write(b.host, relay(1, comm(2)))
check(nostatus(drain(b.master)) == b'' and drain(b.host) == b'', 'SET_MODE of another service while busy is ignored')

# D-Star transmit: header, data, end; nothing goes out before the end (200 byte rule), then paced
hdr = bytes(range(41)); dat = bytes(range(100, 112))
os.write(b.host, relay(0, tlv(b'DSTH', hdr)) + relay(0, tlv(b'DSTD', dat)))
time.sleep(0.15)
check(nostatus(drain(b.master)) == b'', 'D-Star frames wait for the end frame')
os.write(b.host, relay(0, tlv(b'DSTE')))
time.sleep(0.5)
got = nostatus(drain(b.master))
exp = bytes([0xE0, 0x2C, 0x10]) + hdr + bytes([0xE0, 0x0F, 0x11]) + dat + bytes([0xE0, 3, 0x13])
check(got == exp, 'D-Star frames in modem layout\n got %s\n exp %s' % (got.hex(), exp.hex()))

# frames of another service's mode are not sent
os.write(b.host, relay(1, tlv(b'DMD1', bytes([0x18]) + bytes(33))))
time.sleep(0.2)
check(nostatus(drain(b.master)) == b'', 'frames of a service that does not own the modem are dropped')

# RX: D-Star header from the modem goes to the owner, a DMR frame is dropped
rx = bytes([0xE0, 46, 0x10]) + bytes(range(43))
os.write(b.master, rx)
exp = relay(0, tlv(b'DSTH', rx[3:44]))
check(read_frames(b.host, len(exp)) == exp, 'D-Star header from the modem reaches the owner as a service frame')
os.write(b.master, bytes([0xE0, 0x25, 0x18]) + bytes(34))
check(drain(b.host) == b'', 'a frame of another mode is not given to the host while a mode is active')
# wrong length frames are dropped
os.write(b.master, bytes([0xE0, 20, 0x10]) + bytes(17))
check(drain(b.host) == b'', 'a D-Star header of the wrong length is dropped')

# raw frame for the modem ("2MOD")
os.write(b.host, relay(0, tlv(b'2MOD', bytes([0x04, 1, 2]))))
check(b.modem_read(5) == bytes([0xE0, 5, 4, 1, 2]), '2MOD frame reaches the modem')

# the owner gives the modem back
os.write(b.host, relay(0, comm(3)))
check(b.modem_read(4) == bytes([0xE0, 4, 3, 0]), 'SET_IDLE: modem idle')
check(read_frames(b.host, 4) == ACTIVE_NONE, 'host is told idle')

# while idle the frames of any registered mode reach their service
os.write(b.master, bytes([0xE0, 0x25, 0x18]) + bytes(range(34)))
exp = relay(1, tlv(b'DMD1', bytes(range(34))))
check(read_frames(b.host, len(exp)) == exp, 'idle: DMR frame goes to the DMR service')

# DMR slot 2 waits for the space of slot 2
os.write(b.master, status(d1=20, d2=0))
time.sleep(0.1)
os.write(b.host, relay(1, comm(2)))
check(b.modem_read(4) == bytes([0xE0, 4, 3, 2]), 'DMR service gets the modem')
read_frames(b.host, 4)
s2 = bytes([0x1A]) + bytes(range(33))
os.write(b.host, relay(1, tlv(b'DMD2', s2)))
time.sleep(0.4)
check(nostatus(read_frames(b.master, 10**6, 0.5)) == b'', 'DMR slot 2 frame waits while slot 2 has no space')
os.write(b.master, status(d1=20, d2=20))
time.sleep(0.4)
got = nostatus(read_frames(b.master, 10**6, 0.5))
check(got == bytes([0xE0, len(s2) + 2]) + s2, 'DMR slot 2 frame goes when there is space\n got %s' % got.hex())

# the service going away frees the modem
os.write(b.host, ctl(3, b'DMR'))
check(read_frames(b.host, 4) == ACTIVE_NONE, 'MODE_OFF of the owner: host is told idle')
b.stop()

# transmit timeout (compiled to one second here)
b = Bridge('mmdvmhs', exe='bridge_to')
split(b.modem_read(26 + 17 + 3))
os.write(b.host, ctl(2, b'YSF') + ctl(4, bytes([2]) + b'YSF'))
time.sleep(1.3)
drain(b.master); drain(b.host)
os.write(b.master, status())
os.write(b.host, relay(2, comm(2)))
b.modem_read(4); read_frames(b.host, 4)
os.write(b.host, relay(2, tlv(b'YSFD', bytes(120))))
time.sleep(0.3)
drain(b.master)
time.sleep(1.5)
d = nostatus(drain(b.master))
check(bytes([0xE0, 4, 3, 3]) in d, 'TX timeout puts the modem back to rest (YSF is the only mode, so it rests in it)')
check(drain(b.host) == ACTIVE_NONE, 'TX timeout tells the host')
b.stop()

print('FAILS:', fails)
sys.exit(1 if fails else 0)
