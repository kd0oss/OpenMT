#include "ysf_mode.h"
#include "nxdn_mode.h"       /* nxdnDetectLayout() */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include <fec_tools.h>
}

namespace openmt
{

static uint64_t nowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static const unsigned int FL = YSF_FRAME_LENGTH_BYTES;
static const uint8_t WHITE[13] = { 0x93U, 0xD7U, 0x51U, 0x21U, 0x9CU, 0x2FU, 0x6CU, 0xD0U, 0xEFU, 0x0FU, 0xF8U, 0x3DU, 0xF1U };

/* ------------------------------------------------------------------------------------------------------------ */
/* Frames: the layout ysfGenVoiceCallAudio() makes, built a frame at a time for a live call                     */
/* ------------------------------------------------------------------------------------------------------------ */

/* a callsign in a 10 byte field, padded with spaces */
static void padCall(uint8_t* d, const char* s)
{
    size_t n = std::strlen(s);

    std::memset(d, ' ', YSF_CALLSIGN_LENGTH);
    std::memcpy(d, s, n > YSF_CALLSIGN_LENGTH ? YSF_CALLSIGN_LENGTH : n);
}

/* the callsign type field of the FICH (2: callsign), as MMDVMHost sets it for a call from a radio */
static void ficSetCS(YsfFich* f, uint8_t cs)
{
    f->b[0] = (uint8_t)((f->b[0] & 0xCFU) | ((cs & 0x03U) << 4));
}

/* A header or terminator frame with the destination and source in the first half (CSD1) and blank downlink and
   uplink callsigns in the second (CSD2) */
static void buildHeader(uint8_t* f, uint8_t fi, uint8_t dgid, uint8_t cm, const char* dest, const char* src)
{
    uint8_t csd1[20], csd2[20];
    YsfFich h;

    std::memset(f, 0x00, FL);
    ysfAddSync(f);
    ysfFichInit(&h);
    ysfFichSetFI(&h, fi);
    ficSetCS(&h, 2U);
    ysfFichSetCM(&h, cm);
    ysfFichSetFT(&h, 7U);
    ysfFichSetDT(&h, YSF_DT_VD_MODE2);
    ysfFichSetDGId(&h, dgid);
    ysfFichEncode(&h, f);

    padCall(csd1, dest);
    padCall(csd1 + 10, src);
    padCall(csd2, "");
    padCall(csd2 + 10, "");
    ysfPayloadWriteHeader(f, csd1, csd2);
}

/* The short data channel of a V/D mode 2 frame: ten bytes, whitened, CRC, rate 1/2 code, interleaved */
static void buildShort(uint8_t* frame, const uint8_t* ten)
{
    uint8_t out[13], conv[25], bytes[25];

    std::memset(out, 0x00, sizeof(out));
    std::memset(bytes, 0x00, sizeof(bytes));
    for (unsigned int i = 0U; i < 10U; i++)
        out[i] = (uint8_t)(ten[i] ^ WHITE[i]);
    CRC_addCCITT162(out, 12);
    out[12] = 0;
    ysfConvEncode(out, conv, 100);

    unsigned int j = 0U;
    for (unsigned int i = 0U; i < 100U; i++)
    {
        unsigned int n = 2U * (i / 5U) + 40U * (i % 5U);
        int s0 = (conv[j >> 3] >> (7 - (j & 7))) & 1;
        j++;
        int s1 = (conv[j >> 3] >> (7 - (j & 7))) & 1;
        j++;

        if (s0)
            bytes[n >> 3] |= (uint8_t)(0x80 >> (n & 7));
        n++;
        if (s1)
            bytes[n >> 3] |= (uint8_t)(0x80 >> (n & 7));
    }
    for (unsigned int i = 0U; i < 5U; i++)
        std::memcpy(frame + 30 + i * 18, bytes + i * 5, 5);
}

/* A V/D mode 2 voice frame: the FICH, the short data channel (destination in frame 0, source in frame 1, spaces
   otherwise) and five voice channels from blocks: five blocks of 49 bits, 7 bytes apart.  The first 27 bits of a block
   are sent three times, the other 22 once. */
static void buildVoice(uint8_t* f, uint8_t fn, uint8_t dgid, uint8_t cm, const char* dest, const char* src, const uint8_t* blocks)
{
    YsfFich h;
    uint8_t ten[10];

    std::memset(f, 0x00, FL);
    ysfAddSync(f);
    ysfFichInit(&h);
    ysfFichSetFI(&h, YSF_FI_COMMUNICATIONS);
    ficSetCS(&h, 2U);
    ysfFichSetCM(&h, cm);
    ysfFichSetFN(&h, fn);
    ysfFichSetFT(&h, 6U);
    ysfFichSetDT(&h, YSF_DT_VD_MODE2);
    ysfFichSetDGId(&h, dgid);
    ysfFichEncode(&h, f);

    std::memset(ten, ' ', sizeof(ten));
    if (fn == 0U)
        padCall(ten, dest);
    else if (fn == 1U)
        padCall(ten, src);
    buildShort(f, ten);

    for (unsigned int j = 0U; j < 5U; j++)
    {
        unsigned int offset = 40U + 144U * j;
        const uint8_t* u = blocks + 7U * j;
        uint8_t vch[13];

        std::memset(vch, 0x00, sizeof(vch));
        for (unsigned int i = 0U; i < 27U; i++)
            if ((u[i >> 3] >> (7 - (i & 7))) & 1)
                for (unsigned int k = 0U; k < 3U; k++)
                    vch[(3U * i + k) >> 3] |= (uint8_t)(0x80 >> ((3U * i + k) & 7U));
        for (unsigned int i = 0U; i < 22U; i++)
            if ((u[(27U + i) >> 3] >> (7 - ((27U + i) & 7U))) & 1)
                vch[(81U + i) >> 3] |= (uint8_t)(0x80 >> ((81U + i) & 7U));

        for (unsigned int i = 0U; i < 13U; i++)
            vch[i] ^= WHITE[i];

        for (unsigned int i = 0U; i < 104U; i++)
        {
            unsigned int n = i / 26U + 4U * (i % 26U);
            unsigned int p = offset + n;
            int s = (vch[i >> 3] >> (7 - (i & 7))) & 1;

            if (s)
                f[30 + (p >> 3)] |= (uint8_t)(0x80 >> (p & 7));
            else
                f[30 + (p >> 3)] &= (uint8_t)~(0x80 >> (p & 7));
        }
    }
}

/* The five 49 bit blocks out of a V/D mode 2 frame: the voice channels are de-interleaved and de-whitened and each
   bit sent three times is voted.  Returns the number of triplets that were not all equal (out of 135). */
static unsigned int extractVoice(const uint8_t* f, uint8_t* blocks)
{
    unsigned int errors = 0U;

    std::memset(blocks, 0x00, 35U);

    for (unsigned int j = 0U; j < 5U; j++)
    {
        unsigned int offset = 40U + 144U * j;
        uint8_t vch[13];
        uint8_t* u = blocks + 7U * j;

        std::memset(vch, 0x00, sizeof(vch));
        for (unsigned int i = 0U; i < 104U; i++)
        {
            unsigned int p = offset + i / 26U + 4U * (i % 26U);

            if ((f[30 + (p >> 3)] >> (7 - (p & 7))) & 1)
                vch[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        }
        for (unsigned int i = 0U; i < 13U; i++)
            vch[i] ^= WHITE[i];

        for (unsigned int i = 0U; i < 27U; i++)
        {
            unsigned int votes = 0U;

            for (unsigned int k = 0U; k < 3U; k++)
            {
                unsigned int b = 3U * i + k;

                votes += (vch[b >> 3] >> (7 - (b & 7))) & 1U;
            }
            if (votes == 1U || votes == 2U)
                errors++;
            if (votes >= 2U)
                u[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        }
        for (unsigned int i = 0U; i < 22U; i++)
        {
            unsigned int b = 81U + i;

            if ((vch[b >> 3] >> (7 - (b & 7))) & 1)
                u[(27U + i) >> 3] |= (uint8_t)(0x80 >> ((27U + i) & 7U));
        }
    }
    return errors;
}

static std::string trimmed(const uint8_t* field)
{
    size_t n = YSF_CALLSIGN_LENGTH;

    while (n > 0U && (field[n - 1U] == ' ' || field[n - 1U] == 0x00U))
        n--;
    return std::string((const char*)field, n);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* YsfMode                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

YsfMode::YsfMode(ModemLink& link, Vocoder& vocoder, const YsfConfig& cfg, const RadioEvents& events) :
    link_(link),
    vocoder_(vocoder),
    cfg_(cfg),
    ev_(events),
    layout_(cfg.layout >= 0 ? (unsigned int)cfg.layout : (unsigned int)NXDN_CHIP_NOFEC),
    running_(false),
    thread_(),
    rx_(new YsfRx()),
    rxMutex_(),
    rxActive_(false),
    rxInfoKnown_(false),
    rxPay_(),
    rxErrs_(0UL),
    rxBits_(0UL),
    rxVocoderOk_(true),
    rxUnsupportedLogged_(false),
    tx_(),
    txMutex_(),
    txCv_(),
    ring_(),
    ptt_(false),
    txActive_(false),
    hangUntil_(0U)
{
    std::snprintf(call_, sizeof(call_), "%.10s", cfg_.callsign.c_str());
    std::snprintf(dest_, sizeof(dest_), "%.10s", cfg_.dest.c_str());
    if (cfg_.dgid > 99U)
        cfg_.dgid = 99U;

    ysfPayloadInit(&rxPay_);
    ysfRxInit(rx_.get(), &YsfMode::frameCallback, this, (uint8_t)cfg_.rxShape);
}

YsfMode::~YsfMode()
{
    stop();
}

std::string YsfMode::modeName() const
{
    return "YSF";
}

std::string YsfMode::modemType() const
{
    return "C4FM";          /* what ysf_host registers with */
}

void YsfMode::log(const std::string& text) const
{
    if (ev_.log)
        ev_.log(text);
}

void YsfMode::start()
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
    thread_ = std::thread(&YsfMode::txThread, this);
}

void YsfMode::stop()
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

bool YsfMode::transmitting() const
{
    return txActive_;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Receive                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

void YsfMode::rxSamples(const int16_t* samples, size_t count)
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
    ysfRxSamples(rx_.get(), scaled, (unsigned int)count);
}

void YsfMode::frameCallback(void* ctx, const uint8_t* frame, bool lost)
{
    static_cast<YsfMode*>(ctx)->onFrame(frame, lost);
}

void YsfMode::rxStart(const char* source, const char* dest)
{
    CallInfo info;

    rxActive_ = true;
    rxInfoKnown_ = source != nullptr;
    rxErrs_ = rxBits_ = 0UL;

    info.event = CALL_RX_START;
    info.src = info.dst = 0U;
    info.group = true;
    info.ber = 0.0F;
    if (source)
        info.text = std::string(source) + ">" + dest;
    if (ev_.call)
        ev_.call(info);
}

/* the callsigns became known during a call that was entered late */
void YsfMode::rxInfo()
{
    if (rxInfoKnown_ || !rxPay_.hasSource || !rxPay_.hasDest)
        return;

    CallInfo info;

    rxInfoKnown_ = true;
    info.event = CALL_RX_INFO;
    info.src = info.dst = 0U;
    info.group = true;
    info.ber = 0.0F;
    info.text = trimmed(rxPay_.source) + ">" + trimmed(rxPay_.dest);
    if (ev_.call)
        ev_.call(info);
}

void YsfMode::rxEnd(CallEvent how)
{
    if (!rxActive_)
        return;

    CallInfo info;

    info.event = how;
    info.src = info.dst = 0U;
    info.group = true;
    info.ber = rxBits_ > 0UL ? (float)(100.0 * rxErrs_ / rxBits_) : 0.0F;
    if (rxPay_.hasSource && rxPay_.hasDest)
        info.text = trimmed(rxPay_.source) + ">" + trimmed(rxPay_.dest);

    rxActive_ = false;
    rxInfoKnown_ = false;
    rxErrs_ = rxBits_ = 0UL;
    ysfPayloadReset(&rxPay_);

    if (ev_.call)
        ev_.call(info);
}

/* Five voice blocks go to the chip as the chip arranges them, and the speech comes back */
void YsfMode::rxVoice(const uint8_t* frame)
{
    if (!rxVocoderOk_)
        return;

    uint8_t net[35];
    uint8_t blocks[5U * Vocoder::BlockBytes];
    int16_t pcm[5U * Vocoder::Samples];

    rxErrs_ += extractVoice(frame, net);
    rxBits_ += 135UL;

    for (unsigned int b = 0U; b < 5U; b++)
        nxdnAudioChipEncode(layout_, net + 7U * b, blocks + b * Vocoder::BlockBytes);

    if (!vocoder_.decodeBatch(blocks, 5U, pcm))
    {
        rxVocoderOk_ = false;
        log("the vocoder stopped answering: received speech is no longer decoded");
        return;
    }

    if (ev_.rxAudio)
        ev_.rxAudio(pcm, 5U * Vocoder::Samples);
}

void YsfMode::onFrame(const uint8_t* raw, bool lost)
{
    uint8_t frame[YSF_FRAME_LENGTH_BYTES];
    YsfFich fich;

    if (lost)
    {
        rxEnd(CALL_RX_LOST);
        return;
    }

    std::memcpy(frame, raw, sizeof(frame));

    if (!ysfFichDecode(&fich, frame))
        return;                             /* a frame whose FICH is too damaged is skipped */

    unsigned int fi = ysfFichGetFI(&fich);
    unsigned int dt = ysfFichGetDT(&fich);

    if (fi == YSF_FI_HEADER)
    {
        if (rxActive_)
            return;                         /* a repeated header */

        ysfPayloadReset(&rxPay_);
        if (ysfPayloadHeader(&rxPay_, frame) && rxPay_.hasSource && rxPay_.hasDest)
            rxStart(trimmed(rxPay_.source).c_str(), trimmed(rxPay_.dest).c_str());
        else
            rxStart(nullptr, nullptr);
        return;
    }

    if (fi == YSF_FI_TERMINATOR)
    {
        rxEnd(CALL_RX_END);
        return;
    }

    if (fi != YSF_FI_COMMUNICATIONS)
        return;

    if (dt != YSF_DT_VD_MODE2)
    {
        if (!rxUnsupportedLogged_)
        {
            rxUnsupportedLogged_ = true;
            log(std::string("YSF: received a ") + (dt == YSF_DT_VD_MODE1 ? "V/D mode 1" : (dt == YSF_DT_DATA_FR_MODE ? "data FR" : "voice FR")) +
                " call: only V/D mode 2 speech is decoded");
        }
        return;
    }

    if (!rxActive_)
        rxStart(nullptr, nullptr);          /* a call entered late: no header was heard */

    ysfPayloadVDMode2Data(&rxPay_, frame, ysfFichGetFN(&fich), false);
    rxInfo();
    rxVoice(frame);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Transmit                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------ */

void YsfMode::pttDown()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        ring_.clear();
        ptt_ = true;
    }
    txCv_.notify_all();
}

void YsfMode::pttUp()
{
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        ptt_ = false;
    }
    txCv_.notify_all();
}

void YsfMode::micAudio(const int16_t* pcm, size_t count)
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

        while (ring_.size() > 8000U)            /* never more than 1 s behind */
            ring_.pop_front();
    }
    txCv_.notify_all();
}

/* Modulates one frame and hands the samples to the link (which keeps the transmission in real time) */
bool YsfMode::txFrame(const uint8_t* frame)
{
    int16_t buf[YSF_SAMPLES_PER_FRAME];
    unsigned int n = ysfTxFrame(&tx_, frame, buf);

    link_.txSamples(buf, n);
    return link_.isOpen();
}

bool YsfMode::txVoiceFrame(const int16_t* pcm, unsigned int fn)
{
    uint8_t chips[5U * Vocoder::BlockBytes];
    uint8_t net[35];
    uint8_t frame[YSF_FRAME_LENGTH_BYTES];

    /* all five go to the chip before the answers are read */
    if (!vocoder_.encodeBatch(pcm, 5U, chips))
    {
        log("the vocoder stopped answering: ending the transmission");
        return false;
    }

    std::memset(net, 0x00, sizeof(net));
    for (unsigned int b = 0U; b < 5U; b++)
        nxdnAudioChipDecode(layout_, chips + b * Vocoder::BlockBytes, net + 7U * b);     /* the 49 parameter bits */

    uint8_t cm = std::strcmp(dest_, "CQCQCQ") == 0 ? (uint8_t)YSF_CM_GROUP1 : (uint8_t)YSF_CM_INDIVIDUAL;

    buildVoice(frame, (uint8_t)fn, (uint8_t)cfg_.dgid, cm, dest_, call_, net);
    return txFrame(frame);
}

void YsfMode::txThread()
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

void YsfMode::txCall()
{
    uint8_t frame[YSF_FRAME_LENGTH_BYTES];
    int16_t buf[YSF_SAMPLES_PER_SYMBOL * 48U];
    int16_t pcm[5U * Vocoder::Samples];
    const size_t need = sizeof(pcm) / sizeof(pcm[0]);
    const uint8_t cm = std::strcmp(dest_, "CQCQCQ") == 0 ? (uint8_t)YSF_CM_GROUP1 : (uint8_t)YSF_CM_INDIVIDUAL;
    unsigned int fn = 0U, frames = 0U;
    bool ok = true;

    {
        /* a call being received is over for us */
        std::lock_guard<std::mutex> lock(rxMutex_);
        rxEnd(CALL_RX_LOST);
        ysfRxReset(rx_.get());
    }

    txActive_ = true;

    CallInfo info;
    info.event = CALL_TX_START;
    info.src = info.dst = 0U;
    info.group = std::strcmp(dest_, "CQCQCQ") == 0;
    info.ber = 0.0F;
    info.text = std::string(call_) + ">" + dest_;
    if (ev_.call)
        ev_.call(info);

    link_.requestMode();
    link_.txBegin();
    ysfTxInit(&tx_, (cfg_.txLevel > 0.0F ? cfg_.txLevel : 1.0F) * YSF_LEVEL_OUTER_H, (uint8_t)cfg_.txShape);

    /* the preamble lets the receivers' filters settle */
    unsigned int symbols = ((unsigned int)cfg_.txDelayMs * 48U) / 10U;
    symbols &= ~1U;
    while (symbols > 0U)
    {
        unsigned int chunk = symbols > 48U ? 48U : symbols;
        unsigned int n = ysfTxPreamble(&tx_, chunk, buf);

        link_.txSamples(buf, n);
        symbols -= chunk;
    }

    buildHeader(frame, YSF_FI_HEADER, (uint8_t)cfg_.dgid, cm, dest_, call_);
    ok = txFrame(frame);

    /* voice: one frame per 100 ms of speech from the microphone */
    while (ok && running_)
    {
        size_t got = 0U;
        bool last = false;

        {
            std::unique_lock<std::mutex> lock(txMutex_);

            /* wait for five blocks; stop when the key is released and nothing is left; a stalled microphone is padded */
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

        ok = txVoiceFrame(pcm, fn);
        fn = (fn + 1U) % 7U;
        frames++;

        if (last && got < need)
            break;
    }

    /* terminator, then the filter tail and the end of transmission */
    buildHeader(frame, YSF_FI_TERMINATOR, (uint8_t)cfg_.dgid, cm, dest_, call_);
    if (link_.isOpen())
    {
        txFrame(frame);

        int16_t tail[YSF_SAMPLES_PER_SYMBOL * (YSF_FILTER_SPAN + 1U)];
        unsigned int n = ysfTxFlush(&tx_, tail);

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
