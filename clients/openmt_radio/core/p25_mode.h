/* OpenMT Radio: P25 phase 1, 4800 baud C4FM, voice with the IMBE codec of the AMBE3000R chip, in direct (simplex) mode.
 *
 * It is a P25 mobile talking to a simplex hotspot, the way the repo's p25_host talks to a radio: a transmission is the
 * start sync (a run of 0x77 bytes), a header data unit (HDU), pairs of logical link data units (LDU1 with the link
 * control, LDU2 with the message indicator and key information, nine 20 ms voice frames each) and a terminator
 * (TDU).  The frame coding (sync, NID, BCH, Golay, Hamming, Reed-Solomon, IMBE protection) is the repo's own
 * (protocols/P25/p25_func.c), the pulse (raised cosine, 0.2 roll off) and the levels are those of p25_host's
 * transmitter, and the receiver follows what p25_host's sample receiver looks for: the 24 symbol frame sync, the NID
 * that tells the kind of data unit and so its length, and frames that follow each other without a gap.
 *
 * Transmit: speech -> nine 20 ms IMBE frames per LDU (88 bit blocks, no error correction on the chip) -> LDU1 / LDU2
 * -> C4FM samples (24 kHz) -> Modem_Host.   Receive: the reverse.  The receiver is deaf while transmitting (half
 * duplex). */

#ifndef OPENMT_P25_MODE_H
#define OPENMT_P25_MODE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "modem_link.h"
#include "radio_mode.h"
#include "vocoder.h"

namespace openmt
{

struct P25Config
{
    unsigned int nac        = 0x293;    /* network access code, 0 - 4095 */
    unsigned int srcId      = 1;        /* this radio's id, 1 - 16777215 */
    unsigned int dstId      = 9;        /* talk group (1 - 65535) or radio called */
    bool         group      = true;
    unsigned int txDelayMs  = 300;      /* run of start sync bytes before the call, so the hotspot's squelch and AFC settle */
    unsigned int modeHangMs = 1500;     /* the modem is kept for this long after a call, then handed back */
    float        rxGain     = 1.0F;
    float        micGain    = 1.0F;
    std::function<void(const uint8_t*, unsigned int)> txTap;     /* sees each frame sent, with its length (for tests) */
};

class P25Mode : public RadioMode
{
public:
    P25Mode(ModemLink& link, Vocoder& vocoder, const P25Config& cfg, const RadioEvents& events);
    ~P25Mode();

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
    /* receive */
    enum RxState { RX_IDLE, RX_SYNC, RX_NID, RX_FRAME };

    void rxSample(int16_t sample);
    double rxCorrelate(uint64_t end) const;
    void rxLock(uint64_t end);
    void rxNid();
    void rxFrame();
    void rxSlice(uint64_t first, unsigned int symbols, uint8_t* out, bool adapt = false) const;
    void rxLevels(uint64_t first);
    void rxHandle(unsigned int duid, const uint8_t* frame);
    void rxVoice(const uint8_t* frame);
    void rxStart(unsigned int src, unsigned int dst, bool group);
    void rxEnd(CallEvent how);
    void rxLost();
    double rxAt(uint64_t index) const;

    /* transmit */
    void txThread();
    void txCall();
    void txSymbols(const int16_t* levels, size_t count);
    void txBytes(const uint8_t* bytes, size_t count);
    void buildHeader(uint8_t* frame);
    void buildLdu(const uint8_t* imbe, bool second, uint8_t* frame);
    void buildTerminator(uint8_t* frame);
    void sendFrame(const uint8_t* frame, unsigned int length);

    void log(const std::string& text) const;

    ModemLink&   link_;
    Vocoder&     vocoder_;
    P25Config    cfg_;
    RadioEvents  ev_;

    std::atomic<bool> running_;
    std::thread       thread_;

    /* receive state (link reader thread) */
    std::mutex            rxMutex_;
    std::vector<float>    ring_;            /* the one symbol (five sample) average of the last samples, by absolute number */
    uint64_t              pos_;             /* absolute number of the next sample */
    double                sum_;             /* the last five samples */
    int16_t               last_[5];
    double                corr_[128];        /* the sync correlation of the last samples */
    RxState               state_;
    bool                  tracked_;         /* the sync being followed came where the frame before said it would */
    double                symStep_;         /* samples per symbol as the modem delivers them (5 when its clock is exact) */
    uint64_t              syncEnd_;         /* the sample of the last symbol of the sync of the frame being received */
    uint64_t              decideAt_;        /* RX_SYNC: the sample at which the sync of the next frame is looked for */
    uint64_t              nextEnd_;         /* RX_SYNC: where it is expected */
    unsigned int          duid_;            /* the data unit being received */
    unsigned int          frameSyms_;       /* its length, in symbols */
    unsigned int          lastDuid_;
    unsigned int          missed_;          /* frames in a row whose sync was not found */
    double                centre_;          /* levels taken from the sync of the frame, used to slice it */
    double                threshold_;
    bool                  rxActive_;
    bool                  rxInfoKnown_;
    unsigned int          rxSrc_;
    unsigned int          rxDst_;
    bool                  rxGroup_;
    bool                  rxVocoderOk_;
    bool                  rxEncrypted_;
    unsigned int          rxAlgSeen_;       /* the algorithm of the last LDU2, and how many in a row said so */
    unsigned int          rxAlgCount_;
    uint64_t              rxLastMs_;
    double                lvl_;             /* the signal level, a running average */
    unsigned int          quiet_;           /* samples in a row below the level of a signal */
    uint64_t              rxLastN_;         /* the sample at which the last frame was decoded */
    uint64_t              rxVocoderRetryMs_;

    /* transmit state */
    std::mutex              txMutex_;
    std::condition_variable txCv_;
    std::deque<int16_t>     mic_;
    bool                    ptt_;
    std::atomic<bool>       txActive_;
    uint64_t                hangUntil_;
    int16_t                 modHist_[8];    /* the last eight symbol levels, newest first */
};

}

#endif
