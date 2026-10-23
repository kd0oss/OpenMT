YSF gateway

Runs next to ysf_host (service ysf-gateway.service, build with the CMakeLists.txt here).
  ysf_gateway -m <modem 1-10> [-x debug] [-d daemon] [-v]

Link / unlink from the dashboard (reflLinkYSF "link,<id or name>" / "unlink").
The reflector is looked up in this order:
  1. the dashboard reflector table (refl_type 'YSF', Nick = the 5 digit id)
  2. the file YSF/hostsFile from the host configuration
  3. ./YSFHosts.json, ./YSFHosts.txt
  4. /home/pi/OpenMT/protocols/YSF/YSFHosts.json, YSFHosts.txt
A reflector can be given as its id (00001 or 1), its name (Test-One) or country-name (US-Test-One; XX-Name when the
list says use_xx_prefix).  Both list formats are read: the JSON of hostfiles.refcheck.radio ("reflectors" array) and the old
"id;name;description;address;port;count" text.  YSFHostsUpdate.sh downloads the JSON (set YSF_HOSTS_URL for another list).

Optional host configuration (module YSF): hostsFile, networkPort (local UDP port, default any),
staticReflector (linked at start, and again after an inactivity unlink), inactivityTime (minutes without a call in either
direction, 0 = never), suffix (node suffix added to the callsign in the polls, W1ABC-ND).
The polls carry main/callsign padded to 10 characters.

Behaviour (as YSFGateway): three polls when linking, one every 5 s afterwards; any YSFP answer from the reflector means linked;
no answer in 3 s gives a NACK; the link is lost after 120 s of silence; three YSFU are sent when unlinking.  Data packets
(YSFD, 155 bytes) pass unchanged in both directions; only packets from the reflector's address and port are accepted.

Wires-X (YSF/wiresX, default on)
  The radio's Wires-X requests (DX, ALL list, SEARCH, CONNECT, DISCONNECT, CATEGORY) are answered from the reflector list,
  with the frames and timing of G4KLX's WiresX.cpp (checked frame for frame against the original code).  A connect picked
  on the radio links to the reflector or FCS room; a disconnect unlinks.  Node name shown on the radio: YSF/name
  (default OpenMT); frequencies come from config/rxFrequency and txFrequency.
  A call from the reflector is held back for 3 s after a connect, so it does not mix into the answer.  For an XLX
  reflector (name XLX...), the radio's Wires-X data frames are not sent on to it.
  The list comes from the first of YSF/hostsFile, ./YSFHosts.json, ./YSFHosts.txt, /home/pi/OpenMT/protocols/YSF/...
  and is read again when the file changes.

FCS (YSF/enableFCS = 1)
  A link name that starts with FCS links to an FCS room: FCS00101 or FCS001-01 (FCS001 means FCS00100).  The server is
  <FCS001>.xreflector.net, UDP port 62500 (YSF/fcsServer, host or host:port, for another server; YSF/fcsPort for a fixed
  local port, default any).  PING every 800 ms; the node information (frequencies, locator from main/latitude and
  main/longitude, id from YSF/id or DMR/ID) and YSF/options are sent after every answer; CLOSE x3 to leave.
  The rooms shown on the radio come from FCSRooms.txt (YSF/fcsRoomsFile, ./FCSRooms.txt or
  /home/pi/OpenMT/protocols/YSF/FCSRooms.txt); they take the free ids from 00010 up, like YSFGateway.

Not included: DTMF reflector selection, GPS/APRS, the Wires-X command passthrough, the parrot, cross-mode links.

Test without hardware (ysf_gw_sim writes the list files; the gateway is started with the YSF_STUB_<key> variables
that tools_stub.c turns into configuration values):
    gcc -I. -Igateway -o ysf_gw_sim test/ysf_gw_sim.c gateway/ysf_wiresx.c gateway/ysf_reflist.c ysf_func.c <OpenMT>/tools/fec_tools.c -lm
    gcc -I. -I<OpenMT>/tools -o ysf_gateway gateway/ysf_gateway.c gateway/ysf_reflist.c gateway/ysf_wiresx.c ysf_func.c \
        <OpenMT>/tools/test/tools_stub.c -DSTUB_UC=YSF -DSTUB_LC=ysf -DSTUB_DB <OpenMT>/tools/fec_tools.c -lm -lpthread
  links, voice, polls, lists:   ./ysf_gw_sim &      YSF_STUB_wiresX=0 YSF_STUB_hostsFile=/tmp/ysf_hosts.json ./ysf_gateway -m 1
  static reflector:             ./ysf_gw_sim static &   YSF_STUB_staticReflector=Test-One ./ysf_gateway -m 1
  FCS:                          ./ysf_gw_sim fcs &   YSF_STUB_enableFCS=1 YSF_STUB_fcsServer=127.0.0.1 YSF_STUB_options=OPT=1
                                  YSF_STUB_latitude=40 YSF_STUB_longitude=-90 YSF_STUB_rxFrequency=435000000
                                  YSF_STUB_txFrequency=435000000 YSF_STUB_wiresX=0 ./ysf_gateway -m 1
  Wires-X:                      ./ysf_gw_sim wx &    YSF_STUB_enableFCS=1 YSF_STUB_fcsServer=127.0.0.1
                                  YSF_STUB_fcsRoomsFile=/tmp/ysf_fcsrooms.txt YSF_STUB_hostsFile=/tmp/ysf_hosts.json ./ysf_gateway -m 1
