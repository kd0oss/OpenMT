/* OpenMT Radio: the remote control server.
 *
 * A phone (or any other program) connects with a WebSocket over TLS, proves it knows the pairing token, and can then do
 * everything the window does: connect and disconnect, change the settings, push to talk, send DTMF and messages, listen to the
 * received speech and send microphone speech, and watch the signal monitor.  The protocol is in REMOTE_PROTOCOL.md.
 *
 * This file has no Qt in it.  The program that owns the radio implements RemoteHost: the push to talk, the microphone and DTMF
 * are called straight from the server's threads (the safety of the transmitter must not wait for a window); everything else is a
 * request that the host answers later with RemoteServer::reply(), from whatever thread it likes. */

#ifndef OPENMT_REMOTE_SERVER_H
#define OPENMT_REMOTE_SERVER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "remote_json.h"

namespace openmt
{

struct RemoteConfig
{
    std::string  bindAddress     = "0.0.0.0";
    unsigned int port            = 18060;       /* 0 = any free port (tests); port() says which */
    std::string  token;                         /* the pairing token: required */
    bool         tls             = true;        /* false only for tests: the token is then the only protection against a listener */
    std::string  certFile;                      /* PEM, with tls */
    std::string  keyFile;
    unsigned int maxClients      = 4;
    unsigned int maxTxSeconds    = 180;         /* a remote transmission is cut after this long (0 = no limit) */
    unsigned int pttKeepaliveMs  = 1500;        /* the transmitter drops when the key holder has been silent this long */
    unsigned int authTimeoutMs   = 5000;
    unsigned int idleTimeoutMs   = 30000;       /* a client that sends nothing at all for this long is dropped */
    std::string  serverName      = "OpenMT Radio";
    std::string  webPage;                       /* HTML served for GET / (the web client); empty = none.  It holds no secrets */
};

struct RemoteRequest
{
    uint64_t    client = 0U;
    long long   id = 0;
    std::string cmd;
    Json        args;                           /* the whole message, including "cmd" and "id" */
    std::string peer;                           /* "address:port" of the client */
};

class RemoteHost
{
public:
    virtual ~RemoteHost() {}

    /* Called from the server's threads.  They must be safe to call at any time, also while the radio is being stopped. */
    virtual bool remotePtt(bool down, std::string& error) = 0;
    virtual void remoteMic(const int16_t* pcm8k, size_t count) = 0;
    virtual bool remoteDtmf(char digit, std::string& error) = 0;

    /* Every other command.  Return at once; answer with RemoteServer::reply(request.client, request.id, ...) */
    virtual void remoteCommand(const RemoteRequest& request) = 0;

    /* Clients coming and going, failed logins ... */
    virtual void remoteLog(const std::string& text) { (void)text; }
};

class RemoteServer
{
public:
    RemoteServer();
    ~RemoteServer();

    bool start(const RemoteConfig& config, RemoteHost* host, std::string& error);
    void stop();
    bool running() const { return running_; }
    unsigned int port() const { return boundPort_; }

    /* The answer to a request: {"type":"reply","id":id,"ok":ok, "result": body} or, when not ok, "error": body (a string) */
    void reply(uint64_t client, long long id, bool ok, const Json& body);

    /* An event for every authenticated client: {"type": ...}.  setState() also keeps the state for get_state and for new clients */
    void broadcast(const Json& event);
    void setState(const Json& state);

    /* The monitor: only the clients that asked for it (monitor_start) get it, so check monitorSubscribers() first */
    unsigned int monitorSubscribers() const;
    unsigned int monitorIntervalMs() const;                 /* the shortest interval asked for */
    void broadcastMonitor(const Json& event);

    /* 8 kHz speech, to the clients that asked for it (audio_start) */
    void sendRxAudio(const int16_t* pcm8k, size_t count);

    /* The window's own push to talk.  false while a remote client holds the key; the window must not transmit then.  Release
       with localPtt(false) (a no-op when the window does not hold it) */
    bool localPtt(bool down);
    std::string pttOwner() const;                            /* "none", "local" or "remote" */

    unsigned int clients() const;                            /* authenticated */

    /* The pairing token: 24 characters from an alphabet without look alikes, shown in groups of four */
    static std::string generateToken();
    static std::string normalizeToken(const std::string& text);
    static std::string formatToken(const std::string& token);

    /* A self signed certificate for the TLS side (P-256, 10 years).  certFingerprint() is its SHA-256, as "AA:BB:..." */
    static bool generateCertificate(const std::string& certFile, const std::string& keyFile, std::string& error);
    static std::string certFingerprint(const std::string& certFile);

    /* The challenge response of the protocol, hex: HMAC-SHA256(token, prefix || nonce || cnonce); prefix "C" for the client's
       answer, "S" for the server's proof (exposed for the tests and for client programs written in C++) */
    static std::string authMac(const std::string& token, char prefix, const std::string& nonce, const std::string& cnonce);

    /* G.711 mu-law, the audio codec that halves the bandwidth */
    static uint8_t ulawEncode(int16_t sample);
    static int16_t ulawDecode(uint8_t code);

private:
    struct Client;
    struct Impl;

    void acceptLoop();
    void watchdogLoop();
    void clientMain(std::shared_ptr<Client> client);
    void releasePtt(const char* reason);
    void sendEvent(const std::shared_ptr<Client>& c, const Json& message);
    void log(const std::string& text);

    std::unique_ptr<Impl> impl_;
    RemoteConfig          cfg_;
    RemoteHost*           host_;
    std::atomic<bool>     running_;
    unsigned int          boundPort_;
};

}

#endif
