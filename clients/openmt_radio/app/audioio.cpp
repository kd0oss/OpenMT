#include "audioio.h"

#include <QAudioFormat>
#include <QIODevice>
#include <QVector>
#include <algorithm>
#include <cmath>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QMediaDevices>
#else
#include <QAudioDeviceInfo>
#include <QAudioInput>
#include <QAudioOutput>
#endif

/* The play queue is never allowed to hold more than this much speech (8 kHz samples): a stalled sound card
   must not build up seconds of delay. */
static const size_t MAX_QUEUE = 8000U / 2U;

AudioIO::AudioIO(QObject* parent) :
    QObject(parent),
    dec_(),
    interp_(),
    factor_(6U),
    timer_(this),
    inDev_(nullptr),
    outDev_(nullptr),
    in_(nullptr),
    out_(nullptr),
    mutex_(),
    playQueue_(),
    pending_(),
    micGain_(1.0F),
    spkGain_(1.0F),
    running_(false),
    playing_(false),
    idleTicks_(0)
{
    timer_.setInterval(20);
    connect(&timer_, SIGNAL(timeout()), this, SLOT(tick()));
}

AudioIO::~AudioIO()
{
    stop();
}

QStringList AudioIO::inputDevices()
{
    QStringList names;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QList<QAudioDevice> list = QMediaDevices::audioInputs();
    for (const QAudioDevice& d : list)
        names << d.description();
#else
    const QList<QAudioDeviceInfo> list = QAudioDeviceInfo::availableDevices(QAudio::AudioInput);
    for (const QAudioDeviceInfo& d : list)
        names << d.deviceName();
#endif
    return names;
}

QStringList AudioIO::outputDevices()
{
    QStringList names;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QList<QAudioDevice> list = QMediaDevices::audioOutputs();
    for (const QAudioDevice& d : list)
        names << d.description();
#else
    const QList<QAudioDeviceInfo> list = QAudioDeviceInfo::availableDevices(QAudio::AudioOutput);
    for (const QAudioDeviceInfo& d : list)
        names << d.deviceName();
#endif
    return names;
}

bool AudioIO::start(const QString& inName, const QString& outName, unsigned int rate, QString& error)
{
    stop();

    if (!openmt::validDeviceRate(rate))
    {
        error = QString("the sound card rate %1 is not a multiple of 8000").arg(rate);
        return false;
    }

    factor_ = rate / 8000U;
    dec_.reset(new openmt::Decimator(factor_));
    interp_.reset(new openmt::Interpolator(factor_));

    QAudioFormat format;
    format.setSampleRate((int)rate);
    format.setChannelCount(1);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    format.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice inDevice  = QMediaDevices::defaultAudioInput();
    QAudioDevice outDevice = QMediaDevices::defaultAudioOutput();
    const QList<QAudioDevice> ins  = QMediaDevices::audioInputs();
    const QList<QAudioDevice> outs = QMediaDevices::audioOutputs();

    for (const QAudioDevice& d : ins)
        if (d.description() == inName)
            inDevice = d;
    for (const QAudioDevice& d : outs)
        if (d.description() == outName)
            outDevice = d;

    if (inDevice.isNull() || outDevice.isNull())
    {
        error = "no sound card input or output found";
        return false;
    }
    if (!inDevice.isFormatSupported(format) || !outDevice.isFormatSupported(format))
    {
        error = QString("the sound card does not do 16 bit mono at %1 Hz").arg(rate);
        return false;
    }

    in_  = new QAudioSource(inDevice, format, this);
    out_ = new QAudioSink(outDevice, format, this);
#else
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);

    QAudioDeviceInfo inDevice  = QAudioDeviceInfo::defaultInputDevice();
    QAudioDeviceInfo outDevice = QAudioDeviceInfo::defaultOutputDevice();

    const QList<QAudioDeviceInfo> ins  = QAudioDeviceInfo::availableDevices(QAudio::AudioInput);
    const QList<QAudioDeviceInfo> outs = QAudioDeviceInfo::availableDevices(QAudio::AudioOutput);

    for (const QAudioDeviceInfo& d : ins)
        if (d.deviceName() == inName)
            inDevice = d;
    for (const QAudioDeviceInfo& d : outs)
        if (d.deviceName() == outName)
            outDevice = d;

    if (inDevice.isNull() || outDevice.isNull())
    {
        error = "no sound card input or output found";
        return false;
    }
    if (!inDevice.isFormatSupported(format) || !outDevice.isFormatSupported(format))
    {
        error = QString("the sound card does not do 16 bit mono at %1 Hz").arg(rate);
        return false;
    }

    in_  = new QAudioInput(inDevice, format, this);
    out_ = new QAudioOutput(outDevice, format, this);
#endif

    /* about 100 ms of buffer each way */
    out_->setBufferSize((int)(rate / 10U * 2U));

    inDev_  = in_->start();
    outDev_ = out_->start();

    if (inDev_ == nullptr || outDev_ == nullptr)
    {
        error = "could not open the sound card";
        stop();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        playQueue_.clear();
    }
    pending_.clear();
    playing_ = false;
    idleTicks_ = 0;
    running_ = true;
    timer_.start();
    return true;
}

void AudioIO::stop()
{
    timer_.stop();
    running_ = false;

    if (in_ != nullptr)
    {
        in_->stop();
        delete in_;
        in_ = nullptr;
    }
    if (out_ != nullptr)
    {
        out_->stop();
        delete out_;
        out_ = nullptr;
    }
    inDev_  = nullptr;
    outDev_ = nullptr;
}

bool AudioIO::running() const
{
    return running_;
}

void AudioIO::setMicGain(float gain)
{
    micGain_ = gain;
}

void AudioIO::setSpeakerGain(float gain)
{
    spkGain_ = gain;
}

void AudioIO::play(const int16_t* pcm8k, size_t count)
{
    std::lock_guard<std::mutex> lock(mutex_);

    playQueue_.insert(playQueue_.end(), pcm8k, pcm8k + count);

    if (playQueue_.size() > MAX_QUEUE)
        playQueue_.erase(playQueue_.begin(), playQueue_.end() - (long)MAX_QUEUE);
}

static int16_t clip16(float v)
{
    if (v > 32767.0F)
        return 32767;
    if (v < -32768.0F)
        return -32768;
    return (int16_t)v;
}

void AudioIO::tick()
{
    if (!running_)
        return;

    /* microphone */
    QByteArray in = inDev_->readAll();
    if (!in.isEmpty())
    {
        const int16_t* s = reinterpret_cast<const int16_t*>(in.constData());
        size_t n = (size_t)in.size() / 2U;
        std::vector<int16_t> out;

        dec_->process(s, n, out);

        QVector<qint16> pcm;
        pcm.reserve((int)out.size());
        float peak = 0.0F;

        for (size_t i = 0U; i < out.size(); i++)
        {
            int16_t v = clip16((float)out[i] * micGain_);
            pcm.append(v);
            peak = std::max(peak, std::fabs((float)v) / 32768.0F);
        }
        if (!pcm.isEmpty())
            emit micAudio(pcm, peak);
    }

    /* speaker: convert what the radio received and hand it to the sound card as far as it accepts it */
    std::vector<int16_t> speech;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        speech.swap(playQueue_);
    }

    if (!speech.empty())
    {
        std::vector<int16_t> up;
        interp_->process(speech.data(), speech.size(), up);

        float peak = 0.0F;
        for (size_t i = 0U; i < up.size(); i++)
        {
            up[i] = clip16((float)up[i] * spkGain_);
            peak = std::max(peak, std::fabs((float)up[i]) / 32768.0F);
        }
        pending_.append(reinterpret_cast<const char*>(up.data()), (int)(up.size() * sizeof(int16_t)));
        emit playLevel(peak);
    }

    /* Speech comes from the radio in bursts of 80 ms, so playing it as it arrives breaks up whenever a burst is
       late.  A call starts playing once 160 ms are queued; if the queue runs dry the buffer fills again.  The tail
       of a call, shorter than that, plays once nothing more has arrived for 120 ms. */
    const int bytesPerMs = (int)(factor_ * 8U * 2U);
    const int prebuffer  = 160 * bytesPerMs;

    idleTicks_ = speech.empty() ? idleTicks_ + 1 : 0;

    if (!playing_ && !pending_.isEmpty() && (pending_.size() >= prebuffer || idleTicks_ >= 6))
        playing_ = true;

    if (playing_)
    {
        if (!pending_.isEmpty())
        {
            qint64 written = outDev_->write(pending_);
            if (written > 0)
                pending_.remove(0, (int)written);
        }
        else if (out_->bytesFree() >= out_->bufferSize() - 2 * bytesPerMs)
            playing_ = false;               /* the sound card has played everything: fill the buffer again */

        /* keep the delay bounded when the sound card does not keep up: 500 ms at most */
        if (pending_.size() > 500 * bytesPerMs)
            pending_.remove(0, pending_.size() - 500 * bytesPerMs);
    }
}
