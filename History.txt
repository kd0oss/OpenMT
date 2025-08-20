Project History

2025/08/11 Version 1.0 beta
=================================================================
Development environment is Linux.
All testing done on a Raspberry Pi 3.

Used for testing:
Nginx web server with PHP module
Mariadb SQL database Server
MMDVM based modem (Repeater Builders V5)
Modified MMDVM firmware with M17 packet mode

Files (modem host):
modem_host.cpp  (modem host)
makefile        (Used to compile modem_host)
modem_host.html (Web base dashboard)
data_query.php  (Database interface for web app)

Files (M17 Service):
m17_service.cpp
makefile        (Used to compile m17_service)

Files (M17_Gateway):
m17gateway.cpp
makefile        (Used to compile m17gateway)

Shared files: *** These files are from OpenRTX
Correlator.hpp               M17DSP.cpp           M17FrameDecoder.cpp  M17Interleaver.hpp     M17Prbs.hpp         fir.hpp
M17Callsign.cpp              M17DSP.hpp           M17FrameDecoder.hpp  M17LinkSetupFrame.cpp  M17StreamFrame.hpp  m17gateway.o
M17Callsign.hpp              M17Datatypes.hpp     M17FrameEncoder.cpp  M17LinkSetupFrame.hpp  M17Utils.hpp
M17CodePuncturing.hpp        M17Decorrelator.hpp  M17FrameEncoder.hpp  M17Modulator.cpp       M17Viterbi.hpp
M17Constants.hpp             M17Demodulator.cpp   M17Golay.cpp         M17Modulator.hpp       PwmCompensator.hpp
M17ConvolutionalEncoder.hpp  M17Demodulator.hpp   M17Golay.hpp         M17PacketFrame.hpp     Synchronizer.hpp
*** The ring buffer file are from MMDVM
RingBuffer.h
RingBuffer.impl.h

Progress:
Host program working for M17 voice and Packet mode simplex and duplex.
Web based dashboard shows current status and call history.
Dashboard has password protected settings menu.
Dashboard shows modem config. Read only at the moment.
Working on reflector gateway interface.
