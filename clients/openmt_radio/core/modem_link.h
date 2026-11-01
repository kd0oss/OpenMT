/* OpenMT Radio: the link to Modem_Host.
 *
 * The program is a client of Modem_Host like the protocol hosts (nxdn_host, dmr_host, ...): it
 * connects to TCP port 18000 + modem - 1, registers a mode with a MODE packet and then exchanges
 * SAMP packets (96 samples of 24 kHz, 16 bit little endian, in a 200 byte packet) with it.
 * A packet is  0x61  <length hi> <length lo>  <type length = 4>  <4 character type>  <payload>.
 */

#ifndef OPENMT_MODEM_LINK_H
#define OPENMT_MODEM_LINK_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openmt
{

class ModemLink
{
public:
    struct Config
    {
        std::string  host      = "127.0.0.1";
        unsigned int modem     = 1;          /* 1 - 10: the port is basePort + modem - 1 */
        unsigned int basePort  = 18000;
        std::string  modeName  = "NXDN";     /* the label Modem_Host knows this service by */
        std::string  modemType = "4FSK";
        uint8_t      txLevel   = 50;         /* put in the MODE packet, as the hosts do */
        unsigned int txAheadMs = 200;        /* samples queued ahead in Modem_Host (its buffer holds 240 ms), 0 = do not wait */
        unsigned int txDrainMs = 250;        /* the end of transmission waits until the last sample has been on the air this long ago:
                                                a modem un-keys at once and drops what it still has queued (the SX1255 bridge holds
                                                about 250 ms), which cut the end of every transmission; 0 = send it at once */
    };

    typedef std::function<void(const int16_t*, size_t)> SamplesFn;
    typedef std::function<void(bool)>                  DuplexFn;
    typedef std::function<void()>                      ClosedFn;
    typedef std::function<void(int32_t, uint32_t)>     TimingFn;

    ModemLink();
    ~ModemLink();

    /* Handlers are called from the reader thread; set them before open() */
    void setHandlers(const SamplesFn& samples, const DuplexFn& duplex, const ClosedFn& closed);

    void setTimingHandler(const TimingFn& timing);

    /* Reports what arrives from Modem_Host (samples starting and stopping, other packets), from a watcher thread */
    void setLogHandler(const std::function<void(const std::string&)>& log);

    bool open(const Config& cfg, std::string& error);
    void close();
    bool isOpen() const;

    /* Ask Modem_Host to switch to this mode / hand the modem back */
    bool requestMode();
    bool releaseMode();
    /* Hands the modem back whether or not this client thinks it holds it.  Modem_Host only acts on it when the mode it is
       holding is this client's own, so a host of another protocol is never affected */
    bool forceIdle();
    bool modeHeld() const;

    /* Transmit: begin, any number of samples (24 kHz, host counts), end.  txSamples() waits when more than
       txAheadMs are already queued, so the transmission goes out in real time. */
    void txBegin();
    void txSamples(const int16_t* samples, size_t count);
    void txEnd();           /* waits for the queued samples to be played (txDrainMs) before the end is sent */

    static const unsigned int SamplesPerPacket = 96U;
    static const unsigned int PacketBytes      = 200U;

private:
    bool writeAll(const uint8_t* data, size_t length);
    bool readFully(uint8_t* data, size_t length);
    bool registerMode();
    void sendPacket(const char* type);
    void readLoop();
    void watchLoop();

    Config              cfg_;
    int                 fd_;
    std::atomic<bool>   open_;
    std::atomic<bool>   modeHeld_;
    std::thread         reader_;
    std::thread         watcher_;
    std::function<void(const std::string&)> logFn_;
    std::atomic<uint64_t> rxPackets_;       /* every packet that arrived */
    std::atomic<uint64_t> rxSampPackets_;   /* the SAMP ones */
    std::atomic<uint64_t> rxSampCount_;     /* samples in them */
    std::atomic<uint64_t> rxAbsSum_;        /* sum of the magnitudes of those samples */
    std::mutex          typeMutex_;
    std::string         otherTypes_;        /* the other packet types seen (4 characters each) */
    std::mutex          writeMutex_;

    SamplesFn           samplesFn_;
    DuplexFn            duplexFn_;
    ClosedFn            closedFn_;
    TimingFn            timingFn_;

    uint8_t             pkt_[PacketBytes];
    unsigned int        count_;
    uint64_t            playEnd_;       /* ms, steady clock: when the queued samples are played */
};

}

#endif
