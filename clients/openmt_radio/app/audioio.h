/* OpenMT Radio: the host computer's sound card.  The microphone is read and the received speech played at
 * 8000 * N Hz (48000 by default) and converted to and from the 8 kHz speech the engine works with. */

#ifndef OPENMT_AUDIOIO_H
#define OPENMT_AUDIOIO_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QByteArray>

#include <memory>
#include <mutex>
#include <vector>
#include <cstdint>

#include "../core/resampler.h"

class QIODevice;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
class QAudioSource;
class QAudioSink;
#else
class QAudioInput;
class QAudioOutput;
#endif

class AudioIO : public QObject
{
    Q_OBJECT

public:
    explicit AudioIO(QObject* parent = nullptr);
    ~AudioIO();

    static QStringList inputDevices();
    static QStringList outputDevices();

    bool start(const QString& inName, const QString& outName, unsigned int rate, QString& error);
    void stop();
    bool running() const;

    /* Received speech, 8 kHz.  May be called from any thread. */
    void play(const int16_t* pcm8k, size_t count);

    void setMicGain(float gain);
    void setSpeakerGain(float gain);

signals:
    /* Microphone speech, 8 kHz, about every 20 ms; level is the peak 0 - 1 of the block */
    void micAudio(const QVector<qint16>& pcm, float level);
    void playLevel(float level);

private slots:
    void tick();

private:
    std::unique_ptr<openmt::Decimator>    dec_;
    std::unique_ptr<openmt::Interpolator> interp_;
    unsigned int                          factor_;
    QTimer                                timer_;
    QIODevice*                            inDev_;
    QIODevice*                            outDev_;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QAudioSource*                         in_;
    QAudioSink*                           out_;
#else
    QAudioInput*                          in_;
    QAudioOutput*                         out_;
#endif
    std::mutex                            mutex_;
    std::vector<int16_t>                  playQueue_;     /* 8 kHz, waiting for the sound card */
    QByteArray                            pending_;       /* converted, not yet accepted by the sound card */
    float                                 micGain_;
    float                                 spkGain_;
    bool                                  running_;
    bool                                  playing_;       /* false while the jitter buffer fills */
    int                                   idleTicks_;     /* ticks since speech last arrived */
};

#endif
