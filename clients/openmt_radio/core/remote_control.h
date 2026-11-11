/* OpenMT Radio: what the remote control can do, without Qt.
 *
 * RemoteControl is the RemoteHost of the RemoteServer.  It turns the commands of the protocol into calls on two small
 * interfaces that the program implements:
 *
 *   RemoteRadio  the transmitter: push to talk, microphone speech, DTMF.  Called from the server's threads, at any time.
 *   RemoteUi     everything the window does: the settings, connect, disconnect ...  Called only from the thread of the window
 *                (RemoteControl hands each command to the 'post' function the program gives it).
 *
 * The settings are the ones the window keeps (the keys it saves): a table here says which exist, what type they have and which
 * of them may change while the radio runs. */

#ifndef OPENMT_REMOTE_CONTROL_H
#define OPENMT_REMOTE_CONTROL_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "radio_mode.h"
#include "remote_json.h"
#include "remote_server.h"
#include "remote_settings.h"
#include "remote_ui.h"
#include "rf_monitor.h"

namespace openmt
{

class RemoteControl : public RemoteHost
{
public:
    /* post(f) must run f later on the thread of the window */
    typedef std::function<void(const std::function<void()>&)> PostFn;

    RemoteControl(RemoteServer& server, RemoteUi& ui, RemoteRadio& radio, const PostFn& post);
    ~RemoteControl();

    /* From the thread of the window: publishes the status when it changed (call it every few hundred milliseconds, and after a
       command that changes it) */
    void publish();

    /* Events for the clients (any thread) */
    void onCall(const CallInfo& info);
    void onLog(const std::string& text);
    void onRxAudio(const int16_t* pcm8k, size_t count);

    /* The monitor summary a client gets (a snapshot made from the modem's samples or the IQ) */
    static Json monitorJson(const MonitorSnapshot& s);
    static const char* callEventName(int event);

    /* RemoteHost */
    bool remotePtt(bool down, std::string& error) override;
    void remoteMic(const int16_t* pcm8k, size_t count) override;
    bool remoteDtmf(char digit, std::string& error) override;
    void remoteCommand(const RemoteRequest& request) override;
    void remoteLog(const std::string& text) override;

    std::function<void(const std::string&)> log;        /* where remoteLog() goes (any thread) */

private:
    void run(const RemoteRequest& request);             /* on the thread of the window */

    RemoteServer&   server_;
    RemoteUi&       ui_;
    RemoteRadio&    radio_;
    PostFn          post_;
    std::string     lastState_;
    std::shared_ptr<std::atomic<bool> > alive_;         /* a command still waiting for the window when this is gone is dropped */
};

}

#endif
