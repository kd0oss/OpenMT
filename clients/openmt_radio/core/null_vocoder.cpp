#include "vocoder.h"

#include <cstring>

namespace openmt
{

bool NullVocoder::encode(const int16_t* pcm, uint8_t* block)
{
    for (unsigned int k = 0U; k < BlockBytes; k++)
        block[k] = (uint8_t)(pcm[k] & 0xFF);

    return true;
}

bool NullVocoder::decode(const uint8_t* block, int16_t* pcm)
{
    std::memset(pcm, 0x00, Samples * sizeof(int16_t));

    for (unsigned int k = 0U; k < BlockBytes; k++)
        pcm[k] = (int16_t)block[k];

    return true;
}

bool NullVocoder::setCodec(Codec codec)
{
    (void)codec;
    return true;
}

/* the eleven bytes of the block are carried in the low bytes of the first eleven samples of each frame */
bool NullVocoder::encodeImbeBatch(const int16_t* pcm, unsigned int n, uint8_t* blocks)
{
    for (unsigned int i = 0U; i < n; i++)
        for (unsigned int k = 0U; k < ImbeBytes; k++)
            blocks[i * ImbeBytes + k] = (uint8_t)(pcm[i * Samples + k] & 0xFF);

    return true;
}

bool NullVocoder::decodeImbeBatch(const uint8_t* blocks, unsigned int n, int16_t* pcm)
{
    std::memset(pcm, 0x00, n * Samples * sizeof(int16_t));

    for (unsigned int i = 0U; i < n; i++)
        for (unsigned int k = 0U; k < ImbeBytes; k++)
            pcm[i * Samples + k] = (int16_t)blocks[i * ImbeBytes + k];

    return true;
}

std::string NullVocoder::description() const
{
    return "no vocoder (test)";
}

}
