/* OpenMT Radio: sample rate conversion between the sound card (8000 * N Hz, for example 48000) and the 8 kHz
 * speech the vocoder uses.  A linear phase low pass filter at 3.6 kHz is used in both directions. */

#ifndef OPENMT_RESAMPLER_H
#define OPENMT_RESAMPLER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace openmt
{

/* The sound card rate must be a multiple of 8000 */
bool validDeviceRate(unsigned int rate);

class Decimator
{
public:
    explicit Decimator(unsigned int factor);

    /* Appends the 8 kHz samples made from n input samples to out */
    void process(const int16_t* in, size_t n, std::vector<int16_t>& out);
    void reset();

private:
    unsigned int       factor_;
    std::vector<float> taps_;
    std::vector<float> hist_;       /* newest first, as long as the filter */
    unsigned int       phase_;
};

class Interpolator
{
public:
    explicit Interpolator(unsigned int factor);

    /* Appends the factor * n samples made from n 8 kHz samples to out */
    void process(const int16_t* in, size_t n, std::vector<int16_t>& out);
    void reset();

private:
    unsigned int       factor_;
    std::vector<float> taps_;
    std::vector<float> hist_;       /* newest first */
};

}

#endif
