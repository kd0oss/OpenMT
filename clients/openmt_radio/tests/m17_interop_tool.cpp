/* The M17 mode against the repo's own m17_host (see run_m17_interop.sh).
 *
 *   m17_interop_tool capture <samples.raw>
 *       Transmits a call (stand in for Codec2, 20 voice blocks in 10 stream frames, KD0OSS -> ALL, CAN 5) to a fake Modem_Host and writes the 24 kHz
 *       samples (16 bit little endian).
 *   m17_interop_tool replay <samples.raw>
 *       Receives the samples m17_host transmitted for the call m17_sim sent (N0CALL -> ALL, 12 stream frames) and checks
 *       the call.
 */

#define main core_test_main
#include "core_test.cpp"
#undef main

/* the speech the radio is given: block b is 8 bytes (the stand in for Codec2 keeps them), as m17_radio_sim.cpp expects */
static void radioBlock(unsigned int b, uint8_t* block)
{
    for (unsigned int k = 0U; k < 8U; k++)
        block[k] = (uint8_t)(0x1DU * (k + 1U) + 53U * b + 7U);
}

/* the frames m17_sim.cpp sends from the network: frame i has the payload (i * 29 + j * 7 + 3) */
static void hostPayload(unsigned int i, uint8_t* payload)
{
    for (unsigned int j = 0U; j < 16U; j++)
        payload[j] = (uint8_t)(i * 29U + j * 7U + 3U);
}

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

    s.mode            = MODE_M17;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 0;
    s.m17.callsign    = "KD0OSS";
    s.m17.dest        = "ALL";
    s.m17.can         = 5;
    s.m17.txDelayMs   = 300;
    s.m17.modeHangMs  = 200;

    std::string error;

    if (!engine.start(s, error))
    {
        std::printf("start failed: %s\n", error.c_str());
        return 2;
    }

    /* 20 voice blocks: block b carries the 8 bytes radioBlock(b) (the stand in for Codec2 keeps them) */
    const unsigned int blocks = 20U;
    std::vector<int16_t> pcm(blocks * 160U, 0);

    for (unsigned int b = 0U; b < blocks; b++)
    {
        uint8_t chip[8];

        radioBlock(b, chip);
        for (unsigned int k = 0U; k < 8U; k++)
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

/* A text message in packet mode, long enough for several frames; m17_radio_sim checks it at the gateway of m17_host */
static const char* const MESSAGE = "Testing M17 packet mode from OpenMT Radio: the quick brown fox jumps over the lazy dog, 0123456789.";

static int captureMessage(const char* samplesPath)
{
    FakeHost host;
    RadioEngine engine;
    EngineSettings s;

    s.mode            = MODE_M17;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 0;
    s.m17.callsign    = "KD0OSS";
    s.m17.dest        = "ALL";
    s.m17.can         = 5;
    s.m17.txDelayMs   = 300;
    s.m17.modeHangMs  = 200;

    std::string error;

    if (!engine.start(s, error))
    {
        std::printf("start failed: %s\n", error.c_str());
        return 2;
    }
    if (!engine.sendMessage("W1AW", MESSAGE, error))
    {
        std::printf("the message was not accepted: %s\n", error.c_str());
        return 2;
    }
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

    s.mode            = MODE_M17;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 0;
    s.m17.callsign    = "KD0OSS";

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
        if ((c.event == CALL_RX_START || c.event == CALL_RX_INFO) && c.text == "N0CALL>ALL CAN 0")
            ids = true;
        if (c.event == CALL_RX_END || c.event == CALL_RX_LOST)
            ended = true;
    }

    size_t blocks = rec.rx.size() / 160U;

    std::printf("  callsigns %s, call end %s, %zu speech blocks (the call has 12 frames: 24)\n", ids ? "ok" : "MISSING",
                ended ? "ok" : "MISSING", blocks);

    unsigned int same = 0U;

    for (size_t i = 0U; i < blocks && i < 24U; i++)
    {
        uint8_t payload[16];
        bool eq = true;

        hostPayload((unsigned int)(i / 2U), payload);
        for (unsigned int k = 0U; k < 8U && eq; k++)
            eq = (rec.rx[i * 160U + k] == (int16_t)payload[(i % 2U) * 8U + k]);
        if (eq)
            same++;
    }
    std::printf("  %u of 24 speech blocks identical to the ones sent\n", same);

    int rc = (ids && ended && blocks == 24U && same == 24U) ? 0 : 1;

    std::printf("%s\n", rc == 0 ? "PASS" : "FAIL");
    engine.stop();
    return rc;
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "capture") == 0)
        return capture(argv[2]);
    if (argc == 3 && std::strcmp(argv[1], "capture-msg") == 0)
        return captureMessage(argv[2]);
    if (argc == 3 && std::strcmp(argv[1], "replay") == 0)
        return replay(argv[2]);

    std::printf("usage: m17_interop_tool capture <samples.raw> | replay <samples.raw>\n");
    return 2;
}
