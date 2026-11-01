/* OpenMT Radio: NXDN, 4800 bit/s, 2400 baud 4FSK, voice with the AMBE+2 chip.
 *
 * Transmit: speech -> 20 ms vocoder blocks -> four blocks per 80 ms frame with the SACCH call information
 * -> 4FSK samples (24 kHz) -> Modem_Host.   Receive: the reverse.  The channel coding and the modulation are
 * the repo's own (protocols/NXDN/nxdn_func.c, nxdn_dsp.c); the frames are those of nxdnGenVoiceCall(). */

#ifndef OPENMT_NXDN_MODE_H
#define OPENMT_NXDN_MODE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "modem_link.h"
#include "radio_mode.h"
#include "vocoder.h"

#include "nxdn_dsp.h"
#include "nxdn_func.h"

namespace openmt
{

struct NxdnConfig
{
    unsigned int ran        = 1;        /* radio access number 0 - 63 */
    unsigned int srcId      = 1;        /* this radio's unit id */
    unsigned int dstId      = 9;        /* talk group or unit id called */
    bool         group      = true;
    int          layout     = -1;       /* arrangement of the chip's 72 bit block: -1 detect, else NXDN_CHIP_* */
    unsigned int txDelayMs  = 200;      /* preamble before the call */
    unsigned int modeHangMs = 1500;     /* the modem is kept for this long after a call, then handed back */
    unsigned int txShape    = NXDN_SHAPE_RRC;
    unsigned int rxShape    = NXDN_SHAPE_RRC;
    float        rxGain     = 1.0F;     /* scales the samples from Modem_Host */
    float        micGain    = 1.0F;     /* scales the speech before the vocoder */
};

/* Works out which arrangement of the 72 bit block the chip uses (it sends test speech through it and counts the
   corrected bits of the NXDN FEC words).  Returns NXDN_CHIP_NOFEC, NXDN_CHIP_AIR or NXDN_CHIP_CODING. */
unsigned int nxdnDetectLayout(Vocoder& vocoder, std::string* report);

class NxdnMode : public RadioMode
{
public:
    NxdnMode(ModemLink& link, Vocoder& vocoder, const NxdnConfig& cfg, const RadioEvents& events);
    ~NxdnMode();

    std::string modeName() const;
    std::string modemType() const;

    void start();
    void stop();

    void rxSamples(const int16_t* samples, size_t count);

    void pttDown();
    void pttUp();
    void micAudio(const int16_t* pcm, size_t count);
    bool transmitting() const;

    unsigned int layout() const { return layout_; }

private:
    static void frameCallback(void* ctx, const uint8_t* frame, bool lost);
    void onFrame(const uint8_t* raw, bool lost);
    void rxVoice(const uint8_t* net28);
    void rxEnd(CallEvent how);
    void rxSacch(const uint8_t* frame);

    void txThread();
    void txCall();
    bool txFrame(const uint8_t* frame);
    bool txVoiceFrame(const int16_t* pcm, unsigned int part, const uint8_t* l3);

    void log(const std::string& text) const;

    ModemLink&   link_;
    Vocoder&     vocoder_;
    NxdnConfig   cfg_;
    RadioEvents  ev_;
    unsigned int layout_;

    std::atomic<bool> running_;
    std::thread       thread_;

    /* receive */
    std::unique_ptr<NxdnRx> rx_;
    std::mutex       rxMutex_;
    bool             rxActive_;
    unsigned int     rxSrc_;
    unsigned int     rxDst_;
    bool             rxGroup_;
    uint8_t          rxL3_[22];
    uint8_t          rxParts_;
    unsigned long    rxErrs_;
    unsigned long    rxBits_;
    bool             rxVocoderOk_;

    /* transmit */
    NxdnTx             tx_;
    std::mutex         txMutex_;
    std::condition_variable txCv_;
    std::deque<int16_t> ring_;
    bool               ptt_;
    std::atomic<bool>  txActive_;
    uint64_t           hangUntil_;
};

}

#endif
