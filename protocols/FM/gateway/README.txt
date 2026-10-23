FM EchoLink gateway

Created by Rick KD0OSS

Links the FM repeater (fm_host) to EchoLink.  It can connect out to a station, and it accepts stations that
connect to this node.  Runs next to fm_host (service fm-gateway.service, build with the CMakeLists.txt here).
  fm_gateway -m <modem 1-10> [-x debug] [-d daemon] [-v]
fm_host and fm_gateway talk over TCP port 18700 + modem - 1 (fm_host listens, the gateway connects).

Needs
  * an EchoLink account: a validated callsign with the -L (link) or -R (repeater) suffix, and its password
  * libgsm for the GSM 06.10 codec:  sudo apt install libgsm1-dev
  * an OpenMT (sample) modem.  The link works through fm_host's own repeater audio, so it is not available with the
    MMDVM modem, where the firmware does the repeating.
  * for incoming connections: UDP ports 5198 and 5199 reachable from the Internet (forward them in the router)

Settings (dashboard, Miscellaneous tab, module EchoLink; created with defaults the first time the gateway runs)
  callsign       EchoLink login, for example KD0OSS-L            (required)
  password       EchoLink password                               (required)
  name           text sent after the callsign                    (default OpenMT)
  location       text shown in the directory                     (default OpenMT FM repeater)
  servers        directory servers, comma separated, host or host:port   (default servers.echolink.org)
  dtmf           1 = DTMF commands from the radio, 0 = off       (default 1)
  allowUnlisted  1 = accept incoming stations that are not in the directory list (default 0)
  allow          callsigns allowed to connect in, comma separated, * at the end matches a prefix; empty = anyone
  deny           callsigns that are refused
  idleTimeout    minutes without audio before the link is dropped, 0 = never (default 0)
  audioPort      EchoLink audio port, the control port is the next one (default 5198)
  bindAddress    local address for the UDP ports (default 0.0.0.0)
Settings for the audio are in the FM section (Mode tab): linkRxLevel (radio to EchoLink), linkTxLevel (EchoLink to
radio), linkPrebuf (ms of audio collected before the transmitter is keyed, default 120), dtmfEnable.
Changes in the FM section are picked up at once; for the EchoLink settings restart the gateway.

Using it
  Dashboard    the FM Gateway box has a field for a node number or callsign and a Link / Unlink button.
               Nothing about EchoLink is in the dashboard code: fm_host stores two config rows for module FM with
               display_type 'linkinput' (linkLabel, linkPattern) and the dashboard builds the box from them.
               Any mode that has no reflector table can do the same.
  DTMF         key the radio and send  <node number> #  to connect,  #  to disconnect,  *  to clear what was entered.
               Digits are muted in the repeater audio and in the audio sent to EchoLink.
  Incoming     a station that connects is accepted when it is allowed, is listed in the directory with the address
               it connects from, and nobody else is connected.  Only one station is linked at a time; another one
               is sent BYE.  The dashboard shows the callsign.
  While linked, what the radio receives goes to the station and what the station says is transmitted by the repeater
  (with the CTCSS tone, the hang time and the CW ID of the FM host).  The directory shows the node as BUSY.

Protocol notes (as SvxLink's echolib)
  UDP 5198 audio: 12 byte RTP header (0xC0, payload type 0x03, sequence number) and four 33 byte GSM frames = 80 ms.
                  Also the "oNDATA" information and chat text.
  UDP 5199 control: RTCP packets.  SDES (callsign and name) opens a connection and is repeated every 10 s as the
                  keep-alive; BYE ends it.  A station that is silent for 50 s is dropped.
  TCP 5200 directory: "l<call>\254\254<password>\r<state>..." registers ONLINE or BUSY (repeated every 5 minutes) and
                  "s" gets the station list used to look up node numbers and to check incoming stations.
Speex audio is not used.  The gateway cannot be tested here against the real EchoLink network; test/fm_el_sim.py
plays a directory server, a station and the modem host so fm_host and fm_gateway can be run without them.

Test without hardware or the EchoLink network (needs python3 and libgsm; the real fm_host and fm_gateway are built
against the stubbed database):
    protocols/FM/test/run_sim.sh
