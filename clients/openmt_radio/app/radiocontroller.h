/* OpenMT Radio: Qt wrapper around the radio engine.  The engine calls back from its own threads; the signals
 * here are queued to the thread of the window. */

#ifndef OPENMT_RADIOCONTROLLER_H
#define OPENMT_RADIOCONTROLLER_H

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

    void ptt(bool down);
    bool releaseModem();
    bool dtmfSupported() const;
    bool sendDtmf(char digit);

public slots:
    void pushMic(const QVector<qint16>& pcm);

signals:
    void rxAudio(const QVector<qint16>& pcm);
    void callEvent(int event, unsigned int src, unsigned int dst, bool group, float ber, const QString& text);
    void logLine(const QString& text);
    void linkLost();

private:
    openmt::RadioEngine engine_;
};

#endif
