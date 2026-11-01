/* The P25 mode against the repo's own p25_host (see run_p25_interop.sh).
 *
 *   p25_interop_tool capture <samples.raw>
 *       Transmits a call (stand in vocoder, 6 LDUs, ids 1234 -> 9, NAC 293) to a fake Modem_Host and writes the 24 kHz
 *       samples (16 bit little endian).
 *   p25_interop_tool replay <samples.raw>
 *       Receives the samples p25_host transmitted for the call p25_sim sent (ids 1234 -> 9) and checks the call.
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

    s.mode            = MODE_P25;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 0;
    s.p25.nac         = 0x293;
    s.p25.srcId       = 1234;
    s.p25.dstId       = 9;
    s.p25.group       = true;
    s.p25.txDelayMs   = 300;
    s.p25.modeHangMs  = 200;

    std::string error;

    if (!engine.start(s, error))
    {
        std::printf("start failed: %s\n", error.c_str());
        return 2;
    }

    const unsigned int blocks = 6U * 9U;
    std::vector<int16_t> pcm(blocks * 160U, 0);

    for (unsigned int b = 0U; b < blocks; b++)
        for (unsigned int k = 0U; k < 11U; k++)
            pcm[b * 160U + k] = (int16_t)((b * 37U + k * 11U + 5U) & 0xFFU);

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

    s.mode            = MODE_P25;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 0;
    s.p25.nac         = 0x293;

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

    std::vector<int16_t> air = readRaw(samplesPath);
    std::printf("replaying %zu samples (%.1f s)\n", air.size(), air.size() / 24000.0);

    /* optional damage: P25_REPLAY_GAIN, P25_REPLAY_DC, P25_REPLAY_NOISE (rms) */
    {
        const char* g = std::getenv("P25_REPLAY_GAIN");
        const char* d = std::getenv("P25_REPLAY_DC");
        const char* n = std::getenv("P25_REPLAY_NOISE");
        double gain = g ? std::atof(g) : 1.0, dc = d ? std::atof(d) : 0.0, noise = n ? std::atof(n) : 0.0;
        uint32_t rnd = std::getenv("P25_REPLAY_SEED") ? (uint32_t)std::atoi(std::getenv("P25_REPLAY_SEED")) : 1U;

        for (size_t i = 0U; i < air.size(); i++)
        {
            double x = 0.0;

            for (int k = 0; k < 12; k++)
            {
                rnd = rnd * 1664525U + 1013904223U;
                x += (double)(rnd >> 16) / 65536.0;
            }
            double v = air[i] * gain + dc + (x - 6.0) * noise;

            air[i] = (int16_t)std::max(-32768.0, std::min(32767.0, v));
        }
    }
    air.insert(air.begin(), 2400U, 0);
    air.insert(air.end(), 6000U, 0);
    host.inject(air);

    waitFor([&rec]()
    {
        std::lock_guard<std::mutex> l(rec.mutex);
        for (size_t i = 0U; i < rec.calls.size(); i++)
            if (rec.calls[i].event == CALL_RX_END || rec.calls[i].event == CALL_RX_LOST)
                return true;
        return false;
    }, 8000);
    sleepMs(300);

    std::lock_guard<std::mutex> l(rec.mutex);
    bool ids = false, ended = false;

    for (size_t i = 0U; i < rec.calls.size(); i++)
    {
        const CallInfo& c = rec.calls[i];

        std::printf("  call event %d src %u dst %u group %d\n", (int)c.event, c.src, c.dst, c.group ? 1 : 0);
        if ((c.event == CALL_RX_START || c.event == CALL_RX_INFO) && c.src == 1234567U && c.dst == 9U && c.group)
            ids = true;
        if (c.event == CALL_RX_END || c.event == CALL_RX_LOST)
            ended = true;       /* p25_host's simplex transmitter ends a call with silence, not a terminator */
    }

    if (const char* dump = std::getenv("P25_REPLAY_DUMP"))        /* the 11 byte IMBE frames, for offline decoding */
    {
        FILE* f = std::fopen(dump, "wb");

        if (f)
        {
            for (size_t r = 0U; r < rec.rx.size() / 160U; r++)
                std::fwrite(&rec.rx[r * 160U], 1U, 11U, f);
            std::fclose(f);
        }
    }

    /* the sim's voice: IMBE frame n of LDU i is imbeFill(i * 9 + n), seed * 37 + k * 91 + 5 */
    size_t blocks = rec.rx.size() / 160U, right = 0U;

    for (size_t r = 0U; r < blocks; r++)
    {
        bool same = true;

        for (unsigned int k = 0U; k < 11U && same; k++)
            same = (rec.rx[r * 160U + k] == (int16_t)(uint8_t)(r * 37U + k * 91U + 5U));
        if (same)
            right++;
        else
            std::printf("  block %zu differs\n", r);
    }

    std::printf("  ids %s, call end %s, %zu speech blocks, %zu right\n", ids ? "ok" : "MISSING",
                ended ? "ok" : "MISSING", blocks, right);

    /* p25_sim sends LDU_PAIRS pairs: 9 frames per LDU */
    int rc = (ids && ended && blocks >= 9U && right == blocks) ? 0 : 1;

    std::printf("%s\n", rc == 0 ? "PASS" : "FAIL");
    engine.stop();
    return rc;
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "capture") == 0)
        return capture(argv[2]);
    if (argc == 3 && std::strcmp(argv[1], "replay") == 0)
        return replay(argv[2]);

    std::printf("usage: p25_interop_tool capture <samples.raw> | replay <samples.raw>\n");
    return 2;
}
