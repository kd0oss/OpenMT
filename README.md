# OpenMT
Open Modem Tools

OpenMT is an attempt to make a suite of programs that will make developing modem firmware and managment tools that are more flexible than current offering.

This software will take a more modular approach breaking distinct functions out into individual programs. For example most current modem hosts handle everything from modem managemnt to protocol specific encoding and decoding to network gateway management. So if you want to add or remove a protocol to have to edit the host program as well as the firmware.

We will attempt to make modem firmware with functions that are field programmable so we can add mod/demod types and digital protocols on the fly. This will be the most challenging part of the project and will likly require the higher end modem hardware that is available.

The modem host will manage the hardware much like other existing host, sending and receiving packet frames. It will also be responsible for modem configuration and status logging. It will also listen for network connections providing or requiring services. The most likely service will be a digital protocol handler. For example an M17 handler that will monitor the host for any incoming M17 frames that need to be delt with. It will also provide M17 frames as needed for duplex mode or from a linked reflector. Only one service will be enabled for transmit at any given time. A service can log any data to make it available to the user interface software.

The user interface software can be anything from a web app to a cell phone program. All of the configuration, status and call history will be stored in a database. MariaDB SQL server will be used for this purpose allowing concurrent access by any authorized program. Each program or service will have it's own section in the database for storing configuration and data.

With the exception of the modem host, which must run on the PC with the attached modem, any of the other programs can run on any machine with network access to host.
