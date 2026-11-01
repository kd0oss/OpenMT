#include "fm_mode.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace openmt
{

namespace
{

uint64_t nowMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

const unsigned int FRAME24 = 96U;              /* samples in a packet to the modem */
const unsigned int FRAME8  = FRAME24 / 3U;

}

FmMode::FmMode(ModemLink& link, const FmConfig& cfg, const RadioEvents& events) :
    link_(link),
    cfg_(cfg),
    ev_(events),
    running_(false),
    thread_(),
    rxMutex_(),
    rxAudio_(),
    noise_(),
    ctcss_(),
    decim_(),
    lookback_(1440U),               /* 60 ms */
    tail_(2400U),                   /* 100 ms */
    delay_(2400U),
    dly_(),
    dlyOpen_(),
    gate_(false),
    rxActive_(false),
    closedRun_(0U),
    level_(0.0F),
    agcPower_(0.0F),
    agcGain_(1.0F),
    dbgSum_(0.0),
    dbgPeak_(0.0),
    dbgPre_(0.0),
    dbgCount_(0U),
    dbgOpenSeen_(false),
    out_(),
    rxCount_(0U),
    txMutex_(),
    txCv_(),
    mic_(),
    ptt_(false),
    autoKey_(false),
    txActive_(false),
    hangUntil_(0U),
    txAudio_(),
    interp_(),
    tone_()
{
    if ((cfg_.squelch == 1U || cfg_.squelch == 2U) && cfg_.rxCtcss > 0.0F)
    {
        /* the tone squelch needs about 400 ms to recognise the tone: keep that much of the speech that came before it */
        lookback_ = 12000U;
        delay_    = 12000U;
    }
    fmAudioInit(&rxAudio_);
    fmNoiseInit(&noise_, cfg_.noiseOpen, cfg_.noiseClose);
    fmCtcssInit(&ctcss_, cfg_.rxCtcss, cfg_.toneThreshold, cfg_.toneHoldMs);
    fmDecimInit(&decim_);
    fmAudioInit(&txAudio_);
    fmInterpInit(&interp_);
    fmToneInit(&tone_, cfg_.txCtcss);
}

FmMode::~FmMode()
{
    stop();
}

std::string FmMode::modeName() const
{
    return "FM";
}

std::string FmMode::modemType() const
{
    return "FM";
}

void FmMode::log(const std::string& text) const
{
    if (ev_.log)
        ev_.log(text);
}

void FmMode::start()
{
    if (running_)
        return;

    running_ = true;
    thread_ = std::thread(&FmMode::txThread, this);
}

void FmMode::stop()
{
    if (!running_)
        return;

    running_ = false;
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        ptt_ = false;
    }
    txCv_.notify_all();

    if (thread_.joinable())
        thread_.join();

    link_.releaseMode();
}

bool FmMode::transmitting() const
{
    return txActive_;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Receive                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

void FmMode::rxSamples(const int16_t* samples, size_t count)
{
    if (txActive_)
        return;                         /* a transmitter does not hear itself */

    std::lock_guard<std::mutex> lock(rxMutex_);

    for (size_t i = 0U; i < count; i++)
        rxSample((float)samples[i] * cfg_.rxGain);
}

void FmMode::rxSample(float x)
{
    float xd = fmAudioDc(&rxAudio_, x);
    bool nq = fmNoiseProcess(&noise_, xd);
    bool ct = cfg_.rxCtcss > 0.0F ? fmCtcssProcess(&ctcss_, xd) : false;
    float sh = fmAudioShape(&rxAudio_, xd);
    bool open;

    switch (cfg_.squelch)
    {
        case 1U:  open = cfg_.rxCtcss > 0.0F ? ct : nq; break;
        case 2U:  open = cfg_.rxCtcss > 0.0F ? (ct && nq) : nq; break;
        case 3U:  open = true; break;
        default:  open = nq; break;
    }

    level_ = noise_.level;
    rxCount_++;

    /* what the receiver sees, once in 3 seconds while anything is there (the log is how the squelch settings are set) */
    dbgSum_ += (double)x * (double)x;
    if (std::fabs((double)x) > dbgPeak_)
        dbgPeak_ = std::fabs((double)x);
    dbgPre_ += (double)sh * (double)sh;
    dbgOpenSeen_ = dbgOpenSeen_ || open;
    if (++dbgCount_ >= 72000U)
    {
        float bestF = 0.0F, bestFrac = 0.0F;

        if (cfg_.rxCtcss > 0.0F)
            fmCtcssScan(&ctcss_, &bestF, &bestFrac);

        char t[420], toneText[200] = "";

        if (cfg_.rxCtcss > 0.0F)
        {
            std::snprintf(toneText, sizeof(toneText), ", listening for %.1f Hz: %s (strongest %.1f Hz, %.0f%% of the low band)",
                          (double)cfg_.rxCtcss, ct ? "found" : "not found", (double)bestF, (double)bestFrac * 100.0);
            if (bestFrac > 0.5F && std::fabs(bestF - cfg_.rxCtcss) > 1.0F)
                std::snprintf(toneText + std::strlen(toneText), sizeof(toneText) - std::strlen(toneText),
                              " - THE SIGNAL CARRIES %.1f Hz", (double)bestF);
        }
        std::snprintf(t, sizeof(t),
                      "FM receiver: peak %.0f, rms %.0f (speech band %.0f), noise %.1f%% (opens below %.0f, closes above %.0f)%s, squelch %s",
                      dbgPeak_, std::sqrt(dbgSum_ / (double)dbgCount_), std::sqrt(dbgPre_ / (double)dbgCount_), (double)level_,
                      (double)cfg_.noiseOpen, (double)cfg_.noiseClose, toneText, open ? "open" : "closed");
        if (dbgOpenSeen_ || dbgPeak_ > 200.0)
            log(t);
        dbgSum_ = dbgPre_ = dbgPeak_ = 0.0;
        dbgCount_ = 0U;
        dbgOpenSeen_ = false;
    }

    /* the speech goes through a delay: when the squelch opens the last 60 ms are let through, when it closes the last 100 ms,
       which hold the burst of noise before the squelch notices the carrier has gone, are dropped */
    float d8;
    bool have = fmDecimPush(&decim_, sh, &d8);

    if (have)
    {
        dly_.push_back(d8);
        dlyOpen_.push_back(open ? 1 : 0);
    }

    if (open && !gate_)
    {
        size_t back = lookback_ / 3U;

        for (size_t k = 1U; k <= back && k <= dlyOpen_.size(); k++)
            dlyOpen_[dlyOpen_.size() - k] = 1;
    }
    else if (!open && gate_)
    {
        size_t back = tail_ / 3U;

        for (size_t k = 1U; k <= back && k <= dlyOpen_.size(); k++)
            dlyOpen_[dlyOpen_.size() - k] = 0;
    }

    if (open && !gate_)
    {
        gate_ = true;
        closedRun_ = 0U;
        if (!rxActive_)
        {
            rxActive_ = true;

            CallInfo info;

            info.event = CALL_RX_START;
            info.src = info.dst = 0U;
            info.group = true;
            info.ber = 0.0F;
            info.text = cfg_.squelch == 3U ? "open" : (cfg_.rxCtcss > 0.0F && cfg_.squelch != 0U ? "tone" : "carrier");
            if (ev_.call)
                ev_.call(info);
        }
    }
    else if (!open && gate_)
    {
        gate_ = false;
        closedRun_ = 0U;
    }
    else if (!open)
    {
        closedRun_++;
    }

    while (dly_.size() > delay_ / 3U)
    {
        float v = dly_.front();
        char o = dlyOpen_.front();

        dly_.pop_front();
        dlyOpen_.pop_front();
        if (o)
        {
            /* automatic level: the gain follows the power of the speech slowly, and drops at once for a loud one; it is
               not changed while there is nothing but the noise floor, so the squelch tail is not brought up */
            if (cfg_.autoLevel)
            {
                agcPower_ += (v * v - agcPower_) * (1.0F / 1600.0F);
                if (agcPower_ > 400.0F)
                {
                    float want = 3500.0F / std::sqrt(agcPower_);

                    if (want > 40.0F)
                        want = 40.0F;
                    if (want < 0.5F)
                        want = 0.5F;
                    agcGain_ += (want - agcGain_) * (want < agcGain_ ? 0.01F : 0.002F);
                }
                v *= agcGain_;
            }
            float s = v;

            if (s > 32767.0F)
                s = 32767.0F;
            if (s < -32768.0F)
                s = -32768.0F;
            out_.push_back((int16_t)lrintf(s));
        }
        else if (!out_.empty())
        {
            /* the speech stopped: what is left of the block goes out padded */
            while (out_.size() < 160U)
                out_.push_back(0);
        }

        while (out_.size() >= 160U)
        {
            if (ev_.rxAudio)
                ev_.rxAudio(out_.data(), 160U);
            out_.erase(out_.begin(), out_.begin() + 160);
        }
    }

    /* the call ends when the squelch has been closed for a while */
    if (rxActive_ && !gate_ && closedRun_ > (cfg_.rxHangMs * 24U))
        rxClose();
}

void FmMode::rxClose()
{
    CallInfo info;

    rxActive_ = false;
    info.event = CALL_RX_END;
    info.src = info.dst = 0U;
    info.group = true;
    info.ber = 0.0F;
    info.text = "";
    if (ev_.call)
        ev_.call(info);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Transmit                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------ */

void FmMode::pttDown()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        mic_.clear();
        autoKey_ = false;                       /* the person has the key now; DTMF waiting joins the speech */
        ptt_ = true;
    }
    txCv_.notify_all();
}

void FmMode::pttUp()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);

        if (!dtmf_.empty())
            autoKey_ = true;                    /* digits still to go out: the key stays down until they have */
        else
            ptt_ = false;
    }
    txCv_.notify_all();
}

void FmMode::micAudio(const int16_t* pcm, size_t count)
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);

        if (!ptt_ || autoKey_)
            return;

        for (size_t i = 0U; i < count; i++)
        {
            float v = pcm[i] * cfg_.micGain;

            if (v > 32767.0F)
                v = 32767.0F;
            if (v < -32768.0F)
                v = -32768.0F;
            mic_.push_back((int16_t)v);
        }

        while (mic_.size() > 8000U)             /* never more than 1 s behind */
            mic_.pop_front();
    }
    txCv_.notify_all();
}

/* One DTMF digit as 8 kHz audio: 100 ms of the two tones, 80 ms of silence (the repeater's decoder wants about 51 ms of
   the digit and a block without it before the next).  The key is put down if it is not, and the digits go out with
   the speech, or alone when nobody is talking. */
bool FmMode::sendDtmf(char digit)
{
    static const char  KEYS[] = "123A456B789C*0#D";
    static const float LOW[4] = {697.0F, 770.0F, 852.0F, 941.0F};
    static const float HIGH[4] = {1209.0F, 1336.0F, 1477.0F, 1633.0F};
    const char*        at;

    if (digit >= 'a' && digit <= 'd')
        digit = (char)(digit - 'a' + 'A');
    at = digit != '\0' ? std::strchr(KEYS, digit) : NULL;
    if (!running_ || !link_.isOpen() || at == NULL)
        return false;

    const int          key = (int)(at - KEYS);
    const float        fl = LOW[key / 4], fh = HIGH[key % 4];
    const unsigned int on = 800U, off = 640U, ramp = 40U;      /* 100 ms, 80 ms, 5 ms at 8 kHz */
    const float        pi2 = 6.28318530717958647692F;

    std::lock_guard<std::mutex> lock(txMutex_);

    if (dtmf_.size() > 8000U)                   /* a second of digits waiting is enough */
        return false;

    for (unsigned int i = 0U; i < on + off; i++)
    {
        float v = 0.0F;

        if (i < on)
        {
            float a = cfg_.dtmfLevel * 327.67F;

            if (i < ramp)
                a *= (float)i / (float)ramp;
            else if (on - i < ramp)
                a *= (float)(on - i) / (float)ramp;
            v = a * (std::sin(pi2 * fl * (float)i / 8000.0F) + std::sin(pi2 * fh * (float)i / 8000.0F));
        }
        dtmf_.push_back((int16_t)lrintf(v));
    }

    if (!ptt_)
    {
        mic_.clear();
        autoKey_ = true;
        ptt_ = true;
    }
    txCv_.notify_all();
    return true;
}

void FmMode::txThread()
{
    std::unique_lock<std::mutex> lock(txMutex_);

    while (running_)
    {
        if (!ptt_)
        {
            txCv_.wait_for(lock, std::chrono::milliseconds(100));

            if (!ptt_ && !txActive_ && link_.modeHeld() && nowMs() >= hangUntil_)
                link_.releaseMode();
            continue;
        }

        lock.unlock();
        txCall();
        lock.lock();
    }
}

/* One packet to the modem: 96 samples of modulation, the speech (if on) and the tone */
void FmMode::txFrame(const float* speech24, size_t count, bool speechOn)
{
    int16_t out[FRAME24];
    const float toneAmp = cfg_.txToneLevel * 327.67F;

    for (size_t i = 0U; i < FRAME24; i++)
    {
        float a = (speechOn && i < count) ? speech24[i] : 0.0F;
        float v = (a * cfg_.txLevel) + fmToneNext(&tone_) * toneAmp * cfg_.txLevel;

        if (v > 32000.0F)
            v = 32000.0F;
        if (v < -32000.0F)
            v = -32000.0F;
        out[i] = (int16_t)lrintf(v);
    }
    link_.txSamples(out, FRAME24);
}

void FmMode::txCall()
{
    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        if (rxActive_)
            rxClose();
        gate_ = false;
        dly_.clear();
        dlyOpen_.clear();
        out_.clear();
    }

    txActive_ = true;

    CallInfo info;
    info.event = CALL_TX_START;
    info.src = info.dst = 0U;
    info.group = true;
    info.ber = 0.0F;
    if (cfg_.txCtcss > 0.0F)
    {
        char t[48];

        std::snprintf(t, sizeof(t), "CTCSS %.1f", (double)cfg_.txCtcss);
        info.text = t;
    }
    else
        info.text = "carrier";
    if (ev_.call)
        ev_.call(info);

    fmAudioInit(&txAudio_);
    fmInterpInit(&interp_);
    fmToneInit(&tone_, cfg_.txCtcss);

    {
        char t[160], tone[60] = "no tone";

        if (cfg_.txCtcss > 0.0F)
            std::snprintf(tone, sizeof(tone), "CTCSS %.1f Hz at %.1f%%", (double)cfg_.txCtcss, (double)cfg_.txToneLevel);
        std::snprintf(t, sizeof(t), "FM: transmitting with %s, the tone alone for %u ms before the speech", tone, cfg_.txDelayMs);
        log(t);
    }

    link_.requestMode();
    link_.txBegin();

    /* the tone alone, until the repeater has known it for long enough */
    unsigned int lead = (cfg_.txDelayMs * 24U) / FRAME24;
    bool ok = true;

    for (unsigned int i = 0U; i < lead && ok && running_; i++)
    {
        txFrame(NULL, 0U, false);
        ok = link_.isOpen();
        {
            std::lock_guard<std::mutex> lock(txMutex_);
            if (!ptt_ && mic_.empty())
                break;                  /* a very short key down: no speech to wait for */
        }
    }

    /* the speech, 4 ms at a time (32 samples at 8 kHz make one packet of 96 at 24 kHz) */
    while (ok && running_)
    {
        int16_t pcm[FRAME8];
        size_t got = 0U;
        bool last = false;

        {
            std::unique_lock<std::mutex> lock(txMutex_);

            while (running_ && mic_.size() < FRAME8 && ptt_ && !autoKey_)
            {
                if (txCv_.wait_for(lock, std::chrono::milliseconds(200)) == std::cv_status::timeout)
                    break;
            }
            got = mic_.size() < FRAME8 ? mic_.size() : FRAME8;
            for (size_t i = 0U; i < got; i++)
            {
                pcm[i] = mic_.front();
                mic_.pop_front();
            }
            last = !ptt_;

            if (autoKey_)
            {
                /* nobody is talking: the digits are all that goes out, then the key is let go a moment after the last */
                if (dtmf_.empty())
                {
                    if (++dtmfIdle_ * 4U >= cfg_.dtmfHangMs)
                    {
                        autoKey_ = false;
                        ptt_ = false;
                        last = true;
                    }
                }
                else
                    dtmfIdle_ = 0U;
                got = FRAME8;                   /* the frame is made even when there is no speech, the link paces it */
                std::memset(pcm, 0, sizeof(pcm));
            }
            else
                dtmfIdle_ = 0U;

            for (size_t i = 0U; i < FRAME8 && !dtmf_.empty(); i++)
            {
                int v = (int)pcm[i] + (int)dtmf_.front();

                dtmf_.pop_front();
                pcm[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
        }

        if (got == 0U && last)
            break;

        for (size_t i = got; i < FRAME8; i++)
            pcm[i] = 0;

        float s24[FRAME24];

        for (size_t i = 0U; i < FRAME8; i++)
        {
            float three[FM_LINK_DECIM];

            fmInterpRun(&interp_, (float)pcm[i], three);
            for (unsigned int k = 0U; k < (unsigned int)FM_LINK_DECIM; k++)
                s24[i * 3U + k] = fmAudioShape(&txAudio_, fmAudioDc(&txAudio_, three[k]));
        }
        txFrame(s24, FRAME24, true);
        ok = link_.isOpen();

        if (last && got < FRAME8)
            break;
    }

    /* the tone alone at the end, then the end of the transmission */
    if (link_.isOpen())
    {
        unsigned int tail = (cfg_.txTailMs * 24U) / FRAME24;

        for (unsigned int i = 0U; i < tail; i++)
            txFrame(NULL, 0U, false);
        link_.txEnd();
    }

    {
        std::lock_guard<std::mutex> lock(txMutex_);

        if (!ok)
            ptt_ = false;
        if (!ptt_)
        {
            autoKey_ = false;
            dtmf_.clear();
        }
    }

    hangUntil_ = nowMs() + cfg_.modeHangMs;
    txActive_ = false;

    info.event = CALL_TX_END;
    if (ev_.call)
        ev_.call(info);
}

}
