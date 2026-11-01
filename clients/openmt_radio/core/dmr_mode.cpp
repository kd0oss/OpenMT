#include "dmr_mode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C"
{
#include "dmr_func.h"
#include "dmr_fec.h"
}

namespace openmt
{

/* The repo's DMR code keeps its state in the global slotData[]: entry 0 is used for transmit, entry 1 for receive.
   Everything that touches it runs under this lock. */
static std::mutex gDmr;

static const unsigned int TX_SLOT = 0U;
static const unsigned int RX_SLOT = 1U;

static uint64_t nowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/* dmr_host's transmit pulse: root raised cosine, 0.2 roll off, 9 symbols, 5 samples per symbol (taps in Q15) */
static const int16_t RRC[45] = {
    0, 0, 0, 0, 850, 219, -720, -1548, -1795, -1172, 237, 1927, 3120, 3073, 1447, -1431, -4544, -6442,
    -5735, -1633, 5651, 14822, 23810, 30367, 32767, 30367, 23810, 14822, 5651, -1633, -5735, -6442,
    -4544, -1431, 1447, 3073, 3120, 1927, 237, -1172, -1795, -1548, -720, 219, 850};

static const int16_t LEVEL_A = 1362;        /* dibit 11 */
static const int16_t LEVEL_B = 454;         /* 10 */
static const int16_t LEVEL_C = -454;        /* 00 */
static const int16_t LEVEL_D = -1362;       /* 01 */

static const unsigned int SPS = 5U;         /* samples per symbol */

/* The fill that follows each burst of a simplex transmission (MMDVM's CDMRDMOTX) */
static const uint8_t DMO_FILL[39] = {
    0x63U, 0xEAU, 0x00U, 0x76U, 0x6CU, 0x76U, 0xC4U, 0x52U, 0xC8U, 0x78U,
    0x09U, 0x2DU, 0xB8U, 0x79U, 0x27U, 0x57U, 0x9BU, 0x31U, 0xBCU, 0x3EU,
    0xEAU, 0x45U, 0xC3U, 0x30U, 0x49U, 0x17U, 0x93U, 0xAEU, 0x8BU, 0x6DU,
    0xA4U, 0xA5U, 0xADU, 0xA2U, 0xF1U, 0x35U, 0xB5U, 0x3CU, 0x1EU};

/* The data syncs of the four families (base station sourced, mobile sourced, direct mode slot 1 and slot 2) in the sign a
   sample has for each of their 24 symbols; the voice sync of a family is its complement.  A mobile may use any of them. */
static const char* const FAMILY_NAME[4] = {"BS sourced", "MS sourced", "direct slot 1", "direct slot 2"};
static int8_t gPattern[4][24];
static bool   gPatternsReady = false;

static void makePatterns()
{
    const unsigned char* sync[4] = {BS_SOURCED_DATA_SYNC, MS_SOURCED_DATA_SYNC, DIRECT_SLOT1_DATA_SYNC, DIRECT_SLOT2_DATA_SYNC};

    for (unsigned int f = 0U; f < 4U; f++)
        for (unsigned int i = 0U; i < 24U; i++)
        {
            unsigned int bit = 4U + 2U * i;                 /* the 48 bit sync starts after the first nibble */
            unsigned int hi = (sync[f][bit >> 3] >> (7U - (bit & 7U))) & 1U;
            unsigned int lo = (sync[f][(bit + 1U) >> 3] >> (7U - ((bit + 1U) & 7U))) & 1U;

            /* dibit 11 is the lowest symbol and 01 the highest, and a sample has the opposite sign of its symbol */
            gPattern[f][i] = (hi == 1U && lo == 1U) ? (int8_t)1 : (int8_t)-1;
        }
    gPatternsReady = true;
}

static const unsigned int BURST_SYMBOLS   = 132U;
static const unsigned int SYNC_FIRST      = 54U;        /* the sync is symbols 54 - 77 of the burst */
static const unsigned int SYNC_LAST       = 77U;
static const unsigned int PERIOD_SAMPLES  = 1440U;      /* 60 ms */
static const double       SYNC_ACCEPT     = 0.80;       /* correlation needed to lock onto a sync from cold */
static const double       SYNC_TRACK      = 0.60;       /* and to take a sync near where it is expected */
static const int          TRACK_WINDOW    = 4;          /* samples either side of the expected place */
static const unsigned int RING_SIZE       = 16384U;

/* ------------------------------------------------------------------------------------------------------------ */
/* The chip's 72 bit block and the burst                                                                        */
/* ------------------------------------------------------------------------------------------------------------ */

static bool getBit(const uint8_t* p, unsigned int i)
{
    return (p[i >> 3] & (0x80U >> (i & 7U))) != 0U;
}

static void putBit(uint8_t* p, unsigned int i, bool b)
{
    if (b)
        p[i >> 3] = (uint8_t)(p[i >> 3] | (0x80U >> (i & 7U)));
    else
        p[i >> 3] = (uint8_t)(p[i >> 3] & ~(0x80U >> (i & 7U)));
}

/* Where bit k (0 - 71) of the chip block is in the burst, for vocoder frame n (0 - 2) */
static unsigned int airPosition(unsigned int layout, unsigned int n, unsigned int k)
{
    if (layout == (unsigned int)DMR_CHIP_AIR)
        return dmrFecPos(n, k);
    if (k < 24U)
        return dmrFecPos(n, DMR_FEC_A[k]);
    if (k < 47U)
        return dmrFecPos(n, DMR_FEC_B[k - 24U]);
    return dmrFecPos(n, DMR_FEC_C[k - 47U]);
}

static void chipToAir(unsigned int layout, const uint8_t* chip, unsigned int n, uint8_t* burst)
{
    for (unsigned int k = 0U; k < 72U; k++)
        putBit(burst, airPosition(layout, n, k), getBit(chip, k));
}

static void airToChip(unsigned int layout, const uint8_t* burst, unsigned int n, uint8_t* chip)
{
    std::memset(chip, 0x00, Vocoder::BlockBytes);
    for (unsigned int k = 0U; k < 72U; k++)
        putBit(chip, k, getBit(burst, airPosition(layout, n, k)));
}

unsigned int dmrDetectLayout(Vocoder& vocoder, std::string* report)
{
    unsigned long err[3] = {0UL, 0UL, 0UL};
    unsigned int frames = 0U;
    double ph = 0.0;
    uint32_t rnd = 12345U;
    const double TWO_PI = 6.283185307179586;

    for (unsigned int f = 0U; f < 60U; f++)
    {
        int16_t pcm[Vocoder::Samples];
        uint8_t chip[Vocoder::BlockBytes];
        double amp = 600.0 + 150.0 * (f % 20U), freq = 250.0 + 40.0 * f;

        for (unsigned int k = 0U; k < Vocoder::Samples; k++)
        {
            rnd = rnd * 1664525U + 1013904223U;
            ph += TWO_PI * freq / 8000.0;
            pcm[k] = (int16_t)std::lrint(amp * std::sin(ph) + ((int)(rnd >> 24) - 128) * 3.0);
        }

        if (!vocoder.encode(pcm, chip))
            break;

        for (unsigned int l = DMR_CHIP_AIR; l <= (unsigned int)DMR_CHIP_CODING; l++)
        {
            uint8_t burst[33];

            std::memset(burst, 0x00, sizeof(burst));
            chipToAir(l, chip, 0U, burst);
            err[l] += dmrFecFrame(burst, 0U, false);
        }
        frames++;
    }

    if (frames == 0U)
    {
        if (report)
            *report = "layout detection: the chip did not answer, assuming coding order";
        return (unsigned int)DMR_CHIP_CODING;
    }

    unsigned int best = err[DMR_CHIP_CODING] <= err[DMR_CHIP_AIR] ? (unsigned int)DMR_CHIP_CODING : (unsigned int)DMR_CHIP_AIR;

    if (report)
    {
        char text[200];

        std::snprintf(text, sizeof(text), "DMR block layout detected from %u test frames: %s (Golay errors per frame: "
                      "coding order %.2f, air %.2f)", frames, best == (unsigned int)DMR_CHIP_AIR ? "air" : "coding order",
                      (double)err[DMR_CHIP_CODING] / frames, (double)err[DMR_CHIP_AIR] / frames);
        *report = text;
    }
    return best;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* DmrMode                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

DmrMode::DmrMode(ModemLink& link, Vocoder& vocoder, const DmrConfig& cfg, const RadioEvents& events) :
    link_(link),
    vocoder_(vocoder),
    cfg_(cfg),
    ev_(events),
    layout_(cfg.layout >= 0 ? (unsigned int)cfg.layout : (unsigned int)DMR_CHIP_CODING),
    running_(false),
    thread_(),
    rxMutex_(),
    ring_(RING_SIZE, 0),
    pos_(0U),
    state_(RX_IDLE),
    syncEnd_(0U),
    nextEnd_(0U),
    burstDue_(0U),
    burstKind_(0),
    bestC_(0.0),
    bestEnd_(0U),
    refLevel_(0.0),
    bestRaw_(0.0),
    centre_(0.0),
    threshold_(800.0),
    missed_(0),
    rxActive_(false),
    rxInfoKnown_(false),
    rxSrc_(0U),
    rxDst_(0U),
    rxGroup_(true),
    rxVoiceIndex_(0U),
    rxErrs_(0UL),
    rxBits_(0UL),
    rxVocoderOk_(true),
    otherSlotLogMs_(0U),
    rxLastMs_(0U),
    rxVocoderRetryMs_(0U),
    rxHeaderSeen_(false),
    period_(cfg.repeater ? PERIOD_SAMPLES / 2U : PERIOD_SAMPLES),
    lastSlot_(-1),
    noSync_(0U),
    vote_(),
    burstCount_(0U),
    dlRefEnd_(0U),
    dlRefSlot_(0),
    dlLastMs_(0U),
    dlLogged_(false),
    clkMin_(1e18),
    clkPrev_(1e18),
    clkStart_(0.0),
    txRefValid_(false),
    txRefRx_(0),
    txRefK_(0U),
    txT0_(0.0),
    modemSimplex_(false),
    slotCorr_(0),
    slotFixed_(false),
    diagLevel_(0.0),
    diagOn_(0U),
    diagTold_(false),
    diagBestC_(0.0),
    diagFamily_(0),
    diagLocks_(0U),
    diagWrongCc_(0U),
    diagDataLocks_(0U),
    diagVoiceLocks_(0U),
    diagVoiceBad_(0U),
    diagHeaderBad_(0U),
    txMutex_(),
    txCv_(),
    mic_(),
    ptt_(false),
    txActive_(false),
    txMuteUntil_(0U),
    hangUntil_(0U),
    txSyms_(0U),
    txNext_(0U)
{
    if (!gPatternsReady)
        makePatterns();
    for (unsigned int i = 0U; i < 5U; i++)
        histC_[i] = histS_[i] = 0.0;
    std::memset(modHist_, 0x00, sizeof(modHist_));
}

DmrMode::~DmrMode()
{
    stop();
}

std::string DmrMode::modeName() const
{
    return "DMR";
}

std::string DmrMode::modemType() const
{
    return "4FSK";
}

void DmrMode::log(const std::string& text) const
{
    if (ev_.log)
        ev_.log(text);
}

void DmrMode::start()
{
    if (running_)
        return;

    if (cfg_.layout < 0)
    {
        std::string report;

        layout_ = dmrDetectLayout(vocoder_, &report);
        log(report);
    }

    running_ = true;
    thread_ = std::thread(&DmrMode::txThread, this);
}

void DmrMode::stop()
{
    if (!running_)
        return;

    running_ = false;
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        ptt_ = false;
    }
    txCv_.notify_all();

    if (thread_.joinable())
        thread_.join();

    link_.releaseMode();
}

bool DmrMode::transmitting() const
{
    return txActive_;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Receive                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

void DmrMode::rxSamples(const int16_t* samples, size_t count)
{
    if (!running_ || (txActive_ && !cfg_.repeater))
        return;                 /* direct mode is half duplex: the receiver is deaf while transmitting */

    std::lock_guard<std::mutex> lock(rxMutex_);

    for (size_t i = 0U; i < count; i++)
    {
        float v = samples[i] * cfg_.rxGain;

        if (v > 32767.0F)
            v = 32767.0F;
        if (v < -32768.0F)
            v = -32768.0F;
        rxSample((int16_t)v);
    }

    if (cfg_.repeater)
    {
        /* the least delayed packet gives the offset between the sample clock and the wall clock */
        double now = (double)nowMs() / 1000.0;
        double off = now - (double)pos_ / 24000.0;

        if (clkStart_ == 0.0 || now - clkStart_ > 2.0)
        {
            clkPrev_  = clkMin_;
            clkMin_   = off;
            clkStart_ = now;
        }
        else if (off < clkMin_)
            clkMin_ = off;
    }
}

int16_t DmrMode::rxAt(uint64_t index) const
{
    return ring_[index & (RING_SIZE - 1U)];
}

/* The slot of the burst whose sync ends at 'syncEnd', from the CACH of the 12 symbols before it: bit 0 is the access
   type, bit 4 the TC bit (0 for slot 1, 1 for slot 2), bits 8 and 12 the link control start/stop, bits 14, 18 and 22 a
   Hamming (7,4) check of them (as dmr_host's dmrSendCACH builds it).  One bit error is corrected. */
int DmrMode::rxCachSlot(uint64_t syncEnd) const
{
    bool b[24];

    for (int j = 0; j < 12; j++)
    {
        double raw = (double)rxAt((uint64_t)((int64_t)syncEnd + (int64_t)SPS * ((int64_t)(j - 12) - (int64_t)SYNC_LAST)));
        double x = raw - centre_;
        unsigned int dibit;

        if (x < -threshold_)
            dibit = 1U;
        else if (x < 0.0)
            dibit = 0U;
        else if (x < threshold_)
            dibit = 2U;
        else
            dibit = 3U;
        b[j * 2]     = (dibit & 2U) != 0U;
        b[j * 2 + 1] = (dibit & 1U) != 0U;
    }

    bool at = b[0], tc = b[4], ls1 = b[8], ls0 = b[12];
    int s0 = (at ^ tc ^ ls1) != b[14], s1 = (tc ^ ls1 ^ ls0) != b[18], s2 = (at ^ tc ^ ls0) != b[22];
    int syndrome = (s0 << 2) | (s1 << 1) | s2;

    if (syndrome == 7)
        tc = !tc;                       /* the TC bit is the one that fails all three checks; any other single error is in
                                           another bit */
    return tc ? 1 : 0;
}

void DmrMode::modemDuplex(bool duplex)
{
    modemSimplex_ = !duplex;
    if (cfg_.repeater && !duplex)
        log("DMR repeater mode needs a duplex modem: Modem_Host says this modem is simplex (set it to duplex in the modem's "
            "configuration, the receiver gets silence while transmitting and there is no repeater downlink)");
}

void DmrMode::txTiming(int32_t offset, uint32_t k)
{
    if (!cfg_.repeater || !txActive_)
        return;

    std::lock_guard<std::mutex> lock(rxMutex_);

    txRefRx_    = (int64_t)pos_ + (int64_t)offset;
    txRefK_     = k;
    txRefValid_ = true;
}

/* How well the 24 symbols that end at sample 'end' match one of the data syncs: the result is the best correlation, +1
   for a data sync and -1 for the voice sync (the complement), 0 for nothing.  The symbols are sampled every 5 samples and
   compared by sign, so the correlation does not depend on the level.  With 'gate' a window that is too weak or only
   partly filled (the start of a signal) gives 0. */
double DmrMode::rxCorrelate(uint64_t end, double* energy, double* raw, bool gate, int* family) const
{
    double s[24], abs = 0.0, smallest = 1e9, dc = 0.0;

    /* a frequency error shifts all the levels: take the mean of the whole window off */
    for (unsigned int i = 0U; i <= SPS * 23U; i++)
        dc += (double)rxAt(end - i);
    dc /= (double)(SPS * 23U + 1U);

    for (unsigned int i = 0U; i < 24U; i++)
    {
        s[i] = (double)rxAt(end - (uint64_t)SPS * (23U - i)) - dc;
        abs += std::fabs(s[i]);
        smallest = std::min(smallest, std::fabs(s[i]));
    }

    if (energy)
        *energy = abs / 24.0;
    if (abs <= 0.0)
        return 0.0;

    double best = 0.0, bestSum = 0.0;
    int bestFamily = 0;

    for (unsigned int f = 0U; f < 4U; f++)
    {
        double sum = 0.0;

        for (unsigned int i = 0U; i < 24U; i++)
            sum += (double)gPattern[f][i] * s[i];
        if (std::fabs(sum) > std::fabs(bestSum))
        {
            bestSum = sum;
            bestFamily = (int)f;
        }
    }
    best = bestSum / abs;

    if (raw)
        *raw = std::fabs(bestSum);
    if (family)
        *family = bestFamily;

    /* every sync symbol is an outer one: a window that is only partly filled does not count */
    if (gate && (abs / 24.0 < 80.0 || smallest < 0.3 * (abs / 24.0)))
        return 0.0;
    return best;
}

void DmrMode::rxSample(int16_t sample)
{
    const uint64_t n = pos_;

    ring_[n & (RING_SIZE - 1U)] = sample;
    pos_++;

    if (n < (uint64_t)SPS * 24U + 8U)
        return;

    rxDiagnose(sample, n);

    if (state_ == RX_IDLE)
    {
        double energy = 0.0, raw = 0.0;
        double c0 = rxCorrelate(n, &energy, &raw);

        for (unsigned int i = 0U; i < 4U; i++)
        {
            histC_[i] = histC_[i + 1U];
            histS_[i] = histS_[i + 1U];
        }
        histC_[4] = c0;
        histS_[4] = raw;

        /* The correlation by sign says a sync is there but is blind to the amplitude, so it is ambiguous by a sample or
           two; the raw sum has a sharp peak at the right sample.  Take the peak of the raw sum that sits in the middle
           of the last five samples. */
        double mid = std::fabs(histS_[2]);

        if (std::fabs(histC_[2]) >= SYNC_ACCEPT && mid > std::fabs(histS_[0]) && mid >= std::fabs(histS_[1]) &&
            mid >= std::fabs(histS_[3]) && mid > std::fabs(histS_[4]))
        {
            uint64_t end = n - 2U;
            double e = 0.0;

            rxCorrelate(end, &e, NULL);
            vote_[0] = vote_[1] = 0U;
            burstCount_ = 0U;
            noSync_ = 0U;
            syncEnd_   = end;
            burstKind_ = histC_[2] > 0.0 ? 1 : -1;
            burstDue_  = end + (uint64_t)SPS * (BURST_SYMBOLS - 1U - SYNC_LAST);
            refLevel_  = e;
            missed_    = 0;
            state_     = RX_LOCKED;
            diagLocks_++;
            nextEnd_   = 0U;
            bestC_     = 0.0;
            for (unsigned int i = 0U; i < 5U; i++)
                histC_[i] = histS_[i] = 0.0;
        }
        return;
    }

    /* LOCKED: the burst is complete at burstDue_, the next sync is looked for around nextEnd_ */
    if (nextEnd_ != 0U)
    {
        int64_t d = (int64_t)n - (int64_t)nextEnd_;

        if (d >= -TRACK_WINDOW && d <= TRACK_WINDOW)
        {
            double e = 0.0, raw = 0.0;
            double c = rxCorrelate(n, &e, &raw);

            if (std::fabs(c) >= SYNC_TRACK && std::fabs(raw) > bestRaw_)
            {
                bestRaw_ = std::fabs(raw);
                bestC_ = c;
                bestEnd_ = n;
                if (e > 0.0)
                    refLevel_ = e;
            }
        }
        if (d == TRACK_WINDOW)
        {
            if (bestEnd_ != 0U)
            {
                syncEnd_   = bestEnd_;
                burstKind_ = bestC_ > 0.0 ? 1 : -1;
            }
            else
            {
                syncEnd_   = nextEnd_;
                burstKind_ = 0;                     /* a voice burst B - F has no sync: coast */
            }
            burstDue_ = syncEnd_ + (uint64_t)SPS * (BURST_SYMBOLS - 1U - SYNC_LAST);
            nextEnd_  = 0U;
        }
        return;
    }

    if (n == burstDue_)
    {
        uint64_t end = syncEnd_;
        int kind = burstKind_;

        rxBurst(end, kind);

        if (state_ == RX_LOCKED)
        {
            nextEnd_ = end + period_;
            bestC_   = 0.0;
            bestRaw_ = 0.0;
            bestEnd_ = 0U;
        }
    }
}

/* A signal that stays for a second with nothing decoded: say what the receiver sees, once per transmission */
void DmrMode::rxDiagnose(int16_t sample, uint64_t n)
{
    diagLevel_ += (std::fabs((double)sample) - diagLevel_) / 480.0;         /* about 20 ms */

    if (rxActive_)
    {
        diagOn_ = 0U;
        diagTold_ = true;                       /* a call is being received: nothing to tell for this transmission */
        return;
    }

    if (cfg_.repeater && state_ == RX_LOCKED)
    {
        diagOn_ = 0U;                           /* the downlink is there: idle slots are normal */
        return;
    }

    if (diagLevel_ < 120.0)
    {
        if (diagLevel_ < 80.0)
        {
            diagOn_ = 0U;
            diagTold_ = false;
            diagBestC_ = 0.0;
        }
        return;
    }

    diagOn_++;

    int fam = 0;
    double c = std::fabs(rxCorrelate(n, NULL, NULL, true, &fam));       /* gated: noise does not count as a near miss */

    if (c > diagBestC_)
    {
        diagBestC_ = c;
        diagFamily_ = fam;
    }

    if (diagOn_ >= 24000U && !diagTold_)
    {
        char text[480];

        diagTold_ = true;
        std::snprintf(text, sizeof(text), "DMR receiver: a signal of level %.0f has been there for a second but no call was "
                      "decoded. Best sync match %.2f (%s; a lock needs %.2f; noise alone reaches about 0.65, a DMR burst 0.9 or more). Locks: %u data sync, %u voice sync; voice "
                      "bursts rejected %u, voice LC headers that did not decode %u, bursts with another colour code %u. If the level is right but "
                      "nothing decodes, try 'Invert the received signal'",
                      diagLevel_, diagBestC_, FAMILY_NAME[diagFamily_], SYNC_ACCEPT, diagDataLocks_, diagVoiceLocks_,
                      diagVoiceBad_, diagHeaderBad_, diagWrongCc_);
        log(text);
    }
}

void DmrMode::rxBurst(uint64_t syncEnd, int syncKind)
{
    uint8_t frame[33];
    double centre, threshold;

    if (cfg_.repeater)
        burstCount_++;

    /* A call whose end was not seen (a terminator missed or lost) must not stay "being received": the idle slot after it
       would be taken for its speech and the next call would find the receiver still busy. */
    if (cfg_.repeater && rxActive_ && nowMs() - rxLastMs_ > 1000U)
        rxEnd(CALL_RX_LOST);

    /* levels from the sync (all outer symbols) when there is one, else the last ones */
    if (syncKind != 0)
    {
        double mx = -1e9, mn = 1e9, sumAbs = 0.0;

        for (unsigned int i = 0U; i < 24U; i++)
        {
            double s = (double)rxAt(syncEnd - (uint64_t)SPS * (23U - i));

            mx = std::max(mx, s);
            mn = std::min(mn, s);
        }
        centre_ = (mx + mn) / 2.0;
        for (unsigned int i = 0U; i < 24U; i++)
            sumAbs += std::fabs((double)rxAt(syncEnd - (uint64_t)SPS * (23U - i)) - centre_);

        /* the sync is all outer symbols: their mean distance from the centre is the outer level, and the inner
           symbols are a third of it, so the slicing level is two thirds (a peak would be pushed up by the pulse
           overshoot) */
        threshold_ = (sumAbs / 24.0) * (2.0 / 3.0);
    }
    centre = centre_;
    threshold = threshold_;

    std::memset(frame, 0x00, sizeof(frame));

    double level = 0.0;

    for (unsigned int j = 0U; j < BURST_SYMBOLS; j++)
    {
        /* symbol j of the burst; the sync ends with symbol 77 */
        double raw = (double)rxAt((uint64_t)((int64_t)syncEnd + (int64_t)SPS * ((int64_t)j - (int64_t)SYNC_LAST)));
        double x = raw - centre;
        unsigned int dibit;

        level += std::fabs(raw);

        if (x < -threshold)
            dibit = 1U;                 /* 01 */
        else if (x < 0.0)
            dibit = 0U;                 /* 00 */
        else if (x < threshold)
            dibit = 2U;                 /* 10 */
        else
            dibit = 3U;                 /* 11 */

        putBit(frame, j * 2U, (dibit & 2U) != 0U);
        putBit(frame, j * 2U + 1U, (dibit & 1U) != 0U);
    }

    if (syncKind == 0)
    {
        /* a coasted burst only counts when there is signal where it should be */
        if (level / BURST_SYMBOLS < 0.35 * refLevel_)
        {
            missed_++;
            if (missed_ >= 3)
            {
                if (cfg_.repeater && dlLogged_)
                    log("DMR repeater: the downlink is gone");
                dlLogged_ = false;
                rxLost();
            }
            return;
        }
    }
    missed_ = 0;

    if (cfg_.repeater)
    {
        /* the slot of this burst is in the CACH before it; the downlink alternates, so a CACH that does not check
           out is taken as the other slot than the last burst */
        int raw = rxCachSlot(syncEnd);

        /* the downlink strictly alternates, so each CACH votes for which of the two phases the bursts are in: a bit
           error now and then does not matter */
        vote_[raw ^ (int)(burstCount_ & 1U)]++;
        if (vote_[0] + vote_[1] > 40U)
        {
            vote_[0] /= 2U;
            vote_[1] /= 2U;
        }
        int slot = (vote_[1] > vote_[0] ? 1 : 0) ^ (int)(burstCount_ & 1U);

        lastSlot_ = slot;

        dlRefEnd_  = syncEnd;
        dlRefSlot_ = slot;
        dlLastMs_  = nowMs();

        if (!dlLogged_)
        {
            dlLogged_ = true;
            log("DMR repeater: the downlink was found, listening on time slot " + std::to_string(cfg_.slot));
        }

        if (syncKind != 0)
            noSync_ = 0U;
        else
            noSync_++;

        if (!rxActive_ && noSync_ >= 8U)
        {
            /* eight bursts in a row with no sync and no call: noise, not a downlink */
            dlLogged_ = false;
            state_ = RX_IDLE;
            nextEnd_ = 0U;
            for (unsigned int i = 0U; i < 5U; i++)
                histC_[i] = histS_[i] = 0.0;
            return;
        }

        if ((unsigned int)slot + 1U != cfg_.slot)
        {
            /* The other slot.  While this radio transmits, the repeater sends the call back on the slot it took it for:
               our own call in the other slot means the repeater put us there, the transmit timing is half a slot off. */
            if (txActive_ && syncKind > 0 && !slotFixed_)
            {
                uint8_t copy[33];
                uint8_t cc = 0U, dt = 0U, lc[12];

                std::memcpy(copy, frame, sizeof(copy));
                {
                    std::lock_guard<std::mutex> lock(gDmr);

                    DMRSlotType_decode(copy, &cc, &dt);
                    if (cc == cfg_.colorCode && dt == DT_VOICE_LC_HEADER && DMRFullLC_decode(copy, DT_VOICE_LC_HEADER, lc, RX_SLOT))
                    {
                        DMRLC_decode(lc, RX_SLOT);
                        if (DMRLC_getSrcId(RX_SLOT) == cfg_.srcId && DMRLC_getDstId(RX_SLOT) == cfg_.dstId)
                            slotFixed_ = true;
                    }
                }
                if (slotFixed_)
                {
                    slotCorr_ = (slotCorr_ + 720) % 1440;
                    log("DMR repeater: the repeater took this call for time slot " + std::to_string(3U - cfg_.slot) +
                        " (it came back there): the transmit timing is moved by half a slot, the next call goes in slot " +
                        std::to_string(cfg_.slot));
                }
            }
            if (!txActive_ && syncKind != 0 && nowMs() - otherSlotLogMs_ > 5000U)
            {
                /* so that a call that is never heard is not a mystery: it is on the other slot */
                otherSlotLogMs_ = nowMs();
                log("DMR repeater: there is a call on time slot " + std::to_string((unsigned int)slot + 1U) +
                    ", this radio listens on time slot " + std::to_string(cfg_.slot) + " (change 'Slot' to hear it)");
            }
            return;
        }
    }

    if (cfg_.repeater && (txActive_ || nowMs() < txMuteUntil_))
    {
        /* A radio does not hear itself: what the repeater sends back of this radio's own call is neither shown as a call
           nor played (and the one vocoder is busy encoding the microphone). */
        return;
    }

    if (syncKind > 0)
    {
        diagDataLocks_++;
        rxDataBurst(frame);
    }
    else
    {
        if (syncKind < 0)
        {
            rxVoiceIndex_ = 0U;
            diagVoiceLocks_++;
            if (!rxActive_)
            {
                /* late entry: a voice sync with no call going; make sure it is a voice burst before starting a call */
                uint8_t copy[33];

                std::memcpy(copy, frame, sizeof(copy));
                if (dmrFecErrors(copy, false) > 12U)
                {
                    diagVoiceBad_++;
                    state_ = RX_IDLE;
                    nextEnd_ = 0U;
                    for (unsigned int i = 0U; i < 5U; i++)
        histC_[i] = histS_[i] = 0.0;
                    return;
                }
                rxStart(0U, 0U, true);                  /* the ids come from the embedded link control */
            }
        }
        else
        {
            if (!rxActive_ && cfg_.repeater)
                return;                         /* an idle slot: the lock stays, the downlink timing is needed */
            if (!rxActive_)
            {
                /* nothing was decoded since the lock: it was a false one */
                state_ = RX_IDLE;
                nextEnd_ = 0U;
                for (unsigned int i = 0U; i < 5U; i++)
                    histC_[i] = histS_[i] = 0.0;
                return;
            }
            rxVoiceIndex_ = (rxVoiceIndex_ + 1U) % 6U;
        }
        rxVoiceBurst(frame, syncKind < 0);
    }
}

void DmrMode::rxStart(unsigned int src, unsigned int dst, bool group)
{
    rxActive_ = true;
    rxInfoKnown_ = (src != 0U);
    rxSrc_ = src;
    rxDst_ = dst;
    rxGroup_ = group;
    rxErrs_ = 0UL;
    rxBits_ = 0UL;
    rxVocoderOk_ = true;
    rxLastMs_ = nowMs();
    rxHeaderSeen_ = (src != 0U);

    CallInfo info;
    info.event = CALL_RX_START;
    info.src = src;
    info.dst = dst;
    info.group = group;
    info.ber = 0.0F;
    if (ev_.call)
        ev_.call(info);

    if (src != 0U)
    {
        info.event = CALL_RX_INFO;
        if (ev_.call)
            ev_.call(info);
    }
}

void DmrMode::rxEnd(CallEvent how)
{
    if (!rxActive_)
        return;

    CallInfo info;
    info.event = how;
    info.src = rxSrc_;
    info.dst = rxDst_;
    info.group = rxGroup_;
    info.ber = rxBits_ > 0UL ? (float)(100.0 * (double)rxErrs_ / (double)rxBits_) : 0.0F;
    rxActive_ = false;
    if (ev_.call)
        ev_.call(info);
}

void DmrMode::rxLost()
{
    rxEnd(CALL_RX_LOST);
    state_ = RX_IDLE;
    nextEnd_ = 0U;
    for (unsigned int i = 0U; i < 5U; i++)
        histC_[i] = histS_[i] = 0.0;
}

void DmrMode::rxDataBurst(const uint8_t* frame)
{
    uint8_t cc = 0U, dt = 0U;
    uint8_t lc[12];
    unsigned int src = 0U, dst = 0U;
    bool group = true, ok = false;

    {
        std::lock_guard<std::mutex> lock(gDmr);

        DMRSlotType_decode(frame, &cc, &dt);
        if (cc != cfg_.colorCode)
        {
            diagWrongCc_++;
            if (diagWrongCc_ == 1U || diagWrongCc_ % 50U == 0U)
                log("DMR receiver: heard a burst with colour code " + std::to_string((unsigned int)cc) + " (data type " +
                    std::to_string((unsigned int)dt) + "), this radio is set to " + std::to_string(cfg_.colorCode) +
                    ": it is ignored");
            if (!rxActive_)
                state_ = RX_IDLE;                       /* not a burst of ours: the lock was false */
            nextEnd_ = 0U;
            for (unsigned int i = 0U; i < 5U; i++)
        histC_[i] = histS_[i] = 0.0;
            return;
        }

        if (dt == DT_VOICE_LC_HEADER)
        {
            ok = DMRFullLC_decode(frame, DT_VOICE_LC_HEADER, lc, RX_SLOT);
            if (ok)
            {
                DMRLC_decode(lc, RX_SLOT);
                src = DMRLC_getSrcId(RX_SLOT);
                dst = DMRLC_getDstId(RX_SLOT);
                group = DMRLC_getFLCO(RX_SLOT) == FLCO_GROUP;
            }
        }
    }

    if (cfg_.repeater && rxActive_ && (dt == DT_IDLE || dt == DT_CSBK))
    {
        /* the slot went back to idle (or to signalling) without the call's terminator being seen: the call is over, and what
           follows is not speech */
        rxEnd(CALL_RX_LOST);
        return;
    }

    if (dt == DT_VOICE_LC_HEADER)
    {
        if (!ok)
        {
            diagHeaderBad_++;
            return;
        }

        rxLastMs_ = nowMs();
        rxVocoderOk_ = true;
        if (!rxActive_)
            rxStart(src, dst, group);
        else if (!rxInfoKnown_ || src != rxSrc_ || dst != rxDst_)
        {
            rxSrc_ = src;
            rxDst_ = dst;
            rxGroup_ = group;
            rxInfoKnown_ = true;

            CallInfo info;
            info.event = CALL_RX_INFO;
            info.src = src;
            info.dst = dst;
            info.group = group;
            info.ber = 0.0F;
            if (ev_.call)
                ev_.call(info);
        }
        rxHeaderSeen_ = true;
    }
    else if (dt == DT_TERMINATOR_WITH_LC)
    {
        rxEnd(CALL_RX_END);
        if (!cfg_.repeater)
        {
            state_ = RX_IDLE;
            nextEnd_ = 0U;
            for (unsigned int i = 0U; i < 5U; i++)
                histC_[i] = histS_[i] = 0.0;
        }
    }
}

void DmrMode::rxVoiceBurst(const uint8_t* frame, bool hasSync)
{
    (void)hasSync;

    /* bit errors in the protected bits of the three vocoder frames */
    uint8_t copy[33];

    std::memcpy(copy, frame, sizeof(copy));
    unsigned int burstErrs = dmrFecErrors(copy, false);

    rxErrs_ += burstErrs;
    rxBits_ += DMR_FEC_BITS_PER_BURST;
    if (cfg_.repeater && burstErrs > 12U)
        return;                         /* not speech (an idle slot read as voice): no decoding, and it does not keep a call alive */

    /* late entry: the link control comes round in the embedded signalling of bursts B - E */
    if (!rxInfoKnown_ && rxVoiceIndex_ >= 1U && rxVoiceIndex_ <= 4U)
    {
        unsigned int src = 0U, dst = 0U;
        bool got = false, group = true;

        {
            std::lock_guard<std::mutex> lock(gDmr);

            DMREMB_decode(frame, RX_SLOT);
            uint8_t lcss = getLCSS(RX_SLOT);

            if (DMREmbeddedData_decode(frame, lcss, RX_SLOT))
            {
                uint8_t data[9];

                if (DMREmbeddedData_getRawData(data, RX_SLOT))
                {
                    DMRLC_decode(data, RX_SLOT);

                    unsigned int flco = DMRLC_getFLCO(RX_SLOT);

                    if (flco == FLCO_GROUP || flco == FLCO_USER_USER)
                    {
                        src = DMRLC_getSrcId(RX_SLOT);
                        dst = DMRLC_getDstId(RX_SLOT);
                        group = flco == FLCO_GROUP;
                        got = src != 0U;
                    }
                }
            }
        }

        if (got)
        {
            rxSrc_ = src;
            rxDst_ = dst;
            rxGroup_ = group;
            rxInfoKnown_ = true;

            CallInfo info;
            info.event = CALL_RX_INFO;
            info.src = src;
            info.dst = dst;
            info.group = group;
            info.ber = 0.0F;
            if (ev_.call)
                ev_.call(info);
        }
    }

    rxLastMs_ = nowMs();
    if (!rxVocoderOk_)
    {
        /* the vocoder failed: try again every half second rather than staying deaf for the rest of the call */
        if (nowMs() < rxVocoderRetryMs_)
            return;
        rxVocoderOk_ = true;
    }

    uint8_t blocks[3U * Vocoder::BlockBytes];
    int16_t pcm[3U * Vocoder::Samples];

    for (unsigned int n = 0U; n < 3U; n++)
        airToChip(layout_, frame, n, blocks + n * Vocoder::BlockBytes);

    if (!vocoder_.decodeBatch(blocks, 3U, pcm))
    {
        rxVocoderOk_ = false;
        rxVocoderRetryMs_ = nowMs() + 500U;
        log("the vocoder stopped answering: received speech is not decoded for a moment");
        return;
    }

    if (ev_.rxAudio)
        ev_.rxAudio(pcm, 3U * Vocoder::Samples);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Transmit                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------ */

void DmrMode::pttDown()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        mic_.clear();
        ptt_ = true;
    }
    txCv_.notify_all();
}

void DmrMode::pttUp()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        ptt_ = false;
    }
    txCv_.notify_all();
}

void DmrMode::micAudio(const int16_t* pcm, size_t count)
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);

        if (!ptt_)
            return;

        for (size_t i = 0U; i < count; i++)
        {
            float v = pcm[i] * cfg_.micGain;

            if (v > 32767.0F)
                v = 32767.0F;
            if (v < -32768.0F)
                v = -32768.0F;
            mic_.push_back((int16_t)v);
        }

        while (mic_.size() > 8000U)             /* never more than 1 s behind */
            mic_.pop_front();
    }
    txCv_.notify_all();
}

/* Shapes symbol levels into samples: each symbol gives five samples, y[5k + p] = sum over j of RRC[p + 5j] * x[k - j] */
void DmrMode::txSymbols(const int16_t* levels, size_t count)
{
    int16_t out[SPS * 192U];

    txSyms_ += count;

    while (count > 0U)
    {
        size_t chunk = count > 192U ? 192U : count;

        for (size_t k = 0U; k < chunk; k++)
        {
            for (unsigned int j = 8U; j > 0U; j--)
                modHist_[j] = modHist_[j - 1U];
            modHist_[0] = levels[k];

            for (unsigned int p = 0U; p < SPS; p++)
            {
                int64_t acc = 0;

                for (unsigned int j = 0U; j < 9U; j++)
                    acc += (int64_t)RRC[p + SPS * j] * modHist_[j];

                acc >>= 15;
                if (acc > 32767)
                    acc = 32767;
                if (acc < -32768)
                    acc = -32768;
                out[k * SPS + p] = (int16_t)acc;
            }
        }

        link_.txSamples(out, chunk * SPS);
        levels += chunk;
        count -= chunk;
    }
}

/* Four symbols per byte, as dmr_host's writeSamples(): 11 is the highest level, 01 the lowest */
void DmrMode::txDibitBytes(const uint8_t* bytes, size_t count)
{
    int16_t levels[192];
    size_t used = 0U;

    for (size_t i = 0U; i < count; i++)
    {
        uint8_t c = bytes[i];

        for (unsigned int s = 0U; s < 4U; s++, c = (uint8_t)(c << 2))
        {
            switch (c & 0xC0U)
            {
                case 0xC0U: levels[used++] = LEVEL_A; break;
                case 0x80U: levels[used++] = LEVEL_B; break;
                case 0x00U: levels[used++] = LEVEL_C; break;
                default:    levels[used++] = LEVEL_D; break;
            }
        }

        if (used >= 188U)
        {
            txSymbols(levels, used);
            used = 0U;
        }
    }
    if (used > 0U)
        txSymbols(levels, used);
}

/* One 60 ms period: the burst and the fill */
bool DmrMode::txBurst(const uint8_t* frame)
{
    if (cfg_.repeater)
        return txBurstTimed(frame);

    if (cfg_.txTap)
        cfg_.txTap(frame);
    txDibitBytes(frame, 33U);
    txDibitBytes(DMO_FILL, sizeof(DMO_FILL));
    return link_.isOpen();
}

/* Where the next burst of this radio's slot has to start, as a symbol number of the transmission, so that its sync ends
   'txOffsetSamples' after the sync of the downlink burst of the slot (the uplink burst is 27.5 ms of every 60 ms, the
   rest of the transmission is silence).  The start is the first one at or after minSym.  false when there is no downlink
   to take the timing from.  'exact' tells whether the modem's TX timing report was used (else the clocks). */
bool DmrMode::txPlan(uint64_t minSym, uint64_t* start, bool* exact)
{
    std::lock_guard<std::mutex> lock(rxMutex_);

    if (state_ != RX_LOCKED || dlLastMs_ == 0U || nowMs() - dlLastMs_ > 500U)
        return false;

    int mine = (int)cfg_.slot - 1;
    int64_t u = (int64_t)dlRefEnd_ + (dlRefSlot_ == mine ? 0 : 720) + (int64_t)cfg_.txOffsetSamples + (int64_t)slotCorr_;
    double kSync;

    if (txRefValid_)
    {
        kSync = (double)txRefK_ + (double)(u - txRefRx_);
        *exact = true;
    }
    else
    {
        double off = std::min(clkMin_, clkPrev_);

        if (off > 1e17)
            return false;
        kSync = (double)u - ((txT0_ - off) * 24000.0 + (double)cfg_.txTimingMs * 24.0);
        *exact = false;
    }

    /* symbol j of a burst that starts at stream symbol S has its peak at sample 5 (S + j) + 24 (the pulse delay) */
    int64_t a = (int64_t)std::floor((kSync - 24.0) / 5.0 + 0.5) - 77;
    int64_t d = (int64_t)minSym - a;
    int64_t m = d >= 0 ? (d + 287) / 288 : -((-d) / 288);

    *start = (uint64_t)(a + m * 288);
    return true;
}

void DmrMode::txGapTo(uint64_t symbol)
{
    int16_t zeros[192];

    std::memset(zeros, 0x00, sizeof(zeros));
    while (txSyms_ < symbol && link_.isOpen())
    {
        uint64_t n = symbol - txSyms_;

        txSymbols(zeros, n > 192U ? 192U : (size_t)n);
    }
}

/* A burst in the slot: silence up to its place (the other slot's time), the burst, and the place of the next one 60 ms later */
bool DmrMode::txBurstTimed(const uint8_t* frame)
{
    if (txNext_ != 0U)
    {
        if (txNext_ < txSyms_)
        {
            /* late (the microphone or the chip held it up): the next place of the slot */
            uint64_t late = txSyms_ - txNext_;

            txNext_ += ((late + 287U) / 288U) * 288U;
            log("DMR repeater: a burst was late, the slot was missed");
        }
        else
        {
            /* follow the downlink: a symbol at a time at most */
            uint64_t want;
            bool exact = false;

            if (txPlan(txNext_ > 143U ? txNext_ - 143U : 0U, &want, &exact) && exact)
            {
                if (want > txNext_ && txNext_ + 1U >= txSyms_)
                    txNext_++;
                else if (want < txNext_ && txNext_ - 1U >= txSyms_)
                    txNext_--;
            }
        }
        txGapTo(txNext_);
    }
    else
        txNext_ = txSyms_;

    if (cfg_.txTap)
        cfg_.txTap(frame);
    if (cfg_.txStart)
        cfg_.txStart(txSyms_);
    txDibitBytes(frame, 33U);
    txNext_ += 288U;
    return link_.isOpen();
}

void DmrMode::buildHeader(uint8_t type, uint8_t* frame)
{
    std::lock_guard<std::mutex> lock(gDmr);

    setColorCode((uint8_t)cfg_.colorCode, TX_SLOT);
    setPI(false, TX_SLOT);
    setLCSS(0U, TX_SLOT);
    DMRLC_setFLCO(cfg_.group ? (unsigned int)FLCO_GROUP : (unsigned int)FLCO_USER_USER, TX_SLOT);
    DMRLC_setFID(0U, TX_SLOT);
    DMRLC_setSrcId(cfg_.srcId, TX_SLOT);
    DMRLC_setDstId(cfg_.dstId, TX_SLOT);
    DMRLC_setPF(false, TX_SLOT);
    DMRLC_setOVCM(false, TX_SLOT);

    std::memset(frame, 0x00, 33U);
    DMRFullLC_encode(frame, type, TX_SLOT);
    DMRSlotType_encode((uint8_t)cfg_.colorCode, type, frame);
    addDMRDataSync(frame, false);               /* MS sourced */
}

/* The wake-up of a repeater that is not transmitting: a CSBK "BS outbound activation" (the repeater ignores everything but
   this while it is idle, and keys its downlink when it hears it) */
void DmrMode::buildWakeup(uint8_t* frame)
{
    std::lock_guard<std::mutex> lock(gDmr);

    uint8_t* t = slotData[TX_SLOT].tdata;

    std::memset(t, 0x00, 12U);
    t[0] = (uint8_t)(CSBKO_BSDWNACT | 0x80U);       /* last block */
    t[1] = 0x00U;
    t[7] = (uint8_t)(cfg_.srcId >> 16);
    t[8] = (uint8_t)(cfg_.srcId >> 8);
    t[9] = (uint8_t)cfg_.srcId;

    setColorCode((uint8_t)cfg_.colorCode, TX_SLOT);
    setPI(false, TX_SLOT);
    setLCSS(0U, TX_SLOT);
    std::memset(frame, 0x00, 33U);
    CSBK_encode(frame, TX_SLOT);
    DMRSlotType_encode((uint8_t)cfg_.colorCode, DT_CSBK, frame);
    addDMRDataSync(frame, false);
}

/* Voice burst number 'index' of the call: A (voice sync) when it is a multiple of six, else B - F with the embedded
   signalling carrying the link control */
bool DmrMode::buildVoice(const int16_t* pcm, unsigned int index, uint8_t* frame)
{
    uint8_t chips[3U * Vocoder::BlockBytes];

    if (!vocoder_.encodeBatch(pcm, 3U, chips))
        return false;

    std::memset(frame, 0x00, 33U);
    for (unsigned int n = 0U; n < 3U; n++)
        chipToAir(layout_, chips + n * Vocoder::BlockBytes, n, frame);

    std::lock_guard<std::mutex> lock(gDmr);

    if (index % 6U == 0U)
    {
        addDMRAudioSync(frame, false);
    }
    else
    {
        setColorCode((uint8_t)cfg_.colorCode, TX_SLOT);
        setPI(false, TX_SLOT);
        setLCSS(DMREmbeddedData_getData(frame, (unsigned char)(index % 6U), TX_SLOT), TX_SLOT);
        DMREMB_encode(frame, TX_SLOT);
    }
    return true;
}

void DmrMode::txThread()
{
    std::unique_lock<std::mutex> lock(txMutex_);

    while (running_)
    {
        if (!ptt_)
        {
            txCv_.wait_for(lock, std::chrono::milliseconds(100));

            /* hand the modem back once the hang time after a call has passed */
            if (!ptt_ && !txActive_ && link_.modeHeld() && nowMs() >= hangUntil_)
                link_.releaseMode();
            continue;
        }

        lock.unlock();
        txCall();
        lock.lock();
    }
}

/* A tone for the user, as the speaker gets the received speech (8 kHz).  A sweep from startHz to endHz with a short fade
   in and out, so it does not click. */
/* 0 Hz is a pause of the same length */
void DmrMode::playTone(double startHz, double endHz, unsigned int ms)
{
    if (!ev_.rxAudio)
        return;

    const unsigned int total = ms * 8U;
    const unsigned int fade  = 40U;                 /* 5 ms */
    const double       pi    = 3.14159265358979323846;
    std::vector<int16_t> pcm(total);
    double phase = 0.0;

    for (unsigned int i = 0U; i < total; i++)
    {
        double f = startHz + (endHz - startHz) * (double)i / (double)total;
        double a = 9000.0;

        phase += 2.0 * pi * f / 8000.0;
        if (i < fade)
            a *= (double)i / (double)fade;
        else if (total - i < fade)
            a *= (double)(total - i) / (double)fade;
        pcm[i] = (int16_t)(a * std::sin(phase));
    }
    ev_.rxAudio(pcm.data(), pcm.size());
}

void DmrMode::txCall()
{
    uint8_t frame[33];
    int16_t pcm[3U * Vocoder::Samples];
    const size_t need = sizeof(pcm) / sizeof(pcm[0]);
    unsigned int index = 0U, bursts = 0U;
    bool ok = true;

    if (!cfg_.repeater)
    {
        /* a call being received is over for us */
        std::lock_guard<std::mutex> lock(rxMutex_);
        rxLost();
    }

    slotFixed_ = false;
    txActive_ = true;

    CallInfo info;
    info.event = CALL_TX_START;
    info.src = cfg_.srcId;
    info.dst = cfg_.dstId;
    info.group = cfg_.group;
    info.ber = 0.0F;
    if (ev_.call)
        ev_.call(info);

    link_.requestMode();
    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        if (cfg_.repeater && rxActive_)
            rxEnd(CALL_RX_END);             /* the user keyed up: whatever was being received is over */
        txRefValid_ = false;
    }
    txT0_ = (double)nowMs() / 1000.0;
    link_.txBegin();
    std::memset(modHist_, 0x00, sizeof(modHist_));
    txSyms_ = 0U;
    txNext_ = 0U;

    /* the link control of the call, for the embedded signalling of the voice bursts */
    buildHeader(DT_VOICE_LC_HEADER, frame);
    {
        std::lock_guard<std::mutex> lock(gDmr);
        DMREmbeddedData_setLC(TX_SLOT);
    }

    if (cfg_.repeater)
    {
        /* A plain tone (0x5F bytes) first, as in direct mode.  With the downlink there it goes on until the modem has told
           when its samples are on the air (so the slot timing is exact), at most 700 ms. */
        uint8_t chunk[12];
        unsigned int waited = 0U;
        const unsigned int reportWait = 700U;

        std::memset(chunk, 0x5F, sizeof(chunk));
        while (running_ && link_.isOpen())
        {
            bool dl, timed;

            {
                std::lock_guard<std::mutex> lock(rxMutex_);

                dl = state_ == RX_LOCKED && dlLastMs_ != 0U && nowMs() - dlLastMs_ <= 500U;
                timed = txRefValid_;
            }
            if (waited >= cfg_.txDelayMs && (!dl || timed || waited >= reportWait))
                break;
            txDibitBytes(chunk, sizeof(chunk));         /* 48 symbols, 10 ms */
            waited += 10U;
        }

        /* The repeater is not on the air yet (nothing to take the timing from): wake it up, as a radio does, with the
           wake-up CSBK repeated until its downlink is heard */
        bool dl = false;

        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            dl = state_ == RX_LOCKED && dlLastMs_ != 0U && nowMs() - dlLastMs_ <= 500U;
        }
        if (!dl)
        {
            uint8_t wake[33];

            buildWakeup(wake);
            log("DMR repeater: no downlink, sending the wake-up");

            /* A radio sends a few wake-ups and then listens: more of them, arriving after the repeater has keyed, are
               taken by some hosts for a call (a slot activated by a CSBK alone stays keyed).  Three bursts, then the tone
               for a second while the downlink is looked for, at most three rounds. */
            for (unsigned int round = 0U; round < 3U && !dl && running_ && link_.isOpen(); round++)
            {
                for (unsigned int n = 0U; n < 3U; n++)
                {
                    if (cfg_.txTap)
                        cfg_.txTap(wake);
                    txDibitBytes(wake, 33U);
                    txGapTo(txSyms_ + 156U);                /* 60 ms a burst */
                }
                for (unsigned int ms = 0U; ms < 1000U && running_ && link_.isOpen(); ms += 10U)
                {
                    {
                        std::lock_guard<std::mutex> lock(rxMutex_);
                        dl = state_ == RX_LOCKED && dlLastMs_ != 0U && nowMs() - dlLastMs_ <= 500U;
                    }
                    if (dl)
                        break;
                    txDibitBytes(chunk, sizeof(chunk));
                }
            }
            if (dl)
            {
                /* the modem's timing report needs a moment now */
                unsigned int waited = 0U;

                while (running_ && link_.isOpen() && waited < 700U)
                {
                    bool timed;

                    {
                        std::lock_guard<std::mutex> lock(rxMutex_);
                        timed = txRefValid_;
                    }
                    if (timed)
                        break;
                    txDibitBytes(chunk, sizeof(chunk));
                    waited += 10U;
                }
            }
        }

        uint64_t start = 0U;
        bool exact = false;
        const std::string slotText = std::to_string(cfg_.slot);

        if (txPlan(txSyms_, &start, &exact))
        {
            txNext_ = start;
            log(exact ? "DMR repeater: transmitting in time slot " + slotText + ", timed from the downlink with the modem's TX timing report"
                      : "DMR repeater: transmitting in time slot " + slotText + ", timed from the downlink by the clocks (the modem sends no TX timing report; "
                        "tune 'TX slot timing' if the repeater puts the call in the wrong slot)");
            /* the repeater took the radio: two quick beeps */
            playTone(1500.0, 1500.0, 45U);
            playTone(0.0, 0.0, 45U);
            playTone(1500.0, 1500.0, 45U);
        }
        else
        {
            /* no repeater answered: like a radio, a low tone and no transmission (the keyed-up call is dropped) */
            log("DMR repeater: no downlink was heard, the repeater did not answer: the call is cancelled");
            playTone(300.0, 300.0, 600U);
            ok = false;
        }
    }
    else
    {
        uint8_t chunk[32];
        unsigned int bytes = (cfg_.txDelayMs * 6U) / 5U;

        std::memset(chunk, 0x5F, sizeof(chunk));
        while (bytes > 0U)
        {
            unsigned int n = bytes > sizeof(chunk) ? (unsigned int)sizeof(chunk) : bytes;

            txDibitBytes(chunk, n);
            bytes -= n;
        }
    }

    for (unsigned int h = 0U; h < cfg_.headers && ok; h++)
        ok = txBurst(frame);

    /* voice: one burst per 60 ms of speech from the microphone */
    while (ok && running_)
    {
        size_t got = 0U;
        bool last = false;

        {
            std::unique_lock<std::mutex> lock(txMutex_);

            while (running_ && mic_.size() < need && ptt_)
            {
                if (txCv_.wait_for(lock, std::chrono::milliseconds(200)) == std::cv_status::timeout)
                    break;
            }

            got = mic_.size() < need ? mic_.size() : need;
            for (size_t i = 0U; i < got; i++)
            {
                pcm[i] = mic_.front();
                mic_.pop_front();
            }
            last = !ptt_;
        }

        if (got == 0U && last)
            break;

        for (size_t i = got; i < need; i++)
            pcm[i] = 0;

        if (!buildVoice(pcm, index, frame))
        {
            log("the vocoder stopped answering: ending the transmission");
            ok = false;
            break;
        }
        ok = txBurst(frame);
        index++;
        bursts++;

        if (last && got < need)
            break;
    }

    /* terminator with LC, then the tail of the pulse shaping and the end of transmission */
    /* Always closed with a terminator, also when the call was refused: the wake-up CSBK has already made the repeater
       key up and take a slot, and only a terminator tells the host the slot is free again. */
    if (link_.isOpen())
    {
        buildHeader(DT_TERMINATOR_WITH_LC, frame);
        txBurst(frame);

        /* The end of transmission must not cut the terminator off: the modem and the bridge still hold its samples (and
           the firmware of a hotspot needs the whole burst to see it).  Without it the call ended as a "lost signal" and
           a host that cannot take that stayed keyed.  In repeater mode the silence lasts a whole 60 ms slot period. */
        int16_t zeros[48];
        std::memset(zeros, 0x00, sizeof(zeros));
        for (unsigned int n = 0U; n < (cfg_.repeater ? 6U : 1U); n++)
            txSymbols(zeros, cfg_.repeater ? 48U : 10U);
        link_.txEnd();
    }

    if (!ok)
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        ptt_ = false;
    }

    hangUntil_ = nowMs() + cfg_.modeHangMs;
    txMuteUntil_ = nowMs() + 700U;      /* the repeater's echo of the end of the call is still to come */
    txActive_ = false;

    info.event = CALL_TX_END;
    info.text = std::to_string(bursts) + " voice bursts";
    if (ev_.call)
        ev_.call(info);
}

}
