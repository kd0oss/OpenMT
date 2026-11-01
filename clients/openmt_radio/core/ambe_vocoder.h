/* OpenMT Radio: the DVSI AMBE3000R on a serial port (ThumbDV, DVstick 30 and the like).
 * It wraps protocols/NXDN/ambe3000.c, run at the AMBE+2 3600 bit/s rate with FEC (72 bit blocks). */

#ifndef OPENMT_AMBE_VOCODER_H
#define OPENMT_AMBE_VOCODER_H

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "vocoder.h"
#include "imbe_vocoder.h"

extern "C"
{
#include "ambe3000.h"
}

namespace openmt
{

class AmbeVocoder : public Vocoder
{
public:
    AmbeVocoder();
    ~AmbeVocoder();

    bool open(const std::string& device, unsigned int baud, std::string& error);
    void close();
    bool isOpen() const;

    bool encode(const int16_t* pcm, uint8_t* block);
    bool decode(const uint8_t* block, int16_t* pcm);
    bool encodeBatch(const int16_t* pcm, unsigned int n, uint8_t* blocks);
    bool decodeBatch(const uint8_t* blocks, unsigned int n, int16_t* pcm);
    bool setCodec(Codec codec);
    bool encodeImbeBatch(const int16_t* pcm, unsigned int n, uint8_t* blocks);
    bool decodeImbeBatch(const uint8_t* blocks, unsigned int n, int16_t* pcm);
    std::string description() const;

    /* How the packets are written to the chip: chunk bytes at a time with gapUs between (0 = whole packet).
       Process wide, as the C code keeps it in globals.  A speech packet is 328 bytes and four go out per 80 ms
       of speech, so slow pacing makes the dongle too slow for real time. */
    static void setSerialPacing(int chunkBytes, int gapUs);

    /* Receives a line about the encode speed every few seconds of transmit */
    std::function<void(const std::string&)> log;

private:
    Ambe        ambe_;
    Codec       codec_;
    std::unique_ptr<imbe_vocoder> imbeEnc_;     /* P25 speech, in software (the chip has no IMBE codec) */
    std::unique_ptr<imbe_vocoder> imbeDec_;
    bool        open_;
    std::string device_;
    std::mutex  mutex_;         /* the chip is used by the transmit and the receive side */
    unsigned int encCount_;     /* encode timing since the last report */
    double       encTotalMs_;
    double       encMaxMs_;
    void report(const char* what, unsigned int frames, double ms, unsigned int& count, double& total, double& peak);
    unsigned int decCount_;
    double       decTotalMs_;
    double       decMaxMs_;
};

}

#endif
