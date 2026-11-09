/* OpenMT Radio: the signal monitor, a virtual service monitor for the samples that Modem_Host delivers (16 bit, 24 kHz, the
 * demodulated signal: a frequency discriminator output, the same samples the radio modes decode).
 *
 * It keeps the last second of samples and, on request, works out from them: the signal level and clipping, the symbol timing, the
 * eye diagram, the histogram of the symbol values, the four (or two) symbol levels, the deviation relative to the nominal one of
 * the mode (and in Hz), the spread of the levels and a spectrum of the demodulated signal.
 *
 * Scale: the protocol hosts and the modem bridges use 0.7 sample counts per Hz of deviation (a nominal outer symbol of P25 is
 * 1260 counts for 1800 Hz).  The reading in Hz is right when the modem scales its samples the same way; the percentage of the
 * nominal level does not depend on the Hz scale but does on the modem's receive scaling. */

#ifndef OPENMT_RF_MONITOR_H
#define OPENMT_RF_MONITOR_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace openmt
{

/* The frame sync of a mode, when the monitor can look for it: the sync symbols are only the outer levels (+3 and -3), so they give
   the true outer deviation whatever the noise is (the mean of known symbols has no bias) */
enum class SyncKind
{
    None,
    P25,        /* 5575F5FF77FF */
    DMR         /* the four 48 bit syncs of base station and mobile, voice and data */
};

struct MonitorSnapshot
{
    bool     active      = false;   /* a signal is present (the level is above the gate) */
    uint64_t samples     = 0;       /* samples seen since the monitor was reset */
    double   rms         = 0.0;     /* counts */
    int      peak        = 0;       /* counts */
    bool     clipped     = false;   /* samples at the limit of 16 bits */
    double   baud        = 0.0;     /* symbols per second, 0 = a mode with no symbols (FM) */
    unsigned symbolLevels = 0;      /* 2 or 4, 0 = none */

    bool     levelsValid = false;
    double   levels[4]   = {0.0, 0.0, 0.0, 0.0};   /* counts, lowest first (the two outer ones for two levels: levels[0], levels[3]) */
    double   centre      = 0.0;     /* counts: the offset of the middle of the levels from zero */
    double   outer       = 0.0;     /* counts: half the distance between the outer levels */
    double   outerPercent = 0.0;    /* outer / the nominal outer level * 100 */
    double   outerHz     = 0.0;     /* outer deviation in Hz (at 0.7 counts per Hz) */
    double   nominalHz   = 0.0;
    double   spread      = 0.0;     /* counts: average standard deviation around the levels */
    double   blur        = 0.0;     /* spread / the distance between neighbouring levels: above about 0.4 the levels run together */

    bool     approximate = false;   /* the levels overlap (blur above 0.25): the deviation is only roughly right, high by about the noise */

    bool     syncLocked  = false;   /* the deviation comes from frame syncs: exact, however noisy the signal is */
    unsigned syncHits    = 0U;      /* the syncs that were found */
    bool     inverted    = false;   /* the syncs were found upside down: the signal is inverted */
    long long silentMs   = 0;       /* how long since the last samples arrived (0 while they do); the charts then show the last signal */
    double   averagedSeconds = 0.0; /* the sync locked reading is an average over this many seconds (0 = a single reading) */

    bool     fromIq      = false;   /* made from the IQ of the bridge (the true signal), not from the modem's samples */
    double   iqRate      = 0.0;     /* samples a second of the IQ */
    unsigned long iqFrames = 0UL;   /* frames received */
    unsigned long iqLost = 0UL;     /* frames missing in the sequence */
    bool     iqOverload  = false;   /* the converter reached full scale */

    std::vector<float>    eye;      /* eyeTraces rows of eyePoints values, each trace two symbols wide, centred on the sampling instant */
    unsigned              eyeTraces = 0;
    unsigned              eyePoints = 0;
    std::vector<uint32_t> hist;     /* histogram of the symbol values from histMin to histMax */
    double                histMin = 0.0;
    double                histMax = 0.0;
    std::vector<float>    spectrum; /* dB, 0 to 12 kHz */
};

/* The sync locked reading rests on a few syncs a second (P25 sends about 5), so each snapshot of it has a little noise.  This averages
   the readings over a few seconds (an exponential average with a time constant of 2.5 s), snapshot after snapshot.  The averaged
   values replace the levels, the offset, the deviation, the spread and the blur; the eye and the histogram stay as they are.  It
   starts again when the signal goes (1.5 s without a locked reading) or when a reading is far from the average (a different
   transmitter: the deviation differs by more than 20 % or the offset by more than 25 % of it). */
class SyncSmoother
{
public:
    SyncSmoother();

    void reset();

    /* s is changed in place; nowMs is a clock in milliseconds */
    void apply(MonitorSnapshot& s, long long nowMs);

    static const double TimeConstantS;

private:
    bool      have_;
    long long lastMs_;
    long long lastLockedMs_;
    double    levels_[4];
    double    centre_;
    double    outer_;
    double    spread_;
    double    blur_;
    double    seconds_;       /* how long it has been averaging */
};

class RfMonitor
{
public:
    RfMonitor();

    /* baud: symbols per second (0 for none), levels: 2 or 4, nominalOuter: counts of the outer symbol of the mode (0 = unknown) */
    void configure(double baud, unsigned symbolLevels, double nominalOuter, SyncKind sync = SyncKind::None);
    void reset();

    /* from the thread that receives the samples */
    void feed(const int16_t* samples, size_t count);

    /* from any thread (the window): analyses the samples of the last second */
    MonitorSnapshot snapshot();

    static const double COUNTS_PER_HZ;   /* 0.7 */

    /* The symbol part of the analysis, for any sample rate: w is the demodulated signal in counts (0.7 per Hz) of one burst of
       signal.  Fills in the eye, the histogram, the levels, the deviation and the sync lock of s (the rest of s is the caller's). */
    static void analyse(const std::vector<double>& w, double rate, double baud, unsigned levels, double nominal, SyncKind sync,
                        MonitorSnapshot& s);

private:
    static const size_t RING = 24000U;

    std::mutex           mutex_;
    std::vector<int16_t> ring_;
    size_t               head_;         /* next write position */
    uint64_t             total_;
    double               baud_;
    unsigned             levels_;
    double               nominal_;
    SyncKind             sync_;
    long long            lastFeedMs_;
    MonitorSnapshot snapshotRaw();
    SyncSmoother         smoother_;
    std::mutex           smoothMutex_;
};

}

#endif
