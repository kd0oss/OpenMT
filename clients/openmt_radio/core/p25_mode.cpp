#include "p25_mode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdlib>

extern "C"
{
#include "p25_func.h"
}

namespace openmt
{

/* The repo's P25 code keeps its state (link control, ids, NAC) in globals; everything that touches it runs under this lock. */
static std::mutex gP25;

static uint64_t nowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static const unsigned int SPS             = 5U;         /* samples per symbol */
static const unsigned int SYNC_SYMBOLS    = 24U;
static const unsigned int RING_SIZE       = 16384U;
static const double       SYNC_ACCEPT     = 0.80;       /* correlation needed to lock onto a sync from cold */
static const double       SYNC_TRACK      = 0.55;       /* and to take a sync near where it is expected */
static const int          TRACK_WINDOW    = 30;         /* samples either side of the expected place (the modem's symbol clock is not exactly 4800 baud) */
static const double       LEVEL_GATE      = 80.0;       /* mean absolute level of the sync below which there is no signal */
static const unsigned int MAX_MISSED      = 4U;         /* frames in a row without a sync before the call is lost */

/* The symbol levels of p25_host's transmitter (dibit 11, 10, 00, 01) */
static const int16_t LEVEL_A = 1260;
static const int16_t LEVEL_B = 420;
static const int16_t LEVEL_C = -420;
static const int16_t LEVEL_D = -1260;

/* The frame sync in the sign a sample has for each of its 24 symbols (all of them outer symbols: dibit 11 is the highest
   level, 01 the lowest) */
static int8_t gPattern[SYNC_SYMBOLS];
static bool   gPatternReady = false;

static void makePattern()
{
    for (unsigned int i = 0U; i < SYNC_SYMBOLS; i++)
    {
        unsigned int bit = 2U * i;
        unsigned int hi = (P25_SYNC_BYTES[bit >> 3] >> (7U - (bit & 7U))) & 1U;

        gPattern[i] = hi != 0U ? (int8_t)1 : (int8_t)-1;
    }
    gPatternReady = true;
}

/* Symbols of a data unit, from its data unit id; 0 = not one this radio follows */
static unsigned int duidSymbols(unsigned int duid)
{
    switch (duid)
    {
        case P25_DUID_HDU:     return P25_HDR_FRAME_LENGTH_SYMBOLS;
        case P25_DUID_LDU1:
        case P25_DUID_LDU2:    return P25_LDU_FRAME_LENGTH_SYMBOLS;
        case P25_DUID_TDU:     return P25_TERM_FRAME_LENGTH_SYMBOLS;
        case P25_DUID_TDULC:   return P25_TERMLC_FRAME_LENGTH_SYMBOLS;
        default:               return 0U;
    }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* P25Mode                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

P25Mode::P25Mode(ModemLink& link, Vocoder& vocoder, const P25Config& cfg, const RadioEvents& events) :
    link_(link),
    vocoder_(vocoder),
    cfg_(cfg),
    ev_(events),
    running_(false),
    thread_(),
    rxMutex_(),
    ring_(RING_SIZE, 0.0F),
    pos_(0U),
    sum_(0.0),
    state_(RX_IDLE),
    syncEnd_(0U),
    symStep_(5.0),
    tracked_(false),
    decideAt_(0U),
    nextEnd_(0U),
    duid_(0U),
    frameSyms_(0U),
    lastDuid_(0U),
    missed_(0U),
    centre_(0.0),
    threshold_(0.0),
    rxActive_(false),
    rxInfoKnown_(false),
    rxSrc_(0U),
    rxDst_(0U),
    rxGroup_(true),
    rxVocoderOk_(true),
    rxEncrypted_(false),
    rxAlgSeen_(P25_ALGO_UNENCRYPT),
    rxAlgCount_(0U),
    rxLastMs_(0U),
    rxLastN_(0U),
    lvl_(0.0),
    quiet_(0U),
    rxVocoderRetryMs_(0U),
    txMutex_(),
    txCv_(),
    mic_(),
    ptt_(false),
    txActive_(false),
    hangUntil_(0U)
{
    std::memset(last_, 0x00, sizeof(last_));
    std::memset(corr_, 0x00, sizeof(corr_));
    std::memset(modHist_, 0x00, sizeof(modHist_));
    if (!gPatternReady)
        makePattern();
}

P25Mode::~P25Mode()
{
    stop();
}

std::string P25Mode::modeName() const
{
    return "P25";
}

std::string P25Mode::modemType() const
{
    return "4FSK";
}

void P25Mode::log(const std::string& text) const
{
    if (ev_.log)
        ev_.log(text);
}

void P25Mode::start()
{
    if (running_)
        return;

    if (!vocoder_.setCodec(Vocoder::CODEC_IMBE))
        log("the vocoder could not be switched to the IMBE codec of P25: there will be no speech (is this an AMBE3000R, "
            "and does it support the P25 rate?)");

    running_ = true;
    thread_ = std::thread(&P25Mode::txThread, this);
}

void P25Mode::stop()
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

bool P25Mode::transmitting() const
{
    return txActive_;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Receive                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

void P25Mode::rxSamples(const int16_t* samples, size_t count)
{
    if (!running_ || txActive_)
        return;                 /* half duplex: the receiver is deaf while transmitting */

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
}

double P25Mode::rxAt(uint64_t index) const
{
    return (double)ring_[index & (RING_SIZE - 1U)];
}

/* How well the 24 symbols that end at 'end' match the frame sync: 1 is a clean sync, noise stays well below */
double P25Mode::rxCorrelate(uint64_t end) const
{
    double c = 0.0, e = 0.0;

    for (unsigned int k = 0U; k < SYNC_SYMBOLS; k++)
    {
        double v = rxAt(end - (uint64_t)SPS * (SYNC_SYMBOLS - 1U - k));

        c += gPattern[k] * v;
        e += std::fabs(v);
    }

    if (e / SYNC_SYMBOLS < LEVEL_GATE)
        return 0.0;
    return c / e;
}

void P25Mode::rxSample(int16_t sample)
{
    const uint64_t n = pos_++;

    /* the average of the last five samples is the symbol value (p25_host's boxcar filter) */
    sum_ -= last_[n % 5U];
    last_[n % 5U] = sample;
    sum_ += sample;
    ring_[n & (RING_SIZE - 1U)] = (float)(sum_ / 5.0);

    lvl_ = lvl_ * 0.99 + std::fabs((double)sample) * 0.01;
    quiet_ = lvl_ < LEVEL_GATE ? quiet_ + 1U : 0U;

    corr_[n & 127U] = n >= (uint64_t)(SPS * SYNC_SYMBOLS + 8U) ? rxCorrelate(n) : 0.0;

    switch (state_)
    {
        case RX_IDLE:
            /* searching for a sync in the middle of a call: the call is lost if none comes for a second */
            if (rxActive_ && n > rxLastN_ + 24000U)
            {
                rxLost();
                break;
            }
            /* or when the signal has been gone for 100 ms */
            if (rxActive_ && quiet_ >= 2400U)
            {
                rxLost();
                break;
            }
            if (n >= (uint64_t)(SPS * SYNC_SYMBOLS + 16U))
            {
                /* a sync is a peak of the correlation: look back from the newest sample, so there are samples after it */
                uint64_t e = n - 5U;
                double ce = corr_[e & 127U];

                if (ce >= SYNC_ACCEPT)
                {
                    bool peak = true;

                    for (int d = -5; d <= 5 && peak; d++)
                        if (d != 0 && corr_[(e + (uint64_t)(int64_t)d) & 127U] > ce)
                            peak = false;
                    if (peak)
                        rxLock(e);
                }
            }
            break;

        case RX_SYNC:
            if (n >= decideAt_)
            {
                double best = -2.0;
                uint64_t bestEnd = nextEnd_;

                for (int d = -TRACK_WINDOW; d <= TRACK_WINDOW; d++)
                {
                    uint64_t e = nextEnd_ + (uint64_t)(int64_t)d;
                    double c = corr_[e & 127U];

                    if (c > best)
                    {
                        best = c;
                        bestEnd = e;
                    }
                }

                if (best >= SYNC_TRACK)
                {
                    missed_ = 0U;
                    rxLock(bestEnd);
                }
                else
                {
                    /* no sync where it should be.  With no signal there either, the transmitter has gone (the host's
                       simplex transmitter ends a call with silence, not with a terminator). */
                    double level = 0.0;

                    for (unsigned int k = 0U; k < SYNC_SYMBOLS; k++)
                        level += std::fabs(rxAt(nextEnd_ - (uint64_t)SPS * (SYNC_SYMBOLS - 1U - k)));
                    if (level / SYNC_SYMBOLS < LEVEL_GATE)
                    {
                        rxLost();
                        break;
                    }

                    /* a signal without a sync where there should be one (a gap in the samples, or a fade): look for the
                       next one, without decoding what is not known to be a frame */
                    state_ = RX_IDLE;
                }
            }
            break;

        case RX_NID:
            if (n >= syncEnd_ + (uint64_t)(symStep_ * 36.0 + 0.5))
                rxNid();
            break;

        case RX_FRAME:
            if (n >= syncEnd_ - (uint64_t)(symStep_ * (SYNC_SYMBOLS - 1U) + 0.5) + (uint64_t)(symStep_ * (frameSyms_ - 1U) + 0.5))
                rxFrame();
            break;
    }
}

/* The levels of the sync that ends at 'end' set the centre and the threshold for slicing the frame; the frame is then
   followed until its NID tells what it is */
void P25Mode::rxLock(uint64_t end)
{

    /* the correlation is flat near its peak: the amplitude of the sync picks the best sampling point */
    {
        double best = -1.0e30;
        uint64_t bestEnd = end;

        for (int d = -3; d <= 3; d++)
        {
            uint64_t e = end + (uint64_t)(int64_t)d;
            double c = 0.0;

            for (unsigned int k = 0U; k < SYNC_SYMBOLS; k++)
                c += gPattern[k] * rxAt(e - (uint64_t)SPS * (SYNC_SYMBOLS - 1U - k));
            if (c > best)
            {
                best = c;
                bestEnd = e;
            }
        }
        end = bestEnd;
    }

    tracked_ = state_ == RX_SYNC;
    syncEnd_ = end;
    rxLevels(end - (uint64_t)(symStep_ * (SYNC_SYMBOLS - 1U) + 0.5));
    state_ = RX_NID;
}

void P25Mode::rxLevels(uint64_t first)
{
    double sp = 0.0, sn = 0.0;
    unsigned int np = 0U, nn = 0U;

    for (unsigned int k = 0U; k < SYNC_SYMBOLS; k++)
    {
        double v = rxAt(first + (uint64_t)(symStep_ * k + 0.5));

        if (gPattern[k] > 0)
        {
            sp += v;
            np++;
        }
        else
        {
            sn += v;
            nn++;
        }
    }

    double mp = np > 0U ? sp / np : 0.0, mn = nn > 0U ? sn / nn : 0.0;

    centre_    = (mp + mn) / 2.0;
    threshold_ = 0.66 * (mp - mn) / 2.0;        /* between the inner and the outer symbols */
}

/* The symbols of the frame, from its first, as bytes of dibits (11 is the highest level, 01 the lowest) */
void P25Mode::rxSlice(uint64_t first, unsigned int symbols, uint8_t* out, bool adapt) const
{
    std::memset(out, 0x00, (symbols + 3U) / 4U);

    /* the four levels: from the sync, then (for a long frame) refined from the frame's own symbols, since the signal of a
       real modem is not the clean one of the simulator: the levels are compressed and not symmetric */
    double t[3] = {-threshold_, 0.0, threshold_};

    if (adapt && symbols >= 200U)
    {
        std::vector<double> v(symbols);
        const double a = threshold_ / 0.66;
        double c[4] = {-a, -a / 3.0, a / 3.0, a};

        for (unsigned int i = 0U; i < symbols; i++)
            v[i] = rxAt(first + (uint64_t)(symStep_ * i + 0.5)) - centre_;

        for (int iter = 0; iter < 10; iter++)
        {
            double sum[4] = {0.0, 0.0, 0.0, 0.0};
            unsigned int cnt[4] = {0U, 0U, 0U, 0U};

            for (unsigned int i = 0U; i < symbols; i++)
            {
                int k = v[i] > (c[0] + c[1]) / 2.0 ? (v[i] > (c[1] + c[2]) / 2.0 ? (v[i] > (c[2] + c[3]) / 2.0 ? 3 : 2) : 1) : 0;

                sum[k] += v[i];
                cnt[k]++;
            }
            for (int k = 0; k < 4; k++)
                if (cnt[k] > 4U)
                    c[k] = sum[k] / cnt[k];
        }
        if (c[0] < c[1] && c[1] < c[2] && c[2] < c[3])
        {
            t[0] = (c[0] + c[1]) / 2.0;
            t[1] = (c[1] + c[2]) / 2.0;
            t[2] = (c[2] + c[3]) / 2.0;
        }
    }

    for (unsigned int i = 0U; i < symbols; i++)
    {
        double v = rxAt(first + (uint64_t)(symStep_ * i + 0.5)) - centre_;
        unsigned int dibit;

        if (v > t[2])
            dibit = 3U;
        else if (v > t[1])
            dibit = 2U;
        else if (v > t[0])
            dibit = 0U;
        else
            dibit = 1U;

        out[i >> 2] |= (uint8_t)(dibit << (6U - 2U * (i & 3U)));
    }
}

void P25Mode::rxNid()
{
    const uint64_t first = syncEnd_ - (uint64_t)(symStep_ * (SYNC_SYMBOLS - 1U) + 0.5);
    uint8_t buf[16];
    bool valid;
    unsigned int duid;

    rxSlice(first, 60U, buf);
    {
        std::lock_guard<std::mutex> lock(gP25);

        resetNID(cfg_.nac);
        valid = decodeNID(buf);
        duid = getDUID();
    }

    if (!valid)
    {
        /* A sync found where the last frame said the next one would be, with a call going: the NID is damaged, the frames
           of a call follow in a known order.  Any other sync with a bad NID is not a frame of this network. */
        if (!rxActive_ || !tracked_)
        {
            state_ = RX_IDLE;
            return;
        }

        double level = 0.0;

        for (unsigned int i = 24U; i < 56U; i++)
            level += std::fabs(rxAt(first + (uint64_t)(symStep_ * i + 0.5)));
        if (level / 32.0 < 250.0)
        {
            state_ = RX_IDLE;               /* nothing there: the call has ended */
            return;
        }

        duid = lastDuid_ == P25_DUID_LDU1 ? P25_DUID_LDU2 : P25_DUID_LDU1;
    }

    frameSyms_ = duidSymbols(duid);
    if (frameSyms_ == 0U)
    {
        state_ = RX_IDLE;                   /* signalling or data: not for this radio */
        return;
    }

    duid_ = duid;

    state_ = RX_FRAME;
}

void P25Mode::rxFrame()
{
    const uint64_t first = syncEnd_ - (uint64_t)(symStep_ * (SYNC_SYMBOLS - 1U) + 0.5);
    uint8_t frame[P25_LDU_FRAME_LENGTH_BYTES];

    rxSlice(first, frameSyms_, frame, true);
    rxLastMs_ = nowMs();
    rxLastN_ = pos_;
    rxHandle(duid_, frame);
    lastDuid_ = duid_;

    if (duid_ == P25_DUID_TDU || duid_ == P25_DUID_TDULC)
    {
        rxEnd(CALL_RX_END);
        state_ = RX_IDLE;
        missed_ = 0U;
        return;
    }

    /* the next frame follows without a gap */
    nextEnd_ = syncEnd_ + (uint64_t)(symStep_ * frameSyms_ + 0.5);
    decideAt_ = nextEnd_ + (uint64_t)(TRACK_WINDOW + 3);
    state_ = RX_SYNC;
}

void P25Mode::rxHandle(unsigned int duid, const uint8_t* frame)
{
    if (duid == P25_DUID_HDU)
    {
        bool ok;

        {
            std::lock_guard<std::mutex> lock(gP25);
            ok = decodeHeader(frame);
        }
        (void)ok;
        if (!rxActive_)
            rxStart(0U, 0U, true);
        return;
    }

    if (duid == P25_DUID_LDU1)
    {
        bool ok;
        unsigned int src = 0U, dst = 0U;
        bool group = true;

        {
            std::lock_guard<std::mutex> lock(gP25);

            ok = decodeLDU1(frame);
            if (ok)
            {
                src = getSrcId();
                dst = getDstId();
                group = getLCF() == P25_LCF_GROUP;
            }
        }

        if (!rxActive_)
            rxStart(ok ? src : 0U, ok ? dst : 0U, ok ? group : true);       /* late entry when there was no header */
        else if (ok && (!rxInfoKnown_ || src != rxSrc_ || dst != rxDst_))
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
    else if (duid == P25_DUID_LDU2)
    {
        bool ok;
        unsigned int algId = P25_ALGO_UNENCRYPT;

        {
            std::lock_guard<std::mutex> lock(gP25);

            ok = decodeLDU2(frame);
            if (ok)
                algId = getAlgId();
        }

        if (!rxActive_)
            rxStart(0U, 0U, true);
        /* a damaged LDU2 can pass the error correction with a wrong algorithm: two in a row are believed */
        if (ok && algId == rxAlgSeen_)
            rxAlgCount_++;
        else if (ok)
        {
            rxAlgSeen_ = algId;
            rxAlgCount_ = 1U;
        }

        if (ok && rxAlgCount_ >= 2U && algId != P25_ALGO_UNENCRYPT && algId != 0U && !rxEncrypted_)
        {
            rxEncrypted_ = true;
            log("the call is encrypted (algorithm 0x" + std::to_string(algId) + "): it cannot be decoded");
        }
    }
    else
        return;

    rxVoice(frame);
}

void P25Mode::rxVoice(const uint8_t* frame)
{
    if (rxEncrypted_)
        return;

    if (!rxVocoderOk_)
    {
        /* the vocoder failed: try again every half second rather than staying deaf for the rest of the call */
        if (nowMs() < rxVocoderRetryMs_)
            return;
        rxVocoderOk_ = true;
    }

    uint8_t blocks[9U * Vocoder::ImbeBytes];
    int16_t pcm[9U * Vocoder::Samples];

    {
        std::lock_guard<std::mutex> lock(gP25);
        uint8_t fixed[P25_LDU_FRAME_LENGTH_BYTES];

        /* correct the voice frames first: the whitening of a frame is seeded by its first 12 bits, so one bit error
           there turns the whole frame into noise */
        std::memcpy(fixed, frame, sizeof(fixed));
        p25RegenerateLDU(fixed, true);

        for (unsigned int n = 0U; n < 9U; n++)
            audip25decode(fixed, blocks + n * Vocoder::ImbeBytes, n);
    }

    if (!vocoder_.decodeImbeBatch(blocks, 9U, pcm))
    {
        rxVocoderOk_ = false;
        rxVocoderRetryMs_ = nowMs() + 500U;
        log("the vocoder stopped answering: received speech is not decoded for a moment");
        return;
    }

    if (ev_.rxAudio)
        ev_.rxAudio(pcm, 9U * Vocoder::Samples);
}

void P25Mode::rxStart(unsigned int src, unsigned int dst, bool group)
{
    rxActive_ = true;
    rxInfoKnown_ = (src != 0U);
    rxSrc_ = src;
    rxDst_ = dst;
    rxGroup_ = group;
    rxVocoderOk_ = true;
    rxEncrypted_ = false;
    rxAlgSeen_ = P25_ALGO_UNENCRYPT;
    rxAlgCount_ = 0U;

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

void P25Mode::rxEnd(CallEvent how)
{
    if (!rxActive_)
        return;

    CallInfo info;
    info.event = how;
    info.src = rxSrc_;
    info.dst = rxDst_;
    info.group = rxGroup_;
    info.ber = 0.0F;
    rxActive_ = false;
    if (ev_.call)
        ev_.call(info);
}

void P25Mode::rxLost()
{
    rxEnd(CALL_RX_LOST);
    state_ = RX_IDLE;
    missed_ = 0U;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Transmit                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------ */

void P25Mode::pttDown()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        mic_.clear();
        ptt_ = true;
    }
    txCv_.notify_all();
}

void P25Mode::pttUp()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        ptt_ = false;
    }
    txCv_.notify_all();
}

void P25Mode::micAudio(const int16_t* pcm, size_t count)
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

/* Shapes symbol levels into samples: each symbol gives five samples, y[5k + p] = sum over j of RC[p + 5j] * x[k - j]
   (p25_host's interpolating filter) */
void P25Mode::txSymbols(const int16_t* levels, size_t count)
{
    int16_t out[SPS * 192U];

    while (count > 0U)
    {
        size_t chunk = count > 192U ? 192U : count;

        for (size_t k = 0U; k < chunk; k++)
        {
            for (unsigned int j = 7U; j > 0U; j--)
                modHist_[j] = modHist_[j - 1U];
            modHist_[0] = levels[k];

            for (unsigned int p = 0U; p < SPS; p++)
            {
                int64_t acc = 0;

                for (unsigned int j = 0U; j < 8U; j++)
                    acc += (int64_t)RC_0_2_FILTER[p + SPS * j] * modHist_[j];

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

/* Bytes of dibits, the high pair first: 11 is the highest level, 01 the lowest */
void P25Mode::txBytes(const uint8_t* bytes, size_t count)
{
    int16_t levels[192];
    size_t used = 0U;

    for (size_t i = 0U; i < count; i++)
    {
        for (unsigned int s = 0U; s < 4U; s++)
        {
            switch ((bytes[i] >> (6U - 2U * s)) & 3U)
            {
                case 3U:  levels[used++] = LEVEL_A; break;
                case 2U:  levels[used++] = LEVEL_B; break;
                case 0U:  levels[used++] = LEVEL_C; break;
                default:  levels[used++] = LEVEL_D; break;
            }
        }
        if (used + 4U > sizeof(levels) / sizeof(levels[0]))
        {
            txSymbols(levels, used);
            used = 0U;
        }
    }
    if (used > 0U)
        txSymbols(levels, used);
}

void P25Mode::sendFrame(const uint8_t* frame, unsigned int length)
{
    if (cfg_.txTap)
        cfg_.txTap(frame, length);
    txBytes(frame, length);
}

/* The link control and the other fields every frame of the call carries */
static void setCall(const P25Config& cfg)
{
    uint8_t mi[P25_MI_LENGTH_BYTES];

    std::memset(mi, 0x00, sizeof(mi));
    p25Reset();
    resetNID(cfg.nac);
    setLCF(cfg.group ? P25_LCF_GROUP : P25_LCF_PRIVATE);
    setMFId(0x00U);
    setSrcId(cfg.srcId);
    setDstId(cfg.dstId);
    setMI(mi);
    setAlgId(P25_ALGO_UNENCRYPT);
    setKId(0U);
}

void P25Mode::buildHeader(uint8_t* frame)
{
    std::lock_guard<std::mutex> lock(gP25);

    setCall(cfg_);
    std::memset(frame, 0x00, P25_HDR_FRAME_LENGTH_BYTES);
    addSync(frame);
    encodeNID(frame, P25_DUID_HDU);
    encodeHeader(frame);
    addBusyBits(frame, P25_HDR_FRAME_LENGTH_BITS, false, true);
}

void P25Mode::buildLdu(const uint8_t* imbe, bool second, uint8_t* frame)
{
    std::lock_guard<std::mutex> lock(gP25);

    setCall(cfg_);
    std::memset(frame, 0x00, P25_LDU_FRAME_LENGTH_BYTES);
    addSync(frame);
    encodeNID(frame, second ? P25_DUID_LDU2 : P25_DUID_LDU1);
    if (second)
        encodeLDU2(frame);
    else
        encodeLDU1(frame);

    for (unsigned int n = 0U; n < 9U; n++)
        audip25encode(frame, imbe + n * Vocoder::ImbeBytes, n);

    lowSpeedData_setLSD1(0x00U);
    lowSpeedData_setLSD2(0x00U);
    lowSpeedData_encode1(frame);
    addBusyBits(frame, P25_LDU_FRAME_LENGTH_BITS, false, true);
}

void P25Mode::buildTerminator(uint8_t* frame)
{
    std::lock_guard<std::mutex> lock(gP25);

    setCall(cfg_);
    std::memset(frame, 0x00, P25_TERM_FRAME_LENGTH_BYTES);
    addSync(frame);
    encodeNID(frame, P25_DUID_TDU);
    addBusyBits(frame, P25_TERM_FRAME_LENGTH_BITS, true, false);
}

void P25Mode::txThread()
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

void P25Mode::txCall()
{
    uint8_t header[P25_HDR_FRAME_LENGTH_BYTES];
    uint8_t ldu[P25_LDU_FRAME_LENGTH_BYTES];
    uint8_t term[P25_TERM_FRAME_LENGTH_BYTES];
    uint8_t blocks[9U * Vocoder::ImbeBytes];
    int16_t pcm[9U * Vocoder::Samples];
    const size_t need = sizeof(pcm) / sizeof(pcm[0]);
    unsigned int index = 0U, frames = 0U;
    bool ok = true;

    /* a call being received is over for us */
    {
        std::lock_guard<std::mutex> lock(rxMutex_);

        rxEnd(CALL_RX_LOST);
        state_ = RX_IDLE;
        missed_ = 0U;
    }

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
    link_.txBegin();
    std::memset(modHist_, 0x00, sizeof(modHist_));

    /* The start sync (0x77 bytes, a tone), so the hotspot's squelch and AFC settle.  The microphone speech of the first
       LDU is gathered meanwhile: the frames go out one after the other with no gap, so the speech has to be there. */
    {
        uint8_t chunk[24];
        unsigned int bytes = 0U;
        const unsigned int minBytes = (cfg_.txDelayMs * 6U) / 5U;
        const unsigned int maxBytes = 1800U;

        std::memset(chunk, P25_START_SYNC, sizeof(chunk));
        while (running_ && link_.isOpen() && bytes < maxBytes)
        {
            if (bytes >= minBytes)
            {
                std::lock_guard<std::mutex> lock(txMutex_);

                if (mic_.size() >= need || !ptt_)
                    break;
            }
            txBytes(chunk, sizeof(chunk));              /* 20 ms */
            bytes += (unsigned int)sizeof(chunk);
        }
    }

    buildHeader(header);
    sendFrame(header, P25_HDR_FRAME_LENGTH_BYTES);

    /* voice: one LDU per 180 ms of speech from the microphone */
    while (ok && running_ && link_.isOpen())
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

        if (!vocoder_.encodeImbeBatch(pcm, 9U, blocks))
        {
            log("the vocoder stopped answering: ending the transmission");
            ok = false;
            break;
        }

        buildLdu(blocks, (index & 1U) != 0U, ldu);
        sendFrame(ldu, P25_LDU_FRAME_LENGTH_BYTES);
        index++;
        frames++;

        if (last && got < need)
            break;
    }

    /* the terminator, then the tail of the pulse shaping and the end of transmission */
    if (link_.isOpen())
    {
        buildTerminator(term);
        sendFrame(term, P25_TERM_FRAME_LENGTH_BYTES);

        int16_t zeros[48];
        std::memset(zeros, 0x00, sizeof(zeros));
        for (unsigned int n = 0U; n < 6U; n++)          /* the terminator must not be cut off by the end of transmission */
            txSymbols(zeros, 48U);
        link_.txEnd();
    }

    if (!ok)
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        ptt_ = false;
    }

    hangUntil_ = nowMs() + cfg_.modeHangMs;
    txActive_ = false;

    info.event = CALL_TX_END;
    info.text = std::to_string(frames) + " voice frames";
    if (ev_.call)
        ev_.call(info);
}

}
