/* OpenMT Radio: DMR, 4800 baud 4FSK, voice with the AMBE+2 chip, in direct (simplex) mode or as a subscriber radio of a
 * repeater or duplex hotspot.
 *
 * Repeater mode (DmrConfig::repeater): the receiver follows the continuous two slot downlink (base station sync, CACH,
 * a burst every 30 ms alternating between the slots) and decodes the chosen time slot, the slot being told by the TC bit
 * of the CACH like a real radio does.  A transmission is slot timed: one burst every 60 ms placed in the chosen slot,
 * with the transmitter's timing taken from the downlink (the sync of the uplink burst 31 samples after the sync of the
 * downlink burst of the slot), the receiver keeps running while the radio transmits.  Where the modem reports when its
 * transmit samples are on the air (the TXTM report) the placement is exact; otherwise it is estimated from the clocks
 * and DmrConfig::txTimingMs tunes it.
 *
 * Direct mode:
 * It is a mobile station talking to a simplex hotspot, the way the repo's dmr_host talks to a radio: one burst of 27.5 ms
 * and fill in a 60 ms period, MS sourced sync.  The burst coding (BPTC, Reed-Solomon, embedded signalling, slot type) is
 * the repo's own (protocols/DMR/dmr_func.c), the pulse shape and levels are those of dmr_host's transmitter, and the
 * receiver follows what dmr_host's sample receiver looks for.
 *
 * Transmit: speech -> three 20 ms vocoder blocks per burst -> burst (voice LC header, voice bursts A-F with embedded link
 * control, terminator with LC) -> 4FSK samples (24 kHz) -> Modem_Host.   Receive: the reverse. */

#ifndef OPENMT_DMR_MODE_H
#define OPENMT_DMR_MODE_H

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

/* Arrangements of the chip's 72 bit block, the values are those of NXDN_CHIP_* so the window can share its choice */
enum
{
    DMR_CHIP_AUTO   = -1,
    DMR_CHIP_CODING = 2,        /* the block is the three code words a (24), b (23), c (25) one after the other */
    DMR_CHIP_AIR    = 1         /* the block is the frame as it is on the air, already interleaved */
};

struct DmrConfig
{
    unsigned int colorCode  = 1;        /* 0 - 15 */
    unsigned int srcId      = 1;        /* this radio's id, 1 - 16777215 */
    unsigned int dstId      = 9;        /* talk group or radio called */
    bool         group      = true;
    int          layout     = DMR_CHIP_AUTO;
    unsigned int txDelayMs  = 360;      /* run of 0x5F bytes before the call, so the hotspot's squelch and AFC settle */
    unsigned int headers    = 2;        /* voice LC headers before the voice */
    unsigned int modeHangMs = 1500;     /* the modem is kept for this long after a call, then handed back */
    float        rxGain     = 1.0F;
    float        micGain    = 1.0F;
    bool         repeater   = false;    /* repeater or duplex hotspot: two slot downlink, slot timed uplink */
    unsigned int slot       = 1;        /* time slot, 1 or 2 (repeater mode) */
    int          txTimingMs = 100;      /* repeater mode without TX timing reports: delay from handing a sample to the modem to
                                           hearing it on the receive side, tune until the host sees the right slot */
    int          txOffsetSamples = 31;  /* uplink sync this many samples after the downlink sync of the slot, as dmr_host
                                           expects it: it sees a TS1 sync at cycle sample 495 for a downlink sync at 450-469 */
    std::function<void(const uint8_t*)> txTap;     /* sees each 33 byte burst sent (for tests) */
    std::function<void(uint64_t)>       txStart;   /* repeater mode: the symbol of the transmission each burst starts at (tests) */
};

/* Works out which arrangement the chip uses (test speech goes through it and the Golay words of the DMR frame are
   checked).  Returns DMR_CHIP_CODING or DMR_CHIP_AIR. */
unsigned int dmrDetectLayout(Vocoder& vocoder, std::string* report);

class DmrMode : public RadioMode
{
public:
    DmrMode(ModemLink& link, Vocoder& vocoder, const DmrConfig& cfg, const RadioEvents& events);
    ~DmrMode();

    std::string modeName() const;
    std::string modemType() const;

    void start();
    void stop();

    void rxSamples(const int16_t* samples, size_t count);

    void pttDown();
    void pttUp();
    void micAudio(const int16_t* pcm, size_t count);
    bool transmitting() const;

    void modemDuplex(bool duplex);
    void txTiming(int32_t offset, uint32_t k);

    unsigned int layout() const { return layout_; }

private:
    /* receive */
    enum RxState { RX_IDLE, RX_LOCKED };

    void rxSample(int16_t sample);
    double rxCorrelate(uint64_t end, double* energy, double* raw, bool gate = true, int* family = NULL) const;
    void rxDiagnose(int16_t sample, uint64_t n);
    void rxBurst(uint64_t syncEnd, int syncKind);
    void rxVoiceBurst(const uint8_t* frame, bool hasSync);
    void rxDataBurst(const uint8_t* frame);
    void rxStart(unsigned int src, unsigned int dst, bool group);
    void rxEnd(CallEvent how);
    void rxLost();
    int16_t rxAt(uint64_t index) const;
    int rxCachSlot(uint64_t syncEnd) const;       /* 0, 1 */
    bool txPlan(uint64_t minSym, uint64_t* start, bool* exact);
    void txGapTo(uint64_t symbol);
    bool txBurstTimed(const uint8_t* frame);

    /* transmit */
    void txThread();
    void txCall();
    void playTone(double startHz, double endHz, unsigned int ms);   /* a tone through the speaker, as a radio's permit tone */
    bool txBurst(const uint8_t* frame);
    void txSymbols(const int16_t* levels, size_t count);
    void txDibitBytes(const uint8_t* bytes, size_t count);
    void buildHeader(uint8_t type, uint8_t* frame);
    void buildWakeup(uint8_t* frame);
    bool buildVoice(const int16_t* pcm, unsigned int index, uint8_t* frame);

    void log(const std::string& text) const;

    ModemLink&   link_;
    Vocoder&     vocoder_;
    DmrConfig    cfg_;
    RadioEvents  ev_;
    unsigned int layout_;

    std::atomic<bool> running_;
    std::thread       thread_;

    /* receive state (link reader thread) */
    std::mutex            rxMutex_;
    std::vector<int16_t>  ring_;            /* the last samples, indexed by their absolute number modulo the size */
    uint64_t              pos_;             /* absolute number of the next sample */
    RxState               state_;
    uint64_t              syncEnd_;         /* LOCKED: sample number of the last symbol of the sync of the current burst */
    uint64_t              nextEnd_;         /* LOCKED: where the sync of the next burst is expected */
    uint64_t              burstDue_;        /* LOCKED: sample number at which the current burst is complete */
    int                   burstKind_;       /* +1 data sync, -1 voice sync, 0 none (coasting) */
    double                bestC_;           /* search for the sync near its expected place */
    double                bestRaw_;
    uint64_t              bestEnd_;
    double                refLevel_;        /* mean absolute sample of the last sync */
    double                centre_;          /* levels taken from the last sync, used to slice the burst */
    double                threshold_;
    double                histC_[5];        /* correlation and raw sum of the last five samples, newest last */
    double                histS_[5];
    int                   missed_;
    bool                  rxActive_;
    bool                  rxInfoKnown_;
    unsigned int          rxSrc_;
    unsigned int          rxDst_;
    bool                  rxGroup_;
    unsigned int          rxVoiceIndex_;    /* 0 = burst A ... 5 = F */
    unsigned long         rxErrs_;
    unsigned long         rxBits_;
    bool                  rxVocoderOk_;
    uint64_t              otherSlotLogMs_;      /* when a call in the other time slot was last reported */
    uint64_t              rxLastMs_;            /* the last burst of the call being received */
    uint64_t              rxVocoderRetryMs_;    /* when the vocoder is tried again after it failed */
    bool                  rxHeaderSeen_;

    /* repeater mode */
    uint64_t              period_;          /* samples between bursts of the stream: 1440 direct, 720 repeater */
    int                   lastSlot_;        /* slot of the last burst (0, 1), -1 none */
    unsigned int          noSync_;
    unsigned int          vote_[2];
    uint64_t              burstCount_;          /* bursts in a row that had no sync */
    uint64_t              dlRefEnd_;        /* sample number of the sync end of a downlink burst ... */
    int                   dlRefSlot_;       /* ... and its slot */
    uint64_t              dlLastMs_;        /* when it was seen */
    bool                  dlLogged_;
    double                clkMin_;          /* least delayed (arrival time - nominal sample time), this and the last window */
    double                clkPrev_;
    double                clkStart_;
    bool                  txRefValid_;      /* TX sample txRefK_ is on the air as RX sample txRefRx_ */
    int64_t               txRefRx_;
    uint32_t              txRefK_;
    double                txT0_;            /* wall time of the start of the transmission, seconds */
    bool                  modemSimplex_;
    int                   slotCorr_;        /* 0 or 720 samples: half a slot added to the transmit timing, learned from where the
                                               repeater puts our call */
    std::atomic<bool>     slotFixed_;       /* the timing was corrected during this call */

    /* what the receiver tells in the log when a signal is there and nothing is decoded */
    double                diagLevel_;       /* running mean absolute sample, about 20 ms */
    unsigned int          diagOn_;          /* samples the signal has been there without a lock */
    bool                  diagTold_;
    double                diagBestC_;
    int                   diagFamily_;
    unsigned int          diagLocks_;
    unsigned int          diagWrongCc_;
    unsigned int          diagDataLocks_;
    unsigned int          diagVoiceLocks_;
    unsigned int          diagVoiceBad_;
    unsigned int          diagHeaderBad_;

    /* transmit state */
    std::mutex              txMutex_;
    std::condition_variable txCv_;
    std::deque<int16_t>     mic_;
    bool                    ptt_;
    std::atomic<bool>       txActive_;
    uint64_t                hangUntil_;
    std::atomic<uint64_t>   txMuteUntil_;       /* repeater: the repeater's echo of this radio's own call is not received until */
    int16_t                 modHist_[9];    /* the last nine symbol levels, newest first */
    uint64_t                txSyms_;        /* symbols given to the modulator in this transmission */
    uint64_t                txNext_;        /* repeater mode: symbol at which the next burst starts, 0 = not timed */
};

}

#endif
