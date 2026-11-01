/* The D-STAR mode against the repo's own dstar_host (see run_dstar_interop.sh).
 *
 *   dstar_interop_tool capture <samples.raw>
 *       Transmits a call (stand in vocoder, 50 voice blocks, KD0OSS -> CQCQCQ via N0CALL B / N0CALL G) to a fake Modem_Host and writes the 24 kHz
 *       samples (16 bit little endian).
 *   dstar_interop_tool replay <samples.raw>
 *       Receives the samples dstar_host transmitted for the call dstar_sim sent (N0CALL/TEST -> CQCQCQ, 42 voice frames) and checks
 *       the call.
 */

#define main core_test_main
#include "core_test.cpp"
#undef main

/* An AMBE frame with valid error correction, so that nothing along the way changes it: seed b */
static void makeAmbe(uint8_t* ambe, unsigned int da, unsigned int db, unsigned int c)
{
    unsigned int a = golay24128_encode24128(da);
    unsigned int bb = golay24128_encode24128(db) ^ AMBE_PRNG_TABLE[da];
    unsigned int m = 0x800000U;

    std::memset(ambe, 0, 9);
    for (unsigned int i = 0U; i < 24U; i++, m >>= 1)
    {
        DSTAR_FEC_WRITE(ambe, DSTAR_FEC_A[i], a & m);
        DSTAR_FEC_WRITE(ambe, DSTAR_FEC_B[i], bb & m);
        DSTAR_FEC_WRITE(ambe, DSTAR_FEC_C[i], c & m);
    }
}

/* the speech the radio is given (block b), as dstar_radio_sim.c expects it */
static void radioBlock(unsigned int b, uint8_t* chip)
{
    makeAmbe(chip, (37U * b + 5U) & 0xFFFU, (91U * b + 17U) & 0xFFFU, (0x5A5A5AU + 0x10101U * b) & 0xFFFFFFU);
}

/* the frames dstar_sim.c sends from the network: frame i is makeAmbe(seed i + 1) */
static void hostBlock(unsigned int i, uint8_t* ambe)
{
    unsigned int seed = i + 1U;

    makeAmbe(ambe, (seed * 211U + 17U) & 0xFFFU, (seed * 97U + 5U) & 0xFFFU, (seed * 40503U + 12345U) & 0xFFFFFFU);
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

    s.mode            = MODE_DSTAR;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 0;
    s.dstar.callsign    = "KD0OSS";
    s.dstar.ur          = "CQCQCQ";
    s.dstar.rpt1        = "N0CALL B";
    s.dstar.rpt2        = "N0CALL G";
    s.dstar.message     = "OPENMT RADIO TEST 01";
    s.dstar.txDelayMs   = 300;
    s.dstar.modeHangMs  = 200;

    std::string error;

    if (!engine.start(s, error))
    {
        std::printf("start failed: %s\n", error.c_str());
        return 2;
    }

    /* 50 voice blocks: block b carries the AMBE frame radioBlock(b) (the stand in vocoder keeps the chip block) */
    const unsigned int blocks = 50U;
    std::vector<int16_t> pcm(blocks * 160U, 0);

    for (unsigned int b = 0U; b < blocks; b++)
    {
        uint8_t chip[9];

        radioBlock(b, chip);
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

    s.mode            = MODE_DSTAR;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 0;
    s.dstar.callsign    = "KD0OSS";
    s.dstar.rpt1        = "N0CALL B";
    s.dstar.rpt2        = "N0CALL G";

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
        if ((c.event == CALL_RX_START || c.event == CALL_RX_INFO) && c.text == "N0CALL/TEST>CQCQCQ")
            ids = true;
        if (c.event == CALL_RX_END || c.event == CALL_RX_LOST)
            ended = true;
    }

    size_t blocks = rec.rx.size() / 160U;

    std::printf("  callsigns %s, call end %s, %zu speech blocks (the call has 42 frames)\n", ids ? "ok" : "MISSING",
                ended ? "ok" : "MISSING", blocks);

    unsigned int same = 0U;

    for (size_t i = 0U; i < blocks && i < 42U; i++)
    {
        uint8_t want[9];
        bool eq = true;

        hostBlock((unsigned int)i, want);
        for (unsigned int k = 0U; k < 9U && eq; k++)
            eq = (rec.rx[i * 160U + k] == (int16_t)want[k]);
        if (eq)
            same++;
    }
    std::printf("  %u of 42 speech blocks identical to the ones sent\n", same);

    int rc = (ids && ended && blocks == 42U && same == 42U) ? 0 : 1;

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

    std::printf("usage: dstar_interop_tool capture <samples.raw> | replay <samples.raw>\n");
    return 2;
}
