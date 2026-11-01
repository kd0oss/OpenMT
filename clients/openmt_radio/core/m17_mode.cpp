#include "m17_mode.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace openmt
{

static uint64_t nowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static const size_t   RING          = 8192U;
static const unsigned FRAME_SYMBOLS = 192U;
static const unsigned SYMBOL_LEN    = 5U;                               /* samples */
static const unsigned FRAME_SAMPLES = FRAME_SYMBOLS * SYMBOL_LEN;       /* 960 */
static const unsigned SYNC_SYMBOLS  = 8U;
static const double   OUTER_MIN     = 10.0;                             /* smallest outer symbol level that is looked at */
static const double   SYNC_FIRST    = 0.10;                             /* first sync: error as a part of the sync energy */
static const double   SYNC_NEXT     = 0.30;                             /* sync of a frame that follows */
static const unsigned MAX_MISSES    = 3U;

/* m17_host.cpp: the symbol levels, the transmit pulse (45 taps, 9 per phase) and the receive filter (42 taps) */
static const q15_t LEVEL_A =  1481;
static const q15_t LEVEL_B =   494;
static const q15_t LEVEL_C =  -494;
static const q15_t LEVEL_D = -1481;

static const q15_t TX_RRC[45] = { 0, 0, 0, 0, -290, -174, 142, 432, 438, 90, -387, -561, -155, 658, 1225, 767,
                                  -980, -3326, -4648, -3062, 2527, 11552, 21705, 29724, 32767, 29724, 21705,
                                  11552, 2527, -3062, -4648, -3326, -980, 767, 1225, 658, -155, -561, -387, 90,
                                  438, 432, 142, -174, -290 };
static const int16_t RX_RRC[42] = { -147, -88, 72, 220, 223, 46, -197, -285, -79, 334, 623, 390, -498, -1691, -2363, -1556, 1284, 5872, 11033,
                                    15109, 16656, 15109, 11033, 5872, 1284, -1556, -2363, -1691, -498, 390, 623, 334, -79, -285, -197, 46, 223,
                                    220, 72, -88, -147, 0 };

/* the four sync words; the first byte and then the second, two bits a symbol, most significant first */
static const uint8_t SYNC_WORDS[4][2] = { { 0x55U, 0xF7U }, { 0xFFU, 0x5DU }, { 0x75U, 0xFFU }, { 0x55U, 0x5DU } };

/* the sign the receiver sees at each symbol of a sync word: a dibit 11 is high, 01 is low (sync words have no others) */
static int syncSign(unsigned int kind, unsigned int i)
{
    unsigned int dibit = (SYNC_WORDS[kind][i / 4U] >> (6U - 2U * (i % 4U))) & 3U;

    return dibit == 3U ? 1 : -1;
}

static std::string trimmed(const std::string& s)
{
    size_t n = s.size();

    while (n > 0U && (s[n - 1U] == ' ' || s[n - 1U] == '\0'))
        n--;
    return s.substr(0U, n);
}

/* ------------------------------------------------------------------------------------------------------------ */

M17Mode::M17Mode(ModemLink& link, const M17Config& cfg, const RadioEvents& events) :
    link_(link),
    cfg_(cfg),
    ev_(events),
    codec_(),
    running_(false),
    thread_(),
    rxMutex_(),
    state_(RX_SEARCH),
    ring_(RING, 0.0),
    histPos_(0U),
    n_(0U),
    pendingAt_(0U),
    lock_(0U),
    center_(0.0),
    outer_(0.0),
    levelsKnown_(false),
    misses_(0U),
    decoder_(),
    rxActive_(false),
    rxCan_(0U),
    rxErrs_(0UL),
    rxBits_(0UL),
    rxFrames_(0UL),
    rxBadFrames_(0UL),
    rxMiss_(0UL),
    rxMaxErrs_(0),
    seq_(-1),
    seqRun_(0U),
    rxMuted_(0UL),
    rxOuterSum_(0.0),
    lastFn_(-1),
    rxUnsupportedLogged_(false),
    txMutex_(),
    txCv_(),
    mic_(),
    ptt_(false),
    txActive_(false),
    hangUntil_(0U)
{
    std::memset(hist_, 0, sizeof(hist_));
    std::memset(errRing_, 0, sizeof(errRing_));
    std::memset(matchRing_, 0, sizeof(matchRing_));
    std::memset(modState_, 0, sizeof(modState_));
    modFilter_.L           = (uint8_t)SYMBOL_LEN;
    modFilter_.phaseLength = 9U;
    modFilter_.pCoeffs     = TX_RRC;
    modFilter_.pState      = modState_;
}

M17Mode::~M17Mode()
{
    stop();
}

std::string M17Mode::modeName() const
{
    return "M17";
}

std::string M17Mode::modemType() const
{
    return "4FSK";
}

void M17Mode::log(const std::string& text) const
{
    if (ev_.log)
        ev_.log(text);
}

bool M17Mode::open(std::string& error)
{
    codec_ = makeCodec2(cfg_.standIn, error);
    return codec_ != nullptr;
}

void M17Mode::start()
{
    if (running_)
        return;

    if (!codec_)
    {
        std::string error;

        if (!open(error))
            log("M17: " + error);
    }

    running_ = true;
    thread_ = std::thread(&M17Mode::txThread, this);
}

void M17Mode::stop()
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

bool M17Mode::transmitting() const
{
    return txActive_;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Receive                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

void M17Mode::rxSamples(const int16_t* samples, size_t count)
{
    if (txActive_ || !running_ || !codec_)
        return;                 /* half duplex: the receiver is deaf while transmitting */

    std::lock_guard<std::mutex> lock(rxMutex_);

    for (size_t i = 0U; i < count; i++)
        rxSample((double)samples[i] * cfg_.rxGain);
}

void M17Mode::rxReset()
{
    state_ = RX_SEARCH;
    pendingAt_ = 0U;
    misses_ = 0U;
    levelsKnown_ = false;
    decoder_.reset();
    lastFn_ = -1;
}

double M17Mode::y(uint64_t i) const
{
    return ring_[i & (RING - 1U)];
}

void M17Mode::rxSample(double v)
{
    hist_[histPos_] = v;

    double acc = 0.0;
    unsigned int p = histPos_;

    if (cfg_.rxFilter == 0)
    {
        /* the receive filter of the host (42 taps, root raised cosine) */
        for (unsigned int k = 0U; k < 42U; k++)
        {
            acc += (double)RX_RRC[k] * hist_[p];
            p = (p == 0U) ? 63U : p - 1U;
        }
        acc /= 32768.0;
    }
    else
    {
        /* Light: the signal from a modem has been filtered already, and the root raised cosine on top of that blurs the
           symbols into each other. Two samples averaged, taken in the middle of the delay of the other filter */
        acc = 0.5 * (hist_[(histPos_ + 64U - 20U) & 63U] + hist_[(histPos_ + 64U - 21U) & 63U]);
    }
    histPos_ = (histPos_ + 1U) & 63U;

    ring_[n_ & (RING - 1U)] = acc;
    n_++;

    if (n_ < 64U)
        return;

    if (state_ == RX_SEARCH)
        rxSearch();
    else
        rxTrack();
}

/* How well the eight symbols that end at sample c (the centre of the last) are a sync word: the levels of the high and the
   low symbols give the centre and the outer level, the error is what is left against that fit */
bool M17Mode::matchSync(uint64_t c, SyncMatch& m) const
{
    double s[SYNC_SYMBOLS];

    for (unsigned int i = 0U; i < SYNC_SYMBOLS; i++)
        s[i] = y(c - (uint64_t)SYMBOL_LEN * (SYNC_SYMBOLS - 1U - i));

    bool found = false;

    m.err = 1.0e30;
    for (unsigned int kind = 0U; kind < 4U; kind++)
    {
        double hi = 0.0, lo = 0.0;
        unsigned int nh = 0U, nl = 0U;

        for (unsigned int i = 0U; i < SYNC_SYMBOLS; i++)
        {
            if (syncSign(kind, i) > 0)
            {
                hi += s[i];
                nh++;
            }
            else
            {
                lo += s[i];
                nl++;
            }
        }
        hi /= (double)nh;
        lo /= (double)nl;

        double outer = (hi - lo) / 2.0, center = (hi + lo) / 2.0;

        if (outer < OUTER_MIN)
            continue;

        double err = 0.0;

        for (unsigned int i = 0U; i < SYNC_SYMBOLS; i++)
        {
            double d = s[i] - center - (double)syncSign(kind, i) * outer;

            err += d * d;
        }
        err /= (double)SYNC_SYMBOLS * outer * outer;

        if (err < m.err)
        {
            m.err = err;
            m.center = center;
            m.outer = outer;
            m.kind = kind;
            found = true;
        }
    }
    return found;
}

/* Looks for a sync word at every sample.  The first good one starts a short wait, and the best of the six positions that
   follow it is the position of the frame. */
void M17Mode::rxSearch()
{
    uint64_t c = n_ - 1U;
    SyncMatch m;

    if (!matchSync(c, m))
        m.err = 1.0e30;

    if (pendingAt_ == 0U)
    {
        if (m.err < SYNC_FIRST)
        {
            pendingAt_ = c;
            matchRing_[0] = m;
        }
        return;
    }

    uint64_t j = c - pendingAt_;

    if (j < 8U)
        matchRing_[j] = m;

    if (j < SYMBOL_LEN)
        return;

    unsigned int best = 0U;

    for (unsigned int k = 1U; k <= SYMBOL_LEN; k++)
        if (matchRing_[k].err < matchRing_[best].err)
            best = k;

    lock_ = pendingAt_ + best;
    center_ = matchRing_[best].center;
    outer_ = matchRing_[best].outer;
    levelsKnown_ = true;
    pendingAt_ = 0U;
    misses_ = 0U;
    state_ = RX_FRAME;

    if (matchRing_[best].kind == 3U)
    {
        rxEnd(CALL_RX_END);
        rxReset();
    }
}

/* Waits for the end of the frame and the place of the next sync word; decodes the frame, then follows the sync word */
void M17Mode::rxTrack()
{
    if (n_ <= lock_ + FRAME_SAMPLES + 3U)
        return;

    rxFrame(lock_);

    /* the sync word of the next frame, a frame on, within two samples */
    uint64_t expect = lock_ + FRAME_SAMPLES;
    SyncMatch best;
    bool found = false;

    best.err = 1.0e30;
    best.kind = 0U;
    best.center = best.outer = 0.0;

    for (int d = -2; d <= 2; d++)
    {
        SyncMatch m;
        uint64_t c = expect + (uint64_t)(int64_t)d;

        if (matchSync(c, m) && m.err < best.err)
        {
            best = m;
            lock_ = c;
            found = true;
        }
    }

    if (found && best.err < SYNC_NEXT)
    {
        misses_ = 0U;
        center_ = 0.5 * center_ + 0.5 * best.center;
        outer_ = 0.5 * outer_ + 0.5 * best.outer;

        if (best.kind == 3U)
        {
            rxEnd(CALL_RX_END);
            rxReset();
        }
        return;
    }

    lock_ = expect;                     /* no sync word: carry on at the same pace */
    if (++misses_ >= MAX_MISSES)
    {
        rxEnd(CALL_RX_LOST);
        rxReset();
    }
}

std::string M17Mode::rxStartText() const
{
    char can[16];

    std::snprintf(can, sizeof(can), " CAN %u", rxCan_);
    return rxSrc_ + ">" + rxDst_ + can;
}

void M17Mode::rxStart()
{
    CallInfo info;

    rxActive_ = true;
    rxErrs_ = rxBits_ = 0UL;
    rxFrames_ = rxBadFrames_ = 0UL;
    rxMaxErrs_ = 0;
    rxOuterSum_ = 0.0;
    seq_ = -1;
    seqRun_ = 0U;
    rxMuted_ = 0UL;

    info.event = CALL_RX_START;
    info.src = info.dst = 0U;
    info.group = true;
    info.ber = 0.0F;
    info.text = rxStartText();
    if (ev_.call)
        ev_.call(info);
}

void M17Mode::rxEnd(CallEvent how)
{
    if (!rxActive_)
        return;

    CallInfo info;

    info.event = how;
    info.src = info.dst = 0U;
    info.group = true;
    info.ber = rxBits_ > 0UL ? (float)(100.0 * (double)rxErrs_ / (double)rxBits_) : 0.0F;
    info.text = rxStartText();

    if (rxFrames_ > 0UL)
    {
        char t[256];

        std::snprintf(t, sizeof(t),
                      "M17: call ended: %lu frames, %lu with more than 12 bit errors, worst frame %d errors of 368, bit error rate %.1f%%, symbol level %.0f, %lu frames not played (out of sequence)",
                      rxFrames_, rxBadFrames_, rxMaxErrs_, (double)info.ber, rxOuterSum_ / (double)rxFrames_, rxMuted_);
        log(t);
    }

    rxActive_ = false;
    rxErrs_ = rxBits_ = 0UL;
    if (ev_.call)
        ev_.call(info);
}

/* Slices the 192 symbols of the frame whose sync word ends at c into 48 bytes (sync word included, as the host's frames are),
   and decodes them */
void M17Mode::rxFrame(uint64_t c)
{
    M17::frame_t frame;
    uint64_t first = c - (uint64_t)SYMBOL_LEN * (SYNC_SYMBOLS - 1U);
    const double t = outer_ * (2.0 / 3.0);

    frame.fill(0x00);

    double v[FRAME_SYMBOLS];
    double c0 = center_, tl = -t, th = t;

    for (unsigned int k = 0U; k < FRAME_SYMBOLS; k++)
        v[k] = y(first + (uint64_t)SYMBOL_LEN * k);

    /* The sync word fixes the sampling instant to a sample, a fifth of a symbol, and a signal that has been through a modem
       is blurred enough for that to matter. Look at the symbols at a quarter sample steps around it, and keep the instant
       at which they fall into the four levels most tightly (the symbols of a frame are equally spread over the four) */
    {
        double bestCost = 1.0e300, bv[FRAME_SYMBOLS];
        const double step = 0.25, span = 1.0;

        for (double d = -span; d <= span + 1e-9; d += step)
        {
            double w[FRAME_SYMBOLS], srt[FRAME_SYMBOLS];

            for (unsigned int k = 0U; k < FRAME_SYMBOLS; k++)
            {
                double pos = (double)(first + (uint64_t)SYMBOL_LEN * k) + d;
                uint64_t i0 = (uint64_t)std::floor(pos);
                double fr = pos - (double)i0;

                w[k] = y(i0) * (1.0 - fr) + y(i0 + 1U) * fr;
                srt[k] = w[k];
            }
            std::sort(srt, srt + FRAME_SYMBOLS);

            double cost = 0.0;

            for (unsigned int g = 0U; g < 4U; g++)
            {
                double m = 0.0;

                for (unsigned int k = g * 48U; k < (g + 1U) * 48U; k++)
                    m += srt[k];
                m /= 48.0;
                for (unsigned int k = g * 48U; k < (g + 1U) * 48U; k++)
                    cost += (srt[k] - m) * (srt[k] - m);
            }
            if (cost < bestCost)
            {
                bestCost = cost;
                std::memcpy(bv, w, sizeof(bv));
            }
        }
        std::memcpy(v, bv, sizeof(v));
    }

    for (unsigned int k = 0U; k < FRAME_SYMBOLS; k++)
    {
        double x = v[k] - c0;
        unsigned int dibit;

        if (x < tl)
            dibit = 1U;                 /* 01 */
        else if (x < 0.0)
            dibit = 0U;                 /* 00 */
        else if (x < th)
            dibit = 2U;                 /* 10 */
        else
            dibit = 3U;                 /* 11 */
        frame[k / 4U] = (uint8_t)(frame[k / 4U] | (dibit << (6U - 2U * (k % 4U))));
    }

    int before = decoder_.bitErr;
    M17::M17FrameType type = decoder_.decodeFrame(frame);
    int errs = decoder_.bitErr - before;

    if (type == M17::M17FrameType::LINK_SETUP || type == M17::M17FrameType::STREAM)
    {
        M17::M17LinkSetupFrame lsf = decoder_.getLsf();

        if (!rxActive_ && lsf.valid())
        {
            rxSrc_ = trimmed(lsf.getSource());
            rxDst_ = trimmed(lsf.getDestination());
            rxCan_ = lsf.getType().fields.CAN;
            rxStart();
        }

        if (rxActive_)
        {
            if (errs > 0)
                rxErrs_ += (unsigned long)errs;
            rxBits_ += 368UL;
            rxFrames_++;
            rxOuterSum_ += outer_;
            if (errs > rxMaxErrs_)
                rxMaxErrs_ = errs;
            if (errs > 12)
                rxBadFrames_++;
        }

        if (type == M17::M17FrameType::STREAM && rxActive_)
            rxStream();
    }
}

/* The payload of a stream frame: two Codec2 blocks of 20 ms */
void M17Mode::rxStream()
{
    M17::M17StreamFrame sf = decoder_.getStreamFrame();
    int fn = (int)sf.getFrameNumber();

    if (fn == lastFn_)
        return;                         /* the same frame again */
    lastFn_ = fn;

    M17::M17LinkSetupFrame lsf = decoder_.getLsf();
    M17::streamType_t st = lsf.getType();

    if (st.fields.dataMode != 1U || st.fields.dataType != 2U || st.fields.encType != 0U)
    {
        if (!rxUnsupportedLogged_)
        {
            rxUnsupportedLogged_ = true;
            log("M17: a call that is not unencrypted Codec2 3200 voice was heard (type " + std::to_string(st.value) +
                "): its speech is not played");
        }
        return;
    }

    /* The frame number counts up by one in a real call. A frame that does not follow the one before is the result of bit
       errors that the error correction could not undo, and is not played: Codec2 turns bad bits into loud noise */
    int fn15 = fn & 0x7FFF;
    unsigned int delta = seq_ < 0 ? 1U : (unsigned int)((fn15 - seq_) & 0x7FFF);

    if (delta >= 1U && delta <= 3U)
        seqRun_++;
    else
        seqRun_ = 0U;
    seq_ = fn15;
    if (seqRun_ == 0U)
    {
        rxMuted_++;
        return;
    }

    const uint8_t* p = sf.payload().data();

    for (unsigned int b = 0U; b < 2U; b++)
    {
        int16_t pcm[Codec2Codec::Samples];

        codec_->decode(p + b * Codec2Codec::BlockBytes, pcm);
        if (ev_.rxAudio)
            ev_.rxAudio(pcm, Codec2Codec::Samples);
    }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Transmit                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------ */

void M17Mode::pttDown()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        mic_.clear();
        ptt_ = true;
    }
    txCv_.notify_all();
}

void M17Mode::pttUp()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        ptt_ = false;
    }
    txCv_.notify_all();
}

void M17Mode::micAudio(const int16_t* pcm, size_t count)
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

/* Four symbols a byte, the first two bits first: 11 is the highest level, 10 the next, 00 the next below, 01 the lowest */
void M17Mode::txBytes(const uint8_t* bytes, size_t count)
{
    q15_t out[SYMBOL_LEN * 4U * 48U];
    size_t pos = 0U;
    const float scale = cfg_.txLevel > 0.0F ? cfg_.txLevel : 1.0F;

    for (size_t i = 0U; i < count; i++)
    {
        q15_t in[4];
        uint8_t c = bytes[i];

        for (unsigned int k = 0U; k < 4U; k++, c = (uint8_t)(c << 2))
        {
            switch (c & 0xC0U)
            {
                case 0xC0U: in[k] = LEVEL_A; break;
                case 0x80U: in[k] = LEVEL_B; break;
                case 0x00U: in[k] = LEVEL_C; break;
                default:    in[k] = LEVEL_D; break;
            }
            in[k] = (q15_t)((float)in[k] * scale);
        }

        arm_fir_interpolate_q15(&modFilter_, in, out + pos, 4U);
        pos += SYMBOL_LEN * 4U;

        if (pos + SYMBOL_LEN * 4U > sizeof(out) / sizeof(out[0]) || i + 1U == count)
        {
            link_.txSamples(out, pos);
            pos = 0U;
        }
    }
}

/* Lets the last symbols leave the pulse shape */
void M17Mode::txFlush()
{
    q15_t in[4] = { 0, 0, 0, 0 };
    q15_t out[SYMBOL_LEN * 4U * 3U];

    for (unsigned int k = 0U; k < 3U; k++)
        arm_fir_interpolate_q15(&modFilter_, in, out + k * SYMBOL_LEN * 4U, 4U);
    link_.txSamples(out, sizeof(out) / sizeof(out[0]));
}

void M17Mode::txThread()
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

void M17Mode::txCall()
{
    const size_t need = 2U * Codec2Codec::Samples;      /* 40 ms: one stream frame */
    int16_t pcm[2U * Codec2Codec::Samples];
    unsigned int frames = 0U;
    bool ok = true;

    if (!codec_)
    {
        log("M17: there is no Codec2 library, so nothing can be sent");
        std::lock_guard<std::mutex> lock(txMutex_);
        ptt_ = false;
        return;
    }

    {
        /* a call being received is over for us */
        std::lock_guard<std::mutex> lock(rxMutex_);
        rxEnd(CALL_RX_LOST);
        rxReset();
    }

    txActive_ = true;

    CallInfo info;
    info.event = CALL_TX_START;
    info.src = info.dst = 0U;
    info.group = true;
    info.ber = 0.0F;
    info.text = cfg_.callsign + ">" + cfg_.dest;
    if (ev_.call)
        ev_.call(info);

    link_.requestMode();
    link_.txBegin();
    std::memset(modState_, 0, sizeof(modState_));

    M17::M17FrameEncoder encoder;
    M17::M17LinkSetupFrame lsf;
    M17::frame_t frame;

    encoder.reset();
    lsf.setSource(cfg_.callsign);
    lsf.setDestination(cfg_.dest);

    M17::streamType_t st;
    st.value = 0U;
    st.fields.dataMode   = 1U;          /* stream */
    st.fields.dataType   = 2U;          /* voice */
    st.fields.encType    = 0U;          /* not encrypted */
    st.fields.encSubType = 0U;
    st.fields.CAN        = cfg_.can & 0x0FU;
    lsf.setType(st);
    lsf.updateCrc();

    /* the preamble lets the receivers settle */
    {
        uint8_t pre[48];
        unsigned int bytes = (cfg_.txDelayMs * 12U) / 10U;      /* 1200 bytes a second */

        std::memset(pre, 0x77, sizeof(pre));
        while (bytes > 0U)
        {
            unsigned int chunk = bytes > sizeof(pre) ? (unsigned int)sizeof(pre) : bytes;

            txBytes(pre, chunk);
            bytes -= chunk;
        }
    }

    encoder.encodeLsf(lsf, frame);
    txBytes(frame.data(), frame.size());

    while (ok && running_)
    {
        size_t got = 0U;
        bool last = false;

        {
            std::unique_lock<std::mutex> lock(txMutex_);

            /* wait for 40 ms of speech; stop when the key is released and nothing is left; a stalled microphone is padded */
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

        M17::payload_t payload;

        codec_->encode(pcm, payload.data());
        codec_->encode(pcm + Codec2Codec::Samples, payload.data() + Codec2Codec::BlockBytes);

        encoder.encodeStreamFrame(payload, frame, last && got < need);
        txBytes(frame.data(), frame.size());
        frames++;

        if (!link_.isOpen())
            ok = false;
        if (last && got < need)
            break;
    }

    if (link_.isOpen())
    {
        encoder.encodeEotFrame(frame);
        txBytes(frame.data(), frame.size());
        txFlush();
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
