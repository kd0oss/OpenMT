/* OpenMT Radio: a speech coder.
 *
 * One 20 ms frame is 160 samples of 8 kHz speech and a 72 bit (9 byte) block.  The block is the chip's
 * own arrangement; the mode engines turn it into the bits of their air interface.
 */

#ifndef OPENMT_VOCODER_H
#define OPENMT_VOCODER_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace openmt
{

class Vocoder
{
public:
    static const unsigned int Samples    = 160U;
    static const unsigned int BlockBytes = 9U;

    virtual ~Vocoder() {}

    /* 160 samples in, 9 bytes out; false when the chip does not answer */
    virtual bool encode(const int16_t* pcm, uint8_t* block) = 0;
    /* 9 bytes in, 160 samples out */
    virtual bool decode(const uint8_t* block, int16_t* pcm) = 0;

    /* n frames at once: pcm is n * 160 samples, blocks n * 9 bytes.  A dongle sends them all before it reads the
       answers, which takes the time of one round trip instead of n. */
    virtual bool encodeBatch(const int16_t* pcm, unsigned int n, uint8_t* blocks)
    {
        for (unsigned int i = 0U; i < n; i++)
            if (!encode(pcm + i * Samples, blocks + i * BlockBytes))
                return false;
        return true;
    }
    virtual bool decodeBatch(const uint8_t* blocks, unsigned int n, int16_t* pcm)
    {
        for (unsigned int i = 0U; i < n; i++)
            if (!decode(blocks + i * BlockBytes, pcm + i * Samples))
                return false;
        return true;
    }

    /* The P25 voice: IMBE, 20 ms of speech in an 88 bit (11 byte) block with no error correction (the mode adds its own).
       A chip is switched to the IMBE rate with setCodec(CODEC_IMBE) and back with CODEC_AMBE2; the 72 bit calls above
       are for CODEC_AMBE2 and the 88 bit calls below for CODEC_IMBE.  false when the vocoder does not have the codec. */
    enum Codec { CODEC_AMBE2 = 0, CODEC_IMBE = 1, CODEC_DSTAR = 2 };   /* CODEC_DSTAR: the chip's D-STAR interoperable 2400 + 1200 rate, 72 bit blocks as on the air */
    static const unsigned int ImbeBytes = 11U;

    virtual bool setCodec(Codec codec) { return codec == CODEC_AMBE2; }
    virtual bool encodeImbeBatch(const int16_t* pcm, unsigned int n, uint8_t* blocks) { (void)pcm; (void)n; (void)blocks; return false; }
    virtual bool decodeImbeBatch(const uint8_t* blocks, unsigned int n, int16_t* pcm) { (void)blocks; (void)n; (void)pcm; return false; }

    virtual std::string description() const = 0;
};

/* Stand in for tests and for running without a dongle: the nine bytes of the block are carried in the low bytes of the
   first nine samples, so a block can be checked after a trip through a mode's channel coding.  There is no speech in it. */
class NullVocoder : public Vocoder
{
public:
    bool encode(const int16_t* pcm, uint8_t* block);
    bool decode(const uint8_t* block, int16_t* pcm);
    bool setCodec(Codec codec);
    bool encodeImbeBatch(const int16_t* pcm, unsigned int n, uint8_t* blocks);
    bool decodeImbeBatch(const uint8_t* blocks, unsigned int n, int16_t* pcm);
    std::string description() const;
};

}

#endif
