/* OpenMT Radio: the RF spectrum of the received channel, from the HackRF bridge.
 *
 * The bridge (modem/hackrf-openmt-bridge and modem/sx1255-openmt-bridge, tools/rf_spectrum.h has the protocol) serves the spectrum of the 125 kHz around the
 * channel as UDP datagrams, to whoever asks for them.  This client asks (a request every second, only while the Monitor tab is
 * shown), keeps the last frame and a history of frames for the waterfall, and works out the channel power, the noise floor, the
 * frequency of the signal and the overload state of the HackRF's converter.  Modems that cannot do it (every one except the
 * HackRF bridge) just never answer. */

#ifndef OPENMT_RF_SPECTRUM_H
#define OPENMT_RF_SPECTRUM_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openmt
{

struct RfFrame
{
    uint32_t           centreHz  = 0;
    uint32_t           spanHz    = 0;
    std::vector<float> db;                  /* dBFS per bin, lowest frequency first */
    bool               overload  = false;   /* the HackRF's 8 bit samples hit full scale */
    int                adcPeak   = 0;       /* 0 to 127 */
    double             adcClipPercent = 0.0;
    double             binHz     = 0.0;

    /* derived */
    double             channelDb = -200.0;  /* power in the 12.5 kHz channel, dBFS */
    double             noiseDb   = -200.0;  /* median of the bins outside +-10 kHz, dBFS per bin */
    double             peakDb    = -200.0;  /* strongest bin in the channel */
    double             peakOffsetHz = 0.0;  /* its frequency relative to the channel centre */
    double             snrDb     = 0.0;     /* the channel's strongest bin over the noise floor */
};

struct RfSpectrumSnapshot
{
    bool                         valid = false;     /* a frame arrived in the last two seconds */
    RfFrame                      frame;
    std::vector<std::vector<uint8_t>> history;      /* the last frames, oldest first, 0.5 dB steps below full scale (the waterfall) */
};

/* Reads a frame of the bridge's protocol; false when it is not one */
bool parseRfFrame(const uint8_t* data, size_t length, RfFrame& frame, std::vector<uint8_t>* raw);

class SpectrumClient
{
public:
    SpectrumClient();
    ~SpectrumClient();

    bool start(const std::string& host, unsigned int port, std::string& error);   /* port 0: off, true */
    void stop();

    /* the requests are sent while this is true */
    void setWanted(bool wanted);

    RfSpectrumSnapshot snapshot();
    unsigned long framesReceived() const { return frames_; }

    /* "host:port" being asked, empty when it is off (or has not been started) */
    std::string target() const { return target_; }

    static const size_t HistoryRows = 120U;

private:
    void run();

    int                  fd_;
    std::atomic<bool>    running_;
    std::atomic<bool>    wanted_;
    std::thread          thread_;
    std::mutex           mutex_;
    RfFrame              last_;
    long long            lastMs_;
    std::deque<std::vector<uint8_t>> history_;
    std::atomic<unsigned long> frames_;
    unsigned char        addr_[32];            /* sockaddr_in of the bridge */
    size_t               addrLen_;
    std::string          target_;
};

}

#endif
