#include "remote_control.h"

#include <chrono>

namespace openmt
{

/* ------------------------------------------------------------------------------------------------------------ */

RemoteControl::RemoteControl(RemoteServer& server, RemoteUi& ui, RemoteRadio& radio, const PostFn& post) :
    log(),
    server_(server),
    ui_(ui),
    radio_(radio),
    post_(post),
    lastState_(),
    alive_(new std::atomic<bool>(true))
{
}

RemoteControl::~RemoteControl()
{
    *alive_ = false;
}

bool RemoteControl::remotePtt(bool down, std::string& error)
{
    return radio_.ptt(down, error);
}

void RemoteControl::remoteMic(const int16_t* pcm8k, size_t count)
{
    radio_.mic(pcm8k, count);
}

bool RemoteControl::remoteDtmf(char digit, std::string& error)
{
    return radio_.dtmf(digit, error);
}

void RemoteControl::remoteLog(const std::string& text)
{
    if (log)
        log(text);
}

void RemoteControl::remoteCommand(const RemoteRequest& request)
{
    RemoteRequest copy = request;

    std::shared_ptr<std::atomic<bool> > alive = alive_;

    post_([this, copy, alive]()
    {
        if (*alive)
            run(copy);
    });
}

void RemoteControl::run(const RemoteRequest& r)
{
    const std::string& cmd = r.cmd;
    std::string error;

    if (cmd == "get_settings")
    {
        Json s = Json::object();
        std::map<std::string, Json> all = ui_.remoteSettings();

        for (std::map<std::string, Json>::const_iterator it = all.begin(); it != all.end(); ++it)
            s.set(it->first, it->second);

        Json body = Json::object();

        body.set("settings", s);
        server_.reply(r.client, r.id, true, body);
    }
    else if (cmd == "set_settings")
    {
        const Json& given = r.args["settings"];

        if (!given.isObject())
        {
            server_.reply(r.client, r.id, false, Json("\"settings\" must be an object"));
            return;
        }

        std::map<std::string, Json> values(given.members());
        bool connected = ui_.remoteStatus()["connected"].asBool(false);

        error = remoteCheckSettings(values, connected);
        if (error.empty())
            error = ui_.remoteApplySettings(values);
        if (!error.empty())
        {
            server_.reply(r.client, r.id, false, Json(error));
            return;
        }

        Json body = Json::object();

        body.set("applied", (unsigned long long)values.size());
        server_.reply(r.client, r.id, true, body);
        publish();
    }
    else if (cmd == "get_options")
        server_.reply(r.client, r.id, true, ui_.remoteOptions());
    else if (cmd == "connect" || cmd == "disconnect" || cmd == "release_modem" || cmd == "open_dongle")
    {
        if (cmd == "connect")
            error = ui_.remoteConnect();
        else if (cmd == "disconnect")
            error = ui_.remoteDisconnect();
        else if (cmd == "release_modem")
            error = ui_.remoteRelease();
        else
            error = ui_.remoteOpenDongle();

        if (error.empty())
        {
            Json body = Json::object();

            body.set("status", ui_.remoteStatus());
            server_.reply(r.client, r.id, true, body);
        }
        else
            server_.reply(r.client, r.id, false, Json(error));
        publish();
    }
    else if (cmd == "send_message")
    {
        const Json& to = r.args["to"];
        const Json& text = r.args["text"];

        if (!to.isString() || !text.isString() || text.asString().empty() || text.asString().size() > 2000U)
            error = "\"to\" and \"text\" (1 to 2000 characters) are needed";
        else
            error = ui_.remoteSendMessage(to.asString(), text.asString());
        server_.reply(r.client, r.id, error.empty(), error.empty() ? Json::object() : Json(error));
    }
    else if (cmd == "set_meta")
    {
        const Json& text = r.args["text"];

        if (!text.isString() || text.asString().size() > 200U)
            error = "\"text\" (up to 200 characters) is needed";
        else
            error = ui_.remoteSetMeta(text.asString());
        server_.reply(r.client, r.id, error.empty(), error.empty() ? Json::object() : Json(error));
    }
    else
        server_.reply(r.client, r.id, false, Json("unknown command: " + cmd));
}

void RemoteControl::publish()
{
    Json st = ui_.remoteStatus();

    st.set("remoteClients", server_.clients());
    st.set("ptt", server_.pttOwner());

    std::string text = st.dump();

    if (text != lastState_)
    {
        lastState_ = text;
        server_.setState(st);
    }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Events                                                                                                       */
/* ------------------------------------------------------------------------------------------------------------ */

const char* RemoteControl::callEventName(int event)
{
    switch (event)
    {
        case CALL_RX_START: return "rx_start";
        case CALL_RX_INFO:  return "rx_info";
        case CALL_RX_END:   return "rx_end";
        case CALL_RX_LOST:  return "rx_lost";
        case CALL_TX_START: return "tx_start";
        case CALL_TX_END:   return "tx_end";
    }
    return "unknown";
}

void RemoteControl::onCall(const CallInfo& info)
{
    Json e = Json::object();

    e.set("type", "call");
    e.set("event", callEventName((int)info.event));
    e.set("src", info.src);
    e.set("dst", info.dst);
    e.set("group", info.group);
    e.set("ber", (double)info.ber);
    e.set("text", info.text);
    server_.broadcast(e);
}

void RemoteControl::onLog(const std::string& text)
{
    Json e = Json::object();

    e.set("type", "log");
    e.set("text", text);
    e.set("time", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count());
    server_.broadcast(e);
}

void RemoteControl::onRxAudio(const int16_t* pcm8k, size_t count)
{
    server_.sendRxAudio(pcm8k, count);
}

Json RemoteControl::monitorJson(const MonitorSnapshot& s)
{
    Json m = Json::object();
    Json levels = Json::array();

    m.set("type", "monitor");
    m.set("source", s.fromIq ? "iq" : "samples");
    m.set("active", s.active);
    m.set("rms", s.rms);
    m.set("peak", s.peak);
    m.set("clipped", s.clipped);
    m.set("baud", s.baud);
    m.set("levelsValid", s.levelsValid);
    for (int i = 0; i < 4; i++)
        levels.push(s.levels[i]);
    m.set("levels", levels);
    m.set("centre", s.centre);
    m.set("outerHz", s.outerHz);
    m.set("nominalHz", s.nominalHz);
    m.set("outerPercent", s.outerPercent);
    m.set("spread", s.spread);
    m.set("blur", s.blur);
    m.set("approximate", s.approximate);
    m.set("syncLocked", s.syncLocked);
    m.set("syncHits", s.syncHits);
    m.set("inverted", s.inverted);
    m.set("averagedSeconds", s.averagedSeconds);
    m.set("silentMs", (long long)s.silentMs);
    if (s.fromIq)
    {
        m.set("iqRate", s.iqRate);
        m.set("iqFrames", (unsigned long long)s.iqFrames);
        m.set("iqLost", (unsigned long long)s.iqLost);
        m.set("iqOverload", s.iqOverload);
    }
    return m;
}

}
