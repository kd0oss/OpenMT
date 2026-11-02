#include "radiocontroller.h"

RadioController::RadioController(QObject* parent) :
    QObject(parent),
    engine_()
{
}

RadioController::~RadioController()
{
    stop();
}

bool RadioController::start(const openmt::EngineSettings& settings, QString& error)
{
    engine_.events.rxAudio = [this](const int16_t* pcm, size_t count)
    {
        QVector<qint16> v(pcm, pcm + count);
        emit rxAudio(v);
    };
    engine_.events.call = [this](const openmt::CallInfo& info)
    {
        emit callEvent((int)info.event, info.src, info.dst, info.group, info.ber, QString::fromStdString(info.text));
    };
    engine_.events.log = [this](const std::string& text)
    {
        emit logLine(QString::fromStdString(text));
    };
    engine_.linkClosed = [this]()
    {
        emit linkLost();
    };

    std::string err;
    if (!engine_.start(settings, err))
    {
        error = QString::fromStdString(err);
        return false;
    }
    return true;
}

void RadioController::stop()
{
    engine_.stop();
}

bool RadioController::running() const
{
    return engine_.running();
}

QString RadioController::vocoderDescription() const
{
    return QString::fromStdString(engine_.vocoderDescription());
}

void RadioController::ptt(bool down)
{
    engine_.ptt(down);
}

bool RadioController::openDongle(const QString& device, unsigned int baud, int chunkBytes, int gapUs, QString& error)
{
    engine_.events.log = [this](const std::string& text)
    {
        emit logLine(QString::fromStdString(text));
    };

    std::string err;

    if (!engine_.openDongle(device.toStdString(), baud, chunkBytes, gapUs, err))
    {
        error = QString::fromStdString(err);
        return false;
    }
    return true;
}

bool RadioController::dongleOpen() const
{
    return engine_.dongleOpen();
}

bool RadioController::dtmfSupported() const
{
    return engine_.dtmfSupported();
}

bool RadioController::sendDtmf(char digit)
{
    return engine_.sendDtmf(digit);
}

bool RadioController::messagesSupported() const
{
    return engine_.messagesSupported();
}

bool RadioController::sendMessage(const QString& to, const QString& text, QString& error)
{
    std::string err;
    QByteArray utf8 = text.toUtf8();

    if (!engine_.sendMessage(to.toStdString(), std::string(utf8.constData(), (size_t)utf8.size()), err))
    {
        error = QString::fromStdString(err);
        return false;
    }
    return true;
}

void RadioController::setMetaText(const QString& text)
{
    QByteArray utf8 = text.toUtf8();

    engine_.setMetaText(std::string(utf8.constData(), (size_t)utf8.size()));
}

bool RadioController::releaseModem()
{
    return engine_.releaseModem();
}

void RadioController::pushMic(const QVector<qint16>& pcm)
{
    engine_.pushMic(reinterpret_cast<const int16_t*>(pcm.constData()), (size_t)pcm.size());
}
