/* Tests of the radio core, with no sound card, no dongle and no Modem_Host: a fake Modem_Host on a local socket
 * receives a transmitted call, and the samples it received are sent back as a received call. */

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <pty.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "radio_engine.h"
#include "resampler.h"
#include "ambe_vocoder.h"

extern "C"
{
#include "dmr_fec.h"
#include "dstar_fec.h"
#include <fm_dsp.h>
}

using namespace openmt;

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                              std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static uint64_t nowMsTest()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* ------------------------------------------------------------------------------------------------ fake Modem_Host */

class FakeHost
{
public:
    FakeHost() : listenFd(-1), connFd(-1), port(0), setModes(0), setIdles(0), seots(0), gotMode(false), done(false)
    {
        std::memset(mode, 0x00, sizeof(mode));

        listenFd = socket(AF_INET, SOCK_STREAM, 0);

        int on = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        struct sockaddr_in a;
        std::memset(&a, 0x00, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        bind(listenFd, (struct sockaddr*)&a, sizeof(a));
        listen(listenFd, 1);

        socklen_t len = sizeof(a);
        getsockname(listenFd, (struct sockaddr*)&a, &len);
        port = ntohs(a.sin_port);

        thread = std::thread(&FakeHost::run, this);
    }

    ~FakeHost()
    {
        done = true;
        if (connFd >= 0)
            shutdown(connFd, SHUT_RDWR);
        if (listenFd >= 0)
            shutdown(listenFd, SHUT_RDWR);
        if (thread.joinable())
            thread.join();
        if (connFd >= 0)
            close(connFd);
        if (listenFd >= 0)
            close(listenFd);
    }

    /* send samples to the client as the modem host does: 96 samples a packet */
    void inject(const std::vector<int16_t>& samples)
    {
        for (size_t at = 0U; at < samples.size(); at += 96U)
        {
            uint8_t pkt[200];

            std::memset(pkt, 0x00, sizeof(pkt));
            pkt[0] = 0x61;
            pkt[2] = 200;
            pkt[3] = 4;
            std::memcpy(pkt + 4, "SAMP", 4);
            for (size_t i = 0U; i < 96U && at + i < samples.size(); i++)
            {
                pkt[8 + i * 2U] = (uint8_t)(samples[at + i] & 0xFF);
                pkt[9 + i * 2U] = (uint8_t)((samples[at + i] >> 8) & 0xFF);
            }
            send(connFd, pkt, sizeof(pkt), MSG_NOSIGNAL);
        }
    }

    int                  listenFd, connFd;
    unsigned int         port;
    std::mutex           mutex;
    std::vector<int16_t> samples;
    std::vector<char>    order;         /* M: SETMODE, S: sample packet, E: SEOT, I: SETIDLE */
    unsigned int         setModes, setIdles, seots;
    uint8_t              mode[84];
    bool                 gotMode;
    std::atomic<bool>    done;

private:
    bool readFully(uint8_t* p, size_t n)
    {
        size_t got = 0U;

        while (got < n)
        {
            ssize_t r = recv(connFd, p + got, n - got, 0);
            if (r <= 0)
                return false;
            got += (size_t)r;
        }
        return true;
    }

    void run()
    {
        connFd = accept(listenFd, NULL, NULL);
        if (connFd < 0)
            return;

        uint8_t b[512];

        while (!done)
        {
            if (!readFully(b, 1) || b[0] != 0x61 || !readFully(b + 1, 3))
                break;

            unsigned int len = (unsigned int)((b[1] << 8) + b[2]);

            if (len < 5U || len > sizeof(b) || (len > 4U && !readFully(b + 4, len - 4U)))
                break;

            std::lock_guard<std::mutex> lock(mutex);

            if (len == 5U && b[3] == 1U && b[4] == 0x02)
            {
                setModes++;
                order.push_back('M');
            }
            else if (len == 5U && b[3] == 1U && b[4] == 0x03)
            {
                setIdles++;
                order.push_back('I');
            }
            else if (std::memcmp(b + 4, "MODE", 4) == 0 && len == 84U)
            {
                std::memcpy(mode, b, 84);
                gotMode = true;
            }
            else if (std::memcmp(b + 4, "SAMP", 4) == 0)
            {
                for (unsigned int i = 8U; i + 1U < len; i += 2U)
                    samples.push_back((int16_t)(b[i] | (b[i + 1U] << 8)));
                order.push_back('S');
            }
            else if (std::memcmp(b + 4, "SEOT", 4) == 0)
            {
                seots++;
                order.push_back('E');
            }
        }
    }

    std::thread thread;
};

/* ------------------------------------------------------------------------------------------------ resampler    */

static double rms(const std::vector<int16_t>& v, size_t from)
{
    double e = 0.0;
    size_t n = 0U;

    for (size_t i = from; i < v.size(); i++, n++)
        e += (double)v[i] * v[i];
    return n ? std::sqrt(e / n) : 0.0;
}

static void testResampler()
{
    for (unsigned int factor = 1U; factor <= 6U; factor += 5U)      /* 8 kHz and 48 kHz */
    {
        Interpolator up(factor);
        Decimator down(factor);
        std::vector<int16_t> in, hi, back;

        for (unsigned int i = 0U; i < 8000U; i++)
            in.push_back((int16_t)std::lrint(8000.0 * std::sin(2.0 * M_PI * 1000.0 * i / 8000.0)));

        up.process(in.data(), in.size(), hi);
        CHECK(hi.size() == in.size() * factor, "interpolator made %zu samples from %zu", hi.size(), in.size());

        down.process(hi.data(), hi.size(), back);
        CHECK(back.size() == in.size(), "decimator made %zu samples from %zu", back.size(), hi.size());

        double inRms = rms(in, 1000U), outRms = rms(back, 1000U);
        CHECK(std::fabs(outRms / inRms - 1.0) < 0.1, "1 kHz tone gain at factor %u: %.3f", factor, outRms / inRms);
    }

    /* a 10 kHz tone at 48 kHz is far above the speech band and must be removed */
    Decimator down(6U);
    std::vector<int16_t> hi, back;
    for (unsigned int i = 0U; i < 48000U; i++)
        hi.push_back((int16_t)std::lrint(8000.0 * std::sin(2.0 * M_PI * 10000.0 * i / 48000.0)));
    down.process(hi.data(), hi.size(), back);
    CHECK(rms(back, 200U) < 200.0, "10 kHz tone not removed: rms %.0f", rms(back, 200U));

    std::printf("resampler ok\n");
}

/* ------------------------------------------------------------------------------------------------ NXDN loopback */

struct Recorder
{
    std::mutex            mutex;
    std::vector<CallInfo> calls;
    std::vector<int16_t>  rx;
    std::vector<std::string> logs;
};

static bool waitFor(const std::function<bool()>& cond, int ms)
{
    for (int t = 0; t < ms; t += 10)
    {
        if (cond())
            return true;
        sleepMs(10);
    }
    return cond();
}

static void testNxdnLoopback()
{
    FakeHost host;
    Recorder rec;
    RadioEngine engine;
    EngineSettings s;

    s.noVocoder           = true;
    s.link.host           = "127.0.0.1";
    s.link.basePort       = host.port;
    s.link.modem          = 1;
    s.link.txLevel        = 77;
    s.link.txAheadMs      = 0;              /* not real time */
    s.nxdn.ran            = 5;
    s.nxdn.srcId          = 1234;
    s.nxdn.dstId          = 9;
    s.nxdn.group          = true;
    s.nxdn.txDelayMs      = 100;
    s.nxdn.modeHangMs     = 200;

    engine.events.rxAudio = [&rec](const int16_t* p, size_t n)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.rx.insert(rec.rx.end(), p, p + n);
    };
    engine.events.call = [&rec](const CallInfo& c)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.calls.push_back(c);
    };
    engine.events.log = [&rec](const std::string& t)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.logs.push_back(t);
    };

    std::string error;
    bool started = engine.start(s, error);
    CHECK(started, "start: %s", error.c_str());
    if (!started)
        return;

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.gotMode; }, 2000), "no MODE packet");
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(std::strcmp((const char*)host.mode + 8, "NXDN") == 0, "mode name '%s'", (const char*)host.mode + 8);
        CHECK(std::strcmp((const char*)host.mode + 19, "4FSK") == 0, "modem type '%s'", (const char*)host.mode + 19);
        CHECK(host.mode[25] == 77, "tx level %u", host.mode[25]);
        CHECK(host.mode[2] == 84 && host.mode[3] == 4, "MODE packet length bytes");
    }

    /* 20 frames of speech: every 160 sample block carries its own 7 bytes in the low bytes of its first samples */
    const unsigned int frames = 20U, blocks = frames * 4U;
    std::vector<int16_t> pcm(blocks * 160U, 0);
    std::vector<std::vector<uint8_t>> expected;

    for (unsigned int b = 0U; b < blocks; b++)
    {
        uint8_t net[7], chip[9];

        for (unsigned int k = 0U; k < 7U; k++)
            net[k] = (uint8_t)(0x11U * (k + 1U) + 37U * b);
        net[6] &= 0x80U;                    /* 49 parameter bits */

        nxdnAudioChipEncode(NXDN_CHIP_AIR, net, chip);      /* a block with valid FEC, as the chip makes it */

        std::vector<uint8_t> bytes(chip, chip + 9);
        for (unsigned int k = 0U; k < 9U; k++)
            pcm[b * 160U + k] = (int16_t)bytes[k];          /* the stand in vocoder carries it in the samples */
        expected.push_back(bytes);
    }

    engine.ptt(true);
    /* the microphone delivers speech in real time, 20 ms at a time */
    for (size_t pos = 0U; pos < pcm.size(); pos += 160U)
    {
        engine.pushMic(pcm.data() + pos, 160U);
        sleepMs(20);
    }
    engine.ptt(false);

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.seots >= 1U; }, 10000),
          "no end of transmission");
    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.setIdles >= 2U; }, 3000),
          "the modem was not handed back after the hang time");

    std::vector<int16_t> sent;
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(host.setModes == 1U, "SETMODE sent %u times", host.setModes);
        /* the first idle is the one sent when connecting (a mode held by an earlier session); SETMODE follows it */
        CHECK(host.order.size() >= 2U && host.order[0] == 'I' && host.order[1] == 'M', "the connect release and SETMODE did not come first");
        CHECK(host.seots == 1U, "%u SEOT", host.seots);
        sent = host.samples;
    }

    /* preamble (100 ms) + header, 20 voice frames, trailer: 80 ms each, plus the filter tail */
    size_t expectSamples = (size_t)(100U * 24U) + (size_t)(frames + 2U) * 1920U;
    CHECK(sent.size() >= expectSamples && sent.size() < expectSamples + 400U, "sent %zu samples, expected about %zu",
          sent.size(), expectSamples);

    int peak = 0;
    for (size_t i = 0U; i < sent.size(); i++)
        if (std::abs((int)sent[i]) > peak)
            peak = std::abs((int)sent[i]);
    CHECK(peak > 700 && peak < 2000, "peak level %d counts (the outer symbol is 735, the shaped pulses overshoot)", peak);

    {
        std::lock_guard<std::mutex> l(rec.mutex);
        bool sawStart = false, sawEnd = false;

        for (size_t i = 0U; i < rec.calls.size(); i++)
        {
            sawStart |= rec.calls[i].event == CALL_TX_START;
            sawEnd   |= rec.calls[i].event == CALL_TX_END;
        }
        CHECK(sawStart && sawEnd, "transmit events missing");
    }

    /* Now the same samples come back as a received call: some silence, the call, some silence */
    std::vector<int16_t> air(2400U, 0);
    air.insert(air.end(), sent.begin(), sent.end());
    air.insert(air.end(), 4800U, 0);
    host.inject(air);

    CHECK(waitFor([&rec]()
    {
        std::lock_guard<std::mutex> l(rec.mutex);
        for (size_t i = 0U; i < rec.calls.size(); i++)
            if (rec.calls[i].event == CALL_RX_END)
                return true;
        return false;
    }, 5000), "the received call did not end");

    std::unique_lock<std::mutex> l(rec.mutex);
    bool gotStart = false, gotIds = false;
    float ber = 0.0F;

    for (size_t i = 0U; i < rec.calls.size(); i++)
    {
        const CallInfo& c = rec.calls[i];

        if (c.event == CALL_RX_START)
        {
            gotStart = true;
            if (c.src == 1234U && c.dst == 9U)
                gotIds = true;
        }
        if (c.event == CALL_RX_INFO && c.src == 1234U && c.dst == 9U)
            gotIds = true;
        if (c.event == CALL_RX_END)
            ber = c.ber;
    }
    CHECK(gotStart, "no received call start");
    CHECK(gotIds, "the call ids 1234 -> 9 were not received");
    CHECK(ber < 1.0F, "bit error rate %.2f%% on a clean loopback", ber);

    /* the speech blocks come back unchanged; find where they start (the first frame may be needed to lock) */
    size_t rxBlocks = rec.rx.size() / 160U;
    int firstMatch = -1;

    for (size_t r = 0U; r < rxBlocks && firstMatch < 0; r++)
    {
        for (unsigned int b = 0U; b < blocks; b++)
        {
            bool same = true;

            for (unsigned int k = 0U; k < 9U && same; k++)
                same = (rec.rx[r * 160U + k] == (int16_t)expected[b][k]);
            if (same && b == 0U)
            {
                firstMatch = (int)r;
                break;
            }
        }
    }

    size_t good = 0U;
    for (size_t r = 0U; r < rxBlocks; r++)
    {
        for (unsigned int b = 0U; b < blocks; b++)
        {
            bool same = true;

            for (unsigned int k = 0U; k < 9U && same; k++)
                same = (rec.rx[r * 160U + k] == (int16_t)expected[b][k]);
            if (same)
            {
                good++;
                break;
            }
        }
    }

    std::printf("nxdn loopback: sent %zu samples, received %zu speech blocks, %zu of %u identical (first at %d)\n",
                sent.size(), rxBlocks, good, blocks, firstMatch);
    CHECK(good >= blocks - 8U, "only %zu of %u speech blocks came back", good, blocks);

    /* and in order: block r of the received call is block firstMatch + r of the sent one */
    if (firstMatch >= 0)
    {
        size_t inOrder = 0U;

        for (size_t r = (size_t)firstMatch; r < rxBlocks && r - firstMatch < blocks; r++)
        {
            bool same = true;

            for (unsigned int k = 0U; k < 9U && same; k++)
                same = (rec.rx[r * 160U + k] == (int16_t)expected[r - firstMatch][k]);
            if (same)
                inOrder++;
        }
        CHECK(inOrder >= blocks - 8U, "only %zu speech blocks are in order", inOrder);
    }

    l.unlock();                         /* the watcher thread of the link may be logging into rec */
    engine.stop();
    CHECK(!engine.running(), "still running");
    std::printf("nxdn loopback ok\n");
}

/* A 72 bit block whose Golay words are valid, as a chip makes it, in the 'air' arrangement (the stand in vocoder
   hands blocks through unchanged); the 9 bytes are what the test sends through the stand in */
static void makeDmrBlock(unsigned int seed, uint8_t* block)
{
    unsigned int data = (seed * 211U + 17U) & 0xFFFU;
    unsigned int datb = (seed * 97U + 5U) & 0xFFFU;
    unsigned int p    = AMBE_PRNG_TABLE[data] >> 1;
    unsigned int a    = golay24128_encode24128(data);
    unsigned int bw   = (golay24128_encode23127(datb) >> 1) ^ p;
    unsigned int c    = (seed * 40503U + 12345U) & 0x1FFFFFFU;

    std::memset(block, 0x00, 9);
    for (unsigned int i = 0U; i < 24U; i++)
        DMR_FEC_WRITE(block, DMR_FEC_A[i], (a >> (23U - i)) & 1U);
    for (unsigned int i = 0U; i < 23U; i++)
        DMR_FEC_WRITE(block, DMR_FEC_B[i], (bw >> (22U - i)) & 1U);
    for (unsigned int i = 0U; i < 25U; i++)
        DMR_FEC_WRITE(block, DMR_FEC_C[i], (c >> (24U - i)) & 1U);
}

static void testDmrLoopback()
{
    FakeHost host;
    Recorder rec;
    RadioEngine engine;
    EngineSettings s;

    s.mode                = MODE_DMR;
    s.noVocoder           = true;
    s.link.host           = "127.0.0.1";
    s.link.basePort       = host.port;
    s.link.modem          = 1;
    s.link.txAheadMs      = 0;
    s.dmr.colorCode       = 1;
    s.dmr.srcId           = 1234;
    s.dmr.dstId           = 9;
    s.dmr.group           = true;
    s.dmr.txDelayMs       = 100;
    s.dmr.modeHangMs      = 200;

    engine.events.rxAudio = [&rec](const int16_t* p, size_t n)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.rx.insert(rec.rx.end(), p, p + n);
    };
    engine.events.call = [&rec](const CallInfo& c)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.calls.push_back(c);
    };
    engine.events.log = [&rec](const std::string& t)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.logs.push_back(t);
    };

    std::string error;
    bool started = engine.start(s, error);
    CHECK(started, "dmr start: %s", error.c_str());
    if (!started)
        return;

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.gotMode; }, 2000), "no MODE packet");
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(std::strcmp((const char*)host.mode + 8, "DMR") == 0, "mode name '%s'", (const char*)host.mode + 8);
        CHECK(std::strcmp((const char*)host.mode + 19, "4FSK") == 0, "modem type '%s'", (const char*)host.mode + 19);
    }

    /* 20 bursts of speech, 60 ms each */
    const unsigned int bursts = 20U, blocks = bursts * 3U;
    std::vector<int16_t> pcm(blocks * 160U, 0);
    std::vector<std::vector<uint8_t>> expected;

    for (unsigned int b = 0U; b < blocks; b++)
    {
        uint8_t chip[9];

        makeDmrBlock(b + 1U, chip);
        for (unsigned int k = 0U; k < 9U; k++)
            pcm[b * 160U + k] = (int16_t)chip[k];
        expected.push_back(std::vector<uint8_t>(chip, chip + 9));
    }

    engine.ptt(true);
    for (size_t pos = 0U; pos < pcm.size(); pos += 160U)
    {
        engine.pushMic(pcm.data() + pos, 160U);
        sleepMs(20);
    }
    sleepMs(50);
    engine.ptt(false);

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.seots >= 1U; }, 10000),
          "no end of transmission");
    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.setIdles >= 2U; }, 3000),
          "the modem was not handed back after the hang time");

    std::vector<int16_t> sent;
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(host.setModes == 1U, "SETMODE sent %u times", host.setModes);
        CHECK(host.seots == 1U, "%u SEOT", host.seots);
        sent = host.samples;
    }

    /* preamble (100 ms), 2 headers, 20 voice bursts, terminator: 60 ms each, plus the filter tail */
    size_t expectSamples = (size_t)(100U * 24U / 96U * 96U) + (size_t)(2U + bursts + 1U) * 1440U;
    CHECK(sent.size() >= expectSamples && sent.size() < expectSamples + 400U, "sent %zu samples, expected about %zu",
          sent.size(), expectSamples);

    int peak = 0;
    for (size_t i = 0U; i < sent.size(); i++)
        if (std::abs((int)sent[i]) > peak)
            peak = std::abs((int)sent[i]);
    CHECK(peak > 1300 && peak < 3000, "peak level %d counts (the outer symbol is 1362)", peak);

    /* the same samples come back as a received call */
    std::vector<int16_t> air(2400U, 0);
    air.insert(air.end(), sent.begin(), sent.end());
    air.insert(air.end(), 4800U, 0);
    host.inject(air);

    CHECK(waitFor([&rec]()
    {
        std::lock_guard<std::mutex> l(rec.mutex);
        for (size_t i = 0U; i < rec.calls.size(); i++)
            if (rec.calls[i].event == CALL_RX_END || rec.calls[i].event == CALL_RX_LOST)
                return true;
        return false;
    }, 5000), "the received call did not end");

    std::unique_lock<std::mutex> l(rec.mutex);
    bool gotIds = false, ended = false;
    float ber = 100.0F;

    for (size_t i = 0U; i < rec.calls.size(); i++)
    {
        const CallInfo& c = rec.calls[i];

        if ((c.event == CALL_RX_START || c.event == CALL_RX_INFO) && c.src == 1234U && c.dst == 9U && c.group)
            gotIds = true;
        if (c.event == CALL_RX_END)
        {
            ended = true;
            ber = c.ber;
        }
    }
    CHECK(gotIds, "the call ids 1234 -> 9 (group) were not received");
    CHECK(ended, "the call did not end with its terminator");
    CHECK(ber < 1.0F, "bit error rate %.2f%% on a clean loopback", ber);

    size_t rxBlocks = rec.rx.size() / 160U;
    size_t inOrder = 0U;

    for (size_t r = 0U; r < rxBlocks && r < blocks; r++)
    {
        bool same = true;

        for (unsigned int k = 0U; k < 9U && same; k++)
            same = (rec.rx[r * 160U + k] == (int16_t)expected[r][k]);
        if (same)
            inOrder++;
    }

    std::printf("dmr loopback: sent %zu samples, received %zu speech blocks, %zu of %u identical and in order\n",
                sent.size(), rxBlocks, inOrder, blocks);
    CHECK(rxBlocks == blocks, "%zu speech blocks received, %u sent", rxBlocks, blocks);
    CHECK(inOrder == blocks, "only %zu of %u speech blocks are right", inOrder, blocks);

    l.unlock();                         /* the watcher thread of the link may be logging into rec */
    engine.stop();
    CHECK(!engine.running(), "still running");
    std::printf("dmr loopback ok\n");
}

static void testP25Loopback()
{
    FakeHost host;
    Recorder rec;
    RadioEngine engine;
    EngineSettings s;

    s.mode                = MODE_P25;
    s.noVocoder           = true;
    s.link.host           = "127.0.0.1";
    s.link.basePort       = host.port;
    s.link.modem          = 1;
    s.link.txAheadMs      = 0;
    s.p25.nac             = 0x293;
    s.p25.srcId           = 1234;
    s.p25.dstId           = 9;
    s.p25.group           = true;
    s.p25.txDelayMs       = 100;
    s.p25.modeHangMs      = 200;

    engine.events.rxAudio = [&rec](const int16_t* p, size_t n)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.rx.insert(rec.rx.end(), p, p + n);
    };
    engine.events.call = [&rec](const CallInfo& c)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.calls.push_back(c);
    };
    engine.events.log = [&rec](const std::string& t)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.logs.push_back(t);
    };

    std::string error;
    bool started = engine.start(s, error);
    CHECK(started, "dmr start: %s", error.c_str());
    if (!started)
        return;

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.gotMode; }, 2000), "no MODE packet");
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(std::strcmp((const char*)host.mode + 8, "P25") == 0, "mode name '%s'", (const char*)host.mode + 8);
        CHECK(std::strcmp((const char*)host.mode + 19, "4FSK") == 0, "modem type '%s'", (const char*)host.mode + 19);
    }

    /* 6 LDUs of speech, nine 20 ms IMBE frames each; the stand in vocoder carries 11 bytes per frame */
    const unsigned int ldus = 6U, blocks = ldus * 9U;
    std::vector<int16_t> pcm(blocks * 160U, 0);
    std::vector<std::vector<uint8_t>> expected;

    for (unsigned int b = 0U; b < blocks; b++)
    {
        uint8_t chip[11];

        for (unsigned int k = 0U; k < 11U; k++)
            chip[k] = (uint8_t)((b * 37U + k * 11U + 5U) & 0xFFU);
        for (unsigned int k = 0U; k < 11U; k++)
            pcm[b * 160U + k] = (int16_t)chip[k];
        expected.push_back(std::vector<uint8_t>(chip, chip + 11));
    }

    engine.ptt(true);
    for (size_t pos = 0U; pos < pcm.size(); pos += 160U)
    {
        engine.pushMic(pcm.data() + pos, 160U);
        sleepMs(20);
    }
    sleepMs(50);
    engine.ptt(false);

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.seots >= 1U; }, 10000),
          "no end of transmission");
    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.setIdles >= 2U; }, 3000),
          "the modem was not handed back after the hang time");

    std::vector<int16_t> sent;
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(host.setModes == 1U, "SETMODE sent %u times", host.setModes);
        CHECK(host.seots == 1U, "%u SEOT", host.seots);
        sent = host.samples;
    }

    /* start sync (100 ms or until the first LDU of speech is there), header, 6 LDUs, terminator, filter tail */
    size_t minSamples = (size_t)(396U + ldus * 864U + 72U) * 5U;
    CHECK(sent.size() >= minSamples, "sent %zu samples, expected at least %zu", sent.size(), minSamples);

    int peak = 0;
    for (size_t i = 0U; i < sent.size(); i++)
        if (std::abs((int)sent[i]) > peak)
            peak = std::abs((int)sent[i]);
    CHECK(peak > 1000 && peak < 2500, "peak level %d counts (the outer symbol is 1260)", peak);

    /* the same samples come back as a received call */
    std::vector<int16_t> air(2400U, 0);
    air.insert(air.end(), sent.begin(), sent.end());
    air.insert(air.end(), 4800U, 0);
    host.inject(air);

    CHECK(waitFor([&rec]()
    {
        std::lock_guard<std::mutex> l(rec.mutex);
        for (size_t i = 0U; i < rec.calls.size(); i++)
            if (rec.calls[i].event == CALL_RX_END || rec.calls[i].event == CALL_RX_LOST)
                return true;
        return false;
    }, 5000), "the received call did not end");

    std::unique_lock<std::mutex> l(rec.mutex);
    bool gotIds = false, ended = false;
    float ber = 100.0F;

    for (size_t i = 0U; i < rec.calls.size(); i++)
    {
        const CallInfo& c = rec.calls[i];

        if ((c.event == CALL_RX_START || c.event == CALL_RX_INFO) && c.src == 1234U && c.dst == 9U && c.group)
            gotIds = true;
        if (c.event == CALL_RX_END)
        {
            ended = true;
            ber = c.ber;
        }
    }
    CHECK(gotIds, "the call ids 1234 -> 9 (group) were not received");
    CHECK(ended, "the call did not end with its terminator");
    (void)ber;

    size_t rxBlocks = rec.rx.size() / 160U;
    size_t inOrder = 0U;

    for (size_t r = 0U; r < rxBlocks && r < blocks; r++)
    {
        bool same = true;

        for (unsigned int k = 0U; k < 11U && same; k++)
            same = (rec.rx[r * 160U + k] == (int16_t)expected[r][k]);
        if (same)
            inOrder++;
    }

    std::printf("p25 loopback: sent %zu samples, received %zu speech blocks, %zu of %u identical and in order\n",
                sent.size(), rxBlocks, inOrder, blocks);
    CHECK(rxBlocks == blocks, "%zu speech blocks received, %u sent", rxBlocks, blocks);
    CHECK(inOrder == blocks, "only %zu of %u speech blocks are right", inOrder, blocks);

    l.unlock();                         /* the watcher thread of the link may be logging into rec */
    engine.stop();
    CHECK(!engine.running(), "still running");
    std::printf("p25 loopback ok\n");
}

static void testYsfLoopback()
{
    FakeHost host;
    Recorder rec;
    RadioEngine engine;
    EngineSettings s;

    s.mode                = MODE_YSF;
    s.noVocoder           = true;
    s.link.host           = "127.0.0.1";
    s.link.basePort       = host.port;
    s.link.modem          = 1;
    s.link.txAheadMs      = 0;
    s.ysf.callsign        = "KD0OSS";
    s.ysf.dest            = "CQCQCQ";
    s.ysf.dgid            = 0;
    s.ysf.txDelayMs       = 100;
    s.ysf.modeHangMs      = 200;

    engine.events.rxAudio = [&rec](const int16_t* p, size_t n)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.rx.insert(rec.rx.end(), p, p + n);
    };
    engine.events.call = [&rec](const CallInfo& c)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.calls.push_back(c);
    };
    engine.events.log = [&rec](const std::string& t)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.logs.push_back(t);
    };

    std::string error;
    bool started = engine.start(s, error);
    CHECK(started, "ysf start: %s", error.c_str());
    if (!started)
        return;

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.gotMode; }, 2000), "no MODE packet");
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(std::strcmp((const char*)host.mode + 8, "YSF") == 0, "mode name '%s'", (const char*)host.mode + 8);
        CHECK(std::strcmp((const char*)host.mode + 19, "C4FM") == 0, "modem type '%s'", (const char*)host.mode + 19);
    }

    /* 10 voice frames of five 20 ms blocks; every block carries its own chip bytes in the low bytes of its samples */
    const unsigned int frames = 10U, blocks = frames * 5U;
    std::vector<int16_t> pcm(blocks * 160U, 0);
    std::vector<std::vector<uint8_t>> expected;

    for (unsigned int b = 0U; b < blocks; b++)
    {
        uint8_t net[7], chip[9];

        for (unsigned int k = 0U; k < 7U; k++)
            net[k] = (uint8_t)(0x11U * (k + 1U) + 37U * b);
        net[6] &= 0x80U;                    /* 49 parameter bits */

        nxdnAudioChipEncode(NXDN_CHIP_AIR, net, chip);
        std::vector<uint8_t> bytes(chip, chip + 9);
        for (unsigned int k = 0U; k < 9U; k++)
            pcm[b * 160U + k] = (int16_t)bytes[k];
        expected.push_back(bytes);
    }

    engine.ptt(true);
    for (size_t pos = 0U; pos < pcm.size(); pos += 160U)
    {
        engine.pushMic(pcm.data() + pos, 160U);
        sleepMs(20);
    }
    engine.ptt(false);

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.seots >= 1U; }, 10000),
          "no end of transmission");
    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.setIdles >= 2U; }, 3000),
          "the modem was not handed back after the hang time");

    std::vector<int16_t> sent;
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(host.setModes == 1U, "SETMODE sent %u times", host.setModes);
        CHECK(host.seots == 1U, "%u SEOT", host.seots);
        sent = host.samples;
    }

    /* header, 10 voice frames, terminator: 12 frames of 2400 samples at least */
    CHECK(sent.size() >= 12U * 2400U, "sent %zu samples", sent.size());

    int peak = 0;
    for (size_t i = 0U; i < sent.size(); i++)
        if (std::abs((int)sent[i]) > peak)
            peak = std::abs((int)sent[i]);
    CHECK(peak > 1500 && peak < 4000, "peak level %d counts (outer symbol 1890; the pulse shaping overshoots)", peak);

    std::vector<int16_t> air(2400U, 0);
    air.insert(air.end(), sent.begin(), sent.end());
    air.insert(air.end(), 4800U, 0);
    host.inject(air);

    CHECK(waitFor([&rec]()
    {
        std::lock_guard<std::mutex> l(rec.mutex);
        for (size_t i = 0U; i < rec.calls.size(); i++)
            if (rec.calls[i].event == CALL_RX_END || rec.calls[i].event == CALL_RX_LOST)
                return true;
        return false;
    }, 5000), "the received call did not end");

    std::unique_lock<std::mutex> l(rec.mutex);
    bool gotCalls = false, ended = false;

    for (size_t i = 0U; i < rec.calls.size(); i++)
    {
        const CallInfo& c = rec.calls[i];

        if ((c.event == CALL_RX_START || c.event == CALL_RX_INFO) && c.text == "KD0OSS>CQCQCQ")
            gotCalls = true;
        if (c.event == CALL_RX_END)
            ended = true;
    }
    CHECK(gotCalls, "the callsigns KD0OSS>CQCQCQ were not received");
    CHECK(ended, "the call did not end with its terminator");

    size_t rxBlocks = rec.rx.size() / 160U, inOrder = 0U;

    for (size_t r = 0U; r < rxBlocks && r < blocks; r++)
    {
        bool same = true;

        for (unsigned int k = 0U; k < 9U && same; k++)
            same = (rec.rx[r * 160U + k] == (int16_t)expected[r][k]);
        if (same)
            inOrder++;
    }

    std::printf("ysf loopback: sent %zu samples, received %zu speech blocks, %zu of %u identical and in order\n",
                sent.size(), rxBlocks, inOrder, blocks);
    CHECK(rxBlocks == blocks, "%zu speech blocks received, %u sent", rxBlocks, blocks);
    CHECK(inOrder == blocks, "only %zu of %u speech blocks are right", inOrder, blocks);

    l.unlock();                         /* the watcher thread of the link may be logging into rec */
    engine.stop();
    CHECK(!engine.running(), "still running");
    std::printf("ysf loopback ok\n");
}

/* txDelayMs: the length of the alternating bits before the header.  noise: the peak of noise added to the samples (symbol 841), which is what
   a receiver's discriminator gives; with noise the speech may have bit errors, so only the call is checked.  A long preamble with noise
   matches the sync within two bit errors now and then; the receiver must not let such a false start swallow the real one. */
static void testDstarLoopbackWith(unsigned int txDelayMs, int noise, const char* name)
{
    FakeHost host;
    Recorder rec;
    RadioEngine engine;
    EngineSettings s;

    s.mode                = MODE_DSTAR;
    s.noVocoder           = true;
    s.link.host           = "127.0.0.1";
    s.link.basePort       = host.port;
    s.link.modem          = 1;
    s.link.txAheadMs      = 0;
    s.dstar.callsign        = "KD0OSS";
    s.dstar.ur              = "CQCQCQ";
    s.dstar.message         = "OPENMT RADIO TEST 01";
    s.dstar.txDelayMs       = txDelayMs;
    s.dstar.modeHangMs      = 200;

    engine.events.rxAudio = [&rec](const int16_t* p, size_t n)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.rx.insert(rec.rx.end(), p, p + n);
    };
    engine.events.call = [&rec](const CallInfo& c)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.calls.push_back(c);
    };
    engine.events.log = [&rec](const std::string& t)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.logs.push_back(t);
    };

    std::string error;
    bool started = engine.start(s, error);
    CHECK(started, "dstar start: %s", error.c_str());
    if (!started)
        return;

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.gotMode; }, 2000), "no MODE packet");
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(std::strcmp((const char*)host.mode + 8, "DSTAR") == 0, "mode name '%s'", (const char*)host.mode + 8);
        CHECK(std::strcmp((const char*)host.mode + 19, "GMSK") == 0, "modem type '%s'", (const char*)host.mode + 19);
    }

    /* 10 voice frames of five 20 ms blocks; every block carries its own chip bytes in the low bytes of its samples */
    const unsigned int frames = 10U, blocks = frames * 5U;
    std::vector<int16_t> pcm(blocks * 160U, 0);
    std::vector<std::vector<uint8_t>> expected;

    for (unsigned int b = 0U; b < blocks; b++)
    {
        uint8_t chip[9];
        unsigned int da = (37U * b + 5U) & 0xFFFU, db = (91U * b + 17U) & 0xFFFU;
        unsigned int a = golay24128_encode24128(da);
        unsigned int bb = golay24128_encode24128(db) ^ AMBE_PRNG_TABLE[da];
        unsigned int c = (0x5A5A5AU + 0x10101U * b) & 0xFFFFFFU, m = 0x800000U;

        std::memset(chip, 0, sizeof(chip));
        for (unsigned int i = 0U; i < 24U; i++, m >>= 1)
        {
            DSTAR_FEC_WRITE(chip, DSTAR_FEC_A[i], a & m);
            DSTAR_FEC_WRITE(chip, DSTAR_FEC_B[i], bb & m);
            DSTAR_FEC_WRITE(chip, DSTAR_FEC_C[i], c & m);
        }
        std::vector<uint8_t> bytes(chip, chip + 9);
        for (unsigned int k = 0U; k < 9U; k++)
            pcm[b * 160U + k] = (int16_t)bytes[k];
        expected.push_back(bytes);
    }

    engine.ptt(true);
    for (size_t pos = 0U; pos < pcm.size(); pos += 160U)
    {
        engine.pushMic(pcm.data() + pos, 160U);
        sleepMs(20);
    }
    engine.ptt(false);

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.seots >= 1U; }, 10000),
          "no end of transmission");
    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.setIdles >= 2U; }, 3000),
          "the modem was not handed back after the hang time");

    std::vector<int16_t> sent;
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(host.setModes == 1U, "SETMODE sent %u times", host.setModes);
        CHECK(host.seots == 1U, "%u SEOT", host.seots);
        sent = host.samples;
    }

    /* header, 10 voice frames, terminator: 12 frames of 2400 samples at least */
    CHECK(sent.size() >= 12U * 2400U, "sent %zu samples", sent.size());

    int peak = 0;
    for (size_t i = 0U; i < sent.size(); i++)
        if (std::abs((int)sent[i]) > peak)
            peak = std::abs((int)sent[i]);
    CHECK(peak > 500 && peak < 1100, "peak level %d counts (symbol 841)", peak);

    std::vector<int16_t> air(2400U, 0);
    air.insert(air.end(), sent.begin(), sent.end());
    air.insert(air.end(), 4800U, 0);
    if (noise > 0)
    {
        uint32_t seed = 12345U;

        for (size_t k = 0U; k < air.size(); k++)
        {
            int sum = 0;

            for (int j = 0; j < 4; j++)
            {
                seed = seed * 1664525U + 1013904223U;
                sum += (int)((seed >> 16) & 0xFFFFU) - 32768;
            }
            air[k] = (int16_t)(air[k] + (int)((long long)sum * noise / 131072));
        }
    }
    host.inject(air);

    CHECK(waitFor([&rec]()
    {
        std::lock_guard<std::mutex> l(rec.mutex);
        for (size_t i = 0U; i < rec.calls.size(); i++)
            if (rec.calls[i].event == CALL_RX_END || rec.calls[i].event == CALL_RX_LOST)
                return true;
        return false;
    }, 5000), "the received call did not end");

    std::unique_lock<std::mutex> l(rec.mutex);
    bool gotCalls = false, ended = false;

    for (size_t i = 0U; i < rec.calls.size(); i++)
    {
        const CallInfo& c = rec.calls[i];

        if ((c.event == CALL_RX_START || c.event == CALL_RX_INFO) && c.text == "KD0OSS>CQCQCQ")
            gotCalls = true;
        if (c.event == CALL_RX_END)
            ended = true;
    }
    CHECK(gotCalls, "the callsigns KD0OSS>CQCQCQ were not received");
    CHECK(ended, "the call did not end with its terminator");

    size_t rxBlocks = rec.rx.size() / 160U, inOrder = 0U;

    for (size_t r = 0U; r < rxBlocks && r < blocks; r++)
    {
        bool same = true;

        for (unsigned int k = 0U; k < 9U && same; k++)
            same = (rec.rx[r * 160U + k] == (int16_t)expected[r][k]);
        if (same)
            inOrder++;
    }

    std::printf("dstar loopback: sent %zu samples, received %zu speech blocks, %zu of %u identical and in order\n",
                sent.size(), rxBlocks, inOrder, blocks);
    if (noise == 0)
    {
        CHECK(rxBlocks == blocks, "%zu speech blocks received, %u sent", rxBlocks, blocks);
        CHECK(inOrder == blocks, "only %zu of %u speech blocks are right", inOrder, blocks);
    }
    else
        CHECK(rxBlocks + 2U >= blocks && rxBlocks <= blocks, "%zu speech blocks received, %u sent", rxBlocks, blocks);

    l.unlock();                         /* the watcher thread of the link may be logging into rec */
    engine.stop();
    CHECK(!engine.running(), "still running");
    std::printf("%s ok\n", name);
}

static void testDstarLoopback()
{
    testDstarLoopbackWith(100U, 0, "dstar loopback");
    testDstarLoopbackWith(600U, 450, "dstar loopback, long preamble and noise");
}

static void testM17Loopback()
{
    FakeHost host;
    Recorder rec;
    RadioEngine engine;
    EngineSettings s;

    s.mode                = MODE_M17;
    s.noVocoder           = true;
    s.link.host           = "127.0.0.1";
    s.link.basePort       = host.port;
    s.link.modem          = 1;
    s.link.txAheadMs      = 0;
    s.m17.callsign        = "KD0OSS";
    s.m17.dest            = "ALL";
    s.m17.can             = 3;
    s.m17.txDelayMs       = 100;
    s.m17.modeHangMs      = 200;

    engine.events.rxAudio = [&rec](const int16_t* p, size_t n)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.rx.insert(rec.rx.end(), p, p + n);
    };
    engine.events.call = [&rec](const CallInfo& c)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.calls.push_back(c);
    };
    engine.events.log = [&rec](const std::string& t)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.logs.push_back(t);
    };

    std::string error;
    bool started = engine.start(s, error);
    CHECK(started, "m17 start: %s", error.c_str());
    if (!started)
        return;

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.gotMode; }, 2000), "no MODE packet");
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(std::strcmp((const char*)host.mode + 8, "M17") == 0, "mode name '%s'", (const char*)host.mode + 8);
        CHECK(std::strcmp((const char*)host.mode + 19, "4FSK") == 0, "modem type '%s'", (const char*)host.mode + 19);
    }

    /* 10 stream frames of two 20 ms blocks; every block carries its own 8 bytes in the low bytes of its samples */
    const unsigned int frames = 10U, blocks = frames * 2U;
    std::vector<int16_t> pcm(blocks * 160U, 0);
    std::vector<std::vector<uint8_t>> expected;

    for (unsigned int b = 0U; b < blocks; b++)
    {
        std::vector<uint8_t> bytes(8);

        for (unsigned int k = 0U; k < 8U; k++)
        {
            bytes[k] = (uint8_t)(0x1DU * (k + 1U) + 53U * b + 7U);
            pcm[b * 160U + k] = (int16_t)bytes[k];
        }
        expected.push_back(bytes);
    }

    engine.ptt(true);
    for (size_t pos = 0U; pos < pcm.size(); pos += 160U)
    {
        engine.pushMic(pcm.data() + pos, 160U);
        sleepMs(20);
    }
    engine.ptt(false);

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.seots >= 1U; }, 10000),
          "no end of transmission");
    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.setIdles >= 2U; }, 3000),
          "the modem was not handed back after the hang time");

    std::vector<int16_t> sent;
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(host.setModes == 1U, "SETMODE sent %u times", host.setModes);
        CHECK(host.seots == 1U, "%u SEOT", host.seots);
        sent = host.samples;
    }

    /* header, 10 voice frames, terminator: 12 frames of 2400 samples at least */
    CHECK(sent.size() >= 12U * 960U, "sent %zu samples", sent.size());

    int peak = 0;
    for (size_t i = 0U; i < sent.size(); i++)
        if (std::abs((int)sent[i]) > peak)
            peak = std::abs((int)sent[i]);
    CHECK(peak > 500 && peak < 6000, "peak level %d counts (outer symbol 1481)", peak);

    std::vector<int16_t> air(2400U, 0);
    air.insert(air.end(), sent.begin(), sent.end());
    air.insert(air.end(), 4800U, 0);
    host.inject(air);

    CHECK(waitFor([&rec]()
    {
        std::lock_guard<std::mutex> l(rec.mutex);
        for (size_t i = 0U; i < rec.calls.size(); i++)
            if (rec.calls[i].event == CALL_RX_END || rec.calls[i].event == CALL_RX_LOST)
                return true;
        return false;
    }, 5000), "the received call did not end");

    std::unique_lock<std::mutex> l(rec.mutex);
    bool gotCalls = false, ended = false;

    for (size_t i = 0U; i < rec.calls.size(); i++)
    {
        const CallInfo& c = rec.calls[i];

        if ((c.event == CALL_RX_START || c.event == CALL_RX_INFO) && c.text == "KD0OSS>ALL CAN 3")
            gotCalls = true;
        if (c.event == CALL_RX_END)
            ended = true;
    }
    CHECK(gotCalls, "the callsigns KD0OSS>ALL CAN 3 were not received");
    CHECK(ended, "the call did not end with its terminator");

    size_t rxBlocks = rec.rx.size() / 160U, inOrder = 0U;

    for (size_t r = 0U; r < rxBlocks && r < blocks; r++)
    {
        bool same = true;

        for (unsigned int k = 0U; k < 8U && same; k++)
            same = (rec.rx[r * 160U + k] == (int16_t)expected[r][k]);
        if (same)
            inOrder++;
    }

    std::printf("m17 loopback: sent %zu samples, received %zu speech blocks, %zu of %u identical and in order\n",
                sent.size(), rxBlocks, inOrder, blocks);
    CHECK(rxBlocks == blocks, "%zu speech blocks received, %u sent", rxBlocks, blocks);
    CHECK(inOrder == blocks, "only %zu of %u speech blocks are right", inOrder, blocks);

    l.unlock();                         /* the watcher thread of the link may be logging into rec */
    engine.stop();
    CHECK(!engine.running(), "still running");
    std::printf("m17 loopback ok\n");
}


/* M17 text: the metadata text of a voice call (up to four blocks, one in each superframe in turn) and text messages in packet
   mode, each sent and then heard again through the fake host */
static void testM17Text()
{
    FakeHost host;
    Recorder rec;
    RadioEngine engine;
    EngineSettings s;
    std::vector<MessageInfo> messages;
    std::mutex messagesMutex;

    s.mode                = MODE_M17;
    s.noVocoder           = true;
    s.link.host           = "127.0.0.1";
    s.link.basePort       = host.port;
    s.link.modem          = 1;
    s.link.txAheadMs      = 0;
    s.m17.callsign        = "KD0OSS";
    s.m17.dest            = "ALL";
    s.m17.can             = 3;
    s.m17.txDelayMs       = 100;
    s.m17.modeHangMs      = 200;
    s.m17.metaText        = "Rick in Texas, 444.900 MHz";      /* 26 bytes: two blocks */

    engine.events.rxAudio = [&rec](const int16_t* p, size_t n)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.rx.insert(rec.rx.end(), p, p + n);
    };
    engine.events.call = [&rec](const CallInfo& c)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.calls.push_back(c);
    };
    engine.events.log = [&rec](const std::string& t)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.logs.push_back(t);
    };
    engine.events.message = [&messages, &messagesMutex](const MessageInfo& m)
    {
        std::lock_guard<std::mutex> lock(messagesMutex);
        messages.push_back(m);
    };

    std::string error;
    bool started = engine.start(s, error);
    CHECK(started, "m17 text start: %s", error.c_str());
    if (!started)
        return;
    CHECK(engine.messagesSupported(), "M17 does not say it supports messages");

    unsigned int seots = 0U;

    /* sends what the radio sent back to it as received samples and waits for the end of that call */
    auto loopBack = [&]() -> bool
    {
        if (!waitFor([&host, &seots]() { std::lock_guard<std::mutex> l(host.mutex); return host.seots > seots; }, 15000))
            return false;

        std::vector<int16_t> sent;
        {
            std::lock_guard<std::mutex> l(host.mutex);
            seots = host.seots;
            sent  = host.samples;
            host.samples.clear();
        }
        {
            std::lock_guard<std::mutex> l(rec.mutex);
            rec.calls.clear();
        }
        std::vector<int16_t> air(2400U, 0);
        air.insert(air.end(), sent.begin(), sent.end());
        air.insert(air.end(), 4800U, 0);
        host.inject(air);
        return waitFor([&rec]()
        {
            std::lock_guard<std::mutex> l(rec.mutex);
            for (size_t i = 0U; i < rec.calls.size(); i++)
                if (rec.calls[i].event == CALL_RX_END || rec.calls[i].event == CALL_RX_LOST)
                    return true;
            return false;
        }, 8000);
    };

    /* voice with the metadata text: 36 frames, so every block comes round */
    {
        std::vector<int16_t> pcm(72U * 160U, 1000);

        engine.ptt(true);
        for (size_t pos = 0U; pos < pcm.size(); pos += 160U)
        {
            engine.pushMic(pcm.data() + pos, 160U);
            sleepMs(20);
        }
        engine.ptt(false);
        CHECK(loopBack(), "the voice call with text did not come back");

        std::lock_guard<std::mutex> l(rec.mutex);
        bool text = false;

        for (size_t i = 0U; i < rec.calls.size(); i++)
            if (rec.calls[i].event == CALL_RX_INFO && rec.calls[i].text.find("\"Rick in Texas, 444.900 MHz\"") != std::string::npos)
                text = true;
        CHECK(text, "the metadata text was not received with the voice call");
    }

    /* the text changes between calls (and the longest text, four blocks) */
    engine.setMetaText("0123456789 0123456789 0123456789 0123456789 012");     /* 49 bytes: four blocks */
    {
        std::vector<int16_t> pcm(72U * 160U, 1000);

        engine.ptt(true);
        for (size_t pos = 0U; pos < pcm.size(); pos += 160U)
        {
            engine.pushMic(pcm.data() + pos, 160U);
            sleepMs(20);
        }
        engine.ptt(false);
        CHECK(loopBack(), "the second voice call did not come back");

        std::lock_guard<std::mutex> l(rec.mutex);
        bool text = false;

        for (size_t i = 0U; i < rec.calls.size(); i++)
            if (rec.calls[i].event == CALL_RX_INFO &&
                rec.calls[i].text.find("\"0123456789 0123456789 0123456789 0123456789 012\"") != std::string::npos)
                text = true;
        CHECK(text, "the four block metadata text was not received");
    }

    /* messages: one frame (a short one, and one that fills the frame exactly: 1 + 21 + 1 + 2 = 25 bytes), several frames, UTF-8, the longest */
    std::vector<std::string> texts;

    texts.push_back("Hi");
    texts.push_back("123456789012345678901");
    texts.push_back("The repeater is back on the air after the storm. Thanks to everybody who helped put the antenna back up!");
    texts.push_back("Caf\xC3\xA9 \xE2\x82\xAC" "5 \xE2\x80\x94 na\xC3\xAFve");
    texts.push_back(std::string(796U, 'x'));

    for (size_t t = 0U; t < texts.size(); t++)
    {
        std::string err;

        {
            std::lock_guard<std::mutex> l(messagesMutex);
            messages.clear();
        }
        CHECK(engine.sendMessage("W1AW", texts[t], err), "message %zu not accepted: %s", t, err.c_str());
        bool backOk = loopBack();
        CHECK(backOk, "message %zu did not come back", t);
        if (!backOk)
        {
            std::lock_guard<std::mutex> l(rec.mutex);
            for (size_t i = 0U; i < rec.logs.size(); i++)
                std::printf("  log: %s\n", rec.logs[i].c_str());
            for (size_t i = 0U; i < rec.calls.size(); i++)
                std::printf("  call %d: %s\n", (int)rec.calls[i].event, rec.calls[i].text.c_str());
        }
        sleepMs(100);

        std::lock_guard<std::mutex> l(messagesMutex);
        bool sentOk = false, heard = false;

        for (size_t i = 0U; i < messages.size(); i++)
        {
            if (!messages[i].received && messages[i].text == texts[t] && messages[i].to == "W1AW")
                sentOk = true;
            if (messages[i].received && messages[i].text == texts[t] && messages[i].from == "KD0OSS" && messages[i].to == "W1AW")
                heard = true;
        }
        CHECK(sentOk, "message %zu: no event for the message sent", t);
        CHECK(heard, "message %zu (%zu bytes): the message was not received intact", t, texts[t].size());
        if (!heard)
        {
            std::lock_guard<std::mutex> lg(rec.mutex);
            for (size_t i = 0U; i < rec.logs.size(); i++)
                if (rec.logs[i].find("packet") != std::string::npos || rec.logs[i].find("message from") != std::string::npos)
                    std::printf("  log: %s\n", rec.logs[i].c_str());
        }
    }

    std::string err;

    CHECK(!engine.sendMessage("W1AW", std::string(797U, 'x'), err), "a message that is too long was accepted");
    CHECK(!engine.sendMessage("W1AW", "", err), "an empty message was accepted");
    CHECK(!engine.sendMessage("TOOLONGCALLSIGN", "x", err), "a callsign that is too long was accepted");

    engine.stop();
    std::printf("m17 text ok\n");
}


/* Amplitude of one frequency in a block of samples */
static double goertzelAmp(const int16_t* x, size_t n, double freq, double fs)
{
    double w = 2.0 * 3.14159265358979 * freq / fs, c = 2.0 * std::cos(w), s1 = 0.0, s2 = 0.0;

    for (size_t i = 0U; i < n; i++)
    {
        double s0 = x[i] + c * s1 - s2;

        s2 = s1;
        s1 = s0;
    }
    double p = s1 * s1 + s2 * s2 - c * s1 * s2;

    return n == 0U ? 0.0 : 2.0 * std::sqrt(p > 0.0 ? p : 0.0) / (double)n;
}

static void testFmLoopback()
{
    FakeHost host;
    Recorder rec;
    RadioEngine engine;
    EngineSettings s;

    s.mode                = MODE_FM;
    s.noVocoder           = true;
    s.link.host           = "127.0.0.1";
    s.link.basePort       = host.port;
    s.link.modem          = 1;
    s.link.txAheadMs      = 0;
    s.fm.txCtcss          = 88.5F;
    s.fm.txDelayMs        = 200U;
    s.fm.txTailMs         = 100U;
    s.fm.modeHangMs       = 200U;

    engine.events.rxAudio = [&rec](const int16_t* p, size_t n)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.rx.insert(rec.rx.end(), p, p + n);
    };
    engine.events.call = [&rec](const CallInfo& c)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.calls.push_back(c);
    };
    engine.events.log = [&rec](const std::string& t)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        rec.logs.push_back(t);
    };

    std::string error;
    bool started = engine.start(s, error);
    CHECK(started, "fm start: %s", error.c_str());
    if (!started)
        return;

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.gotMode; }, 2000), "no MODE packet");
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(std::strcmp((const char*)host.mode + 8, "FM") == 0, "mode name '%s'", (const char*)host.mode + 8);
        CHECK(std::strcmp((const char*)host.mode + 19, "FM") == 0, "modem type '%s'", (const char*)host.mode + 19);
    }

    /* one second of a 1 kHz tone as speech */
    std::vector<int16_t> pcm(8000U);
    for (size_t i = 0U; i < pcm.size(); i++)
        pcm[i] = (int16_t)(8000.0 * std::sin(2.0 * 3.14159265358979 * 1000.0 * (double)i / 8000.0));

    engine.ptt(true);
    for (size_t pos = 0U; pos < pcm.size(); pos += 160U)
    {
        engine.pushMic(pcm.data() + pos, 160U);
        sleepMs(20);
    }
    engine.ptt(false);

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.seots >= 1U; }, 10000), "no end of transmission");
    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.setIdles >= 2U; }, 3000),
          "the modem was not handed back after the hang time");

    std::vector<int16_t> sent;
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(host.setModes == 1U, "SETMODE sent %u times", host.setModes);
        sent = host.samples;
    }

    /* the tone alone for the first 200 ms, then the speech with the tone, then the tone alone */
    CHECK(sent.size() > 24000U, "sent %zu samples", sent.size());
    double lead = goertzelAmp(sent.data() + 480U, 3600U, 88.5, 24000.0);
    double lead1k = goertzelAmp(sent.data() + 480U, 3600U, 1000.0, 24000.0);
    double mid1k = goertzelAmp(sent.data() + 9600U, 9600U, 1000.0, 24000.0);
    double mid88 = goertzelAmp(sent.data() + 9600U, 9600U, 88.5, 24000.0);

    std::printf("fm loopback: sent %zu samples; lead: 88.5 Hz %.0f 1 kHz %.0f; speech: 1 kHz %.0f 88.5 Hz %.0f\n", sent.size(), lead,
                lead1k, mid1k, mid88);
    CHECK(lead > 400.0 && lead < 600.0, "the tone (1.5%% of full scale, 492) was %.0f", lead);
    CHECK(lead1k < 50.0, "speech in the lead of tone alone: %.0f", lead1k);
    CHECK(mid1k > 6000.0 && mid1k < 9000.0, "the 1 kHz speech came out at %.0f", mid1k);
    CHECK(mid88 > 400.0 && mid88 < 600.0, "the tone with the speech was %.0f", mid88);

    /* receive: noise (no carrier), the transmission as the repeater would send it, noise again */
    unsigned int seed = 12345U;
    auto noise = [&seed](size_t n)
    {
        std::vector<int16_t> v(n);

        for (size_t i = 0U; i < n; i++)
        {
            seed = seed * 1103515245U + 12345U;
            v[i] = (int16_t)((int)((seed >> 16) & 0xFFFF) - 32768) * 9 / 10;
        }
        return v;
    };
    std::vector<int16_t> air = noise(24000U);
    air.insert(air.end(), sent.begin(), sent.end());
    std::vector<int16_t> tailNoise = noise(36000U);
    air.insert(air.end(), tailNoise.begin(), tailNoise.end());
    host.inject(air);

    CHECK(waitFor([&rec]()
    {
        std::lock_guard<std::mutex> l(rec.mutex);
        for (size_t i = 0U; i < rec.calls.size(); i++)
            if (rec.calls[i].event == CALL_RX_END)
                return true;
        return false;
    }, 6000), "the received call did not end");

    std::unique_lock<std::mutex> l(rec.mutex);
    int starts = 0, ends = 0;

    for (size_t i = 0U; i < rec.calls.size(); i++)
    {
        if (rec.calls[i].event == CALL_RX_START)
            starts++;
        if (rec.calls[i].event == CALL_RX_END)
            ends++;
    }
    CHECK(starts == 1 && ends == 1, "%d call starts and %d ends (one of each wanted)", starts, ends);

    /* the speech was 1 s at 8 kHz, with the tone alone around it: the squelch tail is cut, the noise is not heard */
    double rx1k = goertzelAmp(rec.rx.data(), rec.rx.size(), 1000.0, 8000.0);
    double rx88 = goertzelAmp(rec.rx.data(), rec.rx.size(), 88.5, 8000.0);

    std::printf("fm loopback: received %zu audio samples (%.2f s); 1 kHz %.0f, 88.5 Hz %.0f\n", rec.rx.size(),
                (double)rec.rx.size() / 8000.0, rx1k, rx88);
    CHECK(rec.rx.size() > 8000U && rec.rx.size() < 20000U, "%zu samples of audio received", rec.rx.size());
    CHECK(rx1k > 2000.0, "the 1 kHz speech was received at %.0f", rx1k);
    CHECK(rx88 < 0.05 * rx1k, "the tone was not filtered off: %.0f against %.0f", rx88, rx1k);

    l.unlock();
    engine.stop();
    CHECK(!engine.running(), "still running");
    std::printf("fm loopback ok\n");
}

/* DTMF: digits sent with nobody holding the key, decoded with the repeater's own decoder (fm_dsp.c) */
static void testFmDtmf()
{
    FakeHost host;
    RadioEngine engine;
    EngineSettings s;

    s.mode           = MODE_FM;
    s.noVocoder      = true;
    s.link.host      = "127.0.0.1";
    s.link.basePort  = host.port;
    s.link.modem     = 1;
    s.link.txAheadMs = 200U;                    /* in real time, as with a modem */
    s.fm.txDelayMs   = 400U;
    s.fm.txTailMs    = 100U;
    s.fm.modeHangMs  = 200U;

    std::string error;
    bool started = engine.start(s, error);
    CHECK(started, "fm dtmf start: %s", error.c_str());
    if (!started)
        return;

    CHECK(engine.dtmfSupported(), "FM must support DTMF");
    CHECK(!engine.sendDtmf('x'), "a digit that is not one was accepted");
    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.gotMode; }, 2000), "no MODE packet");

    const uint64_t began = nowMsTest();
    const char* digits = getenv("DT_DIGITS") ? getenv("DT_DIGITS") : "14*#D";
    for (const char* d = digits; *d; d++)
    {
        CHECK(engine.sendDtmf(*d), "digit %c refused", *d);
        sleepMs(600);                           /* typed at a keypad: one transmission, the key is held between digits */
    }

    CHECK(waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.seots >= 1U; }, 10000), "no end of transmission");

    std::vector<int16_t> sent;
    {
        std::lock_guard<std::mutex> l(host.mutex);
        CHECK(host.seots == 1U, "%u transmissions for digits typed 600 ms apart (one wanted)", host.seots);
        sent = host.samples;
    }

    const uint64_t took = nowMsTest() - began;

    CHECK(took + 150U >= sent.size() / 24U, "the end came after %llu ms, before the %zu ms of samples had played",
          (unsigned long long)took, sent.size() / 24U);
    FmDecim decim;
    FmDtmf  dec;
    std::string got;

    fmDecimInit(&decim);
    fmDtmfInit(&dec);
    for (size_t i = 0U; i < sent.size(); i++)
    {
        float out;

        if (fmDecimPush(&decim, (float)sent[i], &out))
        {
            char c = fmDtmfPush(&dec, out);

            if (c)
                got += c;
        }
    }
    if (getenv("DT_DUMP"))
        for (size_t i = 0U; i + 2400U <= sent.size(); i += 2400U)
        {
            double e = 0.0;

            for (size_t k = 0U; k < 2400U; k++)
                e += (double)sent[i + k] * sent[i + k];
            std::printf("  %4zu ms: rms %.0f\n", i / 24U, std::sqrt(e / 2400.0));
        }
    std::printf("fm dtmf: sent %zu samples, decoded '%s' (wanted '%s')\n", sent.size(), got.c_str(), digits);
    CHECK(got == digits, "decoded '%s' instead of '%s'", got.c_str(), digits);
    CHECK(sent.size() > 24000U * 4U && sent.size() < 24000U * 7U, "the key was down for %zu samples (expected: five digits 600 ms apart and the 2 s hang)", sent.size());
    engine.stop();
}

/* the dongle is opened by the program, not by a run; a port that is not there is an error, and a run without the dongle works */
static void testDongle()
{
    RadioEngine engine;
    std::string error;

    CHECK(!engine.dongleOpen(), "the dongle is open before anything opened it");
    CHECK(!engine.openDongle("/dev/this_port_is_not_there", 460800U, 64, 500, error), "a port that is not there was opened");
    CHECK(error.find("cannot open") != std::string::npos, "error text: %s", error.c_str());
    CHECK(!engine.dongleOpen(), "open after a failed open");
    engine.closeDongle();                       /* nothing to close: no harm */
}

/* ------------------------------------------------------------------------------------------------ fake AMBE3000R */

/* A chip on a pty: READY, product id, rate and initialise answers, speech to a channel packet and back.  stuck() makes it lose
   the sync like a chip that was left waiting for the rest of a packet: it swallows everything until 200 zero bytes in a row
   (or a reset after those) get it going again. */
struct FakeChip
{
    int                master = -1, slave = -1;
    std::string        name;
    std::atomic<bool>  run, stuckNow, deaf;
    std::thread        th;
    std::atomic<unsigned int> resets, answers, opens;
    bool               watchClose;      /* the chip sees the port being closed and opened (a USB stall that only a new open clears) */

    explicit FakeChip(bool watch = false) : run(true), stuckNow(false), deaf(false), resets(0U), answers(0U), opens(0U),
                                            watchClose(watch)
    {
        openpty(&master, &slave, NULL, NULL, NULL);
        name = ttyname(slave);
        if (watchClose)
        {
            close(slave);               /* master polls POLLHUP while nobody has the port open */
            slave = -1;
        }
        th = std::thread(&FakeChip::loop, this);
    }
    ~FakeChip()
    {
        run = false;
        th.join();
        close(master);
        if (slave >= 0)
            close(slave);
    }
    void send(uint8_t type, const std::vector<uint8_t>& payload)
    {
        std::vector<uint8_t> p;

        p.push_back(0x61);
        p.push_back((uint8_t)(payload.size() >> 8));
        p.push_back((uint8_t)payload.size());
        p.push_back(type);
        p.insert(p.end(), payload.begin(), payload.end());
        (void)!write(master, p.data(), p.size());
    }
    void loop()
    {
        std::vector<uint8_t> in;
        unsigned int zeros = 0U;
        bool announced = false;
        bool closed = watchClose;
        uint64_t began = nowMsTest();

        while (run)
        {
            if (!closed && !announced && nowMsTest() - began > 150U)
            {
                send(0x00, {0x39});                         /* READY */
                announced = true;
            }
            struct pollfd pf = { master, POLLIN, 0 };

            if (poll(&pf, 1, watchClose ? 2 : 20) < 0)
                continue;
            if (watchClose)
            {
                if (pf.revents & POLLHUP)
                {
                    closed = true;
                    usleep(1000);
                    continue;
                }
                if (closed)                                 /* opened again: the chip starts afresh, as a dongle does */
                {
                    closed    = false;
                    deaf      = false;
                    stuckNow  = false;
                    announced = false;
                    began     = nowMsTest();
                    in.clear();
                    opens++;
                }
            }
            if (!(pf.revents & POLLIN))
                continue;
            uint8_t b[512];
            ssize_t n = read(master, b, sizeof(b));

            for (ssize_t i = 0; i < n; i++)
            {
                if (deaf)
                    continue;                               /* nothing gets through any more */
                if (stuckNow)
                {
                    zeros = b[i] == 0U ? zeros + 1U : 0U;
                    if (zeros >= 200U)
                    {
                        stuckNow = false;
                        in.clear();
                    }
                    continue;
                }
                in.push_back(b[i]);
                while (!in.empty() && in[0] != 0x61)
                    in.erase(in.begin());
                if (in.size() >= 4U)
                {
                    size_t len = ((size_t)in[1] << 8) | in[2];

                    if (in.size() >= 4U + len)
                    {
                        uint8_t type = in[3];
                        std::vector<uint8_t> pl(in.begin() + 4, in.begin() + 4 + (long)len);

                        in.erase(in.begin(), in.begin() + 4 + (long)len);
                        answer(type, pl);
                    }
                }
            }
        }
    }
    void answer(uint8_t type, const std::vector<uint8_t>& pl)
    {
        answers++;
        if (type == 0x00 && !pl.empty())
        {
            if (pl[0] == 0x33)
            {
                resets++;
                send(0x00, {0x39});
            }
            else if (pl[0] == 0x30)
                send(0x00, {0x30, 'F', 'A', 'K', 'E'});
            else
                send(0x00, {pl[0], 0x00});
        }
        else if (type == 0x02 && pl.size() >= 322U)         /* speech in, channel out */
        {
            std::vector<uint8_t> ch = {0x01, 72};

            for (unsigned int i = 0U; i < 9U; i++)
                ch.push_back(pl[2U + i * 2U + 1U]);
            send(0x01, ch);
        }
        else if (type == 0x01 && pl.size() >= 11U)           /* channel in, speech out */
        {
            std::vector<uint8_t> sp = {0x00, 160};

            for (unsigned int i = 0U; i < 160U; i++)
            {
                sp.push_back(pl[2U + (i % 9U)]);
                sp.push_back(0x00);
            }
            send(0x02, sp);
        }
    }
};

static void testDongleRecovery()
{
    FakeChip chip;
    AmbeVocoder voc;
    std::string error;
    std::vector<std::string> logs;

    voc.log = [&logs](const std::string& t) { logs.push_back(t); };
    AmbeVocoder::setSerialPacing(0, 0);
    CHECK(voc.open(chip.name, 460800U, error), "the fake chip did not open: %s", error.c_str());
    if (!voc.isOpen())
        return;

    int16_t pcm[160 * 3];
    uint8_t blocks[9 * 3];
    int16_t back[160 * 3];

    for (size_t i = 0U; i < 160U * 3U; i++)
        pcm[i] = (int16_t)(1000 + (int)(i % 97U) * 7);

    CHECK(voc.encodeBatch(pcm, 3U, blocks), "encode with a working chip");
    CHECK(voc.decodeBatch(blocks, 3U, back), "decode with a working chip");
    CHECK(voc.ensureAlive(), "ensureAlive on a working chip");
    CHECK(chip.resets == 0U, "%u resets before any trouble", chip.resets.load());

    /* the chip loses the sync: the decode must come back by itself after a reset */
    chip.stuckNow = true;
    uint64_t t0 = nowMsTest();
    bool ok = voc.decodeBatch(blocks, 3U, back);

    CHECK(ok, "the decode did not recover from a chip that lost the sync");
    CHECK(chip.resets >= 1U, "no reset was sent to a chip that stopped answering");
    bool said = false;

    for (size_t i = 0U; i < logs.size(); i++)
        if (logs[i].find("was reset and is answering again") != std::string::npos)
            said = true;
    CHECK(said, "the recovery was not logged");
    std::printf("dongle recovery: took %llu ms\n", (unsigned long long)(nowMsTest() - t0));
    CHECK(voc.decodeBatch(blocks, 3U, back), "decode after the recovery");

    /* it also stops answering while nobody uses it */
    chip.stuckNow = true;
    CHECK(voc.ensureAlive(), "ensureAlive did not recover a stuck chip");
    CHECK(voc.encodeBatch(pcm, 3U, blocks), "encode after ensureAlive recovered");

    voc.close();
}

/* the read side of the port dies (kernel: "urb stopped: -32"): zeros and resets do nothing, only closing and opening the port helps */
static void testDongleReopen()
{
    FakeChip chip(true);
    AmbeVocoder voc;
    std::string error;
    std::vector<std::string> logs;

    voc.log = [&logs](const std::string& t) { logs.push_back(t); };
    AmbeVocoder::setSerialPacing(0, 0);
    CHECK(voc.open(chip.name, 460800U, error), "the fake chip (watching the port) did not open: %s", error.c_str());
    if (!voc.isOpen())
        return;

    int16_t pcm[160 * 3];
    uint8_t blocks[9 * 3];
    int16_t back[160 * 3];

    for (size_t i = 0U; i < 160U * 3U; i++)
        pcm[i] = (int16_t)(1000 + (int)(i % 97U) * 7);
    CHECK(voc.encodeBatch(pcm, 3U, blocks), "encode before the stall");
    CHECK(chip.opens == 1U, "%u opens before the stall", chip.opens.load());

    chip.deaf = true;
    uint64_t t0 = nowMsTest();

    CHECK(voc.encodeBatch(pcm, 3U, blocks), "the encode did not recover from a dead read side");
    std::printf("dongle reopen: took %llu ms\n", (unsigned long long)(nowMsTest() - t0));
    CHECK(chip.opens >= 2U, "the port was not opened again (%u opens)", chip.opens.load());
    bool said = false;

    for (size_t i = 0U; i < logs.size(); i++)
        if (logs[i].find("closed and opened again") != std::string::npos)
            said = true;
    CHECK(said, "the reopening was not logged");
    CHECK(voc.decodeBatch(blocks, 3U, back), "decode after the port was opened again");

    chip.deaf = true;
    CHECK(voc.ensureAlive(), "ensureAlive did not recover a dead read side");
    voc.close();
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "m17text") == 0)         /* only the M17 text test (while working on it) */
    {
        testM17Text();
        std::printf("FAILS: %d\n", failures);
        return failures ? 1 : 0;
    }
    testResampler();
    testNxdnLoopback();
    testDmrLoopback();
    testP25Loopback();
    testYsfLoopback();
    testDstarLoopback();
    testM17Loopback();
    testM17Text();
    testFmLoopback();
    testFmDtmf();
    testDongle();
    testDongleRecovery();
    testDongleReopen();

    if (failures)
    {
        std::printf("FAILS: %d\n", failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
