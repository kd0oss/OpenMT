#include "iq_monitor.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace openmt
{

const size_t IqMonitor::RING;

static long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static uint32_t get32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool parseIqFrame(const uint8_t* data, size_t length, IqFrame& frame)
{
    if (length < 24U || data[0] != 'I' || data[1] != 'Q' || data[2] != 'F' || data[3] != 'R' || data[4] != 1U)
        return false;

    const size_t n = (size_t)data[6] | ((size_t)data[7] << 8);

    if (n == 0U || length < 24U + 4U * n)
        return false;

    frame.overload   = (data[5] & 1U) != 0U;
    frame.centreHz   = get32(data + 8);
    frame.sampleRate = get32(data + 12);
    frame.sequence   = get32(data + 16);
    frame.mirrored   = data[21] != 0U;
    if (frame.sampleRate < 8000U || frame.sampleRate > 200000U)
        return false;

    const float scale = 1.0F / (32767.0F * (float)(1UL << (data[20] > 15U ? 15U : data[20])));

    frame.samples.resize(n);
    for (size_t k = 0U; k < n; k++)
    {
        const uint8_t* p = data + 24U + 4U * k;
        int16_t re = (int16_t)(uint16_t)(p[0] | (p[1] << 8));
        int16_t im = (int16_t)(uint16_t)(p[2] | (p[3] << 8));

        frame.samples[k] = std::complex<float>((float)re * scale, (float)im * scale);
    }
    return true;
}

IqMonitor::IqMonitor() :
    ring_(RING),
    head_(0U),
    total_(0U),
    rate_(62500.0),
    lastSeq_(0U),
    haveSeq_(false),
    frames_(0UL),
    lost_(0UL),
    overload_(false),
    lastMs_(0),
    baud_(4800.0),
    levels_(4U),
    nominal_(0.0),
    sync_(SyncKind::None)
{
}

void IqMonitor::configure(double baud, unsigned symbolLevels, double nominalOuter, SyncKind sync)
{
    std::lock_guard<std::mutex> lock(mutex_);

    baud_    = baud;
    levels_  = symbolLevels;
    nominal_ = nominalOuter;
    sync_    = sync;
}

void IqMonitor::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::fill(ring_.begin(), ring_.end(), std::complex<float>(0.0F, 0.0F));
    head_     = 0U;
    total_    = 0U;
    haveSeq_  = false;
    frames_   = 0UL;
    lost_     = 0UL;
    overload_ = false;
    lastMs_   = 0;
    std::lock_guard<std::mutex> smooth(smoothMutex_);
    smoother_.reset();
}

void IqMonitor::feed(const IqFrame& frame)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (haveSeq_ && frame.sequence != lastSeq_ + 1U)
        lost_ += (unsigned long)(frame.sequence - lastSeq_ - 1U);
    lastSeq_  = frame.sequence;
    haveSeq_  = true;
    rate_     = (double)frame.sampleRate;
    overload_ = frame.overload;
    for (size_t i = 0U; i < frame.samples.size(); i++)
    {
        ring_[head_] = frame.samples[i];
        head_ = (head_ + 1U) % RING;
    }
    total_ += frame.samples.size();
    frames_++;
    lastMs_ = nowMs();
}

bool IqMonitor::available() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return lastMs_ != 0 && nowMs() - lastMs_ < 2000;
}

unsigned long IqMonitor::framesReceived() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return frames_;
}

/* windowed sinc low pass, unity gain at DC; cutoff as a fraction of the sample rate */
static std::vector<double> lowPass(size_t taps, double cutoff)
{
    std::vector<double> h(taps);
    const double mid = (double)(taps - 1U) / 2.0;
    double sum = 0.0;

    for (size_t i = 0U; i < taps; i++)
    {
        double x = (double)i - mid;
        double v = std::fabs(x) < 1e-9 ? 2.0 * cutoff : std::sin(2.0 * M_PI * cutoff * x) / (M_PI * x);
        double w = 0.42 - 0.5 * std::cos(2.0 * M_PI * (double)i / (double)(taps - 1U)) +
                   0.08 * std::cos(4.0 * M_PI * (double)i / (double)(taps - 1U));

        h[i] = v * w;
        sum += h[i];
    }
    for (size_t i = 0U; i < taps; i++)
        h[i] /= sum;
    return h;
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

MonitorSnapshot IqMonitor::snapshot()
{
    MonitorSnapshot s = snapshotRaw();
    std::lock_guard<std::mutex> lock(smoothMutex_);

    smoother_.apply(s, nowMs());
    return s;
}

MonitorSnapshot IqMonitor::snapshotRaw()
{
    MonitorSnapshot s;

    if (!available())
    {
        /* nothing arrives: no analysis (it costs tens of milliseconds, and this runs on the user interface's timer) */
        std::lock_guard<std::mutex> lock(mutex_);

        s.samples      = total_;
        s.iqFrames     = frames_;
        s.iqLost       = lost_;
        s.fromIq       = true;
        s.iqRate       = rate_;
        s.baud         = baud_;
        s.symbolLevels = levels_;
        s.nominalHz    = nominal_ / RfMonitor::COUNTS_PER_HZ;
        return s;
    }

    std::vector<std::complex<float>> x(RING);
    double rate, baud, nominal;
    unsigned levels;
    SyncKind sync;
    bool overload;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (size_t i = 0U; i < RING; i++)
            x[i] = ring_[(head_ + i) % RING];
        s.samples  = total_;
        s.iqFrames = frames_;
        s.iqLost   = lost_;
        rate       = rate_;
        baud       = baud_;
        levels     = levels_;
        nominal    = nominal_;
        sync       = sync_;
        overload   = overload_;
    }

    s.fromIq       = true;
    s.iqRate       = rate;
    s.iqOverload   = overload;
    s.clipped      = overload;
    s.baud         = baud;
    s.symbolLevels = levels;
    s.nominalHz    = nominal / RfMonitor::COUNTS_PER_HZ;

    /* the last 0.6 s: long enough for several frame syncs, short enough to follow the signal */
    const size_t N = std::min(RING, (size_t)(0.6 * rate));
    std::vector<std::complex<double>> z(N);

    for (size_t i = 0U; i < N; i++)
        z[i] = std::complex<double>(x[RING - N + i]);

    /* the channel: low pass at 7 kHz (a 12.5 kHz channel, with room for a frequency error).  Plain arrays of doubles: this runs on
       the user interface's timer, and std::complex arithmetic is slow in a build without optimisation */
    {
        static const size_t TAPS = 63U;
        std::vector<double> h = lowPass(TAPS, 7000.0 / rate);
        std::vector<double> zr(N), zi(N), fr(N, 0.0), fi(N, 0.0);

        for (size_t i = 0U; i < N; i++)
        {
            zr[i] = z[i].real();
            zi[i] = z[i].imag();
        }
        for (size_t i = TAPS; i < N; i++)
        {
            double ar = 0.0, ai = 0.0;
            const double* pr = &zr[i - TAPS + 1U];
            const double* pi = &zi[i - TAPS + 1U];

            for (size_t k = 0U; k < TAPS; k++)
            {
                ar += pr[TAPS - 1U - k] * h[k];
                ai += pi[TAPS - 1U - k] * h[k];
            }
            fr[i] = ar;
            fi[i] = ai;
        }
        for (size_t i = 0U; i < N; i++)
            z[i] = std::complex<double>(fr[i], fi[i]);
    }

    /* The noise: the bridge's samples hold 22 kHz either side of the channel, so the bins 14 to 20 kHz away from the centre hold only
       noise (the lower of the two sides is taken: an adjacent channel may be strong on one).  It is measured on the unfiltered
       samples, so a signal that lasts the whole window is told from noise as well as one that comes and goes. */
    double noisePerSample = 0.0;
    {
        const size_t FN = 1024U;
        std::vector<double> side[2];
        double sumW2 = 0.0;

        for (size_t i = 0U; i < FN; i++)
        {
            double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * (double)i / (double)(FN - 1U));

            sumW2 += w * w;
        }
        double accSide[2] = {0.0, 0.0};
        size_t nSide[2] = {0U, 0U};
        int segs = 0;

        /* at most 8 segments, spread over the window: enough for a noise floor, and cheap */
        const size_t stride = std::max(FN / 2U, (N - FN) / 7U);

        for (size_t start = 0U; start + FN <= N; start += stride)
        {
            std::vector<std::complex<double>> a(FN);

            for (size_t i = 0U; i < FN; i++)
                a[i] = std::complex<double>(x[RING - N + start + i]) * (0.5 - 0.5 * std::cos(2.0 * M_PI * (double)i / (double)(FN - 1U)));
            fft(a);
            for (size_t k = 0U; k < FN; k++)
            {
                double hz = (k < FN / 2U ? (double)k : (double)k - (double)FN) * rate / (double)FN;

                if (hz >= 14000.0 && hz <= 20000.0)
                {
                    accSide[1] += std::norm(a[k]);
                    nSide[1]++;
                }
                else if (hz <= -14000.0 && hz >= -20000.0)
                {
                    accSide[0] += std::norm(a[k]);
                    nSide[0]++;
                }
            }
            segs++;
        }
        if (segs == 0 || nSide[0] == 0U || nSide[1] == 0U)
            return s;
        double lowest = std::min(accSide[0] / (double)nSide[0], accSide[1] / (double)nSide[1]);

        noisePerSample = lowest / sumW2;
    }

    /* the signal is where the power in the channel is well over the noise in it (6 dB: 5 ms blocks hold about 70 independent
       samples of noise, which varies by about 12 % there) */
    const size_t blk = std::max((size_t)1U, (size_t)(0.005 * rate));
    const size_t nBlk = N / blk;
    std::vector<double> bp(nBlk);

    for (size_t b = 0U; b < nBlk; b++)
    {
        double p = 0.0;

        for (size_t i = 0U; i < blk; i++)
            p += std::norm(z[b * blk + i]);
        bp[b] = p / (double)blk;
    }
    if (nBlk < 20U)
        return s;

    const double gate = std::max(1e-12, 4.0 * noisePerSample * (2.0 * 7000.0 / rate));

    /* the last run of blocks above the gate, closing gaps of 2 blocks */
    long end = -1;
    long begin = -1;

    for (long b = (long)nBlk - 1; b >= 6; b--)
    {
        if (bp[(size_t)b] > gate)
        {
            end = b;
            break;
        }
    }
    if (end < 0)
        return s;
    begin = end;
    for (long b = end - 1, gap = 0; b >= 6; b--)
    {
        if (bp[(size_t)b] > gate)
        {
            begin = b;
            gap = 0;
        }
        else if (++gap > 2)
        {
            break;
        }
    }
    /* a little in from the edges: the carrier comes and goes there */
    begin += 2;
    end -= 1;
    if (end - begin < 20)
        return s;                                                   /* under 100 ms of signal */

    const size_t from = (size_t)begin * blk;
    const size_t to = (size_t)(end + 1) * blk;

    s.active = true;

    /* the frequency discriminator: Hz, positive above the channel centre */
    std::vector<double> d(to - from);

    for (size_t i = from; i < to; i++)
    {
        std::complex<double> p = z[i] * std::conj(z[i - 1]);

        d[i - from] = std::atan2(p.imag(), p.real()) * rate / (2.0 * M_PI);
    }

    /* what the receiver of a mode does after the discriminator: a low pass about the symbol rate (zero when the mode has no symbols) */
    std::vector<double> w(d.size(), 0.0);

    if (baud > 0.0)
    {
        static const size_t TAPS = 81U;
        std::vector<double> h = lowPass(TAPS, 0.95 * baud / rate);

        for (size_t i = TAPS; i < d.size(); i++)
        {
            double acc = 0.0;

            for (size_t k = 0U; k < TAPS; k++)
                acc += d[i - k] * h[k];
            w[i] = acc * RfMonitor::COUNTS_PER_HZ;
        }
        /* the filter's delay is (TAPS - 1) / 2 samples: remove it so the eye is centred like the one of the modem samples */
        const size_t delay = (TAPS - 1U) / 2U;

        w.erase(w.begin(), w.begin() + (long)(TAPS + delay));
    }
    else
    {
        for (size_t i = 0U; i < d.size(); i++)
            w[i] = d[i] * RfMonitor::COUNTS_PER_HZ;
    }

    double sq = 0.0;
    int pk = 0;

    for (size_t i = 0U; i < w.size(); i++)
    {
        sq += w[i] * w[i];
        pk = std::max(pk, (int)std::fabs(w[i]));
    }
    s.rms  = w.empty() ? 0.0 : std::sqrt(sq / (double)w.size());
    s.peak = pk;

    /* spectrum of the demodulated signal, as the other monitor makes it: 512 bins, 0 to 12 kHz */
    {
        const size_t FN = 2048U;
        std::vector<double> acc(FN / 2U, 0.0);
        int cnt = 0;

        const size_t sstride = w.size() > FN ? std::max(FN / 2U, (w.size() - FN) / 5U) : FN;

        for (size_t start = 0U; start + FN <= w.size(); start += sstride)
        {
            std::vector<std::complex<double>> a(FN);

            for (size_t i = 0U; i < FN; i++)
                a[i] = w[start + i] * (0.5 - 0.5 * std::cos(2.0 * M_PI * (double)i / (double)(FN - 1U)));
            fft(a);
            for (size_t i = 0U; i < FN / 2U; i++)
                acc[i] += std::norm(a[i]);
            cnt++;
        }
        s.spectrum.assign(512U, -30.0F);
        if (cnt > 0)
        {
            const double binHz = rate / (double)FN;

            for (size_t i = 0U; i < 512U; i++)
            {
                double pos = (double)i * 12000.0 / 512.0 / binHz;
                size_t lo = (size_t)pos;
                double f = pos - (double)lo;
                double v = acc[lo] * (1.0 - f) + acc[std::min(lo + 1U, FN / 2U - 1U)] * f;

                /* the other monitor's bins hold the 1024 point FFT of 24 kHz samples: the same scale up to a constant */
                s.spectrum[i] = (float)(10.0 * std::log10(v / (double)cnt / 4.0 + 1e-3));
            }
        }
    }

    RfMonitor::analyse(w, rate, baud, levels, nominal, sync, s);
    return s;
}

}
