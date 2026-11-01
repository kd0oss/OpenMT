/* OpenMT Radio: Yaesu System Fusion (YSF), C4FM 9600 bit/s, V/D mode 2, voice with the AMBE+2 chip.
 *
 * Transmit: speech -> 20 ms vocoder blocks -> five blocks per 100 ms frame (the first 27 bits of each block sent three
 * times) with the callsigns in the short data channel -> C4FM samples (24 kHz) -> Modem_Host.   Receive: the reverse.
 * The channel coding and the modulation are the repo's own (protocols/YSF/ysf_func.c, ysf_dsp.c); the frames are
 * those of ysfGenVoiceCallAudio().  V/D mode 1, data FR and voice FR frames are recognised but not decoded. */

#ifndef OPENMT_YSF_MODE_H
#define OPENMT_YSF_MODE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "modem_link.h"
#include "radio_mode.h"
#include "vocoder.h"

#include "nxdn_func.h"          /* the chip's 49 bit parameter layout is the one NXDN uses */
#include "ysf_dsp.h"
#include "ysf_func.h"

namespace openmt
{

struct YsfConfig
{
    std::string  callsign   = "N0CALL"; /* this radio, up to 10 characters */
    std::string  dest       = "CQCQCQ"; /* called station; CQCQCQ is a group call */
    unsigned int dgid       = 0;        /* digital group id, 0 - 99 */
    int          layout     = -1;       /* arrangement of the chip's 72 bit block: -1 detect, else NXDN_CHIP_* */
    unsigned int txDelayMs  = 300;      /* preamble before the call */
    unsigned int modeHangMs = 1500;     /* the modem is kept for this long after a call, then handed back */
    unsigned int txShape    = YSF_SHAPE_RRC;
    unsigned int rxShape    = YSF_SHAPE_RRC;
    float        txLevel    = 1.0F;     /* scales the outer symbol level (1890 counts) */
    float        rxGain     = 1.0F;     /* scales the samples from Modem_Host */
    float        micGain    = 1.0F;     /* scales the speech before the vocoder */
};

class YsfMode : public RadioMode
{
public:
    YsfMode(ModemLink& link, Vocoder& vocoder, const YsfConfig& cfg, const RadioEvents& events);
    ~YsfMode();

    std::string modeName() const;
    std::string modemType() const;

    void start();
    void stop();

    void rxSamples(const int16_t* samples, size_t count);

    void pttDown();
    void pttUp();
    void micAudio(const int16_t* pcm, size_t count);
    bool transmitting() const;

private:
    static void frameCallback(void* ctx, const uint8_t* frame, bool lost);
    void onFrame(const uint8_t* raw, bool lost);
    void rxStart(const char* source, const char* dest);
    void rxInfo();
    void rxVoice(const uint8_t* frame);
    void rxEnd(CallEvent how);

    void txThread();
    void txCall();
    bool txFrame(const uint8_t* frame);
    bool txVoiceFrame(const int16_t* pcm, unsigned int fn);

    void log(const std::string& text) const;

    ModemLink&   link_;
    Vocoder&     vocoder_;
    YsfConfig    cfg_;
    RadioEvents  ev_;
    unsigned int layout_;
    char         call_[YSF_CALLSIGN_LENGTH + 1U];
    char         dest_[YSF_CALLSIGN_LENGTH + 1U];

    std::atomic<bool> running_;
    std::thread       thread_;

    /* receive */
    std::unique_ptr<YsfRx> rx_;
    std::mutex       rxMutex_;
    bool             rxActive_;
    bool             rxInfoKnown_;
    YsfPayload       rxPay_;
    unsigned long    rxErrs_;
    unsigned long    rxBits_;
    bool             rxVocoderOk_;
    bool             rxUnsupportedLogged_;

    /* transmit */
    YsfTx              tx_;
    std::mutex         txMutex_;
    std::condition_variable txCv_;
    std::deque<int16_t> ring_;
    bool               ptt_;
    std::atomic<bool>  txActive_;
    uint64_t           hangUntil_;
};

}

#endif
