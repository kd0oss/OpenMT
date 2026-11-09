#include "rf_monitor.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace openmt
{

const double RfMonitor::COUNTS_PER_HZ = 0.7;

static const double RATE          = 24000.0;
static const double GATE          = 60.0;      /* RMS counts below which there is nothing to measure */
static const size_t MIN_SYMBOLS   = 400U;
static const unsigned EYE_TRACES  = 120U;
static const unsigned EYE_POINTS  = 41U;       /* two symbols, -1 to +1 in steps of 0.05 */
static const unsigned HIST_BINS   = 120U;
static const unsigned FFT_N       = 1024U;

RfMonitor::RfMonitor() :
    ring_(RING, 0),
    head_(0U),
    total_(0U),
    baud_(4800.0),
    levels_(4U),
    nominal_(0.0)
{
}

void RfMonitor::configure(double baud, unsigned symbolLevels, double nominalOuter)
{
    std::lock_guard<std::mutex> lock(mutex_);

    baud_    = baud;
    levels_  = symbolLevels;
    nominal_ = nominalOuter;
}

void RfMonitor::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::fill(ring_.begin(), ring_.end(), (int16_t)0);
    head_  = 0U;
    total_ = 0U;
}

void RfMonitor::feed(const int16_t* samples, size_t count)
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (size_t i = 0U; i < count; i++)
    {
        ring_[head_] = samples[i];
        head_ = (head_ + 1U) % RING;
    }
    total_ += count;
}

/* the value of x at a fractional position (linear interpolation) */
static double at(const std::vector<double>& x, double pos)
{
    long i = (long)std::floor(pos);
    double f = pos - (double)i;

    if (i < 0 || (size_t)(i + 1) >= x.size())
        return 0.0;
    return x[(size_t)i] * (1.0 - f) + x[(size_t)i + 1U] * f;
}

/* in place radix 2 FFT */
static void fft(std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();

    for (size_t i = 1U, j = 0U; i < n; i++)
    {
        size_t bit = n >> 1;

        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2U; len <= n; len <<= 1)
    {
        double ang = -2.0 * M_PI / (double)len;
        std::complex<double> wl(std::cos(ang), std::sin(ang));

        for (size_t i = 0U; i < n; i += len)
        {
            std::complex<double> w(1.0, 0.0);

            for (size_t k = 0U; k < len / 2U; k++)
            {
                std::complex<double> u = a[i + k];
                std::complex<double> v = a[i + k + len / 2U] * w;

                a[i + k] = u + v;
                a[i + k + len / 2U] = u - v;
                w *= wl;
            }
        }
    }
}

/* k-means in one dimension, starting from evenly spread percentiles; returns the centres (sorted) and the standard deviations */
static void cluster(std::vector<double> v, unsigned k, double* centre, double* sd)
{
    std::sort(v.begin(), v.end());

    for (unsigned c = 0U; c < k; c++)
        centre[c] = v[(size_t)((double)v.size() * (c + 0.5) / (double)k)];

    for (int it = 0; it < 20; it++)
    {
        double sum[4] = {0.0, 0.0, 0.0, 0.0};
        size_t cnt[4] = {0U, 0U, 0U, 0U};

        for (size_t i = 0U; i < v.size(); i++)
        {
            unsigned best = 0U;

            for (unsigned c = 1U; c < k; c++)
                if (std::fabs(v[i] - centre[c]) < std::fabs(v[i] - centre[best]))
                    best = c;
            sum[best] += v[i];
            cnt[best]++;
        }
        for (unsigned c = 0U; c < k; c++)
            if (cnt[c] > 0U)
                centre[c] = sum[c] / (double)cnt[c];
    }

    double sq[4] = {0.0, 0.0, 0.0, 0.0};
    size_t cnt[4] = {0U, 0U, 0U, 0U};

    for (size_t i = 0U; i < v.size(); i++)
    {
        unsigned best = 0U;

        for (unsigned c = 1U; c < k; c++)
            if (std::fabs(v[i] - centre[c]) < std::fabs(v[i] - centre[best]))
                best = c;
        sq[best] += (v[i] - centre[best]) * (v[i] - centre[best]);
        cnt[best]++;
    }
    for (unsigned c = 0U; c < k; c++)
        sd[c] = cnt[c] > 1U ? std::sqrt(sq[c] / (double)cnt[c]) : 0.0;
}

MonitorSnapshot RfMonitor::snapshot()
{
    MonitorSnapshot s;
    std::vector<double> x(RING);
    double baud;
    unsigned levels;
    double nominal;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (size_t i = 0U; i < RING; i++)
            x[i] = ring_[(head_ + i) % RING];
        s.samples = total_;
        baud      = baud_;
        levels    = levels_;
        nominal   = nominal_;
    }

    s.baud         = baud;
    s.symbolLevels = levels;
    s.nominalHz    = nominal / COUNTS_PER_HZ;

    /* the last half second is looked at: long enough for the histogram, short enough to follow the signal */
    const size_t N = 12000U;
    std::vector<double> w(x.end() - (long)N, x.end());
    double sq = 0.0;
    int pk = 0;
    size_t clip = 0U;

    for (size_t i = 0U; i < N; i++)
    {
        sq += w[i] * w[i];
        pk = std::max(pk, (int)std::fabs(w[i]));
        if (std::fabs(w[i]) >= 32760.0)
            clip++;
    }
    s.rms     = std::sqrt(sq / (double)N);
    s.peak    = pk;
    s.clipped = clip > 0U;
    s.active  = s.rms >= GATE;

    /* spectrum of the demodulated signal: average of overlapping 1024 point Hann windows */
    {
        std::vector<double> acc(FFT_N / 2U, 0.0);
        int n = 0;

        for (size_t start = 0U; start + FFT_N <= N; start += FFT_N / 2U)
        {
            std::vector<std::complex<double>> a(FFT_N);

            for (size_t i = 0U; i < FFT_N; i++)
                a[i] = w[start + i] * (0.5 - 0.5 * std::cos(2.0 * M_PI * (double)i / (double)(FFT_N - 1U)));
            fft(a);
            for (size_t i = 0U; i < FFT_N / 2U; i++)
                acc[i] += std::norm(a[i]);
            n++;
        }
        s.spectrum.resize(FFT_N / 2U);
        for (size_t i = 0U; i < FFT_N / 2U; i++)
            s.spectrum[i] = (float)(10.0 * std::log10(acc[i] / (double)(n > 0 ? n : 1) + 1e-3));
    }

    if (baud <= 0.0 || levels < 2U || !s.active)
        return s;

    const double sps = RATE / baud;
    const size_t nSym = (size_t)((double)N / sps) - 4U;

    if (nSym < MIN_SYMBOLS)
        return s;

    /* the sampling instant: the offset within a symbol where the samples are, on average, furthest from zero (at the symbol
       centres the signal sits on its levels, between them it moves from one to the next) */
    double bestOff = 0.0;
    double bestPow = -1.0;

    for (double off = 0.0; off < sps; off += 0.25)
    {
        double p = 0.0;

        for (size_t k = 1U; k < nSym; k++)
        {
            double v = at(w, off + sps * (double)k);

            p += v * v;
        }
        if (p > bestPow)
        {
            bestPow = p;
            bestOff = off;
        }
    }

    std::vector<double> sym;

    for (size_t k = 1U; k < nSym; k++)
        sym.push_back(at(w, bestOff + sps * (double)k));

    /* eye: the last traces, each two symbols wide around a sampling instant */
    s.eyeTraces = EYE_TRACES;
    s.eyePoints = EYE_POINTS;
    s.eye.assign((size_t)EYE_TRACES * EYE_POINTS, 0.0F);
    for (unsigned t = 0U; t < EYE_TRACES; t++)
    {
        double centre = bestOff + sps * (double)(nSym - 2U - t);

        for (unsigned j = 0U; j < EYE_POINTS; j++)
            s.eye[(size_t)t * EYE_POINTS + j] = (float)at(w, centre + (-1.0 + 0.05 * (double)j) * sps);
    }

    /* histogram of the symbol values */
    double lo = *std::min_element(sym.begin(), sym.end());
    double hi = *std::max_element(sym.begin(), sym.end());
    double m = std::max(std::fabs(lo), std::fabs(hi));

    s.histMin = -m * 1.05;
    s.histMax =  m * 1.05;
    s.hist.assign(HIST_BINS, 0U);
    for (size_t i = 0U; i < sym.size(); i++)
    {
        int b = (int)((sym[i] - s.histMin) / (s.histMax - s.histMin) * (double)HIST_BINS);

        b = std::max(0, std::min((int)HIST_BINS - 1, b));
        s.hist[(size_t)b]++;
    }

    /* the levels */
    double c[4], sd[4];

    cluster(sym, levels, c, sd);
    if (levels == 2U)
    {
        s.levels[0] = c[0];
        s.levels[3] = c[1];
        s.levels[1] = s.levels[2] = 0.0;
    }
    else
    {
        for (unsigned i = 0U; i < 4U; i++)
            s.levels[i] = c[i];
    }
    s.levelsValid = true;
    s.centre = (c[0] + c[levels - 1U]) / 2.0;
    s.outer  = (c[levels - 1U] - c[0]) / 2.0;
    s.outerHz = s.outer / COUNTS_PER_HZ;
    s.outerPercent = nominal > 0.0 ? 100.0 * s.outer / nominal : 0.0;

    double sdMean = 0.0;

    for (unsigned i = 0U; i < levels; i++)
        sdMean += sd[i];
    sdMean /= (double)levels;
    s.spread = sdMean;

    double gap = (c[levels - 1U] - c[0]) / (double)(levels - 1U);

    s.blur = gap > 0.0 ? sdMean / gap : 0.0;
    s.approximate = s.blur > 0.25;
    return s;
}

}
