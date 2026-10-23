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
# hosts_all.py - the descriptions of the protocol host flow charts.
import sys
from host_common import render

BG_NET = "Network call with no end (gateway unlinked, socket dropped, 1.5 s of silence): the host ends it itself, so the dashboard goes idle"

def dash(upd, refl, what):
    return ("Dashboard commands from the database: %s (reload the settings), %s (%s), each acknowledged in the database" % (upd, refl, what))

P25 = dict(
    file="OpenMT_P25_Host_Flow.pdf",
    title="P25 Host (p25_host)  -  flow chart",
    subtitle="Decodes and builds P25 frames for one modem.  Talks to the Modem Host on :18000+N-1 and to the P25 gateway on :18300+N-1.  Decodes everything but the voice codec.",
    start=["Start\np25_host -m N [-d] [-x]",
           "Ports from N.  Read modem type, NAC, levels, callsign, frequencies from the database",
           "Start startClient: connect to the Modem Host, register MODE P25 and the radio settings",
           "Start the gateway server (startTCPServer) and timerThread",
           "Watch the database for changed settings and link commands, until stopped"],
    rx_title="Modem  ->  gateway   (traffic from the radio)",
    gw_name="P25 gateway\nTCP :18300+N-1",
    rx=[
        "startClient: read a packet from the Modem Host",
        {"text": "Packet type?", "kind": "dec", "out": "SAMP BITS",
         "up": ("command", "Duplex or simplex flag from the Modem Host"),
         "down": ("frame", "mmdvm frame already formed: goes straight to decodeFrame()")},
        "samples(): sync search, header and LDU states.  processBits(): bit state machine.  Frame cut at the NID",
        "decodeP25(): NID decoded (guessed from the unit before if bad), FEC errors counted for the BER",
        {"text": "Data unit?", "kind": "dec", "out": "LDU",
         "up": ("Header, TSDU, PDU", "Header: key the modem (SETMODE), start the call.  TSDU and PDU header: trellis decoded and rebuilt"),
         "down": ("Terminator", "Save last call and history with BER and duration.  Modem back to idle")},
    ],
    tx_title="Gateway  ->  modem   (traffic from the reflector)",
    tx=[
        "processGatewaySocket: read a packet from the gateway",
        {"text": "Packet type?", "kind": "dec", "out": "frame",
         "up": ("link, status", "CONN, DISC, NAK, commands: reflector link state to the dashboard.  DISC or socket drop ends a call in progress"),
         "down": ("other mode", "Dropped when P25 does not hold the modem")},
        "decodeFrame(isNet): decodeP25() reads the call.  The frame is rebuilt from what was decoded (sync, NID, link control, busy bits)",
        {"text": "Modem type?", "kind": "dec", "out": "end",
         "up": ("mmdvm, frame mode", "Write the rebuilt frame to the Modem Host as it is", True),
         "down": ("openmt, openmths", "rxBuffer -> rxThread -> processTx(): start sync, then samples or bits", True)},
        "End of call: terminator rebuilt and sent so the modem stops.  Last call and history saved.  txOn off",
    ],
    bg=["timerThread (1 ms): hang timer; quiet for the hang time -> SETIDLE to the Modem Host",
        BG_NET + " (p25NetEnd)",
        dash("updateConfP25", "reflLinkP25", "link or unlink, passed to the gateway")],
    footer="In duplex the host rebuilds every frame from what it decoded before it relays it, as MMDVMHost does.  Errors go to the error log with reportError().",
)

DMR = dict(
    file="OpenMT_DMR_Host_Flow.pdf",
    title="DMR Host (dmr_host)  -  flow chart",
    subtitle="Two time slots, TDMA timing in the host.  Talks to the Modem Host on :18000+N-1 and to the DMR gateway on :18400+N-1 (the gateway links to BrandMeister).",
    start=["Start\ndmr_host -m N [-d] [-x]",
           "Ports from N.  Read DMR ID, colour code, levels, slot phase and delay, TX hang from the database",
           "Start startClient (MODE DMR and radio settings), gateway server, rxThread, timerThread, dmrTxThread",
           "Watch the database for changed settings and link commands, until stopped"],
    rx_title="Modem  ->  gateway   (traffic from the radio)",
    gw_name="DMR gateway\nTCP :18400+N-1",
    rx=[
        "startClient: read a packet from the Modem Host",
        {"text": "Packet type?", "kind": "dec", "out": "SAMP BITS",
         "up": ("command, timing", "Duplex or simplex flag.  TX timing report from the modem fixes the TDMA slot position"),
         "down": ("frame", "mmdvm DMD1, DML1, DMD2, DML2 frames go to decodeFrame()")},
        "dmrRxSamples(): two receive contexts follow the slots (sync search, slot from the TDMA phase).  dmrRxBits(): bit state machine",
        "dmrRxDeliverFrame(): a 33 byte burst as a DMD or DML frame of slot 1 or 2.  decodeDMR(): slot type and colour code",
        {"text": "Burst?", "kind": "dec", "out": "voice, data",
         "up": ("Voice LC header", "Call starts: LC read (source, destination), ID looked up, dashboard record, modem keyed"),
         "down": ("Terminator, lost", "Call ends: BER, last call and history saved, modem released")},
    ],
    tx_title="Gateway  ->  modem   (traffic from BrandMeister)",
    tx=[
        "processGatewaySocket: read a packet from the gateway",
        {"text": "Packet type?", "kind": "dec", "out": "DMD DML",
         "up": ("link, status", "CONN, DISC, NAK, commands: link state to the dashboard.  DISC or socket drop ends calls in progress"),
         "down": ("other mode", "Dropped when DMR does not hold the modem")},
        "decodeFrame(isNet) -> decodeDMR(): simplex works on slot 1 only.  In duplex each burst is rebuilt: slot type, LC, CSBK, data, EMB, trellis 3/4",
        {"text": "Modem type?", "kind": "dec", "out": "end",
         "up": ("mmdvm, frame mode", "Burst sent to the Modem Host as a data frame", True),
         "down": ("openmt, openmths", "Burst queued in the slot FIFO.  dmrTxThread sends it in its own slot: CACH, slot 1, CACH, slot 2", True)},
        "Terminator or lost frame: the call ends the normal way.  Last call and history saved, modem released",
    ],
    bg=["timerThread (1 ms): hang timer, held while a call is on; quiet -> SETIDLE",
        BG_NET + " (dmrNetEnd, per slot)",
        dash("updateConfDMR", "reflLinkDMR", "link or unlink, passed to the gateway")],
    footer="The TDMA scheduler (dmrTxThread) runs all the time with an absolute-deadline clock, so the 30 ms slot and 60 ms cycle do not drift.  Rate 1 data and the PI header are repeated as received.",
)

DSTAR = dict(
    file="OpenMT_DSTAR_Host_Flow.pdf",
    title="D-STAR Host (dstar_host)  -  flow chart",
    subtitle="Talks to the Modem Host on :18000+N-1 and to the D-STAR gateway on :18100+N-1 (the gateway links to ircDDB).",
    start=["Start\ndstar_host -m N [-d] [-x]",
           "Ports from N.  Read modem and radio settings, callsigns from the database",
           "Set up buffers, filters and timers.  Start startClient (MODE D-STAR and radio settings)",
           "Start the gateway server and timerThread",
           "Watch the database for changed settings and link commands, until stopped"],
    rx_title="Modem  ->  gateway   (traffic from the radio)",
    gw_name="D-STAR gateway\nTCP :18100+N-1",
    rx=[
        "startClient: read a packet from the Modem Host",
        {"text": "Packet type?", "kind": "dec", "out": "SAMP BITS",
         "up": ("command", "Duplex or simplex flag from the Modem Host"),
         "down": ("header, data", "mmdvm firmware frames go straight to processHeader() and processData()")},
        "samples(), decodeHeader(), decodeData() or the bit versions: frame sync, header (FEC decoded), then a voice frame every 20 ms",
        {"text": "Unit?", "kind": "dec", "out": "voice",
         "up": ("Header", "processHeader(): MY, UR, RPT1, RPT2 split out.  Call starts only if RPT1 is this station: modem keyed, dashboard record"),
         "down": ("End sync, lost", "processEOT(): BER, last call and history saved, end sent to the gateway, state reset")},
        "processData(): BER counted, data sync checked or made, slow data text and GPS decoded, dashboard updated.  Frame queued for the gateway",
    ],
    tx_title="Gateway  ->  modem   (traffic from the reflector)",
    tx=[
        "processGatewaySocket: register the gateway, read a packet",
        {"text": "Packet type?", "kind": "dec", "out": "voice",
         "up": ("link, status", "Reflector replies update the link status on the dashboard.  Commands acknowledged"),
         "down": ("other mode", "Dropped when D-STAR does not hold the modem")},
        "Header, voice frame, end of transmission decoded as network traffic: processHeader(), processData(), processEOT() with isNet",
        {"text": "Modem type?", "kind": "dec", "out": "end",
         "up": ("mmdvm, frame mode", "Header, voice frames and end sent to the Modem Host as frames", True),
         "down": ("openmt, openmths", "processTx(): symbol stream written as SAMP or BITS packets", True)},
        "End of call (processEOT): BER, last call and history saved, modem released",
    ],
    bg=["timerThread (1 ms): every timer counts; hang timer -> SETIDLE when quiet",
        BG_NET + " (dstarNetEnd)",
        dash("updateConfDSTAR", "reflLinkDSTAR", "link or unlink, passed to the gateway")],
    footer="The slow data header is checked by its type, so a header split across frames is complete only when all of it has arrived.  Errors go to the error log with reportError().",
)

M17 = dict(
    file="OpenMT_M17_Host_Flow.pdf",
    title="M17 Host (m17_host)  -  flow chart",
    subtitle="Talks to the Modem Host on :18000+N-1 and to the M17 gateway on :18200+N-1 (the gateway links to an M17 reflector).",
    start=["Start\nm17_host -m N [-d] [-x]",
           "Ports from N.  Read callsign, can, levels and radio settings from the database",
           "Set up the filters.  Start startClient (MODE M17, ADF7021 registers, sync word)",
           "Start the gateway server, rxThread and timerThread",
           "Pass dashboard commands to the gateway until the Modem Host connection is lost"],
    rx_title="Modem  ->  gateway   (traffic from the radio)",
    gw_name="M17 gateway\nTCP :18200+N-1",
    rx=[
        "startClient: read a packet from the Modem Host",
        {"text": "Packet type?", "kind": "dec", "out": "SAMP BITS",
         "up": ("command", "Duplex or simplex flag from the Modem Host"),
         "down": ("frame", "M17 frame from the firmware: dropped if a network call has the transmitter (NET TX active)")},
        "samples(): filtered samples, sync word search (link setup, stream, packet), levels and bits.  processBits(): same on bits",
        "decodeFrame(): link setup, stream or packet frame decoded.  Callsigns, text, GNSS position and SMS followed",
        {"text": "Frame?", "kind": "dec", "out": "stream",
         "up": ("Link setup", "Call starts: dashboard record, modem keyed.  Frame sent to the gateway"),
         "down": ("End, lost", "Last call and history saved, modem released")},
    ],
    tx_title="Gateway  ->  modem   (traffic from the reflector)",
    tx=[
        "processGatewaySocket: read a packet from the gateway",
        {"text": "Packet type?", "kind": "dec", "out": "frame",
         "up": ("CONN, DISC, NACK", "Link status to the dashboard.  DISC or socket drop ends a call in progress"),
         "down": ("RF busy", "Frame dropped while RF is transmitting or the mode is not M17")},
        "decodeFrame(isNet): frame decoded and regenerated so it is clean and numbered right",
        {"text": "Modem type?", "kind": "dec", "out": "end",
         "up": ("mmdvm, frame mode", "Frames sent to the Modem Host as they are", True),
         "down": ("openmt, openmths", "rxThread: preamble first when needed, then modulated samples or bits (processTx)", True)},
        "End of transmission: last call and history saved, modem released",
    ],
    bg=["timerThread (1 ms): ticks the timers; quiet for the hang time -> SETIDLE",
        BG_NET + " (m17NetEnd)",
        dash("updateConfM17", "reflLinkM17", "link or unlink, passed to the gateway")],
    footer="currentTxSource says who has the transmitter (RF or network); frames from the other side are rejected until it is free.  Errors go to the error log with reportError().",
)

def ctl_host(file, proto, mod, port, gwname, rxsw, rxbits, ctlname, rxdetail, netname, netdetail, upd, refl, footer, ysf=False):
    return dict(
        file=file,
        title="%s Host (%s)  -  flow chart" % (proto, mod),
        subtitle="Talks to the Modem Host on :18000+N-1 and to the %s gateway on :%d+N-1.  The call logic is in the control module (%s) that this host drives." % (proto, port, ctlname),
        start=["Start\n%s -m N [-d] [-x]" % mod,
               "Ports from N.  Read callsign, levels, settings from the database",
               "Start clientThread: connect to the Modem Host, register MODE %s" % proto,
               "Start serverThread (gateway), gatewayThread, txThread and timerThread (10 ms)",
               "Watch the database for changed settings and link commands, until stopped"],
        rx_title="Modem  ->  gateway   (traffic from the radio)",
        gw_name="%s gateway\nTCP :%d+N-1" % (proto, port),
        rx=[
            "clientThread: read a packet from the Modem Host",
            {"text": "Packet type?", "kind": "dec", "out": "SAMP BITS",
             "up": ("command", "Duplex or simplex flag from the Modem Host"),
             "down": ("frame", "mmdvm firmware frame: straight to onRxFrame()")},
            rxsw + "  " + rxbits,
            "onRxFrame(): used only if this host may take the modem.  %s" % rxdetail,
            {"text": "Call event?", "kind": "dec", "out": "frames",
             "up": ("start, late entry", "onEvent(): modem switched to %s, dashboard record (source, destination)" % proto),
             "down": ("end, lost, rejected", "onEvent(): last call and history saved.  Modem released after the hang time")},
        ],
        tx_title="Gateway  ->  modem   (traffic from the reflector)",
        tx=[
            "gatewayThread: read a packet from the gateway",
            {"text": "Packet type?", "kind": "dec", "out": "data",
             "up": ("link, status", "Link state to the dashboard.  DISC or socket drop ends a call in progress"),
             "down": ("other mode", "Dropped when %s does not hold the modem" % proto)},
            "%s: %s" % (netname, netdetail),
            {"text": "onRf(): frame queued", "kind": "proc", "out": None},
            {"text": "txThread: modem type?", "kind": "dec", "out": "frames",
             "up": ("mmdvm, frame mode", "Frames sent to the Modem Host as they are", True),
             "down": ("openmt, openmths", "Samples modulated (SAMP) or bits made (BITS).  Preamble first", True)},
        ],
        bg=["timerThread (10 ms): control timers; quiet for the hang time -> SETIDLE to the Modem Host",
            BG_NET + " (1.5 s watchdog in the control)",
            dash(upd, refl, "link or unlink, passed to the gateway")],
        footer=footer,
    )

NXDN = ctl_host("OpenMT_NXDN_Host_Flow.pdf", "NXDN", "nxdn_host", 18500, "NXDN",
    "nxdnRxSamples(): symbol sync, frames of 48 bytes.", "nxdnBitRxBytes(): same from bits.",
    "nxdn_func.c",
    "nxdnCtrlRf(): FSW, LICH, SACCH, FACCH1 and voice frames decoded, call followed",
    "nxdnCtrlNet()", "network frames become RF frames; call state and the 1.5 s network watchdog kept",
    "updateConfNXDN", "reflLinkNXDN",
    "A transmission ends with the end frame, or when a frame is more than 400 ms late (TX_UNDERRUN_MS).  Errors go to the error log with reportError().")
NXDN["rx"][4]["out"] = "frames"

YSF = ctl_host("OpenMT_YSF_Host_Flow.pdf", "YSF", "ysf_host", 18600, "YSF",
    "ysfRxSamples(): symbol sync, frames of 120 bytes.", "Same from bits.",
    "ysf_func.c",
    "ysfCtrlRf(): sync and FICH decoded, voice FR or V/D calls, header and terminator followed; a call can start late",
    "ysfCtrlNet()", "151 byte network bodies become RF frames; call state and the 1.5 s network watchdog kept",
    "updateConfYSF", "reflLinkYSF",
    "Wires-X requests from the radio are answered by the gateway.  The host sends each RF frame to the gateway as a 151 byte body.  Errors go to the error log with reportError().")

ALL = [P25, DMR, DSTAR, M17, NXDN, YSF]


FM = dict(
    file="OpenMT_FM_Host_Flow.pdf",
    title="FM Host (fm_host)  -  flow chart",
    subtitle="Analog FM repeater with an EchoLink link.  Talks to the Modem Host on :18000+N-1 and to fm_gateway on :18700+N-1.  Not available on the hotspot modems; the link needs an OpenMT (sample) modem.",
    start=["Start\nfm_host -m N [-d] [-x]",
           "Refuse hotspot modem types.  Read FM settings (access mode, CTCSS, hang, CW ID, link levels, DTMF) from the database",
           "Connect to the Modem Host, register MODE FM",
           "Start fmClientThread (receive), fmTxThread (transmit) and serverThread (EchoLink gateway)",
           "Service loop: fmServiceOnce() until stopped"],
    rx_title="Modem  ->  repeater and EchoLink   (receive)",
    rx_first={"text": "Modem Host\nTCP :18000+N-1", "w": 126},
    rx_last={"text": "fm_gateway\nTCP :18700+N-1", "w": 126},
    rx_l1="packets", rx_l2="ELAU, ELRX, ELDT",
    rx=[
        "fmClientThread: read a packet from the Modem Host",
        {"text": "Packet type?", "kind": "dec", "out": "samples",
         "up": ("command", "Duplex or simplex flag from the Modem Host"),
         "down": ("FMDA, FMST, FMEO, FMRS", "MMDVM firmware packets: fmFirmwareFrame().  The firmware repeats; the host only follows the call for the dashboard")},
        {"text": "fmRxSamples(): discriminator samples at 24 kS/s.  DC blocker, CTCSS detector, noise squelch, voice band filter into the delay ring", "w": 160,
         "down": ("gateway connected", "Decimate to 8 kS/s.  DTMF decoder (Goertzel): a digit goes to the gateway as ELDT and its tones are muted.  fmLinkRx(): audio through an 85 ms delay to the gateway as ELAU while linked; ELRX marks squelch open and closed", True)},
        "fmRxAccess(): CTCSS and squelch combined by the access mode",
        {"text": "Signal?", "kind": "dec", "out": "ELRX",
         "up": ("opens, closes", "fmPostEvent(): call start or end queued for the service loop"),
         "down": ("tone drops", "The latest audio is muted so the squelch tail is not repeated")},
    ],
    tx_title="Repeater and EchoLink  ->  modem   (transmit)",
    tx_first={"text": "Delay ring (receiver)\nLink ring (EchoLink)", "kind": "buf", "w": 126},
    tx_l1="audio", tx_l2="SAMP frames",
    tx=[
        "fmTxThread: every 4 ms (one 96 sample frame, absolute clock) calls fmTxTick()",
        {"text": "Transmitter state?", "kind": "dec", "out": "keyed",
         "up": ("signal opens or link audio", "Take the modem (SETMODE FM).  Wait for the Modem Host to confirm FM mode, then key up"),
         "down": ("signal and link gone", "Hold the carrier for the hang time, then stop")},
        {"text": "fmBuildFrame(): the delayed audio, the link audio, the CTCSS tone when one is set, and the CW ID while it runs", "w": 160,
         "down": ("from the gateway", "ELAU audio from the linked station: 8 to 24 kS/s, pre-buffered (linkPrebuf), mixed with the repeater audio.  ELPT says when the station starts and stops talking")},
        "fmSendSamples(): one 96 sample frame sent to the Modem Host as SAMP",
        "End: the EOT marker frame is sent and the modem is released (SETIDLE)",
    ],
    bg=["fmServiceOnce(): call events written to the database, modem mode checked for the transmitter",
        "Dashboard commands: updateConfFM reloads the settings; reflLinkFM (link,<node or callsign> / unlink) is sent to the gateway as CONN or DISC and answered by its CONN, NACK or DISC.  gatewayThread serves the gateway socket",
        "MMDVM modem: fmSendFirmwareParams() sends the FM settings at start-up, and the call timeout is watched"],
    footer="With the plain MMDVM modem the firmware does the CTCSS and repeater work itself and there is no EchoLink link; with the OpenMT modems this host does it in software.  Errors go to the error log with reportError().",
)
ALL.append(FM)

FMGW = dict(
    file="OpenMT_FM_EchoLink_Gateway_Flow.pdf",
    title="FM EchoLink Gateway (fm_gateway)  -  flow chart",
    subtitle="Links the FM repeater to an EchoLink station.  Connects to fm_host on :18700+N-1; talks to EchoLink over UDP 5198 (audio) and 5199 (control) and to the directory servers over TCP 5200.",
    start=["Start\nfm_gateway -m N [-d] [-x]",
           "Read the EchoLink settings (module EchoLink: callsign, password, servers, allow and deny lists ...) and add missing ones",
           "Start the GSM codec and open the UDP audio and control sockets",
           "Start elinkThread, dirThread and startClient (the connection to fm_host)",
           "Wait until stopped.  On exit: BYE to the station, directory told OFF"],
    rx_title="Repeater  ->  EchoLink station   (audio out, commands in)",
    rx_first={"text": "fm_host\nTCP :18700+N-1", "w": 126},
    rx_last={"text": "EchoLink station\nUDP :5198 audio", "w": 126},
    rx_l1="packets", rx_l2="UDP",
    rx=[
        "startClient: read a packet from fm_host (reconnects every 2 s; the link is dropped if the host goes)",
        {"text": "Packet type?", "kind": "dec", "out": "ELAU",
         "up": ("ELDT digit", "dtmfDigit(): digits then # connect to that node number, # alone disconnects, * clears.  Entries expire after 10 s"),
         "down": ("CONN, DISC, COMM", "Dashboard link or unlink: requestConnect() or dropPeerLocked().  COMM 04 reloads the settings")},
        "ELRX opens or closes a transmission.  ELAU samples (8 kS/s) are collected into 640 sample blocks (80 ms); the last block is padded",
        "elMakeVoice(): four GSM 06.10 frames, RTP header 0xC0 0x03 and a sequence number",
        "sendVoice(): UDP to the station, only while connected",
    ],
    tx_title="EchoLink station  ->  repeater   (audio in, connection handling)",
    tx_first={"text": "EchoLink station\nUDP :5198 / :5199", "w": 126},
    tx_last={"text": "fm_host\nTCP :18700+N-1", "w": 126},
    tx_l1="packets", tx_l2="ELAU, ELPT, CONN",
    tx=[
        "elinkThread: select() on the audio and control ports; tick() every 50 ms",
        {"text": "Which port?", "kind": "dec", "out": "audio",
         "up": ("5199 control", "SDES answers our connect, keeps it alive, or is an incoming call: allow and deny lists, directory check (listed with this address), one station at a time, else BYE.  BYE ends the link"),
         "down": ("timers", "tick(): connect retried 4 times 2 s apart, keep-alive every 10 s, 50 s without a packet ends the link, 400 ms without audio ends a talk, optional idle limit")},
        "handleAudio(): only from the linked station.  oNDATA info text is logged; voice is decoded",
        "elParseVoice(): four GSM frames to 640 samples at 8 kS/s",
        "hostAudio(): ELAU in 160 sample packets.  ELPT 1 before the first packet of a talk, ELPT 0 after it ends",
    ],
    bg_title="Directory, lookup and the database",
    bg=["dirThread: registers the node (ONLINE, BUSY while connected) every 5 minutes and when the state changes; gets the station list on demand and every 10 minutes",
        "Lookup by node number or callsign (a call without -L or -R matches either) in the station list.  Link status goes to fm_host as CONN, NACK or DISC and from there to the dashboard",
        "Settings come from the database, Misc tab, module EchoLink.  Errors go to the error log with reportError()"],
    footer="EchoLink needs a validated callsign with the -L (link) or -R (repeater) suffix and its password.  UDP ports 5198 and 5199 must be reachable from the Internet for incoming connections.",
)
ALL.append(FMGW)

if __name__ == "__main__":
    for spec in ALL:
        render("%s/%s" % (sys.argv[1], spec["file"]), spec)
