NXDN gateway

Runs next to nxdn_host (service nxdn-gateway.service, build with the CMakeLists.txt here).
  nxdn_gateway -m <modem 1-10> [-x debug] [-d daemon] [-v]

Link / unlink from the dashboard (reflLinkNXDN "link,<talkgroup>" / "unlink") as for the other modes.
The reflector is looked up by talkgroup number in the dashboard reflector table (refl_type 'NXDN', Nick = number);
if it is not there the gateway reads NXDNHosts.txt ("id address port" per line):
    NXDN/hostsFile from the host configuration, then ./NXDNHosts.txt, then /home/pi/OpenMT/protocols/NXDN/NXDNHosts.txt
NXDNHostsUpdate.sh downloads a list (set NXDN_HOSTS_URL; the default is the Pi-Star list).
Optional host configuration (module NXDN): hostsFile, networkPort (local UDP port, default 14050 + modem - 1).

Behaviour (as NXDNGateway): the link is polled every 5 s, only group calls to the linked talkgroup are passed on to the radio,
and the link is dropped if the reflector is silent for 60 s.  Packets to the reflector carry the linked talkgroup as destination.

Test without hardware:
    echo "10200 127.0.0.1 34010" > /tmp/nxdn_hosts.txt
    gcc -o nxdn_gw_sim test/nxdn_gw_sim.c
    gcc -I. -I<OpenMT>/tools -o nxdn_gateway gateway/nxdn_gateway.c <OpenMT>/tools/test/tools_stub.c -DSTUB_UC=NXDN -DSTUB_LC=nxdn -lpthread
    ./nxdn_gw_sim &
    NXDN_STUB_hostsFile=/tmp/nxdn_hosts.txt ./nxdn_gateway -m 1
