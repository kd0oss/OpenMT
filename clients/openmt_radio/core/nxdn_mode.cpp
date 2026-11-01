#include "nxdn_mode.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace openmt
{

static uint64_t nowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static const double TWO_PI = 6.28318530717958647692;

/* ------------------------------------------------------------------------------------------------------------ */
/* Frames: the same layout nxdnGenVoiceCall() makes, built a frame at a time for a live call                    */
/* ------------------------------------------------------------------------------------------------------------ */

/* The 22 byte layer 3 message of a call: type, no encryption, call type, source and destination ids */
static void l3Call(uint8_t* l3, uint8_t type, uint16_t src, uint16_t dst, bool group)
{
    std::memset(l3, 0x00, 22U);

    l3[0] = type;
    l3[1] = 0x00U;
    l3[2] = group ? 0x20U : 0x40U;
    l3[3] = (uint8_t)((src >> 8) & 0xFFU);
    l3[4] = (uint8_t)(src & 0xFFU);
    l3[5] = (uint8_t)((dst >> 8) & 0xFFU);
    l3[6] = (uint8_t)(dst & 0xFFU);
}

/* A frame that carries a layer 3 message in both FACCH1 slots (call header and trailer) */
static void buildFacchFrame(uint8_t ran, const uint8_t* l3, uint8_t* frame)
{
    uint8_t sacch[5];
    const uint8_t idle[3] = { NXDN_MESSAGE_TYPE_IDLE, 0x00U, 0x00U };

    std::memset(frame, 0x00, NXDN_FRAME_LENGTH_BYTES);
    nxdnAddSync(frame);
    nxdnLichEncode(nxdnLichMake(NXDN_LICH_RFCT_RDCH, NXDN_LICH_USC_SACCH_NS, NXDN_LICH_STEAL_FACCH,
                                NXDN_LICH_DIRECTION_INBOUND), frame);

    std::memset(sacch, 0x00, sizeof(sacch));
    nxdnSacchSetRAN(sacch, ran);
    nxdnSacchSetStructure(sacch, NXDN_SR_SINGLE);
    nxdnSacchSetData(sacch, idle);
    nxdnSacchEncode(sacch, frame);

    nxdnFacch1Encode(l3, frame, NXDN_FACCH1_OFFSET_1);
    nxdnFacch1Encode(l3, frame, NXDN_FACCH1_OFFSET_2);

    nxdnScramble(frame);
}

/* A voice frame: this part (0 - 3) of the call information in the SACCH superblock and four voice blocks */
static void buildVoiceFrame(uint8_t ran, const uint8_t* l3, unsigned int part, const uint8_t* net28, uint8_t* frame)
{
    static const uint8_t order[4] = { NXDN_SR_1_4, NXDN_SR_2_4, NXDN_SR_3_4, NXDN_SR_4_4 };
    uint8_t sacch[5];
    uint8_t chunk[3];

    std::memset(frame, 0x00, NXDN_FRAME_LENGTH_BYTES);
    nxdnAddSync(frame);
    nxdnLichEncode(nxdnLichMake(NXDN_LICH_RFCT_RDCH, NXDN_LICH_USC_SACCH_SS, NXDN_LICH_STEAL_NONE,
                                NXDN_LICH_DIRECTION_INBOUND), frame);

    std::memset(chunk, 0x00, sizeof(chunk));
    for (unsigned int b = 0U; b < 18U; b++)
    {
        unsigned int bit = part * 18U + b;

        if (l3[bit >> 3] & (0x80U >> (bit & 7U)))
            chunk[b >> 3] |= (uint8_t)(0x80U >> (b & 7U));
    }

    std::memset(sacch, 0x00, sizeof(sacch));
    nxdnSacchSetRAN(sacch, ran);
    nxdnSacchSetStructure(sacch, order[part & 3U]);
    nxdnSacchSetData(sacch, chunk);
    nxdnSacchEncode(sacch, frame);

    nxdnAudioEncode(net28, frame + NXDN_FSW_LICH_SACCH_LENGTH_BYTES + 0U);
    nxdnAudioEncode(net28 + 14U, frame + NXDN_FSW_LICH_SACCH_LENGTH_BYTES + 18U);

    nxdnScramble(frame);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Chip layout detection                                                                                        */
/* ------------------------------------------------------------------------------------------------------------ */

unsigned int nxdnDetectLayout(Vocoder& vocoder, std::string* report)
{
    unsigned long err[3] = { 0UL, 0UL, 0UL };
    unsigned int frames = 0U;
    double ph = 0.0;
    uint32_t rnd = 12345U;

    for (unsigned int f = 0U; f < 60U; f++)
    {
        int16_t pcm[Vocoder::Samples];
        uint8_t chip[Vocoder::BlockBytes], net[7];
        double amp = 600.0 + 150.0 * (f % 20U), freq = 250.0 + 40.0 * f;

        for (unsigned int k = 0U; k < Vocoder::Samples; k++)
        {
            rnd = rnd * 1664525U + 1013904223U;
            ph += TWO_PI * freq / 8000.0;
            pcm[k] = (int16_t)std::lrint(amp * std::sin(ph) + ((int)(rnd >> 24) - 128) * 3.0);
        }

        if (!vocoder.encode(pcm, chip))
            break;

        for (unsigned int l = 0U; l < 3U; l++)
            err[l] += nxdnAudioChipDecode(l, chip, net);
        frames++;
    }

    if (frames == 0U)
    {
        if (report)
            *report = "layout detection: the chip did not answer, assuming no parity";
        return NXDN_CHIP_NOFEC;
    }

    /* a layout is right if its FEC words are (nearly) always valid; with no parity, if the padding stays zero */
    unsigned int best = NXDN_CHIP_AIR;

    if (err[NXDN_CHIP_CODING] < err[NXDN_CHIP_AIR])
        best = NXDN_CHIP_CODING;
    if (err[best] * 4UL > frames && err[NXDN_CHIP_NOFEC] == 0UL)
        best = NXDN_CHIP_NOFEC;

    if (report)
    {
        char text[200];

        std::snprintf(text, sizeof(text), "AMBE block layout detected from %u test frames: %s (corrected bits per frame: "
                      "air %.2f, coding order %.2f; no-parity padding bits set %.2f)", frames,
                      best == NXDN_CHIP_AIR ? "air" : (best == NXDN_CHIP_CODING ? "coding order" : "no parity"),
                      (double)err[NXDN_CHIP_AIR] / frames, (double)err[NXDN_CHIP_CODING] / frames,
                      (double)err[NXDN_CHIP_NOFEC] / frames);
        *report = text;
    }
    return best;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* NxdnMode                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------ */

NxdnMode::NxdnMode(ModemLink& link, Vocoder& vocoder, const NxdnConfig& cfg, const RadioEvents& events) :
    link_(link),
    vocoder_(vocoder),
    cfg_(cfg),
    ev_(events),
    layout_(cfg.layout >= 0 ? (unsigned int)cfg.layout : (unsigned int)NXDN_CHIP_NOFEC),
    running_(false),
    thread_(),
    rx_(new NxdnRx()),
    rxMutex_(),
    rxActive_(false),
    rxSrc_(0U),
    rxDst_(0U),
    rxGroup_(true),
    rxParts_(0U),
    rxErrs_(0UL),
    rxBits_(0UL),
    rxVocoderOk_(true),
    tx_(),
    txMutex_(),
    txCv_(),
    ring_(),
    ptt_(false),
    txActive_(false),
    hangUntil_(0U)
{
    std::memset(rxL3_, 0x00, sizeof(rxL3_));
    nxdnRxInit(rx_.get(), &NxdnMode::frameCallback, this, (uint8_t)cfg_.rxShape);
}

NxdnMode::~NxdnMode()
{
    stop();
}

std::string NxdnMode::modeName() const
{
    return "NXDN";
}

std::string NxdnMode::modemType() const
{
    return "4FSK";
}

void NxdnMode::log(const std::string& text) const
{
    if (ev_.log)
        ev_.log(text);
}

void NxdnMode::start()
{
    if (running_)
        return;

    if (cfg_.layout < 0)
    {
        std::string report;

        layout_ = nxdnDetectLayout(vocoder_, &report);
        log(report);
    }

    running_ = true;
    thread_ = std::thread(&NxdnMode::txThread, this);
}

void NxdnMode::stop()
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

bool NxdnMode::transmitting() const
{
    return txActive_;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Receive                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

void NxdnMode::rxSamples(const int16_t* samples, size_t count)
{
    int16_t scaled[1024];

    if (txActive_ || !running_)
        return;                 /* half duplex: the receiver is deaf while transmitting */

    if (count > 1024U)
        count = 1024U;

    for (size_t i = 0U; i < count; i++)
    {
        float v = samples[i] * cfg_.rxGain;

        if (v > 32767.0F)
            v = 32767.0F;
        if (v < -32768.0F)
            v = -32768.0F;
        scaled[i] = (int16_t)v;
    }

    std::lock_guard<std::mutex> lock(rxMutex_);
    nxdnRxSamples(rx_.get(), scaled, (unsigned int)count);
}

void NxdnMode::frameCallback(void* ctx, const uint8_t* frame, bool lost)
{
    static_cast<NxdnMode*>(ctx)->onFrame(frame, lost);
}

void NxdnMode::rxEnd(CallEvent how)
{
    if (!rxActive_)
        return;

    CallInfo info;

    info.event = how;
    info.src   = rxSrc_;
    info.dst   = rxDst_;
    info.group = rxGroup_;
    info.ber   = rxBits_ > 0UL ? (float)(100.0 * rxErrs_ / rxBits_) : 0.0F;

    rxActive_ = false;
    rxSrc_ = rxDst_ = 0U;
    rxParts_ = 0U;
    rxErrs_ = rxBits_ = 0UL;

    if (ev_.call)
        ev_.call(info);
}

/* The call information is spread over four SACCH parts of 18 bits; when all four are in, the ids are known */
void NxdnMode::rxSacch(const uint8_t* frame)
{
    uint8_t sacch[5], data[3];

    if (!nxdnSacchDecode(frame, sacch))
        return;

    unsigned int structure = NXDN_SACCH_STRUCTURE(sacch);
    unsigned int part = structure == NXDN_SR_1_4 ? 0U : (structure == NXDN_SR_2_4 ? 1U : (structure == NXDN_SR_3_4 ? 2U : 3U));

    nxdnSacchGetData(sacch, data);

    for (unsigned int b = 0U; b < 18U; b++)
    {
        unsigned int bit = part * 18U + b;
        uint8_t mask = (uint8_t)(0x80U >> (bit & 7U));

        if (data[b >> 3] & (0x80U >> (b & 7U)))
            rxL3_[bit >> 3] |= mask;
        else
            rxL3_[bit >> 3] &= (uint8_t)~mask;
    }
    rxParts_ |= (uint8_t)(1U << part);

    if (rxParts_ == 0x0FU)
    {
        rxParts_ = 0U;

        unsigned int type = NXDN_L3_TYPE(rxL3_);

        if (type == NXDN_MESSAGE_TYPE_VCALL || type == NXDN_MESSAGE_TYPE_VCALL_IV)
        {
            unsigned int src = NXDN_L3_SRC(rxL3_), dst = NXDN_L3_DST(rxL3_);
            bool group = NXDN_L3_GROUP(rxL3_);

            if (src != rxSrc_ || dst != rxDst_)
            {
                rxSrc_ = src;
                rxDst_ = dst;
                rxGroup_ = group;

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
    }
}

/* Four voice blocks (two groups of 14 bytes): FEC corrected 49 bit blocks go to the chip as the chip arranges them */
void NxdnMode::rxVoice(const uint8_t* net28)
{
    if (!rxVocoderOk_)
        return;

    uint8_t blocks[4U * Vocoder::BlockBytes];
    int16_t pcm[4U * Vocoder::Samples];

    for (unsigned int b = 0U; b < 4U; b++)
    {
        uint8_t bits[7];
        const uint8_t* grp = net28 + (b >> 1) * 14U;
        unsigned int off = (b & 1U) * 49U;

        std::memset(bits, 0x00, sizeof(bits));
        for (unsigned int i = 0U; i < 49U; i++)
            if (grp[(off + i) >> 3] & (0x80U >> ((off + i) & 7U)))
                bits[i >> 3] |= (uint8_t)(0x80U >> (i & 7U));

        nxdnAudioChipEncode(layout_, bits, blocks + b * Vocoder::BlockBytes);
    }

    /* all four go to the chip before the answers are read */
    if (!vocoder_.decodeBatch(blocks, 4U, pcm))
    {
        rxVocoderOk_ = false;
        log("the vocoder stopped answering: received speech is no longer decoded");
        return;
    }

    if (ev_.rxAudio)
        ev_.rxAudio(pcm, 4U * Vocoder::Samples);
}

void NxdnMode::onFrame(const uint8_t* raw, bool lost)
{
    uint8_t frame[48];
    uint8_t lich = 0U, facch[12], l3[22];

    if (lost)
    {
        rxEnd(CALL_RX_LOST);
        return;
    }

    std::memcpy(frame, raw, sizeof(frame));
    nxdnScramble(frame);                    /* frames from the receiver are as on the air */

    if (!nxdnLichDecode(frame, &lich))
        return;                             /* trailing frames after the transmitter has dropped */

    unsigned int usc = NXDN_LICH_FCT(lich);

    if (usc == NXDN_LICH_USC_SACCH_SS && NXDN_LICH_OPTION(lich) == NXDN_LICH_STEAL_NONE)
    {
        const uint8_t* aud = frame + NXDN_FSW_LICH_SACCH_LENGTH_BYTES;
        uint8_t net[28];
        unsigned int errors = 0U;

        if (!rxActive_)
        {
            /* a call entered late: no header was heard */
            rxActive_ = true;
            rxSrc_ = rxDst_ = 0U;
            rxParts_ = 0U;
            rxErrs_ = rxBits_ = 0UL;

            CallInfo info;
            info.event = CALL_RX_START;
            info.src = info.dst = 0U;
            info.group = true;
            info.ber = 0.0F;
            if (ev_.call)
                ev_.call(info);
        }

        std::memset(net, 0x00, sizeof(net));
        nxdnAudioDecode(aud + 0U, net + 0U, &errors);
        nxdnAudioDecode(aud + 18U, net + 14U, &errors);

        rxErrs_ += errors;
        rxBits_ += 188UL;

        rxSacch(frame);
        rxVoice(net);
        return;
    }

    if (usc != NXDN_LICH_USC_UDCH && nxdnFacch1Decode(frame, NXDN_FACCH1_OFFSET_1, facch))
    {
        nxdnL3Decode(l3, facch, 80U, 0U);

        unsigned int type = NXDN_L3_TYPE(l3);

        if (type == NXDN_MESSAGE_TYPE_VCALL || type == NXDN_MESSAGE_TYPE_VCALL_IV)
        {
            if (!rxActive_)
            {
                rxActive_ = true;
                rxParts_ = 0U;
                rxErrs_ = rxBits_ = 0UL;
                rxSrc_ = NXDN_L3_SRC(l3);
                rxDst_ = NXDN_L3_DST(l3);
                rxGroup_ = NXDN_L3_GROUP(l3);

                CallInfo info;
                info.event = CALL_RX_START;
                info.src = rxSrc_;
                info.dst = rxDst_;
                info.group = rxGroup_;
                info.ber = 0.0F;
                if (ev_.call)
                    ev_.call(info);
            }
        }
        else if (type == NXDN_MESSAGE_TYPE_TX_REL)
        {
            rxEnd(CALL_RX_END);
        }
    }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Transmit                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------ */

void NxdnMode::pttDown()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        ring_.clear();
        ptt_ = true;
    }
    txCv_.notify_all();
}

void NxdnMode::pttUp()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        ptt_ = false;
    }
    txCv_.notify_all();
}

void NxdnMode::micAudio(const int16_t* pcm, size_t count)
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
            ring_.push_back((int16_t)v);
        }

        while (ring_.size() > 8000U)            /* never more than 1 s behind: a slow dongle drops speech, it does not build up delay */
            ring_.pop_front();
    }
    txCv_.notify_all();
}

/* Modulates one frame and hands the samples to the link (which keeps the transmission in real time) */
bool NxdnMode::txFrame(const uint8_t* frame)
{
    int16_t buf[NXDN_SAMPLES_PER_FRAME];
    unsigned int n = nxdnTxFrame(&tx_, frame, buf);

    link_.txSamples(buf, n);
    return link_.isOpen();
}

bool NxdnMode::txVoiceFrame(const int16_t* pcm, unsigned int part, const uint8_t* l3)
{
    uint8_t net[28];
    uint8_t frame[NXDN_FRAME_LENGTH_BYTES];

    std::memset(net, 0x00, sizeof(net));

    uint8_t chips[4U * Vocoder::BlockBytes];

    /* all four frames go to the chip before the answers are read */
    if (!vocoder_.encodeBatch(pcm, 4U, chips))
    {
        log("the vocoder stopped answering: ending the transmission");
        return false;
    }

    for (unsigned int b = 0U; b < 4U; b++)
    {
        uint8_t bits[7];
        unsigned int grp = b >> 1, off = (b & 1U) * 49U;

        std::memset(bits, 0x00, sizeof(bits));
        nxdnAudioChipDecode(layout_, chips + b * Vocoder::BlockBytes, bits);       /* the 49 parameter bits; the frame builder adds the NXDN FEC */

        for (unsigned int i = 0U; i < 49U; i++)
            if (bits[i >> 3] & (0x80U >> (i & 7U)))
                net[grp * 14U + ((off + i) >> 3)] |= (uint8_t)(0x80U >> ((off + i) & 7U));
    }

    buildVoiceFrame((uint8_t)cfg_.ran, l3, part, net, frame);
    return txFrame(frame);
}

void NxdnMode::txThread()
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

void NxdnMode::txCall()
{
    uint8_t l3[22], frame[NXDN_FRAME_LENGTH_BYTES];
    int16_t buf[NXDN_SAMPLES_PER_SYMBOL * 48U];
    int16_t pcm[4U * Vocoder::Samples];
    const size_t need = sizeof(pcm) / sizeof(pcm[0]);
    unsigned int part = 0U, frames = 0U;
    bool ok = true;

    {
        /* a call being received is over for us */
        std::lock_guard<std::mutex> lock(rxMutex_);
        rxEnd(CALL_RX_LOST);
        nxdnRxReset(rx_.get());
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
    nxdnTxInit(&tx_, NXDN_LEVEL_OUTER, (uint8_t)cfg_.txShape);

    /* the preamble lets the receivers' filters settle */
    unsigned int symbols = ((unsigned int)cfg_.txDelayMs * 24U) / 10U;
    symbols &= ~1U;
    while (symbols > 0U)
    {
        unsigned int chunk = symbols > 48U ? 48U : symbols;
        unsigned int n = nxdnTxPreamble(&tx_, chunk, buf);

        link_.txSamples(buf, n);
        symbols -= chunk;
    }

    /* header: VCALL in both FACCH1 slots */
    l3Call(l3, NXDN_MESSAGE_TYPE_VCALL, (uint16_t)cfg_.srcId, (uint16_t)cfg_.dstId, cfg_.group);
    buildFacchFrame((uint8_t)cfg_.ran, l3, frame);
    ok = txFrame(frame);

    /* voice: one frame per 80 ms of speech from the microphone */
    while (ok && running_)
    {
        size_t got = 0U;
        bool last = false;

        {
            std::unique_lock<std::mutex> lock(txMutex_);

            /* wait for four blocks; stop when the key is released and nothing is left; a stalled microphone is padded */
            while (running_ && ring_.size() < need && ptt_)
            {
                if (txCv_.wait_for(lock, std::chrono::milliseconds(200)) == std::cv_status::timeout)
                    break;
            }

            got = ring_.size() < need ? ring_.size() : need;
            for (size_t i = 0U; i < got; i++)
            {
                pcm[i] = ring_.front();
                ring_.pop_front();
            }
            last = !ptt_;
        }

        if (got == 0U && last)
            break;

        for (size_t i = got; i < need; i++)
            pcm[i] = 0;

        ok = txVoiceFrame(pcm, part, l3);
        part = (part + 1U) & 3U;
        frames++;

        if (last && got < need)
            break;
    }

    /* trailer: TX_REL in both FACCH1 slots, then the filter tail and the end of transmission */
    l3Call(l3, NXDN_MESSAGE_TYPE_TX_REL, (uint16_t)cfg_.srcId, (uint16_t)cfg_.dstId, cfg_.group);
    buildFacchFrame((uint8_t)cfg_.ran, l3, frame);
    if (link_.isOpen())
    {
        txFrame(frame);

        int16_t tail[NXDN_SAMPLES_PER_SYMBOL * (NXDN_FILTER_SPAN + 1U)];
        unsigned int n = nxdnTxFlush(&tx_, tail);

        link_.txSamples(tail, n);
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
