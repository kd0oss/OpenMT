#include "codec2_codec.h"

#include <cstring>
#include <dlfcn.h>

namespace openmt
{

namespace
{

typedef void* (*CreateFn)(int);
typedef void (*DestroyFn)(void*);
typedef void (*EncodeFn)(void*, unsigned char*, short*);
typedef void (*DecodeFn)(void*, short*, const unsigned char*);
typedef int (*CountFn)(void*);

const int CODEC2_MODE_3200 = 0;

class LibCodec2 : public Codec2Codec
{
public:
    LibCodec2() : lib_(NULL), c2_(NULL), destroy_(NULL), encode_(NULL), decode_(NULL) {}

    ~LibCodec2()
    {
        if (c2_ != NULL && destroy_ != NULL)
            destroy_(c2_);
        if (lib_ != NULL)
            dlclose(lib_);
    }

    bool open(std::string& error)
    {
        static const char* NAMES[] = { "libcodec2.so.1.2", "libcodec2.so.1.1", "libcodec2.so.1.0", "libcodec2.so.1", "libcodec2.so",
                                       "libcodec2.so.0.9", "libcodec2.so.0.8", "libcodec2.so.0.7" };

        for (size_t i = 0U; i < sizeof(NAMES) / sizeof(NAMES[0]) && lib_ == NULL; i++)
            lib_ = dlopen(NAMES[i], RTLD_NOW);

        if (lib_ == NULL)
        {
            error = "the Codec2 library (libcodec2) is not installed; on Debian or Ubuntu: sudo apt install libcodec2-1.2";
            return false;
        }

        CreateFn create = (CreateFn)dlsym(lib_, "codec2_create");
        CountFn samples = (CountFn)dlsym(lib_, "codec2_samples_per_frame");
        CountFn bits    = (CountFn)dlsym(lib_, "codec2_bits_per_frame");

        destroy_ = (DestroyFn)dlsym(lib_, "codec2_destroy");
        encode_  = (EncodeFn)dlsym(lib_, "codec2_encode");
        decode_  = (DecodeFn)dlsym(lib_, "codec2_decode");

        if (!create || !samples || !bits || !destroy_ || !encode_ || !decode_)
        {
            error = "libcodec2 does not have the functions needed";
            return false;
        }

        c2_ = create(CODEC2_MODE_3200);
        if (c2_ == NULL || samples(c2_) != (int)Samples || bits(c2_) != (int)(BlockBytes * 8U))
        {
            error = "libcodec2 does not give 160 samples and 64 bits for mode 3200";
            return false;
        }
        return true;
    }

    void encode(const int16_t* pcm, uint8_t* block)
    {
        short in[Samples];

        std::memcpy(in, pcm, sizeof(in));
        encode_(c2_, block, in);
    }

    void decode(const uint8_t* block, int16_t* pcm)
    {
        short out[Samples];

        decode_(c2_, out, block);
        std::memcpy(pcm, out, sizeof(out));
    }

private:
    void*     lib_;
    void*     c2_;
    DestroyFn destroy_;
    EncodeFn  encode_;
    DecodeFn  decode_;
};

class StandIn : public Codec2Codec
{
public:
    void encode(const int16_t* pcm, uint8_t* block)
    {
        for (unsigned int k = 0U; k < BlockBytes; k++)
            block[k] = (uint8_t)(pcm[k] & 0xFF);
    }

    void decode(const uint8_t* block, int16_t* pcm)
    {
        std::memset(pcm, 0, Samples * sizeof(int16_t));
        for (unsigned int k = 0U; k < BlockBytes; k++)
            pcm[k] = (int16_t)block[k];
    }
};

}

std::unique_ptr<Codec2Codec> makeCodec2(bool standIn, std::string& error)
{
    if (standIn)
        return std::unique_ptr<Codec2Codec>(new StandIn());

    std::unique_ptr<LibCodec2> codec(new LibCodec2());

    if (!codec->open(error))
        return std::unique_ptr<Codec2Codec>();
    return std::unique_ptr<Codec2Codec>(codec.release());
}

}
