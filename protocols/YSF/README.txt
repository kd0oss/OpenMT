Yaesu System Fusion (YSF) for OpenMT: ysf_host + ysf_gateway

Contents
  ysf_host.c ysf_func.c ysf_dsp.c    host service (C4FM 9600 bit/s digital voice/data, FICH, V/D modes 1 and 2, data FR,
                                      voice FR), module "YSF", ports 18000+n-1 (modem) / 18500+n-1 (gateway)
  YSF_pkg/                           CMakeLists.txt, ysf-host.service
  gateway/                           ysf_gateway, CMakeLists.txt, ysf-gateway.service, YSFHostsUpdate.sh, README.txt
  (modem host)                       modem/Modem_Host.c, modem/mmdvm.c and modem/mmdvm.h at the top of the OpenMT tree
                                     already contain the NXDN and YSF support (enable/disable, queue space, frame pacing);
                                     there is nothing to patch
  dashboard/updateHosts_ysf.patch    adds the YSF reflector list (refl_type 'YSF', Nick = id, Name = country-name)
  OPENMT_HS.txt                      modem notes
  test/                              unit tests and simulators (control, DSP loopback, host/modem, gateway)
  The shared tools/fec_tools.c and fec_tools.h at the top of this package have the new golay24128_correct23127().

Build: put ysf/ under protocols/YSF in the OpenMT tree, then
  cmake -S . -B build && cmake --build build         (in protocols/YSF and protocols/YSF/gateway)
Host configuration, module YSF: selfOnly, remoteGateway, repeat, rxLevel, txLevel, txDelay (500), rfPower, loDev,
modeHang, timeout, txShape, rxShape, rxMinAmp, bitFlags0/1 (OpenMT_HS mode data bytes 56/57, see OPENMT_HS.txt).  The callsign is main/callsign.

What was verified (simulation only, no radio or modem hardware):
  - FICH and the data channel payload against G4KLX's MMDVMHost code, bit for bit (compiled and compared).
  - Control, DSP loopback (noise, offsets), host with simulated OpenMT / OpenMT_HS / MMDVM modems and gateway,
    gateway with a simulated reflector (links by id, name, JSON and text lists, polls, data both ways, unlink, NACK).

Limitations
  - Not tried on real YSF radios or an OpenMT_HS modem.
  - The AMBE / IMBE voice is passed as received: no FEC regeneration, so bit errors are not cleaned up.
  - Wires-X and FCS are in the gateway (see gateway/README.txt).  No DTMF, GPS/APRS, Wires-X passthrough.
  - BER is only measured for V/D mode 2.
  - Frame number handling differs slightly from MMDVMHost when a FICH cannot be decoded.
  - The reflector list URL (hostfiles.refcheck.radio) is from G4KLX's script and has not been checked from here.
