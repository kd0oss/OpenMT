#include "radio_engine.h"

#include "ambe_vocoder.h"

namespace openmt
{

bool modeAvailable(ModeId mode)
{
    return mode == MODE_NXDN || mode == MODE_DMR || mode == MODE_P25 || mode == MODE_YSF || mode == MODE_DSTAR || mode == MODE_M17 || mode == MODE_FM;
}

const char* modeLabel(ModeId mode)
{
    switch (mode)
    {
        case MODE_NXDN:  return "NXDN";
        case MODE_DMR:   return "DMR";
        case MODE_P25:   return "P25";
        case MODE_YSF:   return "YSF";
        case MODE_DSTAR: return "D-STAR";
        case MODE_M17:   return "M17";
        case MODE_FM:    return "FM";
    }
    return "?";
}

RadioEngine::RadioEngine() :
    events(),
    linkClosed(),
    link_(),
    vocoder_(NULL),
    nullVoc_(),
    dongle_(),
    dongleDevice_(),
    dongleBaud_(0U),
    mode_(),
    running_(false),
    capture_(NULL)
{
}

RadioEngine::~RadioEngine()
{
    stop();
    closeDongle();
}

bool RadioEngine::openDongle(const std::string& device, unsigned int baud, int chunkBytes, int gapUs, std::string& error)
{
    if (running_ && dongle_ && vocoder_ == dongle_.get() && (device != dongleDevice_ || baud != dongleBaud_))
    {
        error = "the dongle is in use: disconnect before changing its port";
        return false;
    }

    AmbeVocoder::setSerialPacing(chunkBytes, gapUs);

    if (dongle_ && dongle_->isOpen())
    {
        if (device == dongleDevice_ && baud == dongleBaud_)
        {
            dongle_->log = events.log;
            if (!dongle_->ensureAlive())        /* it stopped answering while nobody used it: reset it, else open it afresh */
            {
                dongle_.reset();
                dongleDevice_.clear();
                dongleBaud_ = 0U;
                return openDongle(device, baud, chunkBytes, gapUs, error);
            }
            return true;
        }
        closeDongle();
    }

    std::unique_ptr<AmbeVocoder> ambe(new AmbeVocoder());

    ambe->log = events.log;
    if (!ambe->open(device, baud, error))
        return false;
    dongle_ = std::move(ambe);
    dongleDevice_ = device;
    dongleBaud_ = baud;
    if (events.log)
        events.log("AMBE3000R: " + dongle_->description() + " on " + device + " opened; the port stays open while the program runs");
    return true;
}

void RadioEngine::closeDongle()
{
    if (!dongle_)
        return;
    if (running_ && vocoder_ == dongle_.get())
        return;                         /* in use */
    dongle_.reset();
    dongleDevice_.clear();
    dongleBaud_ = 0U;
}

bool RadioEngine::dongleOpen() const
{
    return dongle_ && dongle_->isOpen();
}

bool RadioEngine::running() const
{
    return running_;
}

std::string RadioEngine::vocoderDescription() const
{
    return vocoder_ != NULL ? vocoder_->description() : std::string("none");
}

bool RadioEngine::start(const EngineSettings& settings, std::string& error)
{
    if (running_)
    {
        error = "already running";
        return false;
    }

    if (!modeAvailable(settings.mode))
    {
        error = std::string(modeLabel(settings.mode)) + " is not available in this version";
        return false;
    }

    /* the vocoder first: there is no point in connecting to Modem_Host without speech */
    EngineSettings s = settings;

    if (s.mode == MODE_FM)
    {
        nullVoc_.reset(new NullVocoder());              /* FM is analogue speech: no vocoder at all */
        vocoder_ = nullVoc_.get();
    }
    else if (s.mode == MODE_M17)
    {
        nullVoc_.reset(new NullVocoder());              /* M17 speech is Codec2, in software: the dongle is not used */
        vocoder_ = nullVoc_.get();
        s.m17.standIn = s.noVocoder;
    }
    else if (s.noVocoder)
    {
        nullVoc_.reset(new NullVocoder());
        vocoder_ = nullVoc_.get();
        if (s.nxdn.layout < 0)
            s.nxdn.layout = (int)NXDN_CHIP_AIR;         /* the stand in passes the 72 bit block as it is */
        if (s.dmr.layout < 0)
            s.dmr.layout = (int)DMR_CHIP_AIR;
        if (s.ysf.layout < 0)
            s.ysf.layout = (int)NXDN_CHIP_AIR;
    }
    else
    {
        if (!openDongle(s.ambeDevice, s.ambeBaud, s.ambeChunk, s.ambeGapUs, error))
            return false;
        vocoder_ = dongle_.get();
        vocoder_->setCodec(Vocoder::CODEC_AMBE2);       /* the chip may have been left in D-STAR's rate by the last run */
    }

    switch (s.mode)
    {
        case MODE_NXDN:
            mode_.reset(new NxdnMode(link_, *vocoder_, s.nxdn, events));
            break;
        case MODE_DMR:
            mode_.reset(new DmrMode(link_, *vocoder_, s.dmr, events));
            break;
        case MODE_P25:
            mode_.reset(new P25Mode(link_, *vocoder_, s.p25, events));
            break;
        case MODE_YSF:
            mode_.reset(new YsfMode(link_, *vocoder_, s.ysf, events));
            break;
        case MODE_DSTAR:
            mode_.reset(new DstarMode(link_, *vocoder_, s.dstar, events));
            break;
        case MODE_M17:
        {
            std::unique_ptr<M17Mode> m17(new M17Mode(link_, s.m17, events));

            if (!m17->open(error))
                return false;
            mode_ = std::move(m17);
            break;
        }
        case MODE_FM:
            mode_.reset(new FmMode(link_, s.fm, events));
            break;
        default:
            error = "mode not available";
            return false;
    }

    s.link.modeName  = mode_->modeName();
    s.link.modemType = mode_->modemType();

    RadioMode* mode = mode_.get();

    if (!s.rxCapture.empty())
    {
        capture_ = std::fopen(s.rxCapture.c_str(), "wb");
        if (capture_ != NULL && events.log)
            events.log("recording the samples received from Modem_Host to " + s.rxCapture);
        else if (events.log)
            events.log("cannot write " + s.rxCapture);
    }

    FILE** cap = &capture_;
    link_.setHandlers(
        [mode, cap](const int16_t* samples, size_t count)
        {
            if (*cap != NULL)
            {
                for (size_t i = 0U; i < count; i++)
                {
                    uint8_t b[2] = {(uint8_t)(samples[i] & 0xFF), (uint8_t)((samples[i] >> 8) & 0xFF)};
                    std::fwrite(b, 1, 2, *cap);
                }
            }
            mode->rxSamples(samples, count);
        },
        [this, mode](bool duplex)
        {
            if (events.log)
                events.log(duplex ? "Modem_Host says the modem is duplex" : "Modem_Host says the modem is simplex");
            mode->modemDuplex(duplex);
        },
        [this]() { onLinkClosed(); });
    link_.setLogHandler(events.log);
    link_.setTimingHandler([mode](int32_t offset, uint32_t k) { mode->txTiming(offset, k); });

    if (!link_.open(s.link, error))
    {
        if (capture_ != NULL)
        {
            std::fclose(capture_);
            capture_ = NULL;
        }
        mode_.reset();
        vocoder_ = NULL;
        nullVoc_.reset();
        return false;
    }

    link_.forceIdle();                  /* a mode this radio held when it was last closed or crashed */
    mode_->start();
    running_ = true;
    return true;
}

void RadioEngine::stop()
{
    if (!running_ && !mode_)
        return;

    running_ = false;

    if (mode_)
        mode_->stop();

    link_.forceIdle();
    link_.close();
    if (capture_ != NULL)
    {
        std::fclose(capture_);
        capture_ = NULL;
    }
    mode_.reset();
    if (dongle_ && vocoder_ == dongle_.get())
        vocoder_->setCodec(Vocoder::CODEC_AMBE2);    /* the port stays open, the chip goes back to its usual rate */
    vocoder_ = NULL;
    nullVoc_.reset();
}

bool RadioEngine::releaseModem()
{
    if (!running_)
        return false;
    if (events.log)
        events.log("Modem_Host: asked to release the mode (only this radio's own mode is released)");
    return link_.forceIdle();
}

bool RadioEngine::dtmfSupported() const
{
    return running_ && mode_ && mode_->supportsDtmf();
}

bool RadioEngine::sendDtmf(char digit)
{
    if (!running_ || !mode_)
        return false;
    return mode_->sendDtmf(digit);
}

bool RadioEngine::messagesSupported() const
{
    return running_ && mode_ && mode_->supportsMessages();
}

bool RadioEngine::sendMessage(const std::string& to, const std::string& text, std::string& error)
{
    if (!running_ || !mode_)
    {
        error = "not connected";
        return false;
    }
    return mode_->sendMessage(to, text, error);
}

void RadioEngine::setMetaText(const std::string& text)
{
    if (running_ && mode_)
        mode_->setMetaText(text);
}

void RadioEngine::ptt(bool down)
{
    if (!running_ || !mode_)
        return;

    if (down)
        mode_->pttDown();
    else
        mode_->pttUp();
}

void RadioEngine::pushMic(const int16_t* pcm8k, size_t count)
{
    if (running_ && mode_)
        mode_->micAudio(pcm8k, count);
}

void RadioEngine::onLinkClosed()
{
    if (linkClosed)
        linkClosed();
}

}
