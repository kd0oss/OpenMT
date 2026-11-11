/* OpenMT Radio: Qt wrapper around the radio engine.  The engine calls back from its own threads; the signals
 * here are queued to the thread of the window. */

#ifndef OPENMT_RADIOCONTROLLER_H
#define OPENMT_RADIOCONTROLLER_H

#include <functional>
#include <mutex>

#include <QObject>
#include <QString>
#include <QVector>

#include "../core/radio_engine.h"

class RadioController : public QObject
{
    Q_OBJECT

public:
    explicit RadioController(QObject* parent = nullptr);
    ~RadioController();

    bool start(const openmt::EngineSettings& settings, QString& error);
    void stop();
    bool running() const;
    QString vocoderDescription() const;
    openmt::RfMonitor& monitor() { return engine_.monitor(); }
    openmt::SpectrumClient& spectrum() { return engine_.spectrum(); }
    openmt::IqMonitor& iq() { return engine_.iq(); }
    openmt::RadioEngine& engine() { return engine_; }

    /* Sees the received speech (8 kHz, from the engine's thread) as well as the window does; the remote control sends it on */
    void setRxAudioTap(const std::function<void(const int16_t*, size_t)>& tap);

    void ptt(bool down);
    bool releaseModem();
    bool dtmfSupported() const;
    bool openDongle(const QString& device, unsigned int baud, int chunkBytes, int gapUs, QString& error);
    bool dongleOpen() const;
    bool sendDtmf(char digit);
    bool messagesSupported() const;
    bool sendMessage(const QString& to, const QString& text, QString& error);
    void setMetaText(const QString& text);

public slots:
    void pushMic(const QVector<qint16>& pcm);

signals:
    void rxAudio(const QVector<qint16>& pcm);
    void callEvent(int event, unsigned int src, unsigned int dst, bool group, float ber, const QString& text);
    void logLine(const QString& text);
    void linkLost();

private:
    openmt::RadioEngine engine_;
    std::mutex          tapMutex_;
    std::function<void(const int16_t*, size_t)> rxTap_;
};

#endif
