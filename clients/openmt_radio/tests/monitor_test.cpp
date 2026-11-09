/* Tests of the signal monitor: signals of known deviation, with and without noise. */

#include "rf_monitor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace openmt;

static int failures = 0;

static void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

/* symbols at 4800 baud shaped by a raised cosine pulse (five samples per symbol, as the modes transmit) */
static std::vector<int16_t> make(unsigned nSym, const double* lv, unsigned nLv, double scale, double noise, unsigned seed)
{
    std::srand(seed);
    std::vector<double> sym(nSym);

    for (unsigned i = 0U; i < nSym; i++)
        sym[i] = lv[std::rand() % nLv] * scale;

    std::vector<int16_t> out((size_t)nSym * 5U);

    for (size_t n = 0U; n < out.size(); n++)
    {
        double y = 0.0;

        for (int j = -3; j <= 3; j++)
        {
            long k = (long)(n / 5U) + j;
            double t = ((double)n - 5.0 * (double)k) / 5.0;    /* time from the symbol's centre, in symbols */

            if (k < 0 || (unsigned long)k >= nSym)
                continue;
            double sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(M_PI * t) / (M_PI * t);
            double a = 0.2 * t;
            double cosw = std::fabs(2.0 * a) >= 1.0 - 1e-9 ? 0.0 : std::cos(M_PI * a) / (1.0 - 4.0 * a * a);

            y += sym[(size_t)k] * sinc * cosw;
        }
        y += noise * ((std::rand() / (double)RAND_MAX) - 0.5) * 3.4641;   /* uniform, standard deviation = noise */
        out[n] = (int16_t)std::lround(std::max(-32768.0, std::min(32767.0, y)));
    }
    return out;
}

static const double L4[4] = {1260.0, 420.0, -420.0, -1260.0};
static const double L2[2] = {841.0, -841.0};

int main()
{
    {
        RfMonitor m;
        m.configure(4800.0, 4U, 1260.0);
        std::vector<int16_t> s = make(4000U, L4, 4U, 1.0, 20.0, 1U);
        m.feed(s.data(), s.size());
        MonitorSnapshot r = m.snapshot();
        check(r.active, "a 4 level signal is seen");
        check(r.levelsValid && std::fabs(r.outer - 1260.0) < 40.0, "the outer level of a nominal signal is 1260 counts");
        check(std::fabs(r.outerPercent - 100.0) < 3.0, "a nominal signal reads 100 % of the nominal deviation");
        check(std::fabs(r.outerHz - 1800.0) < 60.0, "1260 counts read as 1800 Hz");
        check(r.blur < 0.15, "a clean signal has well separated levels");
        check(!r.clipped, "no clipping on a normal signal");
        check(r.eye.size() == (size_t)r.eyeTraces * r.eyePoints && r.eyeTraces > 0U, "the eye has its traces");
        unsigned total = 0U;
        for (size_t i = 0U; i < r.hist.size(); i++)
            total += r.hist[i];
        check(total > 1000U, "the histogram is filled");
        check(r.spectrum.size() == 512U, "the spectrum has 512 bins");
    }
    {
        RfMonitor m;
        m.configure(4800.0, 4U, 1260.0);
        std::vector<int16_t> s = make(4000U, L4, 4U, 0.28, 20.0, 2U);       /* the capture of the P25 radio: 28 % */
        m.feed(s.data(), s.size());
        MonitorSnapshot r = m.snapshot();
        check(std::fabs(r.outerPercent - 28.0) < 3.0, "a signal with 28 % of the nominal deviation reads 28 %");
    }
    {
        RfMonitor m;
        m.configure(4800.0, 4U, 1260.0);
        std::vector<int16_t> s = make(4000U, L4, 4U, 1.0, 400.0, 3U);
        m.feed(s.data(), s.size());
        MonitorSnapshot r = m.snapshot();
        check(r.blur > 0.25, "a noisy signal shows blurred levels");
        check(r.approximate && std::fabs(r.outer - 1260.0) < 160.0, "with noise the deviation is flagged as approximate and within 13 %");
    }
    {
        RfMonitor m;
        m.configure(4800.0, 4U, 1260.0);
        std::vector<int16_t> s = make(4000U, L4, 4U, 1.0, 20.0, 4U);
        for (size_t i = 0U; i < s.size(); i++)
            s[i] = (int16_t)std::max(-32768.0, std::min(32767.0, s[i] * 40.0));
        m.feed(s.data(), s.size());
        check(m.snapshot().clipped, "samples at the limit are reported as clipping");
    }
    {
        RfMonitor m;
        m.configure(4800.0, 2U, 841.0);
        std::vector<int16_t> s = make(4000U, L2, 2U, 1.0, 15.0, 5U);
        m.feed(s.data(), s.size());
        MonitorSnapshot r = m.snapshot();
        check(r.levelsValid && std::fabs(r.outer - 841.0) < 40.0 && std::fabs(r.outerPercent - 100.0) < 5.0, "a 2 level (GMSK) signal reads 841 counts");
    }
    {
        RfMonitor m;
        m.configure(4800.0, 4U, 1260.0);
        std::vector<int16_t> s(24000U, 3);
        m.feed(s.data(), s.size());
        MonitorSnapshot r = m.snapshot();
        check(!r.active && !r.levelsValid, "silence is not a signal");
    }
    {
        RfMonitor m;
        m.configure(0.0, 0U, 0.0);
        std::vector<int16_t> s = make(4000U, L4, 4U, 1.0, 20.0, 6U);
        m.feed(s.data(), s.size());
        MonitorSnapshot r = m.snapshot();
        check(r.active && !r.levelsValid && r.spectrum.size() == 512U, "FM: no symbol analysis, the spectrum is there");
    }
    std::printf("%s\n", failures == 0 ? "all passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
