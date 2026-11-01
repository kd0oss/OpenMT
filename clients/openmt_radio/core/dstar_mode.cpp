#include "dstar_mode.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "dstar_codec.h"

extern "C" {
#include <fec_tools.h>
}
#include "dstar_fec.h"          /* the host's AMBE frame error count (protocols/DSTAR) */

namespace openmt
{

static uint64_t nowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static const size_t   RING           = 8192U;           /* samples kept for the receiver */
static const double   LEVEL_SYMBOL   = 841.0;           /* dstar_host's DSTAR_LEVEL1 */
static const double   SIGNAL_LEVEL   = 8.0;             /* average sample magnitude below which there is nothing to look at */
static const double   QUIET_RATIO    = 0.2;             /* a call is over when the level falls below this part of its level at the start */
static const uint64_t QUIET_SAMPLES  = 2400U;           /* 100 ms of nothing ends a call */
static const unsigned HEADER_SKIP    = 4U;              /* the host's sync match takes these first four (always zero) header bits */
static const unsigned HEADER_BITS    = 664U;            /* 4 skipped + 660 coded bits */
static const unsigned FRAME_BITS     = 96U;             /* 72 bits of voice, 24 of slow data */

/* dstar_host's GMSK pulse: its TX_GAUSSIAN_0_35_FILTER, applied as arm_fir_interpolate_q15 does (the coefficients are in
   time reversed order), three symbols, five samples each */
static const int32_t PULSE[15] = { 0, 0, 0, 0, 1001, 3514, 9333, 18751, 28499, 32767, 28499, 18751, 9333, 3514, 1001 };

static const uint8_t SCRAMBLER[3]  = { 0x70U, 0x4FU, 0x93U };
static const uint8_t DATA_SYNC[3]  = { 0x55U, 0x2DU, 0x16U };
static const uint8_t END_SYNC[6]   = { 0x55U, 0x55U, 0x55U, 0x55U, 0xC8U, 0x7AU };
static const uint64_t END_PATTERN  = 0x0000AAAAAAAA135EULL;     /* the end pattern as the receiver's bit register sees it */
static const uint8_t FRAME_SYNC[2] = { 0xEAU, 0xA6U };          /* before the header, after the bit sync */

/* the bit sync and frame sync as the receiver's register sees them: the last 32 bits before the header */
static uint32_t syncPattern()
{
    const uint8_t bytes[4] = { 0xAAU, 0xAAU, FRAME_SYNC[0], FRAME_SYNC[1] };
    uint32_t v = 0U;

    for (unsigned int i = 0U; i < 4U; i++)
        for (unsigned int k = 0U; k < 8U; k++)
            v = (v << 1) | ((bytes[i] >> k) & 1U);          /* bits go out least significant first */
    return v;
}

static unsigned int popcount64(uint64_t v)
{
    unsigned int n = 0U;

    while (v)
    {
        n += (unsigned int)(v & 1U);
        v >>= 1;
    }
    return n;
}

static void padCall(char* d, const std::string& s, size_t width)
{
    std::memset(d, ' ', width);
    std::memcpy(d, s.c_str(), s.size() > width ? width : s.size());
    d[width] = '\0';
}

static std::string trimmed(const char* s, size_t width)
{
    size_t n = width;

    while (n > 0U && (s[n - 1U] == ' ' || s[n - 1U] == '\0'))
        n--;
    return std::string(s, n);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* DstarMode                                                                                                    */
/* ------------------------------------------------------------------------------------------------------------ */

DstarMode::DstarMode(ModemLink& link, Vocoder& vocoder, const DstarConfig& cfg, const RadioEvents& events) :
    link_(link),
    vocoder_(vocoder),
    cfg_(cfg),
    ev_(events),
    running_(false),
    thread_(),
    rxMutex_(),
    state_(RX_SEARCH),
    ring_(RING, 0),
    n_(0U),
    dc_(0.0),
    lockLevel_(0.0),
    diagAt_(0U),
    diagBest_(32U),
    diagLevel_(0.0),
    level_(0.0),
    pos_(0.0),
    prevBit_(-1),
    bitCount_(0U),
    eotReg_(0U),
    frameIndex_(0U),
    syncMisses_(0U),
    pendingC_(0U),
    diagBad_(0U),
    badHeaders_(0U),
    inv_(false),
    quietFrom_(0U),
    rxActive_(false),
    rxInfoKnown_(false),
    rxMessageSeen_(0U),
    rxBlock_(-1),
    rxErrs_(0UL),
    rxBits_(0UL),
    rxVocoderOk_(true),
    rxAudio_(),
    txMutex_(),
    txCv_(),
    mic_(),
    ptt_(false),
    txActive_(false),
    hangUntil_(0U)
{
    padCall(my_, cfg_.callsign, 8U);
    padCall(suffix_, cfg_.suffix, 4U);
    padCall(ur_, cfg_.ur, 8U);
    padCall(rpt1_, cfg_.rpt1, 8U);
    padCall(rpt2_, cfg_.rpt2, 8U);
    std::memset(sr_, 0x00, sizeof(sr_));
    std::memset(hdr_, 0x00, sizeof(hdr_));
    std::memset(frame_, 0x00, sizeof(frame_));
    std::memset(rxMessage_, ' ', sizeof(rxMessage_));
    rxMessage_[20] = '\0';
    std::memset(txHist_, 0x00, sizeof(txHist_));
    std::memset(slow_, 0x00, sizeof(slow_));
}

DstarMode::~DstarMode()
{
    stop();
}

std::string DstarMode::modeName() const
{
    return "DSTAR";
}

std::string DstarMode::modemType() const
{
    return "GMSK";
}

void DstarMode::log(const std::string& text) const
{
    if (ev_.log)
        ev_.log(text);
}

void DstarMode::start()
{
    if (running_)
        return;

    if (!vocoder_.setCodec(Vocoder::CODEC_DSTAR))
        log("D-STAR: the vocoder did not accept the D-STAR rate: speech will not be right");

    running_ = true;
    thread_ = std::thread(&DstarMode::txThread, this);
}

void DstarMode::stop()
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

bool DstarMode::transmitting() const
{
    return txActive_;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Receive                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

void DstarMode::rxSamples(const int16_t* samples, size_t count)
{
    if (txActive_ || !running_)
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

void DstarMode::rxReset()
{
    state_ = RX_SEARCH;
    bitCount_ = 0U;
    prevBit_ = -1;
    pendingC_ = 0U;
    std::memset(sr_, 0x00, sizeof(sr_));
    quietFrom_ = 0U;
}

void DstarMode::rxSample(int16_t s)
{
    ring_[n_ & (RING - 1U)] = s;
    n_++;

    level_ += (std::fabs((double)s) - level_) / 64.0;
    dc_ += ((double)s - dc_) / 4096.0;

    if (state_ == RX_SEARCH)
    {
        if (n_ >= 4U)
            rxSearch(n_ - 2U);
        return;
    }

    /* a call whose signal has gone */
    if (level_ < QUIET_RATIO * lockLevel_)
    {
        if (quietFrom_ == 0U)
            quietFrom_ = n_;
        else if (n_ - quietFrom_ > QUIET_SAMPLES)
        {
            rxEnd(CALL_RX_LOST);
            rxReset();
            return;
        }
    }
    else
        quietFrom_ = 0U;

    rxBits();
}

/* c is the sample number that is the candidate centre of a bit: the bit sync and frame sync before a header are looked for
   at each of the five positions in turn.  A match starts the header only a few samples later, when the five positions
   around it can be compared and the one nearest the centre of the bits chosen.  The sync is accepted upside down too. */
void DstarMode::rxSearch(uint64_t c)
{
    static const uint32_t SYNC = syncPattern();

    double v = (double)ring_[(c - 1U) & (RING - 1U)] + (double)ring_[c & (RING - 1U)] + (double)ring_[(c + 1U) & (RING - 1U)] - 3.0 * dc_;
    unsigned int ph = (unsigned int)(c % 5U);

    sr_[ph] = ((sr_[ph] << 1) | (v < 0.0 ? 1U : 0U)) & 0xFFFFFFFFULL;

    if (pendingC_ != 0U)
    {
        if (n_ < pendingC_ + 6U)
            return;

        /* the score of each position: the sync bits seen through the three sample sums, signs as they should be */
        double best = -1.0e30;
        uint64_t at = pendingC_;

        for (int d = -2; d <= 2; d++)
        {
            double score = 0.0;
            uint64_t m = pendingC_ + (uint64_t)(int64_t)d;

            for (unsigned int k = 0U; k < 32U; k++)
            {
                uint64_t p = m - 5U * k;
                double s = (double)ring_[(p - 1U) & (RING - 1U)] + (double)ring_[p & (RING - 1U)] + (double)ring_[(p + 1U) & (RING - 1U)] - 3.0 * dc_;
                bool bit = ((SYNC >> k) & 1U) != 0U;

                score += (bit != inv_) ? -s : s;
            }
            if (score > best)
            {
                best = score;
                at = m;
            }
        }

        state_ = RX_HEADER;
        pos_ = (double)at + 5.0;
        prevBit_ = ((SYNC & 1U) != 0U) ? 1 : 0;
        bitCount_ = 0U;
        std::memset(hdr_, 0x00, sizeof(hdr_));
        quietFrom_ = 0U;
        pendingC_ = 0U;
        return;
    }

    if (level_ < SIGNAL_LEVEL)
        return;

    unsigned int straight = popcount64(sr_[ph] ^ SYNC), upside = popcount64(sr_[ph] ^ (~SYNC & 0xFFFFFFFFU));

    /* a signal that is there but not recognised: say so now and then (level, and how near a sync came) */
    if (straight < diagBest_)
        diagBest_ = straight;
    if (upside < diagBest_)
        diagBest_ = upside;
    if (level_ > diagLevel_)
        diagLevel_ = level_;
    if (diagAt_ == 0U)
        diagAt_ = n_ + 48000U;
    else if (n_ >= diagAt_)
    {
        /* only when a header was found and failed its CRC: an idle channel shows a level and near misses all the time */
        if (badHeaders_ != diagBad_)
        {
            char text[200];

            std::snprintf(text, sizeof(text), "D-STAR: a header was heard but its CRC failed (%u so far, signal level %.0f): weak or noisy signal, or the bit clock was off",
                          badHeaders_, diagLevel_);
            log(text);
            diagBad_ = badHeaders_;
        }
        diagAt_ = n_ + 48000U;
        diagBest_ = 32U;
        diagLevel_ = 0.0;
    }

    if (straight > 2U && upside > 2U)
        return;

    inv_ = upside < straight;
    lockLevel_ = level_;
    pendingC_ = c;
}

/* decides the bits that have arrived, follows the bit clock with the zero crossings of the samples */
void DstarMode::rxBits()
{
    for (;;)
    {
        uint64_t i = (uint64_t)(pos_ + 0.5);

        if (i + 2U >= n_)
            return;

        double v = (double)ring_[(i - 1U) & (RING - 1U)] + (double)ring_[i & (RING - 1U)] + (double)ring_[(i + 1U) & (RING - 1U)] - 3.0 * dc_;
        int bit = (v < 0.0) != inv_ ? 1 : 0;
        double corr = 0.0;

        if (prevBit_ >= 0 && bit != prevBit_)
        {
            /* the crossing between the two centres should be half way */
            double mid = pos_ - 2.5, best = 1000.0;
            int64_t from = (int64_t)(pos_ - 5.0 + 0.5);

            for (int64_t k = from; k < (int64_t)i; k++)
            {
                double a = (double)ring_[(uint64_t)k & (RING - 1U)] - dc_;
                double b = (double)ring_[(uint64_t)(k + 1) & (RING - 1U)] - dc_;

                if ((a < 0.0) != (b < 0.0) && a != b)
                {
                    double x = (double)k + a / (a - b);

                    if (std::fabs(x - mid) < std::fabs(best))
                        best = x - mid;
                }
            }
            if (std::fabs(best) < 3.0)
                corr = 0.05 * best;
        }

        prevBit_ = bit;
        pos_ += 5.0 + corr;
        rxBit(bit != 0);

        if (state_ == RX_SEARCH)
            return;
    }
}

void DstarMode::rxBit(bool bit)
{
    if (state_ == RX_HEADER)
    {
        if (bit && bitCount_ >= HEADER_SKIP)
        {
            unsigned int k = bitCount_ - HEADER_SKIP;       /* dstarRxHeader's buffer starts after them */

            hdr_[k >> 3] |= (uint8_t)(1U << (k & 7U));
        }
        bitCount_++;
        if (bitCount_ == HEADER_BITS)
            rxHeader();
        return;
    }

    /* the voice frames: the end pattern is looked for at every bit */
    eotReg_ = ((eotReg_ << 1) | (bit ? 1U : 0U)) & 0xFFFFFFFFFFFFULL;
    if (frameIndex_ > 0U && popcount64(eotReg_ ^ END_PATTERN) <= 1U)
    {
        rxFlushAudio();
        rxEnd(CALL_RX_END);
        rxReset();
        return;
    }

    if (bit)
        frame_[bitCount_ >> 3] |= (uint8_t)(1U << (bitCount_ & 7U));
    bitCount_++;
    if (bitCount_ == FRAME_BITS)
    {
        bitCount_ = 0U;
        rxFrame();
        std::memset(frame_, 0x00, sizeof(frame_));
    }
}

std::string DstarMode::rxText() const
{
    std::string text = rxMy_ + ">" + rxUr_;

    if (rxMessageSeen_ == 0x0FU)
    {
        std::string msg = trimmed(rxMessage_, 20U);

        if (!msg.empty())
            text += "  \"" + msg + "\"";
    }
    return text;
}

void DstarMode::rxStart(const std::string& text)
{
    CallInfo info;

    rxActive_ = true;
    rxInfoKnown_ = true;
    rxErrs_ = rxBits_ = 0UL;
    rxAudio_.clear();

    info.event = CALL_RX_START;
    info.src = info.dst = 0U;
    info.group = true;
    info.ber = 0.0F;
    info.text = text;
    if (ev_.call)
        ev_.call(info);
}

void DstarMode::rxEnd(CallEvent how)
{
    if (!rxActive_)
        return;

    CallInfo info;

    info.event = how;
    info.src = info.dst = 0U;
    info.group = true;
    info.ber = rxBits_ > 0UL ? (float)(100.0 * rxErrs_ / rxBits_) : 0.0F;
    info.text = rxText();

    rxActive_ = false;
    rxInfoKnown_ = false;
    rxErrs_ = rxBits_ = 0UL;
    rxMessageSeen_ = 0U;
    rxBlock_ = -1;
    std::memset(rxMessage_, ' ', 20U);
    rxAudio_.clear();

    if (ev_.call)
        ev_.call(info);
}

void DstarMode::rxHeader()
{
    uint8_t in[83], out[42];

    std::memcpy(in, hdr_, sizeof(in));
    std::memset(out, 0x00, sizeof(out));

    if (!dstarRxHeader(in, out))
    {
        /* a false sync, or a header too damaged to read */
        badHeaders_++;
        rxReset();
        return;
    }

    rxRpt2_ = trimmed((const char*)out + 3, 8U);
    rxRpt1_ = trimmed((const char*)out + 11, 8U);
    rxUr_   = trimmed((const char*)out + 19, 8U);
    rxMy_   = trimmed((const char*)out + 27, 8U);

    std::string suffix = trimmed((const char*)out + 35, 4U);

    if (!suffix.empty())
        rxMy_ += "/" + suffix;

    if (rxActive_)
        rxEnd(CALL_RX_LOST);                    /* a new header: the call before it never ended */

    rxStart(rxMy_ + ">" + rxUr_);

    state_ = RX_DATA;
    bitCount_ = 0U;
    frameIndex_ = 0U;
    syncMisses_ = 0U;
    eotReg_ = 0U;
    std::memset(frame_, 0x00, sizeof(frame_));
}

/* the text message of the slow data: frame 1 of a pair has the block type and two characters, frame 2 the other three */
void DstarMode::rxSlowData(const uint8_t* data, unsigned int index)
{
    uint8_t d[3];

    for (unsigned int k = 0U; k < 3U; k++)
        d[k] = (uint8_t)(data[k] ^ SCRAMBLER[k]);

    if (rxBlock_ >= 0)
    {
        for (unsigned int k = 0U; k < 3U; k++)
            rxMessage_[rxBlock_ * 5 + 2 + (int)k] = (char)d[k];
        rxMessageSeen_ |= 1U << rxBlock_;
        rxBlock_ = -1;

        if (rxMessageSeen_ == 0x0FU)
        {
            CallInfo info;

            info.event = CALL_RX_INFO;
            info.src = info.dst = 0U;
            info.group = true;
            info.ber = 0.0F;
            info.text = rxText();
            if (ev_.call)
                ev_.call(info);
        }
        return;
    }

    if ((index & 1U) == 1U && (d[0] & 0xF0U) == 0x40U && (d[0] & 0x0FU) < 4U)
    {
        unsigned int block = d[0] & 0x0FU;

        rxMessage_[block * 5U + 0U] = (char)d[1];
        rxMessage_[block * 5U + 1U] = (char)d[2];
        rxBlock_ = (int)block;
    }
}

void DstarMode::rxFlushAudio()
{
    unsigned int n = (unsigned int)(rxAudio_.size() / Vocoder::BlockBytes);

    if (n == 0U)
        return;

    if (rxVocoderOk_)
    {
        std::vector<int16_t> pcm((size_t)n * Vocoder::Samples);

        if (!vocoder_.decodeBatch(rxAudio_.data(), n, pcm.data()))
        {
            rxVocoderOk_ = false;
            log("the vocoder stopped answering: received speech is no longer decoded");
        }
        else if (ev_.rxAudio)
            ev_.rxAudio(pcm.data(), pcm.size());
    }
    rxAudio_.clear();
}

/* One frame of 12 bytes: nine bytes of voice (an AMBE block as the chip makes it) and three of slow data */
void DstarMode::rxFrame()
{
    unsigned int pos = frameIndex_ % 21U;

    if (pos == 0U)
    {
        /* the frame sync: its absence a few times in a row means the signal is not a D-STAR call any more */
        unsigned int wrong = 0U;

        for (unsigned int k = 0U; k < 3U; k++)
            wrong += popcount64((uint64_t)(frame_[9U + k] ^ DATA_SYNC[k]));
        if (wrong > 6U)
        {
            if (++syncMisses_ >= 3U)
            {
                rxFlushAudio();
                rxEnd(CALL_RX_LOST);
                rxReset();
                return;
            }
        }
        else
            syncMisses_ = 0U;
    }
    else
        rxSlowData(frame_ + 9U, pos);

    uint8_t ambe[Vocoder::BlockBytes];

    std::memcpy(ambe, frame_, sizeof(ambe));
    rxErrs_ += dstarFecErrors(ambe, true);          /* the errors it finds are corrected before the chip gets the block */
    rxBits_ += DSTAR_FEC_BITS_PER_FRAME;
    rxAudio_.insert(rxAudio_.end(), ambe, ambe + sizeof(ambe));

    frameIndex_++;

    if (rxAudio_.size() >= 5U * Vocoder::BlockBytes)
        rxFlushAudio();
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Transmit                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------ */

void DstarMode::pttDown()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        mic_.clear();
        ptt_ = true;
    }
    txCv_.notify_all();
}

void DstarMode::pttUp()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        ptt_ = false;
    }
    txCv_.notify_all();
}

void DstarMode::micAudio(const int16_t* pcm, size_t count)
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

/* Modulates bytes, least significant bit first: a 1 is the low level.  Each symbol gives five samples through the pulse
   shape of three symbols. */
void DstarMode::txBytes(const uint8_t* bytes, size_t count)
{
    int16_t out[5U * 8U * 16U];
    size_t pos = 0U;
    const double level = LEVEL_SYMBOL * (cfg_.txLevel > 0.0F ? cfg_.txLevel : 1.0F);

    for (size_t i = 0U; i < count; i++)
    {
        for (unsigned int k = 0U; k < 8U; k++)
        {
            int x = ((bytes[i] >> k) & 1U) ? (int)(-level) : (int)level;

            txHist_[2] = txHist_[1];
            txHist_[1] = txHist_[0];
            txHist_[0] = x;

            for (unsigned int p = 0U; p < 5U; p++)
            {
                int64_t y = 0;

                for (unsigned int j = 0U; j < 3U; j++)
                    y += (int64_t)PULSE[14U - (p + 5U * j)] * txHist_[j];
                out[pos++] = (int16_t)(y >> 15);
            }
        }

        if (pos >= sizeof(out) / sizeof(out[0]) - 40U || i + 1U == count)
        {
            link_.txSamples(out, pos);
            pos = 0U;
        }
    }
}

/* Lets the last symbols leave the pulse shape */
void DstarMode::txFlush()
{
    int16_t out[5U * 3U];
    size_t pos = 0U;

    for (unsigned int s = 0U; s < 3U; s++)
    {
        txHist_[2] = txHist_[1];
        txHist_[1] = txHist_[0];
        txHist_[0] = 0;

        for (unsigned int p = 0U; p < 5U; p++)
        {
            int64_t y = 0;

            for (unsigned int j = 0U; j < 3U; j++)
                y += (int64_t)PULSE[14U - (p + 5U * j)] * txHist_[j];
            out[pos++] = (int16_t)(y >> 15);
        }
    }
    link_.txSamples(out, pos);
}

/* The header: flags, RPT2, RPT1, UR, MY, suffix and the CRC, coded, behind the frame sync */
void DstarMode::txHeader()
{
    uint8_t header[42], coded[83], stream[2U + 83U];

    std::memset(header, 0x00, sizeof(header));
    std::memcpy(header + 3, rpt2_, 8U);
    std::memcpy(header + 11, rpt1_, 8U);
    std::memcpy(header + 19, ur_, 8U);
    std::memcpy(header + 27, my_, 8U);
    std::memcpy(header + 35, suffix_, 4U);
    dstarHeaderCrc(header);

    std::memset(coded, 0x00, sizeof(coded));
    dstarTxHeader(header, coded);

    stream[0] = FRAME_SYNC[0];
    stream[1] = FRAME_SYNC[1];
    std::memcpy(stream + 2, coded, sizeof(coded));
    txBytes(stream, sizeof(stream));
}

/* One frame: the voice block and its three bytes of slow data (the frame sync in the first of every 21) */
void DstarMode::txFrame(unsigned int index, const uint8_t* ambe)
{
    uint8_t frame[12];
    unsigned int pos = index % 21U;

    std::memcpy(frame, ambe, 9U);
    if (pos == 0U)
        std::memcpy(frame + 9, DATA_SYNC, 3U);
    else
        std::memcpy(frame + 9, slow_ + (pos - 1U) * 3U, 3U);
    txBytes(frame, sizeof(frame));
}

void DstarMode::txThread()
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

void DstarMode::txCall()
{
    int16_t pcm[5U * Vocoder::Samples];
    uint8_t chips[5U * Vocoder::BlockBytes];
    const size_t need = sizeof(pcm) / sizeof(pcm[0]);
    unsigned int index = 0U, frames = 0U;
    bool ok = true;

    {
        /* a call being received is over for us */
        std::lock_guard<std::mutex> lock(rxMutex_);
        rxFlushAudio();
        rxEnd(CALL_RX_LOST);
        rxReset();
    }

    /* the slow data of the frames 1 to 20: the text message in four blocks of five characters (each behind a type byte),
       then the filler 0x66, all scrambled */
    {
        uint8_t raw[60];

        std::memset(raw, 0x66, sizeof(raw));
        if (!cfg_.message.empty())
        {
            char text[21];

            padCall(text, cfg_.message, 20U);
            for (unsigned int b = 0U; b < 4U; b++)
            {
                raw[b * 6U] = (uint8_t)(0x40U | b);
                std::memcpy(raw + b * 6U + 1U, text + b * 5U, 5U);
            }
        }
        for (unsigned int k = 0U; k < sizeof(raw); k++)
            slow_[k] = (uint8_t)(raw[k] ^ SCRAMBLER[k % 3U]);
    }

    txActive_ = true;

    CallInfo info;
    info.event = CALL_TX_START;
    info.src = info.dst = 0U;
    info.group = true;
    info.ber = 0.0F;
    info.text = trimmed(my_, 8U) + ">" + trimmed(ur_, 8U);
    if (ev_.call)
        ev_.call(info);

    link_.requestMode();
    link_.txBegin();
    std::memset(txHist_, 0x00, sizeof(txHist_));

    /* the bit sync lets the receivers settle */
    {
        uint8_t sync[48];
        unsigned int bytes = (cfg_.txDelayMs * 6U) / 10U;       /* 600 bytes a second */

        std::memset(sync, 0xAA, sizeof(sync));
        while (bytes > 0U)
        {
            unsigned int chunk = bytes > sizeof(sync) ? (unsigned int)sizeof(sync) : bytes;

            txBytes(sync, chunk);
            bytes -= chunk;
        }
    }

    txHeader();

    /* voice: five frames (100 ms) at a time so that the chip is asked once for all of them */
    while (ok && running_)
    {
        size_t got = 0U;
        bool last = false;

        {
            std::unique_lock<std::mutex> lock(txMutex_);

            /* wait for five blocks; stop when the key is released and nothing is left; a stalled microphone is padded */
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

        if (!vocoder_.encodeBatch(pcm, 5U, chips))
        {
            log("the vocoder stopped answering: ending the transmission");
            ok = false;
            break;
        }

        for (unsigned int b = 0U; b < 5U; b++, index++)
            txFrame(index, chips + b * Vocoder::BlockBytes);
        frames += 5U;

        if (!link_.isOpen())
            ok = false;
        if (last && got < need)
            break;
    }

    /* the end pattern, then the filter tail and the end of transmission */
    if (link_.isOpen())
    {
        for (unsigned int k = 0U; k < 3U; k++)
            txBytes(END_SYNC, sizeof(END_SYNC));
        txFlush();
        link_.txEnd();
    }

    if (!ok)
    {
        /* the call could not go on (vocoder or link failed): do not start another one by itself */
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
