#include "ambe_vocoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>


namespace openmt
{

AmbeVocoder::AmbeVocoder() :
    ambe_(),
    codec_(CODEC_AMBE2),
    open_(false),
    device_(),
    mutex_(),
    encCount_(0U),
    encTotalMs_(0.0),
    encMaxMs_(0.0),
    decCount_(0U),
    decTotalMs_(0.0),
    decMaxMs_(0.0)
{
    std::memset(&ambe_, 0x00, sizeof(ambe_));
    ambe_.fd = -1;
}

AmbeVocoder::~AmbeVocoder()
{
    close();
}

bool AmbeVocoder::open(const std::string& device, unsigned int baud, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (open_)
        return true;

    ambeDefaultRate(&ambe_);

    if (!ambeOpen(&ambe_, device.c_str(), baud))
    {
        error = "cannot open the AMBE3000R on " + device + " at " + std::to_string(baud) +
                " baud (is the port right, and is another program using it?)";
        return false;
    }

    device_ = device;
    open_   = true;
    return true;
}

void AmbeVocoder::close()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (open_)
    {
        ambeClose(&ambe_);
        open_ = false;
    }
}

bool AmbeVocoder::isOpen() const
{
    return open_;
}

void AmbeVocoder::setSerialPacing(int chunkBytes, int gapUs)
{
    ambeChunk = chunkBytes;
    ambeGapUs = gapUs;
}

void AmbeVocoder::report(const char* what, unsigned int frames, double ms, unsigned int& count, double& total,
                         double& peak)
{
    double per = ms / frames;

    count += frames;
    total += ms;
    if (per > peak)
        peak = per;

    /* every 50 frames (one second of speech at real time) say how fast the dongle is */
    if (count >= 50U)
    {
        double avg = total / count;
        char text[200];

        std::snprintf(text, sizeof(text), "dongle %s %.1f ms per 20 ms frame (max %.1f ms)%s", what, avg, peak,
                      avg > 18.0 ? " - too slow for real time" : "");
        if (log)
            log(text);
        count = 0U;
        total = 0.0;
        peak = 0.0;
    }
}

static double elapsedMs(const std::chrono::steady_clock::time_point& t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool AmbeVocoder::encode(const int16_t* pcm, uint8_t* block)
{
    return encodeBatch(pcm, 1U, block);
}

bool AmbeVocoder::decode(const uint8_t* block, int16_t* pcm)
{
    return decodeBatch(block, 1U, pcm);
}

bool AmbeVocoder::encodeBatch(const int16_t* pcm, unsigned int n, uint8_t* blocks)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!open_ || n == 0U)
        return false;

    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    bool ok = true;

    for (unsigned int i = 0U; i < n && ok; i++)
        ok = ambeEncodeSend(&ambe_, pcm + i * AMBE_SAMPLES);
    for (unsigned int i = 0U; i < n && ok; i++)
        ok = ambeEncodeRecv(&ambe_, blocks + i * AMBE_BLOCK_BYTES);

    report("encode", n, elapsedMs(t0), encCount_, encTotalMs_, encMaxMs_);
    return ok;
}

bool AmbeVocoder::decodeBatch(const uint8_t* blocks, unsigned int n, int16_t* pcm)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!open_ || n == 0U)
        return false;

    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    bool ok = true;

    for (unsigned int i = 0U; i < n && ok; i++)
        ok = ambeDecodeSend(&ambe_, blocks + i * AMBE_BLOCK_BYTES);
    for (unsigned int i = 0U; i < n && ok; i++)
        ok = ambeDecodeRecv(&ambe_, pcm + i * AMBE_SAMPLES);

    report("decode", n, elapsedMs(t0), decCount_, decTotalMs_, decMaxMs_);
    return ok;
}

/* P25 speech: the AMBE3000R has no IMBE codec (its rates are AMBE+2 only), so the IMBE frames are made in software with
   Pavel Yazev's fixed point vocoder (third_party/imbe_vocoder, from OP25).  The chip is not touched. */
bool AmbeVocoder::setCodec(Codec codec)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!open_)
        return false;

    /* the rate words of the chip: AMBE+2 2450 + 1150 (DMR, NXDN, YSF) and the one that "is interoperable with DSTAR" in
       table 115 of the AMBE-3000R manual (2400 + 1200, 3600 bit/s, the 72 bit block as D-STAR sends it) */
    static const uint16_t RATE_AMBE2[6] = { 0x0431U, 0x0754U, 0x2400U, 0x0000U, 0x0000U, 0x6F48U };
    static const uint16_t RATE_DSTAR[6] = { 0x0130U, 0x0763U, 0x4000U, 0x0000U, 0x0000U, 0x0048U };

    if (codec == CODEC_DSTAR || (codec == CODEC_AMBE2 && codec_ == CODEC_DSTAR))
    {
        if (!ambeSetRate(&ambe_, codec == CODEC_DSTAR ? RATE_DSTAR : RATE_AMBE2, 72U))
            return false;
    }
    codec_ = codec;
    return true;
}

/* the 88 bit frame of the callers is u0 .. u7 (12, 12, 12, 12, 11, 11, 11 and 7 bits), most significant bit first, as OP25's
   imbe_pack makes it */
static const unsigned int U_BITS[8] = {12U, 12U, 12U, 12U, 11U, 11U, 11U, 7U};

static void packU(const int16_t* u, uint8_t* out)
{
    unsigned int pos = 0U;
    int16_t w[8];

    std::memcpy(w, u, sizeof(w));
    w[7] = (int16_t)((w[7] >> 1) & 0x7F);       /* the vocoder keeps u7 shifted left by one (bit 0 is free) */

    std::memset(out, 0x00, 11U);
    for (unsigned int v = 0U; v < 8U; v++)
        for (int b = (int)U_BITS[v] - 1; b >= 0; b--, pos++)
            if ((w[v] >> b) & 1)
                out[pos >> 3] |= (uint8_t)(0x80U >> (pos & 7U));
}

static void unpackU(const uint8_t* in, int16_t* u)
{
    unsigned int pos = 0U;

    for (unsigned int v = 0U; v < 8U; v++)
    {
        int value = 0;

        for (unsigned int b = 0U; b < U_BITS[v]; b++, pos++)
            value = (value << 1) | ((in[pos >> 3] >> (7U - (pos & 7U))) & 1);
        u[v] = (int16_t)value;
    }
    u[7] = (int16_t)(u[7] << 1);
}

bool AmbeVocoder::encodeImbeBatch(const int16_t* pcm, unsigned int n, uint8_t* blocks)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!open_ || codec_ != CODEC_IMBE || n == 0U)
        return false;

    if (!imbeEnc_)
        imbeEnc_.reset(new imbe_vocoder());

    for (unsigned int i = 0U; i < n; i++)
    {
        int16_t snd[AMBE_SAMPLES], u[8];

        std::memcpy(snd, pcm + i * AMBE_SAMPLES, sizeof(snd));
        imbeEnc_->imbe_encode(u, snd);
        packU(u, blocks + i * ImbeBytes);
    }
    return true;
}

bool AmbeVocoder::decodeImbeBatch(const uint8_t* blocks, unsigned int n, int16_t* pcm)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!open_ || codec_ != CODEC_IMBE || n == 0U)
        return false;

    if (!imbeDec_)
        imbeDec_.reset(new imbe_vocoder());

    for (unsigned int i = 0U; i < n; i++)
    {
        int16_t u[8];

        unpackU(blocks + i * ImbeBytes, u);
        imbeDec_->imbe_decode(u, pcm + i * AMBE_SAMPLES);
    }
    return true;
}

std::string AmbeVocoder::description() const
{
    std::string text = "AMBE3000R on " + device_;

    if (ambe_.product[0] != '\0')
        text += std::string(" (") + ambe_.product + ")";
    return text;
}

}
