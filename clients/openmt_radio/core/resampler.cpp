#include "resampler.h"

#include <cmath>

namespace openmt
{

static const double PI = 3.14159265358979323846;

bool validDeviceRate(unsigned int rate)
{
    return rate >= 8000U && (rate % 8000U) == 0U && rate <= 192000U;
}

/* Windowed sinc low pass, cut off at 3.6 kHz of a rate of 8000 * factor Hz; unity gain at DC */
static std::vector<float> makeTaps(unsigned int factor)
{
    unsigned int count = 12U * factor + 1U;         /* odd: linear phase with a whole sample of delay */
    std::vector<float> taps(count);
    double fc = 3600.0 / (8000.0 * factor);         /* cycles per input sample at the high rate */
    double mid = (count - 1U) / 2.0;
    double sum = 0.0;

    for (unsigned int i = 0U; i < count; i++)
    {
        double x = i - mid;
        double s = (x == 0.0) ? 2.0 * fc : std::sin(2.0 * PI * fc * x) / (PI * x);
        double w = 0.54 - 0.46 * std::cos(2.0 * PI * i / (count - 1U));     /* Hamming */

        taps[i] = (float)(s * w);
        sum += s * w;
    }

    for (unsigned int i = 0U; i < count; i++)
        taps[i] = (float)(taps[i] / sum);

    return taps;
}

static int16_t clip(float v)
{
    if (v > 32767.0F)
        return 32767;
    if (v < -32768.0F)
        return -32768;
    return (int16_t)std::lrint(v);
}

Decimator::Decimator(unsigned int factor) :
    factor_(factor == 0U ? 1U : factor),
    taps_(makeTaps(factor_)),
    hist_(taps_.size(), 0.0F),
    phase_(0U)
{
}

void Decimator::reset()
{
    for (size_t i = 0U; i < hist_.size(); i++)
        hist_[i] = 0.0F;
    phase_ = 0U;
}

void Decimator::process(const int16_t* in, size_t n, std::vector<int16_t>& out)
{
    size_t len = taps_.size();

    for (size_t k = 0U; k < n; k++)
    {
        /* shift the history: newest first */
        for (size_t i = len - 1U; i > 0U; i--)
            hist_[i] = hist_[i - 1U];
        hist_[0] = (float)in[k];

        if (++phase_ == factor_)
        {
            phase_ = 0U;

            float acc = 0.0F;
            for (size_t i = 0U; i < len; i++)
                acc += taps_[i] * hist_[i];

            out.push_back(clip(acc));
        }
    }
}

Interpolator::Interpolator(unsigned int factor) :
    factor_(factor == 0U ? 1U : factor),
    taps_(makeTaps(factor_)),
    hist_((taps_.size() + factor_ - 1U) / factor_, 0.0F)
{
}

void Interpolator::reset()
{
    for (size_t i = 0U; i < hist_.size(); i++)
        hist_[i] = 0.0F;
}

void Interpolator::process(const int16_t* in, size_t n, std::vector<int16_t>& out)
{
    size_t len = taps_.size();

    for (size_t k = 0U; k < n; k++)
    {
        for (size_t i = hist_.size() - 1U; i > 0U; i--)
            hist_[i] = hist_[i - 1U];
        hist_[0] = (float)in[k];

        /* zero stuffing: output phase p uses the taps p, p + factor, p + 2 factor ... on the 8 kHz history */
        for (unsigned int p = 0U; p < factor_; p++)
        {
            float acc = 0.0F;

            for (size_t j = 0U; p + j * factor_ < len && j < hist_.size(); j++)
                acc += taps_[p + j * factor_] * hist_[j];

            out.push_back(clip(acc * (float)factor_));
        }
    }
}

}
