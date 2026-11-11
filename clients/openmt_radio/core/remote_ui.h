/* OpenMT Radio: what the remote control needs from the program (no Qt, no server). */

#ifndef OPENMT_REMOTE_UI_H
#define OPENMT_REMOTE_UI_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "remote_json.h"

namespace openmt
{

/* The transmitter: push to talk, microphone speech, DTMF.  Called from the server's threads, at any time. */
class RemoteRadio
{
public:
    virtual ~RemoteRadio() {}
    virtual bool ptt(bool down, std::string& error) = 0;
    virtual void mic(const int16_t* pcm8k, size_t count) = 0;
    virtual bool dtmf(char digit, std::string& error) = 0;
};

/* Everything the window does.  Called only from the thread of the window. */
class RemoteUi
{
public:
    virtual ~RemoteUi() {}

    /* The value of every setting in the table (remoteSettingTable), in its own type */
    virtual std::map<std::string, Json> remoteSettings() = 0;
    /* Sets the given settings (already checked against the table); returns "" or an error text */
    virtual std::string remoteApplySettings(const std::map<std::string, Json>& values) = 0;
    /* {"modes":[{"id":0,"label":"NXDN","available":true}...], "choices":{"audioIn":[{"value":"..","label":".."}], ...}} */
    virtual Json remoteOptions() = 0;
    /* {"connected":bool, "mode":int, "modeLabel":"P25", "status":"text", "heard":"text", "vocoder":"text", "dtmf":bool,
        "messages":bool, "rx":bool, "tx":bool} */
    virtual Json remoteStatus() = 0;
    virtual std::string remoteConnect() = 0;        /* "" when connected, else why not */
    virtual std::string remoteDisconnect() = 0;
    virtual std::string remoteRelease() = 0;
    virtual std::string remoteOpenDongle() = 0;
    virtual std::string remoteSendMessage(const std::string& to, const std::string& text) = 0;
    virtual std::string remoteSetMeta(const std::string& text) = 0;
};

}

#endif
