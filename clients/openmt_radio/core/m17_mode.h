/* OpenMT Radio: M17, 4FSK at 4800 baud (9600 bit/s), voice with Codec2 3200 (software, no dongle).
 *
 * Transmit: speech -> two Codec2 blocks (8 bytes each) per 40 ms stream frame -> link setup frame (callsigns, type) and
 * stream frames (LICH, frame number, payload) from the repo's M17FrameEncoder -> 4FSK samples (24 kHz) -> Modem_Host.
 * Receive: the reverse; the frames are decoded by the repo's M17FrameDecoder (the LICH of the stream frames gives the link
 * setup of a call that is joined late).  The levels and the modulation are those of protocols/m17/m17_host.cpp. */

#ifndef OPENMT_M17_MODE_H
#define OPENMT_M17_MODE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "codec2_codec.h"
#include "modem_link.h"
#include "radio_mode.h"

#include <M17/M17Datatypes.hpp>
#include <M17/M17FrameDecoder.hpp>
#include <M17/M17FrameEncoder.hpp>
#include <M17/M17LinkSetupFrame.hpp>

#include "dsp_compat.h"

namespace openmt
{

struct M17Config
{
    std::string  callsign   = "N0CALL"; /* this radio, up to 9 characters */
    std::string  dest       = "ALL";    /* the station called; "ALL" or "@ALL" call everyone */
    unsigned int can        = 0;        /* channel access number, 0 - 15 */
    unsigned int txDelayMs  = 200;      /* preamble before the call */
    unsigned int modeHangMs = 1500;     /* the modem is kept for this long after a call, then handed back */
    float        txLevel    = 1.0F;     /* scales the symbol level (outer symbol 1481 counts) */
    int          rxFilter   = 1;        /* 1: light (two samples averaged; for the discriminator output of a modem, which is already
                                           filtered), 0: root raised cosine (the host's own, for a clean signal) */
    float        rxGain     = 1.0F;     /* scales the samples from Modem_Host (-1 turns the polarity round) */
    float        micGain    = 1.0F;     /* scales the speech before the vocoder */
    bool         standIn    = false;    /* tests: a stand in for Codec2 that passes the 8 byte block through */
    std::string  metaText;              /* sent with every voice transmission (LSF metadata text), at most 52 bytes of UTF-8 */
};

class M17Mode : public RadioMode
{
public:
    M17Mode(ModemLink& link, const M17Config& cfg, const RadioEvents& events);
    ~M17Mode();

    std::string modeName() const;
    std::string modemType() const;

    /* false (and an error text) when the Codec2 library is not there */
    bool open(std::string& error);

    void start();
    void stop();

    void rxSamples(const int16_t* samples, size_t count);

    void pttDown();
    void pttUp();
    void micAudio(const int16_t* pcm, size_t count);
    bool transmitting() const;

    /* Text: the metadata text of the voice transmissions (changes from the next transmission on) and text messages
       (SMS, M17 packet mode).  A message is sent as soon as the transmitter is free. */
    bool supportsMessages() const { return true; }
    bool sendMessage(const std::string& to, const std::string& text, std::string& error);
    void setMetaText(const std::string& text);

    /* Limits of the text: the metadata holds four blocks of 13 bytes; a packet is at most 800 bytes: the type byte, the text, a
       zero byte and the 2 byte CRC */
    static const size_t MAX_META_BYTES = 52U;
    static const size_t MAX_SMS_BYTES  = 796U;

private:
    enum RxState { RX_SEARCH = 0, RX_FRAME };

    struct SyncMatch
    {
        double       err;       /* squared error of the 8 sync symbols against the pattern, as a part of their energy */
        double       center;
        double       outer;
        unsigned int kind;      /* 0 link setup, 1 stream, 2 packet, 3 end */
    };

    void rxSample(double v);
    bool matchSync(uint64_t c, SyncMatch& m) const;
    void rxSearch();
    void rxTrack();
    void rxFrame(uint64_t c);
    void rxStream();
    void rxMeta(M17::M17LinkSetupFrame& lsf);
    void rxPacket();
    void rxEnd(CallEvent how);
    void rxStart();
    void rxReset();
    std::string rxStartText() const;
    double y(uint64_t i) const;

    void txThread();
    void txCall();
    void txPacket(const std::string& to, const std::string& text);
    void txPreamble();
    void txBytes(const uint8_t* bytes, size_t count);
    void txFlush();

    void log(const std::string& text) const;

    ModemLink&   link_;
    M17Config    cfg_;
    RadioEvents  ev_;
    std::unique_ptr<Codec2Codec> codec_;

    std::atomic<bool> running_;
    std::thread       thread_;

    /* receive */
    std::mutex       rxMutex_;
    RxState          state_;
    std::vector<double> ring_;              /* matched filtered samples, indexed by sample number */
    double           hist_[64];             /* input history of the matched filter */
    unsigned int     histPos_;
    uint64_t         n_;                    /* samples received */
    double           errRing_[8];
    SyncMatch        matchRing_[8];
    uint64_t         pendingAt_;            /* sample number of the first match waiting for the best position, 0 = none */
    uint64_t         lock_;                 /* centre of the last symbol of the sync word of the frame being received */
    double           center_, outer_;       /* levels, smoothed over the frames */
    bool             levelsKnown_;
    unsigned int     misses_;               /* frames in a row without a sync word */
    M17::M17FrameDecoder decoder_;
    bool             rxActive_;
    std::string      rxSrc_, rxDst_;
    unsigned int     rxCan_;
    unsigned long    rxErrs_;
    unsigned long    rxBits_;
    unsigned long    rxFrames_, rxBadFrames_, rxMiss_;
    int              rxMaxErrs_;
    int              seq_;
    unsigned int     seqRun_;
    unsigned long    rxMuted_;
    double           rxOuterSum_;
    int              lastFn_;
    bool             rxUnsupportedLogged_;
    std::string      rxMetaBlocks_[4];      /* the text blocks of the call being received */
    unsigned int     rxMetaHave_;           /* bit field of the blocks received */
    std::string      rxMetaText_;           /* the text complete so far, shown once per call and change */
    std::vector<uint8_t> pktData_;          /* the packet being received */
    int              pktNext_;              /* the frame number expected next, -1 = no packet under way */

    /* transmit */
    std::mutex         txMutex_;
    std::condition_variable txCv_;
    std::deque<int16_t> mic_;
    bool               ptt_;
    std::atomic<bool>  txActive_;
    uint64_t           hangUntil_;
    std::string        metaText_;           /* the metadata text for the next voice transmission */
    struct Message
    {
        std::string to, text;
    };
    std::deque<Message> messages_;          /* waiting to be sent */
    arm_fir_interpolate_instance_q15 modFilter_;
    q15_t              modState_[16];
};

}

#endif
