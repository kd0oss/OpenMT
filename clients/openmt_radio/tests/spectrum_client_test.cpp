/* Tests of the RF spectrum client: frames of the bridge's protocol, parsed and measured; a fake bridge answers requests. */

#include "iq_monitor.h"
#include "rf_spectrum.h"

#include <arpa/inet.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace openmt;

static int failures = 0;

static void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

/* a frame as the bridge makes it: noise at noiseDb, a tone (width of three bins) of toneDb at toneHz */
static std::vector<uint8_t> makeFrame(double toneHz, double toneDb, double noiseDb, bool overload, int adcPeak, unsigned clipHundredths)
{
    const unsigned n = 1024U;
    std::vector<uint8_t> f(20U + n, 0);

    std::memcpy(f.data(), "SPEC", 4);
    f[4] = 1;
    f[5] = overload ? 1 : 0;
    f[6] = (uint8_t)(n & 0xFF);
    f[7] = (uint8_t)(n >> 8);
    uint32_t c = 449900000U, sp = 125000U;
    for (int i = 0; i < 4; i++)
    {
        f[8 + i]  = (uint8_t)(c >> (8 * i));
        f[12 + i] = (uint8_t)(sp >> (8 * i));
    }
    f[16] = (uint8_t)adcPeak;
    f[18] = (uint8_t)(clipHundredths & 0xFF);
    f[19] = (uint8_t)(clipHundredths >> 8);
    int toneBin = 512 + (int)std::lround(toneHz / (125000.0 / 1024.0));
    for (unsigned k = 0U; k < n; k++)
    {
        double db = noiseDb;

        if ((int)k >= toneBin - 1 && (int)k <= toneBin + 1)
            db = (int)k == toneBin ? toneDb : toneDb - 6.0;
        f[20 + k] = (uint8_t)std::min(255.0, std::max(0.0, std::round(-db * 2.0)));
    }
    return f;
}

int main()
{
    {
        std::vector<uint8_t> f = makeFrame(2000.0, -20.0, -90.0, false, 40, 0);
        RfFrame r;
        check(parseRfFrame(f.data(), f.size(), r, NULL), "a frame is parsed");
        check(r.centreHz == 449900000U && r.spanHz == 125000U && r.db.size() == 1024U, "centre, span and bins");
        check(std::fabs(r.peakOffsetHz - 2000.0) < 130.0, "the signal is found at +2 kHz");
        check(std::fabs(r.peakDb - (-20.0)) < 0.6, "its level is -20 dBFS");
        check(std::fabs(r.noiseDb - (-90.0)) < 0.6, "the noise floor is -90 dBFS");
        check(std::fabs(r.snrDb - 70.0) < 1.0, "the signal to noise ratio is 70 dB");
        /* a tone of -20 dBFS, with the two bins beside it 6 dB lower, adds up to about -20 + 10 log(1 + 2 x 0.25) - 1.76 dB */
        check(std::fabs(r.channelDb - (-20.0 + 10.0 * std::log10(1.5) - 1.76)) < 0.8, "the channel power adds the bins and takes off the window's noise bandwidth");
        check(!r.overload && r.adcPeak == 40, "no overload, ADC peak 40");
        std::vector<uint8_t> o = makeFrame(0.0, -3.0, -80.0, true, 127, 1250);
        RfFrame ro;
        check(parseRfFrame(o.data(), o.size(), ro, NULL) && ro.overload && ro.adcPeak == 127 && std::fabs(ro.adcClipPercent - 12.5) < 0.01, "overload and the share at full scale (12.5 %)");
        std::vector<uint8_t> bad = f;
        bad[0] = 'X';
        check(!parseRfFrame(bad.data(), bad.size(), r, NULL), "a datagram that is not a frame is refused");
        check(!parseRfFrame(f.data(), 100, r, NULL), "a short frame is refused");
        bad = f;
        bad[4] = 2;
        check(!parseRfFrame(bad.data(), bad.size(), r, NULL), "an unknown version is refused");
    }
    {
        /* a fake bridge: answers each request with a frame, counts the requests */
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(28061);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        bind(fd, (struct sockaddr*)&a, sizeof(a));
        struct timeval tv = {0, 100000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        std::atomic<bool> stop(false);
        std::atomic<int> requests(0);
        std::thread server([&]()
        {
            struct sockaddr_in peer;
            socklen_t pl = sizeof(peer);
            bool have = false;
            int sent = 0;
            while (!stop)
            {
                char buf[16];
                ssize_t r = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr*)&peer, &pl);
                if (r >= 4 && std::memcmp(buf, "SPEC", 4) == 0)
                {
                    requests++;
                    have = true;
                }
                if (have)
                {
                    std::vector<uint8_t> f = makeFrame(-3000.0, -30.0, -85.0, false, 30, 0);
                    sendto(fd, f.data(), f.size(), 0, (struct sockaddr*)&peer, pl);
                    sent++;
                }
            }
        });

        SpectrumClient c;
        std::string err;
        check(c.start("127.0.0.1", 28061, err), "the client starts");
        usleep(1500000);
        check(requests == 0 && !c.snapshot().valid, "nothing is asked for while the tab is not shown");
        c.setWanted(true);
        usleep(2500000);
        RfSpectrumSnapshot s = c.snapshot();
        check(requests >= 2 && requests <= 4, "it asks about once a second");
        check(s.valid && std::fabs(s.frame.peakOffsetHz - (-3000.0)) < 130.0, "frames come in and the signal is at -3 kHz");
        check(s.history.size() > 3U && s.history.size() <= SpectrumClient::HistoryRows && s.history[0].size() == 1024U, "the history for the waterfall fills");
        c.setWanted(false);
        int before = requests;
        usleep(2500000);
        check(requests == before, "it stops asking when the tab is hidden");
        c.stop();
        stop = true;
        server.join();
        close(fd);
    }
    {
        /* a fake bridge that serves IQ: "IQST" starts frames, and the client hands them to the IQ monitor */
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(28062);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        bind(fd, (struct sockaddr*)&a, sizeof(a));
        struct timeval tv = {0, 5000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        std::atomic<bool> stop(false);
        std::atomic<int> iqRequests(0);
        std::atomic<int> specRequests(0);
        std::thread server([&]()
        {
            struct sockaddr_in peer;
            socklen_t pl = sizeof(peer);
            bool have = false;
            uint32_t seq = 0U;
            long long last = 0;
            while (!stop)
            {
                char buf[16];
                ssize_t r = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr*)&peer, &pl);
                if (r >= 4 && std::memcmp(buf, "IQST", 4) == 0)
                {
                    iqRequests++;
                    have = true;
                    last = 0;
                }
                if (r >= 4 && std::memcmp(buf, "SPEC", 4) == 0)
                    specRequests++;
                if (have)
                {
                    uint8_t f[24 + 4 * 300];
                    std::memset(f, 0, sizeof(f));
                    std::memcpy(f, "IQFR", 4);
                    f[4] = 1;
                    f[6] = 300 & 0xFF;
                    f[7] = 300 >> 8;
                    f[12] = 0x24; f[13] = 0xF4;                 /* 62500 */
                    f[16] = (uint8_t)(seq & 0xFF); f[17] = (uint8_t)((seq >> 8) & 0xFF);
                    seq++;
                    for (int k = 0; k < 300; k++)
                    {
                        int16_t re = (int16_t)(10000 * std::cos(0.1 * k)), im = (int16_t)(10000 * std::sin(0.1 * k));
                        std::memcpy(f + 24 + 4 * k, &re, 2);
                        std::memcpy(f + 26 + 4 * k, &im, 2);
                    }
                    sendto(fd, f, sizeof(f), 0, (struct sockaddr*)&peer, pl);
                    usleep(4000);
                    (void)last;
                }
            }
        });

        SpectrumClient c;
        IqMonitor iq;
        std::string err;
        c.setIqSink(&iq);
        check(c.start("127.0.0.1", 28062, err), "the IQ client starts");
        usleep(1300000);
        check(iqRequests == 0 && !iq.available(), "no IQ is asked for until it is wanted");
        c.setIqWanted(true);
        usleep(2500000);
        check(iqRequests >= 2 && iqRequests <= 4, "IQ is asked for about once a second");
        check(iq.available() && iq.framesReceived() > 100UL, "the IQ frames reach the IQ monitor");
        check(specRequests == 0, "and no spectrum is asked for while only IQ is wanted");
        c.setIqWanted(false);
        int before = iqRequests;
        usleep(2500000);
        check(iqRequests == before, "it stops asking for IQ when it is not wanted");
        c.stop();
        stop = true;
        server.join();
        close(fd);
    }
    {
        /* automatic port: two candidate ports, only the second has a bridge; the client finds it and stays with it */
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(28065);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        bind(fd, (struct sockaddr*)&a, sizeof(a));
        struct timeval tv = {0, 100000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        std::atomic<bool> stop(false);
        std::atomic<int> requests(0);
        std::thread server([&]()
        {
            struct sockaddr_in peer;
            socklen_t pl = sizeof(peer);
            bool have = false;
            while (!stop)
            {
                char buf[16];
                ssize_t r = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr*)&peer, &pl);
                if (r >= 4 && std::memcmp(buf, "SPEC", 4) == 0)
                {
                    requests++;
                    have = true;
                }
                if (have)
                {
                    std::vector<uint8_t> f = makeFrame(1000.0, -30.0, -85.0, false, 30, 0);
                    sendto(fd, f.data(), f.size(), 0, (struct sockaddr*)&peer, pl);
                }
            }
        });

        SpectrumClient c;
        std::string err;
        check(c.startAuto("127.0.0.1", {28064U, 28065U}, err), "the automatic client starts");
        check(c.target().find("trying ports 28064, 28065") != std::string::npos, "it says that it is trying both ports");
        c.setWanted(true);
        usleep(1800000);
        check(c.answeringPort() == 28065U && c.target() == "127.0.0.1:28065", "it finds the port that answers");
        check(c.snapshot().valid, "and frames come in");
        c.stop();
        stop = true;
        server.join();
        close(fd);
    }
    {
        SpectrumClient c;
        std::string err;
        check(c.start("127.0.0.1", 0, err) && !c.snapshot().valid, "port 0 turns it off");
    }
    std::printf("%s\n", failures == 0 ? "all passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
