/* The FM mode against the repo's own fm_host (see run_fm_interop.sh).
 *
 *   fm_interop_tool capture <samples.raw>
 *       Transmits 1.5 s of a 1 kHz tone as speech (CTCSS 88.5 Hz, as the repeater's default) to a fake Modem_Host and writes
 *       the 24 kHz samples (16 bit little endian).
 *   fm_interop_tool replay <samples.raw>
 *       Receives what fm_host transmitted for that call, with noise (no carrier) before and after it as a real receiver
 *       gives, and checks the squelch, the call and the speech.
 */

#define main core_test_main
#include "core_test.cpp"
#undef main

static std::vector<int16_t> readRaw(const char* path)
{
    std::vector<int16_t> v;
    FILE* f = std::fopen(path, "rb");

    if (!f)
        return v;

    uint8_t b[2];
    while (std::fread(b, 1, 2, f) == 2)
        v.push_back((int16_t)(b[0] | (b[1] << 8)));
    std::fclose(f);
    return v;
}

static int capture(const char* samplesPath)
{
    FakeHost host;
    RadioEngine engine;
    EngineSettings s;

    s.mode            = MODE_FM;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 0;
    s.fm.txCtcss      = getenv("FM_TX_TONE") ? (float)atof(getenv("FM_TX_TONE")) : 88.5F;
    if (getenv("FM_TONE_LEVEL"))
        s.fm.txToneLevel = (float)atof(getenv("FM_TONE_LEVEL"));
    s.fm.modeHangMs   = 200U;

    std::string error;

    if (!engine.start(s, error))
    {
        std::printf("start failed: %s\n", error.c_str());
        return 2;
    }

    std::vector<int16_t> pcm(12000U);

    for (size_t i = 0U; i < pcm.size(); i++)
        pcm[i] = (int16_t)(8000.0 * std::sin(2.0 * 3.14159265358979 * 1000.0 * (double)i / 8000.0));

    engine.ptt(true);
    for (size_t pos = 0U; pos < pcm.size(); pos += 160U)
    {
        engine.pushMic(pcm.data() + pos, 160U);
        sleepMs(20);
    }
    sleepMs(50);
    engine.ptt(false);

    if (!waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.seots >= 1U; }, 10000))
    {
        std::printf("no end of transmission\n");
        return 2;
    }

    {
        std::lock_guard<std::mutex> l(host.mutex);
        FILE* f = std::fopen(samplesPath, "wb");

        for (size_t i = 0U; f && i < host.samples.size(); i++)
        {
            uint8_t b[2] = {(uint8_t)(host.samples[i] & 0xFF), (uint8_t)((host.samples[i] >> 8) & 0xFF)};
            std::fwrite(b, 1, 2, f);
        }
        if (f)
            std::fclose(f);
        std::printf("wrote %zu samples\n", host.samples.size());
    }

    engine.stop();
    return 0;
}

static int replay(const char* samplesPath)
{
    FakeHost host;
    Recorder rec;
    RadioEngine engine;
    EngineSettings s;

    s.mode            = MODE_FM;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 0;
    if (getenv("FM_SQUELCH"))
        s.fm.squelch = (unsigned int)atoi(getenv("FM_SQUELCH"));
    if (getenv("FM_RX_TONE"))
        s.fm.rxCtcss = (float)atof(getenv("FM_RX_TONE"));

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
    engine.events.log = [](const std::string& t) { std::printf("  log: %s\n", t.c_str()); };

    std::string error;

    if (!engine.start(s, error))
    {
        std::printf("start failed: %s\n", error.c_str());
        return 2;
    }
    waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.gotMode; }, 2000);

    std::vector<int16_t> sig = readRaw(samplesPath);
    std::printf("replaying %zu samples (%.1f s)\n", sig.size(), sig.size() / 24000.0);

    unsigned int seed = 777U;
    auto noise = [&seed](size_t n)
    {
        std::vector<int16_t> v(n);

        for (size_t i = 0U; i < n; i++)
        {
            seed = seed * 1103515245U + 12345U;
            v[i] = (int16_t)(((int)((seed >> 16) & 0xFFFF) - 32768) * 9 / 10);
        }
        return v;
    };

    std::vector<int16_t> air = noise(24000U);
    air.insert(air.end(), sig.begin(), sig.end());
    std::vector<int16_t> tail = noise(36000U);
    air.insert(air.end(), tail.begin(), tail.end());
    host.inject(air);

    waitFor([&rec]()
    {
        std::lock_guard<std::mutex> l(rec.mutex);
        for (size_t i = 0U; i < rec.calls.size(); i++)
            if (rec.calls[i].event == CALL_RX_END)
                return true;
        return false;
    }, 8000);
    sleepMs(300);

    std::lock_guard<std::mutex> l(rec.mutex);
    int starts = 0, ends = 0;

    for (size_t i = 0U; i < rec.calls.size(); i++)
    {
        std::printf("  call event %d text '%s'\n", (int)rec.calls[i].event, rec.calls[i].text.c_str());
        if (rec.calls[i].event == CALL_RX_START)
            starts++;
        if (rec.calls[i].event == CALL_RX_END)
            ends++;
    }

    double rx1k = goertzelAmp(rec.rx.data(), rec.rx.size(), 1000.0, 8000.0);
    double rx88 = goertzelAmp(rec.rx.data(), rec.rx.size(), 88.5, 8000.0);
    double secs = (double)rec.rx.size() / 8000.0;

    std::printf("  %d call starts, %d ends, %.2f s of audio, 1 kHz %.0f, 88.5 Hz %.0f\n", starts, ends, secs, rx1k, rx88);

    /* the speech is 1.5 s; the repeater's audio delay and tone before and after it make up the rest */
    bool ok = starts == 1 && ends == 1 && secs > 1.2 && secs < 4.0 && rx1k > 1500.0 && rx88 < 0.05 * rx1k;

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    engine.stop();
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "capture") == 0)
        return capture(argv[2]);
    if (argc == 3 && std::strcmp(argv[1], "replay") == 0)
        return replay(argv[2]);

    std::printf("usage: fm_interop_tool capture <samples.raw> | replay <samples.raw>\n");
    return 2;
}
