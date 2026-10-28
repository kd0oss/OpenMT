# Modem bridges

Goal: Modem_Host knows nothing about radio protocols (DMR, M17, ...) or
firmware families (MMDVM, OpenMT, OpenMT_HS). It only knows three kinds of
packet stream. A small bridge program per modem family does the rest.

| Stream | Meaning | Bridge |
|--------|---------|--------|
| FRAME  | whole service frames relayed to a bridge that speaks the modem's own framing (relay frames, 0x62 start) | mmdvm_bridge |
| BIT    | bit-stream frames with a connection id (0x61 start) | openmths_bridge |
| SAMPLE | audio sample frames (0x61 start) | openmt_bridge, sx1255_bridge |

## How a bridge works

1. Reads its settings from `modem_host.config` (module `config`) at start-up,
   creating missing ones with `bridgeDefault()` (never overwrites saved values).
2. Creates a virtual serial port (`bridgeOpenPty()`), optionally with a stable
   symlink. Modem_Host's `port` setting points at it.
3. Opens the real serial port itself (its own port/baud settings) and does all
   modem-specific set-up, timing and calibration.
4. Publishes `streamType` (FRAME, BIT or SAMPLE) with
   `bridgePublishStreamType()`.
5. Re-reads its settings when it receives the control frame RELOAD from the
   host (`bridgeCtlParse()`); it must not forward control frames to the modem.
   HOST_UP (sent once when Modem_Host starts) means the host knows nothing about
   the modem: the bridge brings the modem to a fresh state.

## What Modem_Host does (implemented)

* `resolveStreamType()`: waits for the bridge to publish `streamType` (up to a
  minute) and reads the facts the bridge publishes (see below).
* `configWatchThread()`: every 2 s compares `hostConfigDigest()` of the
  modem's `config` rows; on a change it queues the reload frame for the bridge.
* At start it sends HOST_UP once, before the TCP server accepts the first service (HOST_UP
  makes the bridge forget the registered modes, so a service that registered first would be
  forgotten); dashboard `setConfig` sends RELOAD.

## Code

* `tools/bridge_common.[ch]`: stream names, control frame build/parse, pty,
  DB helpers. `tools/tools.c`: `hostConfigDigest()`.

## MMDVM bridge (FRAME) - implemented

`modem/mmdvm-bridge/mmdvm_bridge.c`. It owns the real serial port (`port`,
`baud`), the config and frequency frames (byte-for-byte what `set_Config`,
`set_ConfigHS` and `setFrequency` built; checked by
`test/mb_test.py`), the enabled-mode list and the mode commands. With a
bridge present Modem_Host opens `bridgePort` instead of `port`, sends the
bridge control frames (reload, mode on/off by name) and no longer builds
MMDVM config frames itself.

Control frames: `FE <len> <type> [payload]`; RELOAD `01`, MODE_ON `02`,
MODE_OFF `03` (payload = mode name), MODE_CONN `04` (conn byte + mode name),
COMMAND `05` (dashboard command text), HOST_UP `06`, and from the bridge to the
host ACTIVE `07` (one byte: the connection that owns the modem, `FF` = idle).

Relay frames (FRAME streams): `62 <len hi> <len lo> <conn> <service frame>` in
both directions. The host does not look inside the service frame (`61 len len
typeLen type payload`): it only knows which connection it came from or goes to.
The bridge (`mmdvm_frames.c`) turns the service frames into MMDVM frames and
back; the byte layouts are those the host used before, including their quirks.
The bridge also

* decides which service owns the modem (SET_MODE is honoured only while idle,
  SET_IDLE only from the owner) and tells the host with ACTIVE;
* keeps the modem at rest between calls: in the idle mode, which scans, when several
  modes are enabled, but in the mode itself when only one is enabled (a hotspot board
  listens only for the mode it is in, so a single-mode hotspot would hear nothing
  while idle);
* gives the modem the frames of the owner only, and the owner (or, when idle,
  the service of that mode) the modem's frames;
* queues the transmit frames and paces them: it asks for the modem status after
  every frame and steers the delay by the free space of the mode (NXDN and YSF
  have their own rules; DMR slot 2 is checked against slot 2). D-Star waits for
  200 bytes or the end frame, M17 for 500 bytes or the end frame;
* decodes the status reply (the free space of each mode, `mode_space.c`);
* ends a transmission that runs longer than 300 s (`TX_TIMEOUT_MS`);
* handles `2MOD` frames (a service's own raw frame for the modem).

Tests: `test/mb_test.py` (frames of the bridge against the old host code, the
translation, ownership, pacing and timeout against a fake modem) and
`test/run_e2e.sh` (the real Modem_Host and mmdvm_bridge between fake services
and a fake modem).

Known differences from the old direct path: a reload also re-sends the
hotspot frequencies, so a frequency change in the dashboard takes effect;
an FM enable on a hotspot board sends the hotspot config frame (the old code
sent the 40 byte full-board frame there).

## OpenMT_HS bridge (BIT) - implemented

`modem/openmths-bridge/openmths_bridge.c`. It owns the real port, SET_CONFIG
(`61 04 20 81`, sent on HOST_UP only, since it clears the firmware's mode table),
the rx offsets applied to register 0 of each mode's ADD_MODE block, and the
MEAS calibration/probe state machine. The host sends MODE_CONN then the raw
ADD_MODE frame; the bridge remembers the block and re-sends it with the current
offset when offsets are reloaded. The host forwards `reloadOffsets`,
`calibrate ` and `probe ` dashboard commands as COMMAND frames. Tested with
the firmware simulator (`BRIDGE=1 modem/OPENMT_HS/test/run_sim.sh`).

## OpenMT bridge (SAMPLE) - implemented

`modem/openmt-bridge/openmt_bridge.c`. Serial modem with the 196 byte SET_CONFIG
(flags + RX level): the bridge sends it, and the firmware version request, on
HOST_UP, and the config again on RELOAD. Frames pass through unchanged (host
to modem frame by frame so a control frame is never mistaken for data).
Test: `modem/openmt-bridge/test/ob_test.py`.

## SX1255 bridge (SAMPLE) - implemented, needs hardware testing

`modem/sx1255-openmt-bridge` keeps its radio settings in the config database
(`app/bridge_db.c`) instead of the 21 byte SET_CONFIG frame the host built. The
command line options give the defaults for rows that do not exist; saved rows
win. RELOAD and HOST_UP read the rows again and reconfigure the SX1255 only if
something changed. It publishes `streamType` SAMPLE and `bridgePort`. An old
host that still sends the 21 byte frame keeps working.

## Modem_Host without modem names - implemented

Modem_Host no longer has code for any modem family. At start it waits for a
bridge to publish `streamType`, then reads the other facts the bridge
publishes (every bridge writes all of them, so nothing stays from an earlier
bridge):

| Setting | Meaning |
|---------|---------|
| `streamType` | FRAME, BIT or SAMPLE |
| `bridgePort` | name of the virtual port below /dev that the host opens |
| `creditPacing` | the modem has a fixed TX buffer: poll it for free space before sending samples |
| `calibration` | the bridge can calibrate the RX frequency (the dashboard shows the Calibration tab) |

What moved into the bridges: the modem's configuration frames, the translation
of the services' frames (FRAME), the frame pacing, the free-space table, the
transmit timeout of FRAME modems, the TX timing filter (SAMPLE), the SET_MODE /
ADD_MODE handling for modems that need it (the OpenMT bridge drops them for
firmware that has no modes), RX offsets, `calibrate` / `probe` / `reloadOffsets`,
and the Raspberry Pi UART write-gate (`bridgeWaitTxEmpty`, serial bridges only).
The old MMDVM config code is kept only as the test reference in
`modem/mmdvm-bridge/test/legacy/`.

Services (DMR, D-STAR, P25, YSF, NXDN, M17, FM) read `streamType` instead of
the old `modem` setting. The dashboard has no Device Type list any more: the
bridge you install decides, and MMDVM uses its own `board` setting (hotspot or
full).

A bridge is now required. Start it before the modem host (the drop-ins in each
bridge directory order the services); the host waits up to a minute for it.

## What the host still does with names

Nothing it depends on. The mode name a service registers with is only a label:
the host shows it on the dashboard (`active_mode`), passes it to the bridge
(MODE_ON/OFF, MODE_CONN) and compares it with another label on the BIT and
SAMPLE streams to find the active service. There is no table of radio protocols
in Modem_Host any more: the free-space table, the TX timing filter (the OpenMT
bridge passes TX timing reports of connections named DMR only) and the frame
pacing are in the bridges.
