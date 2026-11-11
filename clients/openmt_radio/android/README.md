# OpenMT Radio for Android

An Android app that controls an OpenMT Radio running on another computer (a PC or the Pi) over the network: it has the same
controls as the window (connect, disconnect, release the modem, all the settings, DTMF, messages), the signal monitor and the log,
and it carries the speech both ways: you hear what the radio receives and push to talk to send your voice. The radio, the modem
and the vocoders stay on the computer; the phone only needs the network. The protocol is in [../REMOTE_PROTOCOL.md](../REMOTE_PROTOCOL.md).

## Build

Open the `android` folder in Android Studio (Koala or newer) and run it on a phone (Android 8.0 or newer), or on the command line:

    cd android
    ./gradlew assembleDebug          # app/build/outputs/apk/debug/app-debug.apk

The app has no libraries beyond Android itself (no AndroidX, no OkHttp): the WebSocket, the TLS pinning and the login are in
`app/src/main/java/org/openmt/radio/core`, which has no Android classes in it and is tested on a plain JVM
(`../tests/android_core_test.py`, part of `ctest`). Needs the Android SDK (platform 34) to build the app itself.

## Use

1. On the radio's computer: tick *Allow remote control* (Radio tab of OpenMT Radio). It shows the token and the certificate fingerprint.
2. On the phone: enter the computer's address, the port (18060) and the token. The app shows the radio's certificate fingerprint:
   **compare it with the one in OpenMT Radio**, then *Trust and pair*. The app pins that certificate from then on, and refuses a radio
   that shows another one.
3. The app connects, starts the audio and keeps running in the background with a notification (*Disconnect* is on it). Use *Connect
   radio* to connect the radio to Modem_Host, *PUSH TO TALK* to transmit (hold it, or tick the tap-to-toggle box). The volume-down key
   can be the push to talk while the app is open.

The speaker volume and the microphone level on the Radio tab are the phone's own; they do not change the radio's settings.

## Safety

* A phone that has the token can transmit through your radio. Use it on your own network or over a VPN (WireGuard, Tailscale); do not
  forward the port to the internet.
* The key is released by the radio itself when the phone goes silent for 1.5 s, loses the connection, or has transmitted for 180 s.
  The app also releases it when the screen goes away.
* The token is kept in the app's private storage (not encrypted at rest). *Forget this radio* removes it.
