# OpenMT Radio remote control protocol, version 1

A program on another machine (the Android app, a web page, a script) can use an OpenMT Radio over the network: listen to the
received speech, push to talk and send microphone speech, connect and disconnect, change every setting of the window, and watch the
signal. This is the whole protocol. `tests/remote_client.py` is a complete client written from this text (about 200 lines of Python,
standard library only); `tests/remote_test.py` and `tests/remote_control_test.py` test the server against it.

## Security first

A client that can reach the port and knows the token can **transmit**. The protocol protects against eavesdropping and against
guessing, but the radio is only as safe as the network it is on:

- Use it on your own network or over a VPN (WireGuard, Tailscale). Do not forward the port to the internet.
- The token is 24 characters (120 bits) and is made by the radio. *New token* in the window disconnects everyone who has the old one.
- The connection is TLS 1.2 or newer with a self signed certificate. A client must **pin the certificate's SHA-256 fingerprint**
  (shown in the window) the first time it connects, and refuse a different one later. Without the pin, TLS only protects against
  passive listening.
- The login is a challenge and response (below): the token is never sent, and the client checks that the server knows it too.
- Five wrong tokens from one address lock that address out for 60 seconds.
- A transmission a client starts ends by itself when the client goes silent for 1.5 s, disconnects, or has transmitted for 180 s.

## Transport

A WebSocket (RFC 6455, version 13) over TLS: `wss://HOST:PORT/openmt`. The default port is 18060. Text frames carry JSON objects
(UTF-8), binary frames carry audio. Clients must mask their frames, as the RFC requires; the server does not. Messages up to
256 KB; fragmented messages are put together. The server answers pings. A client that sends nothing at all for 30 s is
dropped, so send a `ping` command every 10 s or so.

The same port also serves the **web client** (`web/index.html`, built into the program) for a plain `GET /`: open
`https://HOST:18060/` in a browser, accept the certificate once, and enter the token. Nothing but that one page is served; it holds no
secrets. A browser cannot pin a certificate, but the login proof (below) tells the page whether it is talking to a radio that knows the
token.

## Login

1. The server sends `{"type":"hello","protocol":1,"server":"OpenMT Radio","nonce":"<32 hex characters>"}`.
2. The client sends `{"type":"auth","cnonce":"<16 to 128 hex characters, random>","response":"<hex>"}` within 5 s, where

       response = hex( HMAC-SHA256( key = token, message = "C" + nonce + cnonce ) )

   `token` is the 24 characters, upper case, without dashes or spaces (the window shows it in groups of four; clients should accept
   any grouping and case). `nonce` and `cnonce` are the hex strings exactly as sent, as ASCII.
3. The server answers `{"type":"auth_ok","protocol":1,"server":"...","proof":"<hex>","clientId":N}` where

       proof = hex( HMAC-SHA256( key = token, message = "S" + nonce + cnonce ) )

   The client must check `proof` and drop the connection if it is wrong (it is talking to something that does not know the token).
   Then the server sends the current `state` event.
   On failure: `{"type":"auth_failed"}` and the connection closes (code 1008; 1013 when there are too many clients, then
   `"error":"too many clients"` is added). Anything but `auth` before the login closes the connection.

Test vector: token `ABCDEFGHJKLMNPQRSTUVWXYZ`, nonce `00112233445566778899aabbccddeeff`, cnonce
`ffeeddccbbaa99887766554433221100`: the client's response is
`24babe65aec78bdfc7d810dd8f438e942c3c29cca803cf93e2f0c0230d85fb73`.

## Commands

A command is `{"id":N,"cmd":"name", ...arguments}`. `id` is any integer the client chooses (use a counter; the reply carries it back).
The reply is `{"type":"reply","id":N,"ok":true,"result":{...}}` or `{"type":"reply","id":N,"ok":false,"error":"text"}`. Commands
that change the radio are carried out on the window's thread, so a reply can take a moment (`connect` waits until the radio has
connected or failed).

| command | arguments | what it does |
|---|---|---|
| `ping` | | answers `{"pong":true}` |
| `get_state` | | the current state (same as the `state` event) |
| `get_settings` | | `{"settings":{key:value ...}}`: every setting, in its type (table below) |
| `set_settings` | `settings`: object | changes settings, all or none; `{"applied":n}`. While the radio is connected only the live ones can change |
| `get_options` | | `{"modes":[{"id","label","available"}], "choices":{key:[{"value","label"}]}}`: what the combo boxes hold (audio devices of the radio's computer, rates, modes ...) |
| `connect` | | connects to Modem_Host with the settings; `{"status":{...}}`. Error text says why not |
| `disconnect` | | |
| `release_modem` | | tells Modem_Host to let go of this radio's mode |
| `open_dongle` | | opens the AMBE3000R dongle's port again |
| `ptt` | `down`: bool | takes or releases the transmitter (see Push to talk) |
| `ptt_keepalive` | | keeps the key down (see below); no reply unless it has an `id` |
| `dtmf` | `digit`: one of `0-9 * # A-D` | sends a DTMF tone (modes whose host listens for it: FM) |
| `send_message` | `to`, `text` | sends a text message (M17) |
| `set_meta` | `text` | the text that goes with every transmission (M17), up to 200 characters |
| `audio_start` | `codec`: `pcm16` or `ulaw`; `rx`, `tx`: bool (default true) | starts the audio (see Audio) |
| `audio_stop` | | |
| `monitor_start` | `intervalMs`: 100 to 5000 (default 500) | sends `monitor` events (the radio analyses at most every 250 ms) |
| `monitor_stop` | | |

### Settings

`get_settings` returns, and `set_settings` takes, these keys (the keys the window saves). Types: i integer, d number, b boolean,
s text (a number is accepted for text). Values outside what a control of the window allows are clamped by the window; a value a
combo box does not have (`mode`, `rate`, `audioIn` ...) is refused: `get_options` lists the choices.

`live` settings can change while connected: `micGain`, `spkGain` (percent, 0 to 400), `m17Meta`, `m17MsgTo`. All the others need
`disconnect` first.

```
host s, modem i, mode i (0 NXDN, 1 DMR, 2 P25, 3 YSF, 4 D-STAR, 5 M17, 6 FM), layout i,
ran i, src i, dst i, group b                                  NXDN
dmrColorCode i, dmrSrc i, dmrDst i, dmrGroup b, dmrRepeater i, dmrSlot i, dmrTiming i
p25Nac s (hex), p25Src i, p25Dst i, p25Group b
ysfCall s, ysfDest s, ysfDgid i
dstarCall s, dstarSuffix s, dstarUr s, dstarRpt1 s, dstarRpt2 s, dstarMsg s
m17Call s, m17Dest s, m17Can i, m17Filter i, m17Meta s (live), m17MsgTo s (live)
fmTxTone d, fmToneLevel d, fmDtmfLevel d, fmDtmfHang i, fmSquelch i, fmRxTone d, fmDelay i, fmSquelchLevel d, fmAuto b
ambeDevice s, ambeBaud s, ambeChunk i, ambeGap i, noVocoder b, capture b, invertRx b
txLevel i, rxLevel i, spectrumPort i, spectrumAuto b
audioIn s, audioOut s, rate i, micGain i (live), spkGain i (live)
```

The remote control's own settings (on, port, token) are not among them: a client cannot read the token or turn the remote control off.

## Events

The server sends these to every logged in client, without being asked:

- `{"type":"state", ...}`: sent after the login and whenever it changes (within about 250 ms).
  `connected` bool, `mode` int, `modeLabel`, `status` (the window's status line), `heard` (its "Last heard" line), `vocoder`,
  `dtmf` bool, `messages` bool, `rx` bool, `tx` bool, `remoteClients` int, `ptt` (`none`, `local` or `remote`).
- `{"type":"call","event":"rx_start|rx_info|rx_end|rx_lost|tx_start|tx_end","src":n,"dst":n,"group":bool,"ber":percent,"text":"..."}`:
  the calls heard and sent. `text` is mode specific (callsigns for YSF, D-STAR, M17).
- `{"type":"log","text":"...","time":ms since 1970}`: the window's log lines.
- `{"type":"ptt","owner":"none|local|remote","who":"address or desktop","reason":"..."}`: who holds the transmitter. `reason`
  says why it was released when the server did it (`no keepalive from the client`, `time limit`, `the client went away`,
  `desktop released` ...).
- `{"type":"monitor", ...}` (only after `monitor_start`): `source` (`samples` or `iq`), `active`, `rms`, `peak`, `clipped`, `baud`,
  `levelsValid`, `levels`[4], `centre`, `outerHz`, `nominalHz`, `outerPercent`, `spread`, `blur`, `approximate`, `syncLocked`,
  `syncHits`, `inverted`, `averagedSeconds`, `silentMs`, and with IQ `iqRate`, `iqFrames`, `iqLost`, `iqOverload`. The numbers are the
  ones the Monitor tab shows (see the README).
- `{"type":"error","error":"..."}`: a message that was not a JSON object. The connection stays.

## Push to talk

Only one party holds the transmitter: the desktop's own button, or one client.

- `ptt` with `down:true` takes it if nobody else has it. Errors: `busy: another client holds the push to talk`, `the desktop holds
  the push to talk`, or the radio's (`not connected to Modem_Host`). Pressing again while holding it is fine.
- While you hold it, **send microphone audio, or `ptt_keepalive` at least every 500 ms**. After 1.5 s of silence the server releases
  the key and sends a `ptt` event with the reason. It does the same when you disconnect and after 180 s of transmitting. So a phone
  that loses its network, or whose app is killed, cannot leave the transmitter on.
- `ptt` with `down:false` releases it. Releasing a key one does not hold is harmless.
- The desktop's button refuses to transmit while a client holds the key.

## Audio

After `audio_start`, binary frames flow both ways: **8 kHz, mono, 160 samples (20 ms) per frame**.

    byte 0      type: 1 = from the radio (received speech), 2 = to the radio (microphone)
    byte 1      codec: 0 = 16 bit signed little endian PCM (320 bytes), 1 = G.711 mu-law (160 bytes)
    bytes 2-3   sequence number, big endian (counts frames to this client; the client's own count for type 2)
    bytes 4-    the samples

- Type 1 frames go to clients that asked for `rx`. The radio hands speech over in bursts (20 ms to 180 ms, depending on the mode),
  and the server cuts it into frames of 160 samples, so several frames arrive together; play them through a jitter buffer of about
  160 ms.
- Type 2 frames are used only from the client that holds the key; others are ignored. Send them as they are recorded, 20 ms
  apart. Frames of other sizes (up to 2048 samples) are accepted.
- A client that cannot keep up loses audio frames (they are dropped, not queued); at 2 MB of waiting events it is disconnected.
  pcm16 needs 128 kbit/s each way, mu-law 64 kbit/s.

## Versions and limits

`protocol` is 1. A change that old clients cannot ignore raises it; new commands, events and fields can appear without. Clients
must ignore events and fields they do not know. Up to 4 clients at once.
