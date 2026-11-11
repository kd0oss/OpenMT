/* OpenMT Radio: the remote control inside the program.  It owns the server (core/remote_server.h) and the command layer
 * (core/remote_control.h), connects them to the radio and to the window, and keeps the clients informed.  When the program was
 * built without OpenSSL it does nothing and says so. */

#ifndef OPENMT_REMOTEBRIDGE_H
#define OPENMT_REMOTEBRIDGE_H

#include <memory>

#include <QObject>
#include <QPointer>
#include <QString>

#include "../core/remote_ui.h"
#include "radiocontroller.h"

class QTimer;
class RadioController;

namespace openmt
{
class RemoteServer;
class RemoteControl;
}

class RemoteBridge : public QObject, public openmt::RemoteRadio
{
    Q_OBJECT

public:
    RemoteBridge(openmt::RemoteUi* ui, RadioController* radio, QObject* parent = nullptr);
    ~RemoteBridge();

    static bool available();                                    /* built with OpenSSL */

    /* dir holds the certificate (made when it is not there).  The token is the pairing token. */
    bool start(unsigned int port, const QString& token, const QString& dir, QString& error);
    void stop();
    bool running() const;
    unsigned int port() const;
    unsigned int clients() const;
    QString fingerprint() const;

    /* The window's own push to talk: false while a client holds the key */
    bool localPtt(bool down);

    static QString newToken();                                  /* 24 characters */
    static QString formatToken(const QString& token);           /* in groups of four */

    /* RemoteRadio: called from the server's threads */
    bool ptt(bool down, std::string& error) override;
    void mic(const int16_t* pcm8k, size_t count) override;
    bool dtmf(char digit, std::string& error) override;

signals:
    void remoteLog(const QString& text);

private slots:
    void onTick();
    void onCall(int event, unsigned int src, unsigned int dst, bool group, float ber, const QString& text);
    void onLog(const QString& text);

private:
    openmt::RemoteUi*                         ui_;
    QPointer<RadioController>                 radio_;       /* the window deletes its children in order: the radio can be gone before this */
    QTimer*                                   timer_;
    std::unique_ptr<openmt::RemoteServer>     server_;
    std::unique_ptr<openmt::RemoteControl>    control_;
    QString                                   certFile_;
    long long                                 lastMonitorMs_;
};

#endif
