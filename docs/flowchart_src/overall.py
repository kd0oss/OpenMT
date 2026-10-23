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
# overall.py - the system flow chart: every host and gateway running together.
import sys
from chartlib import Chart


def draw(path):
    c = Chart(path, 1224, 792, "OpenMT  -  the whole system",
              "Every program that runs on a station, and how data moves between them.  N is the modem number (1 to 10); every port below is shown for modem 1.",
              cellw=200, cellh=62, ox=105, oy=118)

    c.lane(1.55, -0.5 + 0.0, 1.0, 7.1, "Protocol hosts", fill="#EEF3F9")
    c.lane(2.55, -0.5 + 0.0, 1.0, 7.1, "Gateways", fill="#F6F1EA", border="#CDBFA8")
    c.lane(3.55, -0.5 + 0.0, 1.0, 7.1, "Network", fill="#FAF0F3", border="#D9B3BF")
    # ---------------- radio side
    c.node("radio", 0, 2.0, "Radio\n(RF, any of the modes)", "ext", w=150, h=44)
    c.node("hw", 0, 3.4, "Modem\nMMDVM board or hotspot, OpenMT board, SX1255 bridge\nmmdvm, mmdvmhs, openmt, openmths, openmtsx", "hw", w=170, h=70)
    c.node("mh", 1, 3.4, "Modem Host\nModem_Host -m N\nTCP :18000+N-1\nowns the serial port\none mode at a time", "host", w=150, h=84, size=8)
    c.link("radio", "hw", "RF", both=True)
    c.link("hw", "mh", "serial UART\nor TCP bridge", both=True, lside=1)

    # ---------------- protocol rows
    rows = [
        ("DMR", "dmr_host", 18400, "dmr_gateway", "BrandMeister master\n(HomeBrew UDP)"),
        ("D-STAR", "dstar_host", 18100, "dstar_gateway", "ircDDB gateway\n(UDP)"),
        ("P25", "p25_host", 18300, "p25_gateway", "P25 reflector\n(UDP)"),
        ("YSF", "ysf_host", 18600, "ysf_gateway", "YSF reflector, FCS room,\nWires-X (UDP)"),
        ("NXDN", "nxdn_host", 18500, "nxdn_gateway", "NXDN reflector\n(UDP)"),
        ("M17", "m17_host", 18200, "m17_gateway", "M17 reflector\n(UDP)"),
        ("FM", "fm_host", 18700, "fm_gateway", "EchoLink directory and\nstations (UDP 5198/5199,\nTCP 5200)"),
    ]
    for i, (name, host, port, gw, ext) in enumerate(rows):
        r = i * 1.0 + 0.2
        c.node("h" + name, 2.05, r, "%s\n%s" % (host, name), "host", w=130, h=40)
        c.link("mh", "h" + name, None, s="r", e="l", mid=-28)
        if gw:
            c.node("g" + name, 3.05, r, gw, "proc", w=130, h=40)
            c.node("x" + name, 4.05, r, ext, "ext", w=150, h=40)
            c.link("h" + name, "g" + name, ":%d" % port, both=True)
            c.link("g" + name, "x" + name, None, both=True)
        else:
            c.node("g" + name, 3.05, r, "no gateway\n(repeated locally)", "note", w=130, h=36)
    c.text(4.05, 7.6 - 0.35, "", 7)

    # ---------------- database and dashboard
    c.node("db", 2.85, 7.7, "MariaDB   -   settings, live status, last calls, history, dashboard commands, reflector lists, error log", "ext", w=850, h=34)
    c.node("web", 0.3, 8.85, "Dashboard in a browser\ndashboard.html", "proc", w=160, h=40)
    c.node("php", 1.55, 8.85, "data_query.php\nadmin login, settings, Manage Data", "proc", w=190, h=40)
    c.link("web", "php", "HTTP", both=True)
    px_ = c.nodes["php"].x
    c.arrow([(px_, c.nodes["php"].y + 20), (px_, c.nodes["db"].y - 17)], "SQL", both=True)
    c.arrow([(c.nodes["mh"].x, c.nodes["mh"].y - 42), (c.nodes["mh"].x, c.nodes["db"].y + 17)], "config, active mode", dashed=True, both=True, lpos=0.8)
    for col, lbl in ((2.05, "each host: settings,\ncalls, history, commands"), (3.05, "each gateway: link\nstate, reflector lists")):
        x = c.px(col)
        c.arrow([(x, c.py(6.7)), (x, c.nodes["db"].y + 17)], lbl, dashed=True, both=True, lpos=0.5)

    # notes
    c.text(-0.32, 4.75, "How a station runs", 9, True, "#1F4E79")
    c.text(-0.32, 5.0,
           "One Modem Host per modem, then one host and one gateway for each mode in use, all running together.  "
           "A mode takes the modem with SETMODE when it is idle and gives it back with SETIDLE after the hang time.  "
           "Hosts decode and rebuild the frames; gateways only carry them to and from the network.  "
           "Packet: 0x61, length (2 bytes), type length, type (MODE, SAMP, BITS, SEOT, COMM, CONN, DISC ...), payload.",
           7.5, False, "#333333", width=215)
    c.footer("Reflector links are made from the dashboard: the command goes into the database, the host reads it, tells its gateway to link or unlink, and writes the result back.\nWith -m N each port is 18000+N-1 (host to Modem Host), 18100+N-1 (D-STAR), 18200+N-1 (M17), 18300+N-1 (P25), 18400+N-1 (DMR), 18500+N-1 (NXDN), 18600+N-1 (YSF), 18700+N-1 (FM).")
    c.newpage("OpenMT  -  one call through the system",
              "A call from the radio to the network, a call from the network to the radio, and how a call ends and gives the modem back.")
    page2(c)
    c.save()


def page2(c):
    c.cw, c.ch, c.ox, c.oy = 148, 80, 88, 112
    # lane 1
    c.lane(0, -0.6, 7.9, 2.5, "Radio  ->  network")
    c.node("a1", 0, 0.6, "Radio sends\na call", "ext", w=104, h=40)
    c.node("a2", 1, 0.6, "Modem receives it and demodulates (or its firmware decodes it)", w=130)
    c.node("a3", 2, 0.6, "Modem Host: frame kept only if its mode is the current mode, wrapped as an OpenMT packet", w=130)
    c.node("a4", 3, 0.6, "Protocol host: sync, header and NID decoded.  FEC errors counted (BER)", w=130)
    c.node("a5", 4, 0.6, "First frame: SETMODE takes the modem.  Dashboard record written (who, to whom)", w=130)
    c.node("a6", 5, 0.6, "Frames rebuilt from what was decoded.  Voice codec data untouched", w=130)
    c.node("a7", 6, 0.6, "Gateway sends the frames to the reflector or master", w=130)
    c.node("a8", 7, 0.6, "Network", "ext", w=84, h=40)
    ch = ["a1", "a2", "a3", "a4", "a5", "a6", "a7", "a8"]
    for u, v in zip(ch, ch[1:]):
        c.link(u, v)
    c.node("an1", 2, 1.5, "Frames of a mode that does not hold the modem are not passed on", "note", w=136)
    c.node("an2", 5, 1.5, "In duplex the rebuilt frames are also sent back to the modem and repeated on air", "note", w=136)
    c.link("a3", "an1", None, dashed=True, color="#999999")
    c.link("a6", "an2", None, dashed=True, color="#999999")

    # lane 2
    c.lane(0, 2.05, 7.9, 2.4, "Network  ->  radio")
    c.node("b8", 0, 3.1, "Network", "ext", w=104, h=40)
    c.node("b7", 1, 3.1, "Gateway receives from the reflector or master and wraps it for the host", w=130)
    c.node("b6", 2, 3.1, "Protocol host: frame decoded as network traffic and rebuilt clean", w=130)
    c.node("b5", 3, 3.1, "Modem idle: SETMODE.  Another mode holds it: the frame is dropped", w=130)
    c.node("b4", 4, 3.1, "Frame mode: sent to the Modem Host as it is.  SAMP, BITS: modulated by the host", w=130)
    c.node("b3", 5, 3.1, "Modem Host: rxModeBuffer, paced to the modem (credit or free space)", w=130)
    c.node("b2", 6, 3.1, "Modem transmits", "hw", w=110, h=40)
    c.node("b1", 7, 3.1, "Radio", "ext", w=84, h=40)
    ch = ["b8", "b7", "b6", "b5", "b4", "b3", "b2", "b1"]
    for u, v in zip(ch, ch[1:]):
        c.link(u, v)
    c.node("bn1", 3, 4.0, "All the gateways can be connected at once, but the modem sends one mode at a time", "note", w=250, h=32)

    # lane 3
    c.lane(0, 4.6, 7.9, 2.5, "End of a call (either direction)")
    c.node("e1", 0, 5.75, "Terminator frame arrives.  Or, on the network side: gateway DISC, socket drop, 1.5 s of silence", w=130)
    c.node("e2", 1.25, 5.75, "Host ends the call itself, making a terminator if there was none", w=130)
    c.node("e3", 2.5, 5.75, "Last call and history saved (BER, duration).  txOn cleared.  Dashboard shows idle", w=140)
    c.node("e4", 3.85, 5.75, "Quiet for the hang time?", "dec", w=140, h=66)
    c.node("e5", 5.2, 5.75, "Host sends SETIDLE to the Modem Host", w=120)
    c.node("e6", 6.5, 5.75, "Mode idle, rxModeBuffer cleared, modem set idle.  Another mode can take it", w=130)
    ch = ["e1", "e2", "e3", "e4"]
    for u, v in zip(ch, ch[1:]):
        c.link(u, v)
    c.link("e4", "e5", "yes")
    c.link("e5", "e6")
    c.node("e7", 3.85, 6.75, "More activity: the hang timer restarts and the call goes on", "note", w=190, h=34)
    c.link("e4", "e7", "no", s="b", e="t")
    c.footer("A transmission that runs too long is ended by the txTimeout in the Modem Host, which also sets the mode idle.  Errors anywhere go to the error log table with reportError().")


if __name__ == "__main__":
    draw(sys.argv[1])
