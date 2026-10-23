# Created by Rick KD0OSS
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# Draws the Modem Host flow chart (docs/OpenMT_Modem_Host_Flow.pdf).
import sys
from chartlib import Chart

def draw(path):
    c = Chart(path, 1224, 792, "Modem Host (Modem_Host)  -  flow chart",
              "Owns the modem's serial port and serves every protocol host on TCP port 18000 + modem number - 1.  One Modem Host runs for each modem (1 to 10).",
              cellw=170, cellh=66, ox=105, oy=105)

    # ---- lane 1: start-up
    c.lane(0, -0.6, 7, 1.3, "main()  -  start-up, then the serial loop")
    c.node("s1", 0, 0.0, "Start\nModem_Host -m N [-d] [-x] [-r]", "term")
    c.node("s2", 1, 0.0, "Modem name modemN, TCP port 18000+N-1.  Daemon fork if -d.  Signal and crash handlers")
    c.node("s3", 2, 0.0, "Read modem type, port, baud, mode from the database.  Create defaults on first run")
    c.node("s4", 3, 0.0, "Open the serial port (8N1), flush old bytes")
    c.node("s5", 4, 0.0, "Start threads: TX threads (by modem type), timer, TCP server, command")
    c.node("s6", 5, 0.0, "Send start-up configuration to the modem (by modem type)")
    c.node("s7", 6, 0.0, "Serial loop until stopped.  Then clear gateways and modes, free buffers", "term")
    for a, b in (("s1", "s2"), ("s2", "s3"), ("s3", "s4"), ("s4", "s5"), ("s5", "s6"), ("s6", "s7")):
        c.link(a, b)

    # ---- lane 2: modem -> services
    c.lane(0, 0.8, 7, 2.5, "Modem  ->  protocol hosts   (received air traffic)")
    c.node("m", 0, 2.5, "Modem\n(serial UART, or the TCP bridge)", "hw")
    c.node("r1", 1, 2.5, "processMMDVMSerial() or processSerial(): read one frame  0x61, length, type, payload.  Resync if bytes were lost")
    c.node("r2", 2, 2.5, "Frame type?", "dec", w=140)
    c.node("r2a", 2, 1.55, "ACK, NAK, version, debug, status: update free space / credit (setSpace)", w=150)
    c.node("r3", 3, 2.5, "Wrap as an OpenMT frame (0x61 len 0x04 type payload).  Kept only if its mode is the current mode")
    c.node("q1", 4, 2.5, "txModeBuffer", "buf", w=110)
    c.node("r4", 5, 2.5, "modeRxThread (one per service, twice a millisecond): take a frame, write it to the service socket, plus that service's queued commands")
    c.node("h1", 6, 2.5, "Protocol host\nTCP :18000+N-1", "ext", w=130)
    c.link("m", "r1", "bytes")
    c.link("r1", "r2")
    c.link("r2", "r2a", "status", s="t", e="b")
    c.link("r2", "r3", "voice / data")
    c.link("r3", "q1")
    c.link("q1", "r4")
    c.link("r4", "h1", "frames")
    c.node("cm", 3, 1.55, "commandThread, idle and OpenMT modem: frames go to every connected service", w=150)
    c.link("q1", "cm", "when idle", s="t", e="r", dashed=True)

    # ---- lane 3: services -> modem
    c.lane(0, 3.45, 7, 3.65, "Protocol hosts  ->  modem   (transmit)")
    c.node("h2", 0, 5.2, "Protocol host\n(DMR, P25, D-STAR, M17, NXDN, YSF, FM)", "ext", w=140)
    c.node("t1", 1, 5.2, "processClientSocket (one per service): read packet  0x61 len type")
    c.node("t2", 2, 5.2, "Packet type?", "dec", w=140)
    c.node("t2a", 2, 4.15, "MODE: store name and tx level, addMode, setProtocol.  Queue idle / duplex / simplex command", w=165)
    c.node("t2b", 2, 6.2, "COMM: SET_MODE (if idle, this service becomes the current mode, setMode).  SET_IDLE (back to idle, clear rxModeBuffer)", w=165)
    c.node("t3", 3, 5.2, "Samples, bits, frames, end of transmission: convert to the modem's format, check there is room, set txOn")
    c.node("q2", 4, 5.2, "rxModeBuffer", "buf", w=110)
    c.node("t4", 5, 5.2, "modeTxThread (openmt: paced by credit) or modeMMDVMTxThread (frameDelay timer and modem free space) -> modemTxBuffer", w=150)
    c.node("t5", 6, 5.2, "modemTxThread: wait for the UART queue to drain, write the frame to the serial port", w=140)
    c.link("h2", "t1", "packets")
    c.link("t1", "t2")
    c.link("t2", "t2a", "MODE", s="t", e="b")
    c.link("t2", "t2b", "COMM", s="b", e="t")
    c.link("t2", "t3", "data")
    c.link("t3", "q2")
    c.link("q2", "t4")
    c.link("t4", "t5")
    c.node("m2", 6, 6.5, "Modem", "hw", w=110, h=34)
    c.link("t5", "m2", "bytes")
    c.node("t6", 0, 6.5, "A service disconnects: protocol marked inactive, delGateway / delMode, configuration sent again", w=150)
    c.link("h2", "t6", "closes socket", dashed=True)

    # ---- lane 4: housekeeping
    c.lane(0, 7.25, 7, 1.5, "Background threads and the database")
    c.node("k1", 0.5, 8.0, "timerThread, every 1 ms: advance timers; steer the MMDVM frame delay to keep 10-12 frames free; txTimeout ends a stuck transmission (idle)", w=250)
    c.node("k2", 2.75, 8.0, "commandThread, every chkComTimeout: read the dashboard command 'modemHost'.  setConfig -> send configuration to the modem again, tell every service duplex or simplex, acknowledge 'success'", w=300)
    c.node("db", 5.2, 8.0, "MariaDB: config, status, active_mode, dashboard commands, error log", "ext", w=250)
    c.link("k2", "db", "read / ack", both=True)

    c.legend(0, 9.05)
    c.footer("Only one protocol owns the modem at a time (the current mode); a service takes it with COMM SET_MODE when idle and gives it back with SET_IDLE.  All errors go to the error log table with reportError().")
    c.save()

if __name__ == "__main__":
    draw(sys.argv[1])
