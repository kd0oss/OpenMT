/* OpenMT Radio: FM, a voice radio on a repeater run by protocols/FM/fm_host.
 *
 * Transmit: the microphone speech (8 kHz) is band limited to 300 Hz - 3 kHz, raised to 24 kHz, and the sub-audible CTCSS tone
 * is added to it; the samples are the modulating signal the modem puts on the carrier.  The tone is sent from the moment the
 * key goes down, before the speech, as the repeater needs some time to recognise it.
 * Receive: the demodulated samples (24 kHz) go through the same filters and the noise squelch and tone squelch of fm_host
 * (protocols/FM/fm_dsp.c is used as it is); what is left, with the CTCSS tone filtered off, is the speech. */

#ifndef OPENMT_FM_MODE_H
#define OPENMT_FM_MODE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "modem_link.h"
#include "radio_mode.h"

#include <fm_dsp.h>

namespace openmt
{

struct FmConfig
{
    float        txCtcss      = 88.5F;      /* Hz, sent with the speech to open the repeater; 0 = none */
    unsigned int dtmfHangMs   = 2000U;      /* the key stays down this long after the last digit, for the next one */
    float        dtmfLevel    = 2.5F;       /* percent of full scale for each of the two DTMF tones (2.5 is about 1.5 kHz each on the SX1255 bridge) */
    float        txToneLevel  = 1.5F;       /* percent of full scale; 1.5 is about 1 kHz of tone deviation on the SX1255 bridge */
    float        rxCtcss      = 0.0F;       /* Hz, tone squelch on the receiver; 0 = none */
    unsigned int squelch      = 0;          /* 0 noise squelch (carrier), 1 tone squelch, 2 both, 3 open (always on) */
    float        noiseOpen    = 8.0F;       /* noise squelch, percent of full scale: opens at or below this ... */
    float        noiseClose   = 14.0F;      /* ... and closes at or above this (the host's own values) */
    float        toneThreshold = 15.0F;     /* tone squelch, percent */
    unsigned int toneHoldMs   = 400U;       /* the tone squelch closes this long after the tone is gone */
    unsigned int txDelayMs    = 400U;       /* tone only, before the speech: the repeater needs about 400 ms to know the tone */
    unsigned int txTailMs     = 100U;       /* tone only, after the speech */
    unsigned int rxHangMs     = 300U;       /* the call is over this long after the squelch closed */
    unsigned int modeHangMs   = 1500U;      /* the modem is kept for this long after a call, then handed back */
    bool         autoLevel    = true;       /* the received speech is brought to a steady level (a discriminator can be very quiet) */
    float        rxGain       = 1.0F;       /* scales the samples from Modem_Host, before the squelch */
    float        micGain      = 1.0F;       /* scales the speech */
    float        txLevel      = 1.0F;       /* scales the modulation (speech and tone) */
};

class FmMode : public RadioMode
{
public:
    FmMode(ModemLink& link, const FmConfig& cfg, const RadioEvents& events);
    ~FmMode();

    std::string modeName() const;
    std::string modemType() const;

    void start();
    void stop();

    void rxSamples(const int16_t* samples, size_t count);

    void pttDown();
    void pttUp();
    void micAudio(const int16_t* pcm, size_t count);
    bool transmitting() const;

    bool supportsDtmf() const { return true; }
    bool sendDtmf(char digit);

private:
    void rxSample(float x);
    void rxEmit(bool open);
    void rxClose();

    void txThread();
    void txCall();
    void txFrame(const float* speech24, size_t count, bool speechOn);

    void log(const std::string& text) const;

    ModemLink&   link_;
    FmConfig     cfg_;
    RadioEvents  ev_;

    std::atomic<bool> running_;
    std::thread       thread_;

    /* receive */
    std::mutex       rxMutex_;
    FmAudio          rxAudio_;
    FmNoiseSq        noise_;
    FmCtcssDet       ctcss_;
    FmDecim          decim_;
    unsigned int     lookback_, tail_, delay_;      /* in 24 kHz samples */
    std::deque<float>  dly_;                        /* delayed audio, 8 kHz samples with their gate flag */
    std::deque<char>   dlyOpen_;
    bool             gate_;
    bool             rxActive_;
    unsigned int     closedRun_;                    /* 24 kHz samples since the gate closed */
    float            level_;                        /* smoothed noise level for the log */
    float            agcPower_, agcGain_;           /* automatic level */
    double           dbgSum_, dbgPeak_, dbgPre_;    /* the receiver, for the log line */
    uint64_t         dbgCount_;
    bool             dbgOpenSeen_;
    std::vector<int16_t> out_;
    uint64_t         rxCount_;

    /* transmit */
    std::mutex         txMutex_;
    std::condition_variable txCv_;
    std::deque<int16_t> mic_;
    bool               ptt_;
    unsigned int       dtmfIdle_ = 0U;              /* frames of 4 ms without a digit while the key is held by them */
    bool               autoKey_;                    /* the key was put down by a DTMF digit, not by the person */
    std::deque<int16_t> dtmf_;                      /* DTMF audio (8 kHz) waiting to go out with the speech */
    std::atomic<bool>  txActive_;
    uint64_t           hangUntil_;
    FmAudio            txAudio_;
    FmInterp           interp_;
    FmTone             tone_;
};

}

#endif
