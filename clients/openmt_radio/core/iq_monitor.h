/* OpenMT Radio: the IQ monitor.  The HackRF and SX1255 bridges can send the received channel itself (tools/rf_spectrum.h, the "IQST"
 * request: complex samples at 62.5 kS/s, low passed at 22 kHz).  From them this class makes what the sample based monitor
 * (rf_monitor.h) makes from the modem's discriminator samples, but from the true signal: a frequency discriminator of its own
 * gives the real deviation in Hz (no assumption about the modem's scaling), the carrier offset is a real frequency error, and
 * the timing is chosen at 13 samples a symbol instead of 5.
 *
 * It fills the same MonitorSnapshot, in counts at 0.7 per Hz, so the Monitor tab draws it with the same code. */

#ifndef OPENMT_IQ_MONITOR_H
#define OPENMT_IQ_MONITOR_H

#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "rf_monitor.h"

namespace openmt
{

struct IqFrame
{
    uint32_t centreHz   = 0;
    uint32_t sampleRate = 0;
    uint32_t sequence   = 0;
    bool     overload   = false;
    bool     mirrored   = false;
    std::vector<std::complex<float>> samples;   /* full scale 1.0, the bridge's shift divided out */
};

/* Reads an "IQFR" datagram; false when it is not one */
bool parseIqFrame(const uint8_t* data, size_t length, IqFrame& frame);

class IqMonitor
{
public:
    IqMonitor();

    void configure(double baud, unsigned symbolLevels, double nominalOuter, SyncKind sync);
    void reset();

    /* from the thread that receives the frames */
    void feed(const IqFrame& frame);

    /* IQ arrived in the last two seconds */
    bool available() const;

    /* from any thread: analyses the last 0.6 s.  valid() of the result is true when the IQ is there */
    MonitorSnapshot snapshot();

    unsigned long framesReceived() const;

private:
    static const size_t RING = 62500U;      /* one second at 62.5 kS/s */

    mutable std::mutex                mutex_;
    std::vector<std::complex<float>>  ring_;
    size_t                            head_;
    uint64_t                          total_;
    double                            rate_;
    uint32_t                          lastSeq_;
    bool                              haveSeq_;
    unsigned long                     frames_;
    unsigned long                     lost_;
    bool                              overload_;
    long long                         lastMs_;
    double                            baud_;
    unsigned                          levels_;
    double                            nominal_;
    SyncKind                          sync_;
    SyncSmoother                      smoother_;
    std::mutex                        smoothMutex_;
    MonitorSnapshot snapshotRaw();
};

}

#endif
