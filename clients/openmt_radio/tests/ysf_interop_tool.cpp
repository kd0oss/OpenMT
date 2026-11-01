/* The YSF mode against the repo's own ysf_host (see run_ysf_interop.sh).
 *
 *   ysf_interop_tool capture <samples.raw>
 *       Transmits a call (stand in vocoder, 10 voice frames, KD0OSS -> CQCQCQ) to a fake Modem_Host and writes the 24 kHz
 *       samples (16 bit little endian).
 *   ysf_interop_tool replay <samples.raw>
 *       Receives the samples ysf_host transmitted for the call ysf_sim sent (W1ABC -> CQCQCQ, 20 voice frames) and checks
 *       the call.
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

    s.mode            = MODE_YSF;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 0;
    s.ysf.callsign    = "KD0OSS";
    s.ysf.dest        = "CQCQCQ";
    s.ysf.dgid        = 0;
    s.ysf.txDelayMs   = 300;
    s.ysf.modeHangMs  = 200;

    std::string error;

    if (!engine.start(s, error))
    {
        std::printf("start failed: %s\n", error.c_str());
        return 2;
    }

    /* 10 voice frames; block b carries the 49 bits net[k] = 0x11 * (k + 1) + 37 * b (the stand in vocoder keeps the chip block) */
    const unsigned int blocks = 10U * 5U;
    std::vector<int16_t> pcm(blocks * 160U, 0);

    for (unsigned int b = 0U; b < blocks; b++)
    {
        uint8_t net[7], chip[9];

        for (unsigned int k = 0U; k < 7U; k++)
            net[k] = (uint8_t)(0x11U * (k + 1U) + 37U * b);
        net[6] &= 0x80U;
        nxdnAudioChipEncode(NXDN_CHIP_AIR, net, chip);
        for (unsigned int k = 0U; k < 9U; k++)
            pcm[b * 160U + k] = (int16_t)chip[k];
    }

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

    s.mode            = MODE_YSF;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 0;
    s.ysf.callsign    = "KD0OSS";

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

        std::printf("  call event %d text '%s' ber %.2f\n", (int)c.event, c.text.c_str(), c.ber);
        if ((c.event == CALL_RX_START || c.event == CALL_RX_INFO) && c.text == "W1ABC>CQCQCQ")
            ids = true;
        if (c.event == CALL_RX_END || c.event == CALL_RX_LOST)
            ended = true;
    }

    size_t blocks = rec.rx.size() / 160U;

    std::printf("  callsigns %s, call end %s, %zu speech blocks (the call has 20 frames: 100)\n", ids ? "ok" : "MISSING",
                ended ? "ok" : "MISSING", blocks);

    int rc = (ids && ended && blocks == 100U) ? 0 : 1;

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

    std::printf("usage: ysf_interop_tool capture <samples.raw> | replay <samples.raw>\n");
    return 2;
}
