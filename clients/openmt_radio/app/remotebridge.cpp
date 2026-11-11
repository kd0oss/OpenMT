#include "remotebridge.h"

#include "radiocontroller.h"

#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QTimer>

#include <chrono>

#include "../core/remote_control.h"       /* the headers need no OpenSSL; only the server's code does */
#include "../core/remote_server.h"
#include "../core/web_client.h"

static long long steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool RemoteBridge::available()
{
#ifdef OPENMT_REMOTE
    return true;
#else
    return false;
#endif
}

#ifdef OPENMT_REMOTE

RemoteBridge::RemoteBridge(openmt::RemoteUi* ui, RadioController* radio, QObject* parent) :
    QObject(parent),
    ui_(ui),
    radio_(radio),
    timer_(new QTimer(this)),
    server_(),
    control_(),
    certFile_(),
    lastMonitorMs_(0)
{
    timer_->setInterval(250);
    QObject::connect(timer_, SIGNAL(timeout()), this, SLOT(onTick()));
    QObject::connect(radio_, SIGNAL(callEvent(int, unsigned int, unsigned int, bool, float, QString)),
                     this, SLOT(onCall(int, unsigned int, unsigned int, bool, float, QString)));
    QObject::connect(radio_, SIGNAL(logLine(QString)), this, SLOT(onLog(QString)));
}

RemoteBridge::~RemoteBridge()
{
    stop();
}

bool RemoteBridge::start(unsigned int port, const QString& token, const QString& dir, QString& error)
{
    stop();

    QDir().mkpath(dir);
    certFile_ = dir + "/cert.pem";

    const QString keyFile = dir + "/key.pem";
    std::string err;

    if (!QFile::exists(certFile_) || !QFile::exists(keyFile))
    {
        if (!openmt::RemoteServer::generateCertificate(certFile_.toStdString(), keyFile.toStdString(), err))
        {
            error = "the certificate could not be made: " + QString::fromStdString(err);
            return false;
        }
    }

    openmt::RemoteConfig cfg;

    cfg.port = port;
    cfg.token = token.toStdString();
    cfg.certFile = certFile_.toStdString();
    cfg.keyFile = keyFile.toStdString();
    cfg.webPage = openmt::webClientPage();

    server_.reset(new openmt::RemoteServer());
    control_.reset(new openmt::RemoteControl(*server_, *ui_, *this,
        [this](const std::function<void()>& f)
        {
            /* to the thread of the window (this object lives there) */
            std::function<void()> copy = f;

            QMetaObject::invokeMethod(this, [copy]() { copy(); }, Qt::QueuedConnection);
        }));
    control_->log = [this](const std::string& text)
    {
        emit remoteLog(QString::fromStdString(text));
    };
    radio_->setRxAudioTap([this](const int16_t* pcm, size_t count)
    {
        if (server_)
            server_->sendRxAudio(pcm, count);
    });

    if (!server_->start(cfg, control_.get(), err))
    {
        error = QString::fromStdString(err);
        if (radio_)
            radio_->setRxAudioTap(std::function<void(const int16_t*, size_t)>());
        control_.reset();
        server_.reset();
        return false;
    }
    lastMonitorMs_ = 0;
    timer_->start();
    return true;
}

void RemoteBridge::stop()
{
    timer_->stop();
    if (radio_)
        radio_->setRxAudioTap(std::function<void(const int16_t*, size_t)>());
    if (server_)
        server_->stop();
    control_.reset();            /* before the server: commands still queued for the window are dropped */
    server_.reset();
}

bool RemoteBridge::running() const
{
    return server_ && server_->running();
}

unsigned int RemoteBridge::port() const
{
    return server_ ? server_->port() : 0U;
}

unsigned int RemoteBridge::clients() const
{
    return server_ ? server_->clients() : 0U;
}

QString RemoteBridge::fingerprint() const
{
    return certFile_.isEmpty() ? QString() : QString::fromStdString(openmt::RemoteServer::certFingerprint(certFile_.toStdString()));
}

bool RemoteBridge::localPtt(bool down)
{
    return server_ ? server_->localPtt(down) : true;
}

QString RemoteBridge::newToken()
{
    return QString::fromStdString(openmt::RemoteServer::generateToken());
}

QString RemoteBridge::formatToken(const QString& token)
{
    return QString::fromStdString(openmt::RemoteServer::formatToken(openmt::RemoteServer::normalizeToken(token.toStdString())));
}

bool RemoteBridge::ptt(bool down, std::string& error)
{
    if (!radio_ || !radio_->running())
    {
        error = "not connected to Modem_Host";
        return false;
    }
    radio_->ptt(down);
    return true;
}

void RemoteBridge::mic(const int16_t* pcm8k, size_t count)
{
    if (radio_)
        radio_->engine().pushMic(pcm8k, count);
}

bool RemoteBridge::dtmf(char digit, std::string& error)
{
    if (!radio_ || !radio_->sendDtmf(digit))
    {
        error = "the digit was not sent (the mode has no DTMF, or too many digits are waiting)";
        return false;
    }
    return true;
}

void RemoteBridge::onTick()
{
    if (!control_)
        return;

    control_->publish();

    /* the monitor, only while a client wants it (the analysis costs some milliseconds) */
    if (radio_ && server_->monitorSubscribers() > 0U && radio_->running())
    {
        const long long now = steadyMs();
        const long long every = (long long)server_->monitorIntervalMs() < 250LL ? 250LL : (long long)server_->monitorIntervalMs();

        if (now - lastMonitorMs_ >= every)
        {
            lastMonitorMs_ = now;

            openmt::MonitorSnapshot s = radio_->monitor().snapshot();

            if (radio_->iq().available())
            {
                openmt::MonitorSnapshot q = radio_->iq().snapshot();

                if (q.active || s.silentMs > 0)
                    s = q;
            }
            server_->broadcastMonitor(openmt::RemoteControl::monitorJson(s));
        }
    }
}

void RemoteBridge::onCall(int event, unsigned int src, unsigned int dst, bool group, float ber, const QString& text)
{
    if (!control_)
        return;

    openmt::CallInfo info;

    info.event = (openmt::CallEvent)event;
    info.src = src;
    info.dst = dst;
    info.group = group;
    info.ber = ber;
    info.text = text.toStdString();
    control_->onCall(info);
}

void RemoteBridge::onLog(const QString& text)
{
    if (control_)
        control_->onLog(text.toStdString());
}

#else   /* built without OpenSSL: no remote control */

RemoteBridge::RemoteBridge(openmt::RemoteUi* ui, RadioController* radio, QObject* parent) :
    QObject(parent), ui_(ui), radio_(radio), timer_(nullptr), server_(), control_(), certFile_(), lastMonitorMs_(0)
{
}

RemoteBridge::~RemoteBridge()
{
}

bool RemoteBridge::start(unsigned int, const QString&, const QString&, QString& error)
{
    error = "this build has no remote control (OpenSSL was not found when it was built)";
    return false;
}

void RemoteBridge::stop() {}
bool RemoteBridge::running() const { return false; }
unsigned int RemoteBridge::port() const { return 0U; }
unsigned int RemoteBridge::clients() const { return 0U; }
QString RemoteBridge::fingerprint() const { return QString(); }
bool RemoteBridge::localPtt(bool) { return true; }
QString RemoteBridge::newToken() { return QString(); }
QString RemoteBridge::formatToken(const QString& token) { return token; }
bool RemoteBridge::ptt(bool, std::string& error) { error = "no remote control"; return false; }
void RemoteBridge::mic(const int16_t*, size_t) {}
bool RemoteBridge::dtmf(char, std::string& error) { error = "no remote control"; return false; }
void RemoteBridge::onTick() {}
void RemoteBridge::onCall(int, unsigned int, unsigned int, bool, float, const QString&) {}
void RemoteBridge::onLog(const QString&) {}

#endif
