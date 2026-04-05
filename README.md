# OpenMT
Open Modem Tools

OpenMT is an attempt to make a suite of programs that will make
developing modem firmware and managment tools that are more
flexible than current offerings.

This software will take a more modular approach breaking distinct
functions out into individual programs. For example most current
modem hosts handle everything from modem managemnt to protocol
specific encoding and decoding to network gateway management. So
if you want to add or remove a protocol you have to edit the host
program as well as the firmware.

OpenMT will attempt to make modem firmware with functions that are
field programmable so you can add mod/demod types and digital
protocols on the fly. This will be the most challenging part of
the project and will likly require the higher end modem hardware
that is available.

The modem host will manage the hardware much like other existing
hosts, sending and receiving packet frames. It will also be
responsible for modem configuration and status logging. It will
listen for network connections providing or requiring services.
The most likely service will be a digital protocol handler. For
example an M17 handler that will monitor the host for any
incoming M17 frames that need to be delt with. It will provide
M17 frames as needed for duplex mode or from a linked reflector.
Only one service will be enabled for transmit at any given time.
A service can log any data to make it available to the user
interface software.

The user interface software can be anything from a web app to a
cell phone program. All of the configuration, status and call
history will be stored in a database. MariaDB SQL server will be
used for this purpose allowing concurrent access by any
authorized program. Each program or service will have it's own
section in the database for storing configuration and data.

With the exception of the modem host, which must run on the PC
with the attached modem, any of the other programs can run on any
machine with network access to host.

Directory Stucture
===================================================================
OpenMT              // project root
    docs            // Documentation
    db              // Database schema location. MariaDB
    dsp_lib         // CMISDSP libaray compiled for Raspberry Pi3
    dashboard       // Dashboard root
        www         // Web base dashboard files
    modem           // Host program and related files
    tools           // Code for functions used by all programs
    protocols
        DMR         // DMR protocol service program
            gateway // DMR gateway program
        DSTAR       // DSTAR protocol service program
            gateway // DSTAR gateway to IRCDDBGateway daemon
        m17         // M17 open source protocol service program
            gateway // M17 gateway program
        P25         // P25 protocol service program
            gateway // P25 gateway program
        ...         // other protocols as needed

SQL Server (MariaDB)
========================================================================
An SQL database will be used to store configuration and history data.
A .ini file stored in the /etc directory will contain the address and
passord to access the database.

Modem types
===========================================================================
This software is intended to work with 4 modem types:
    1. MMDVM        // used mostly for repeaters
    2. MMDVM_HS     // used mostly for hotspots
    3. OPENMT       // This will be new firmware that will run on MMDVM
                       compatiable modems. It will be a bare bones
                       system that will stream ADC/DAC samples to and
                       from the host. This will make it protocol agnostic.
    4. OPENMT_HS    // This will be new firmware that will run on MMDVM_HS
                       compatiable hardware with the same intention of
                       creating a protocol agnostic modem.

Modem Host (multimode)
========================================================================
The modem host program will handle all data flowing accrose the serial
port to and from the modem. It will then route the data to the
appropriate protocol service connected to the host via a TCP connection.
With MMDVM based modems this will be a simple matter of looking at the
frame headers. For the OPENMT types the host will incorporate some of
the functions provided by the MMDVM firmware.

Protocol Service Programs
========================================================================
These programs will be protocol specific and will handle all the
encode and decode functions as well as logging data to the db server.
They will connect to the host via a TCP port selected by the host
program at connection time based on the modem Id passed at startup
and the number of services already connected. Using a network connection
will allow the service to be run on an alternate computer if more
resources are needed.

Protocol Gateway Programs
=======================================================================
The gateway programs will handle the job of communicating to reflectors
or reflector services. They will connect via TCP to the relavent
protocol service program for traffic relay.

Dashboard
=======================================================================
The dashboard currently is a web based, protocol agnostic, display and
configuration tool. It gets all of it's data from the database server.
It can also update certain things on the database with the appropriate
authorization. There is very little static information coded into the
dashboard. Most of the content is created on the fly as service
programs connect to the host. The config table has columns to intruct
the dashboard how to display data for configuration. Such as read or
read-only fieids. The dashboard can handle the display of multiple
modems if needed.

Notes
=======================================================================
This project is intended as an alternative to the MMDVM ecosystem
but not a replacement. Since this project is primarily for HAM radio
use and as such some of the protocol functions may be incomplete as
compared to MMDVM and MMDVMHost programs.

As stated above I would like this project to be as flexible as possible
with regards to adding new radio protocols and services.
