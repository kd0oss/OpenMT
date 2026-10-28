#!/usr/bin/env python3
"""Tests openmt_bridge against a fake modem (a pty)."""
import os, pty, subprocess, sys, time, select, tempfile, tty

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
STUB = os.environ.get('MYSQL_STUB', '/tmp/fakemysql')
BUILD = os.path.join(tempfile.gettempdir(), 'omt_bridge_build')
os.makedirs(BUILD, exist_ok=True)
fails = 0

def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print('FAIL:', msg)

r = subprocess.run(['gcc', '-w', '-I' + STUB, '-I' + ROOT + '/tools', HERE + '/../openmt_bridge.c',
                    ROOT + '/tools/bridge_common.c', ROOT + '/modem/mmdvm-bridge/test/stubdb.c', '-lutil', '-o', BUILD + '/bridge'],
                   capture_output=True, text=True)
if r.returncode:
    sys.exit(r.stderr)

def read_n(fd, n, t=3.0):
    data = b''
    end = time.time() + t
    while len(data) < n and time.time() < end:
        if select.select([fd], [], [], 0.1)[0]:
            data += os.read(fd, 1024)
    return data

def drain(fd, quiet=0.3):
    data = b''
    while select.select([fd], [], [], quiet)[0]:
        data += os.read(fd, 1024)
    return data

master, slave = pty.openpty()
tty.setraw(master)
cfg = os.path.join(BUILD, 'cfg.txt')
rows = [('config', 'mode', 'duplex'), ('config', 'rxInvert', 'true'), ('config', 'txInvert', 'false'),
        ('config', 'pttInvert', 'true'), ('config', 'rxLevel', '77'), ('config', 'port', os.ttyname(slave)[5:]),
        ('config', 'baud', '921600')]
def write_cfg(rows):
    with open(cfg, 'w') as f:
        for x in rows:
            f.write('|'.join(x) + '\n')
write_cfg(rows)
out = os.path.join(BUILD, 'out.txt')
open(out, 'w').close()
link = os.path.join(BUILD, 'hostport')
p = subprocess.Popen([BUILD + '/bridge', '-l', link], env=dict(os.environ, STUBCFG=cfg, STUBOUT=out), stderr=subprocess.PIPE)
time.sleep(0.5)
host = os.open(link, os.O_RDWR | os.O_NOCTTY)
tty.setraw(host)

d = read_n(master, 196)
check(len(d) == 196 and d[:3] == bytes([0x61, 0xC4, 0x20]), 'start-up SET_CONFIG frame: %s' % d[:6].hex())
check(d[3] == 0b1011 and d[4] == 77, 'config flags %02x level %d' % (d[3], d[4]))
check(not any(d[5:]), 'rest of config frame is zero')
o = open(out).read()
check('set|config|streamType|SAMPLE' in o, 'stream type published')
check('set|config|bridgePort|hostport' in o, 'bridge port published')
check('set|config|creditPacing|true' in o, 'credit pacing published')
check('default|config|rxFrequency' not in o and 'default|config|baud' not in o, 'saved settings are not defaulted')

os.write(host, bytes([0xFE, 3, 6]))
d = read_n(master, 199)
check(len(d) == 199 and d[:3] == bytes([0x61, 0xC4, 0x20]) and d[196:] == bytes([0x61, 3, 0xD0]), 'HOST_UP: config + version request')

write_cfg([r for r in rows if r[1] != 'rxLevel'] + [('config', 'rxLevel', '55'), ('config', 'mode', 'simplex')])
rows2 = [r for r in rows if r[1] not in ('rxLevel', 'mode')] + [('config', 'rxLevel', '55'), ('config', 'mode', 'simplex')]
write_cfg(rows2)
os.write(host, bytes([0xFE, 3, 1]))
d = read_n(master, 196)
check(len(d) == 196 and d[3] == 0b1010 and d[4] == 55, 'RELOAD: new config %s' % d[:5].hex())
check(drain(master) == b'', 'RELOAD sends no version request')

# host frames reach the modem whole; a 0xFE inside a frame is not a control frame
frame = bytes([0x61, 8, 0x70, 0xFE, 3, 1, 9, 9])
os.write(host, frame)
d = read_n(master, 8)
check(d == frame, 'frame with 0xFE inside is relayed unchanged: %s' % d.hex())
check(drain(master) == b'', 'no config sent for it')

# mode frames are for modems with modes: this one gets none of them
for t in (0x30, 0x40, 0x50):
    os.write(host, bytes([0x61, 4, t | 1, 0]))
check(drain(master) == b'', 'SET_MODE, ADD_MODE and DEL_MODE are dropped')
os.write(host, bytes([0xFE, 8, 5]) + b'probe')
time.sleep(0.3)
check('ack|modemHost|result|error' in open(out).read(), 'a passed-on dashboard command is answered')

# modem bytes reach the host unchanged
os.write(master, bytes([0x61, 5, 0xE0, 1, 2]))
d = read_n(host, 5)
check(d == bytes([0x61, 5, 0xE0, 1, 2]), 'modem frame reaches the host')

# TX timing reports go to the DMR connection only
os.write(host, bytes([0xFE, 3 + 1 + 3, 4, 1]) + b'DMR')
os.write(host, bytes([0xFE, 3 + 1 + 3, 4, 2]) + b'M17')
time.sleep(0.2)
tm_dmr = bytes([0x61, 0x0B, 0xB1]) + bytes(8)
tm_m17 = bytes([0x61, 0x0B, 0xB2]) + bytes(8)
samp   = bytes([0x61, 5, 0x71, 7, 7])
os.write(master, samp + tm_m17 + samp + tm_dmr + samp)
d = read_n(host, 5 + 5 + 11 + 5)
check(d == samp + samp + tm_dmr + samp, 'TX timing of a non-DMR connection is dropped, DMR kept, order kept: %s' % d.hex())
check(drain(host) == b'', 'nothing else arrives')

# a control frame split in two writes
os.write(host, bytes([0xFE, 3]))
time.sleep(0.1)
os.write(host, bytes([1]))
d = read_n(master, 196)
check(len(d) == 196, 'split control frame is found')

p.terminate(); p.wait(timeout=3)
print('FAILS:', fails)
sys.exit(1 if fails else 0)
