/* OpenMT Radio: Codec2 3200 (M17 speech).  The library is loaded when the mode starts (dlopen), so the program builds and
 * runs without it; M17 then says that it is missing.  A stand in that carries the 8 byte block in the low bytes of the
 * samples serves the tests, as NullVocoder does for the dongle. */

#ifndef OPENMT_CODEC2_CODEC_H
#define OPENMT_CODEC2_CODEC_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace openmt
{

class Codec2Codec
{
public:
    static const unsigned int Samples    = 160U;    /* 20 ms of 8 kHz speech */
    static const unsigned int BlockBytes = 8U;      /* 64 bits */

    virtual ~Codec2Codec() {}

    virtual void encode(const int16_t* pcm, uint8_t* block) = 0;
    virtual void decode(const uint8_t* block, int16_t* pcm) = 0;
};

/* The real codec (libcodec2, mode 3200), or the stand in; null and an error text when the library is not there */
std::unique_ptr<Codec2Codec> makeCodec2(bool standIn, std::string& error);

}

#endif
