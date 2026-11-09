/* Tests of the IQ monitor: a C4FM like signal of known deviation, frequency error and noise, as the bridges send it. */

#include "iq_monitor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace openmt;

static int failures = 0;

static void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

static const double RATE = 62500.0;

/* symbols (+-1, +-1/3, the frame sync every 864 symbols) -> instantaneous frequency in Hz (a raised cosine pulse of the symbols) */
static std::vector<double> frequencyOf(unsigned nSym, double outerHz, double offsetHz, bool invert, unsigned seed)
{
    std::mt19937 rng(seed);
    const double lv[4] = {1.0, 1.0 / 3.0, -1.0 / 3.0, -1.0};
    const unsigned long long sync = 0x5575F5FF77FFULL;
    std::vector<double> sym(nSym);

    for (unsigned i = 0U; i < nSym; i++)
        sym[i] = lv[rng() % 4U];
    for (unsigned at = 200U; at + 24U < nSym; at += 864U)
        for (int i = 0; i < 24; i++)
            sym[at + (unsigned)i] = ((unsigned)((sync >> (2 * (23 - i))) & 3U) == 1U) ? 1.0 : -1.0;

    const double sps = RATE / 4800.0;
    std::vector<double> f((size_t)((double)nSym * sps), 0.0);

    for (size_t n = 0U; n < f.size(); n++)
    {
        double y = 0.0;
        long c = (long)((double)n / sps);

        for (long k = c - 3; k <= c + 3; k++)
        {
            double t = ((double)n - sps * ((double)k + 0.5)) / sps;      /* in symbols from the symbol's centre */

            if (k < 0 || (size_t)k >= sym.size())
                continue;
            double sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(M_PI * t) / (M_PI * t);
            double a = 0.2 * t;
            double cosw = std::fabs(2.0 * a) >= 1.0 - 1e-9 ? 0.0 : std::cos(M_PI * a) / (1.0 - 4.0 * a * a);

            y += sym[(size_t)k] * sinc * cosw;
        }
        f[n] = (invert ? -y : y) * outerHz + offsetHz;
    }
    return f;
}

/* the IQ of a signal: silence (noise), then the signal, then noise again */
static std::vector<std::complex<float>> iqOf(const std::vector<double>& freq, double amp, double noise, size_t before, size_t after, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    std::vector<std::complex<float>> out;
    double ph = 0.0;

    for (size_t i = 0U; i < before; i++)
        out.push_back(std::complex<float>((float)(noise * g(rng)), (float)(noise * g(rng))));
    for (size_t i = 0U; i < freq.size(); i++)
    {
        ph += 2.0 * M_PI * freq[i] / RATE;
        out.push_back(std::complex<float>((float)(amp * std::cos(ph) + noise * g(rng)), (float)(amp * std::sin(ph) + noise * g(rng))));
    }
    for (size_t i = 0U; i < after; i++)
        out.push_back(std::complex<float>((float)(noise * g(rng)), (float)(noise * g(rng))));
    return out;
}

static void feedAll(IqMonitor& m, const std::vector<std::complex<float>>& iq, uint32_t& seq, bool dropOne = false)
{
    for (size_t at = 0U; at + 300U <= iq.size(); at += 300U)
    {
        IqFrame f;

        f.centreHz = 449900000U;
        f.sampleRate = 62500U;
        f.sequence = seq++;
        if (dropOne && at == 3000U)
            seq++;                                  /* one frame lost on the way */
        f.samples.assign(iq.begin() + (long)at, iq.begin() + (long)at + 300);
        m.feed(f);
    }
}

int main()
{
    /* the datagram */
    {
        uint8_t d[24 + 4 * 3];
        IqFrame f;

        std::fill(d, d + sizeof(d), (uint8_t)0);
        d[0] = 'I'; d[1] = 'Q'; d[2] = 'F'; d[3] = 'R'; d[4] = 1; d[5] = 1; d[6] = 3;
        d[8] = 0x80; d[9] = 0x8D; d[10] = 0x5B; d[11] = 0x1A;          /* 458,... any */
        d[12] = 0x24; d[13] = 0xF4;                                     /* 62500 */
        d[16] = 7;
        d[20] = 2;                                                      /* shift 2 */
        d[24] = 0x00; d[25] = 0x20;                                     /* I = 8192 */
        d[26] = 0x00; d[27] = 0xE0;                                     /* Q = -8192 */
        check(parseIqFrame(d, sizeof(d), f), "a datagram is read");
        check(f.sampleRate == 62500U && f.sequence == 7U && f.overload && f.samples.size() == 3U, "its header fields");
        check(std::fabs(f.samples[0].real() - 8192.0 / 32767.0 / 4.0) < 1e-6 && f.samples[0].imag() < 0.0F, "the shift is divided out");
        d[0] = 'S';
        check(!parseIqFrame(d, sizeof(d), f), "another frame type is not an IQ frame");
        d[0] = 'I';
        check(!parseIqFrame(d, 20U, f), "a short datagram is refused");
    }
    {
        /* 1.2 s of signal, -40 dBFS, 3 dB under the amplitude of the noise power ... : a clean one first */
        IqMonitor m;
        m.configure(4800.0, 4U, 1260.0, SyncKind::P25);
        uint32_t seq = 0U;
        feedAll(m, iqOf(frequencyOf(5760U, 1900.0, 350.0, false, 1U), 0.01, 0.0003, 9000U, 3000U, 2U), seq);
        MonitorSnapshot r = m.snapshot();

        std::printf("     outer %.1f Hz (true 1900), carrier %.1f Hz (true 350), hits %u, blur %.2f\n", r.outerHz, r.centre / 0.7, r.syncHits, r.blur);
        check(m.available() && r.fromIq && r.active, "the signal is found among the noise");
        check(r.levelsValid && r.syncLocked, "the frame syncs lock");
        check(std::fabs(r.outerHz - 1900.0) < 25.0, "the true deviation is read within 25 Hz (a 5 % of nothing: no scale assumed)");
        check(std::fabs(r.centre / 0.7 - 350.0) < 25.0, "the frequency error is read");
        check(std::fabs(r.outerPercent - 100.0 * 1900.0 * 0.7 / 1260.0) < 2.5, "and as a percentage of the nominal deviation");
        check(r.eyeTraces > 0U && r.eye.size() == (size_t)r.eyeTraces * r.eyePoints, "the eye is made");
        check(r.spectrum.size() == 512U, "the spectrum of the demodulated signal");
    }
    {
        /* a signal with the wrong sign (an inverted transmitter) */
        IqMonitor m;
        m.configure(4800.0, 4U, 1260.0, SyncKind::P25);
        uint32_t seq = 0U;
        feedAll(m, iqOf(frequencyOf(5760U, 1800.0, 0.0, true, 3U), 0.01, 0.0003, 9000U, 3000U, 4U), seq);
        MonitorSnapshot r = m.snapshot();

        check(r.syncLocked && r.inverted && std::fabs(r.outerHz - 1800.0) < 25.0, "an inverted signal is found and read");
    }
    {
        /* noise only: no signal */
        IqMonitor m;
        m.configure(4800.0, 4U, 1260.0, SyncKind::P25);
        uint32_t seq = 0U;
        feedAll(m, iqOf(std::vector<double>(), 0.0, 0.0003, 40000U, 0U, 5U), seq);
        MonitorSnapshot r = m.snapshot();

        check(!r.active && !r.levelsValid, "noise alone is not a signal");
    }
    {
        /* noisy: 10 dB of signal over the noise in the channel */
        IqMonitor m;
        m.configure(4800.0, 4U, 1260.0, SyncKind::P25);
        uint32_t seq = 0U;
        feedAll(m, iqOf(frequencyOf(5760U, 1800.0, -200.0, false, 6U), 0.01, 0.0035, 9000U, 3000U, 7U), seq);
        MonitorSnapshot r = m.snapshot();

        std::printf("     noisy: outer %.1f Hz (true 1800), carrier %.1f (true -200), hits %u, blur %.2f\n", r.outerHz, r.centre / 0.7, r.syncHits, r.blur);
        check(r.active && r.levelsValid, "a noisy signal is still read");
        check(std::fabs(r.outerHz - 1800.0) < 150.0, "its deviation within 150 Hz");
    }
    {
        /* frames lost on the way are counted */
        IqMonitor m;
        m.configure(4800.0, 4U, 1260.0, SyncKind::P25);
        uint32_t seq = 0U;
        feedAll(m, iqOf(frequencyOf(3000U, 1800.0, 0.0, false, 8U), 0.01, 0.0003, 9000U, 3000U, 9U), seq, true);
        MonitorSnapshot r = m.snapshot();

        check(r.iqLost == 1UL && r.iqFrames > 100UL, "a lost frame is counted");
    }
    {
        /* a mode with no symbols (FM): no symbol analysis, the spectrum is there */
        IqMonitor m;
        m.configure(0.0, 0U, 0.0, SyncKind::None);
        uint32_t seq = 0U;
        feedAll(m, iqOf(frequencyOf(5760U, 1800.0, 0.0, false, 10U), 0.01, 0.0003, 9000U, 3000U, 11U), seq);
        MonitorSnapshot r = m.snapshot();

        check(r.active && !r.levelsValid && r.spectrum.size() == 512U, "FM: no symbol analysis, the spectrum is there");
    }
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
