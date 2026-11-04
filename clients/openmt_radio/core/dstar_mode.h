/* OpenMT Radio: D-STAR, GMSK 4800 bit/s, voice with the AMBE chip at its D-STAR rate.
 *
 * Transmit: speech -> 20 ms vocoder blocks (9 bytes) -> frames of 9 bytes of voice and 3 bytes of slow data (the text
 * message, or the frame sync every 21st frame) behind a header (RPT2, RPT1, UR, MY) -> GMSK samples (24 kHz) -> Modem_Host.
 * Receive: the reverse.  The header coding is the one of protocols/DSTAR/dstar_host.c (core/dstar_codec.c is cut out of
 * it by tools/gen_dstar_codec.py); the bit and frame layout and the modulation are those of dstar_host. */

#ifndef OPENMT_DSTAR_MODE_H
#define OPENMT_DSTAR_MODE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "modem_link.h"
#include "radio_mode.h"
#include "vocoder.h"

namespace openmt
{

struct DstarConfig
{
    std::string  callsign   = "N0CALL"; /* MY: this radio, up to 8 characters */
    std::string  suffix     = "";       /* the four character suffix after MY */
    std::string  ur         = "CQCQCQ"; /* UR: the station called; CQCQCQ is a call to everyone */
    std::string  rpt1       = "N0CALL B"; /* RPT1: the repeater or hotspot heard (callsign and module) */
    std::string  rpt2       = "N0CALL G"; /* RPT2: its gateway */
    std::string  message    = "";       /* the 20 character text message of the slow data */
    unsigned int txDelayMs  = 300;      /* bit sync before the header */
    unsigned int modeHangMs = 1500;     /* the modem is kept for this long after a call, then handed back */
    float        txLevel    = 1.0F;     /* scales the symbol level (841 counts) */
    float        rxGain     = 1.0F;     /* scales the samples from Modem_Host (-1 turns the polarity round) */
    float        micGain    = 1.0F;     /* scales the speech before the vocoder */
};

class DstarMode : public RadioMode
{
public:
    DstarMode(ModemLink& link, Vocoder& vocoder, const DstarConfig& cfg, const RadioEvents& events);
    ~DstarMode();

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
    enum RxState { RX_SEARCH = 0, RX_HEADER, RX_DATA };

    void rxSample(int16_t s);
    void rxSearch(uint64_t c);
    void rxBits();
    void rxBit(bool bit);
    void rxHeader();
    void rxFrame();
    void rxSlowData(const uint8_t* data, unsigned int index);
    void rxFlushAudio();
    void rxStart(const std::string& text);
    void rxEnd(CallEvent how);
    void rxReset();
    std::string rxText() const;

    void txThread();
    void txCall();
    void txBytes(const uint8_t* bytes, size_t count);
    void txFlush();
    void txFrame(unsigned int index, const uint8_t* ambe);
    void txHeader();

    void log(const std::string& text) const;

    ModemLink&   link_;
    Vocoder&     vocoder_;
    DstarConfig  cfg_;
    RadioEvents  ev_;
    char         my_[9];
    char         suffix_[5];
    char         ur_[9];
    char         rpt1_[9];
    char         rpt2_[9];

    std::atomic<bool> running_;
    std::thread       thread_;

    /* receive */
    std::mutex       rxMutex_;
    RxState          state_;
    std::vector<int16_t> ring_;             /* the last samples, indexed by sample number */
    uint64_t         n_;                    /* number of samples received */
    double           dc_;                   /* slow estimate of the offset of the demodulated samples */
    double           lockLevel_;            /* level when the header was found */
    uint64_t         diagAt_;               /* sample number of the next report of an unrecognised signal */
    unsigned int     diagBest_;             /* fewest sync errors seen since the last report */
    double           diagLevel_;            /* highest level seen since the last report */
    double           level_;                /* average magnitude of the samples */
    uint64_t         sr_[5];                /* the last bits decided at each of the five possible bit positions */
    double           pos_;                  /* sample number of the centre of the next bit */
    int              prevBit_;
    unsigned int     bitCount_;             /* bits of the header or frame collected */
    uint8_t          hdr_[83];
    uint8_t          frame_[12];
    uint64_t         eotReg_;
    unsigned int     frameIndex_;
    unsigned int     syncMisses_;
    uint64_t         hdrReg_;               /* the last 32 bits decided while a header is collected (a better sync match restarts it) */
    unsigned int     lockErr_;              /* bit errors of the sync match the header started at */
    unsigned int     candErr_;              /* ... of the match waiting for its phase to be chosen */
    uint64_t         pendingC_;             /* sample of a sync match waiting for the best phase to be chosen, 0 = none */
    unsigned int     diagBad_;              /* badHeaders_ at the last report */
    unsigned int     badHeaders_;           /* headers whose CRC was wrong */
    bool             inv_;                  /* the signal is upside down: bits are decided the other way */
    uint64_t         quietFrom_;            /* sample number since when the level has been low, 0 = it has not */
    bool             rxActive_;
    bool             rxInfoKnown_;
    std::string      rxMy_, rxUr_, rxRpt1_, rxRpt2_;
    char             rxMessage_[21];
    unsigned int     rxMessageSeen_;        /* bit mask of the four text blocks received */
    int              rxBlock_;              /* text block whose last three characters are in the next frame, -1 none */
    unsigned long    rxErrs_;
    unsigned long    rxBits_;
    bool             rxVocoderOk_;
    std::vector<uint8_t> rxAudio_;          /* voice blocks waiting for the vocoder, 9 bytes each */

    /* transmit */
    std::mutex         txMutex_;
    std::condition_variable txCv_;
    std::deque<int16_t> mic_;
    bool               ptt_;
    std::atomic<bool>  txActive_;
    uint64_t           hangUntil_;
    int                txHist_[3];          /* the last three symbol levels of the modulator */
    uint8_t            slow_[60];           /* the slow data of frames 1 to 20, scrambled */
};

}

#endif
