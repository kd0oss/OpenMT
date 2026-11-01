/* The DMR mode against the repo's own dmr_host (see run_dmr_interop.sh).
 *
 *   dmr_interop_tool capture <samples.raw> <bursts.txt>
 *       Transmits a call (stand in vocoder, blocks with valid Golay words) to a fake Modem_Host and writes the 24 kHz
 *       samples (16 bit little endian) and the bursts that were sent (hex, one per line).
 *   dmr_interop_tool replay <samples.raw>
 *       Receives the samples dmr_host transmitted for the call dmr_sim sent (12 voice bursts, ids 1234 -> 9) and checks
 *       the call and the speech blocks.
 */

#define main core_test_main
#include "core_test.cpp"
#undef main

extern "C"
{
#include "sim_common.h"
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

static int capture(const char* samplesPath, const char* burstsPath)
{
    FakeHost host;
    RadioEngine engine;
    EngineSettings s;
    std::vector<std::vector<uint8_t>> bursts;
    std::mutex burstMutex;

    s.mode            = MODE_DMR;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 0;
    s.dmr.colorCode   = 1;
    s.dmr.srcId       = 1234;
    s.dmr.dstId       = 9;
    s.dmr.group       = true;
    s.dmr.txDelayMs   = 360;
    s.dmr.modeHangMs  = 200;
    s.dmr.txTap       = [&](const uint8_t* b)
    {
        std::lock_guard<std::mutex> l(burstMutex);
        bursts.push_back(std::vector<uint8_t>(b, b + 33));
    };

    std::string error;

    if (!engine.start(s, error))
    {
        std::printf("start failed: %s\n", error.c_str());
        return 2;
    }

    const unsigned int voiceBursts = 12U;
    std::vector<int16_t> pcm(voiceBursts * 3U * 160U, 0);

    for (unsigned int b = 0U; b < voiceBursts * 3U; b++)
    {
        uint8_t chip[9];

        makeDmrBlock(b + 1U, chip);
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

    FILE* f = std::fopen(burstsPath, "w");
    for (size_t i = 0U; f && i < bursts.size(); i++)
    {
        for (int k = 0; k < 33; k++)
            std::fprintf(f, "%02X", bursts[i][k]);
        std::fprintf(f, "\n");
    }
    if (f)
        std::fclose(f);
    std::printf("wrote %zu bursts\n", bursts.size());

    engine.stop();
    return 0;
}

static int replay(const char* samplesPath)
{
    FakeHost host;
    Recorder rec;
    RadioEngine engine;
    EngineSettings s;

    s.mode            = MODE_DMR;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 0;
    s.dmr.colorCode   = 1;
    /* DMR_REPLAY_REPEATER=<slot>: the radio listens to a repeater's two slot downlink on that time slot */
    if (std::getenv("DMR_REPLAY_REPEATER") != NULL)
    {
        s.dmr.repeater = true;
        s.dmr.slot     = (unsigned int)std::atoi(std::getenv("DMR_REPLAY_REPEATER"));
    }

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
    /* optional damage, to see how much the receiver stands: DMR_REPLAY_GAIN, DMR_REPLAY_DC, DMR_REPLAY_NOISE (rms) */
    {
        const char* g = std::getenv("DMR_REPLAY_GAIN");
        const char* d = std::getenv("DMR_REPLAY_DC");
        const char* n = std::getenv("DMR_REPLAY_NOISE");
        double gain = g ? std::atof(g) : 1.0, dc = d ? std::atof(d) : 0.0, noise = n ? std::atof(n) : 0.0;
        uint32_t rnd = 1U;

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
    float ber = 100.0F;

    for (size_t i = 0U; i < rec.calls.size(); i++)
    {
        const CallInfo& c = rec.calls[i];

        std::printf("  call event %d src %u dst %u group %d ber %.2f\n", (int)c.event, c.src, c.dst, c.group ? 1 : 0, c.ber);
        if ((c.event == CALL_RX_START || c.event == CALL_RX_INFO) && c.src == 1234U && c.dst == 9U && c.group)
            ids = true;
        if (c.event == CALL_RX_END)
        {
            ended = true;
            ber = c.ber;
        }
    }

    /* the sim's voice: burst i (seed i + 1), vocoder frame n is makeDmrBlock((i + 1) * 3 + n) */
    size_t blocks = rec.rx.size() / 160U, right = 0U;

    for (size_t r = 0U; r < blocks; r++)
    {
        uint8_t want[9];
        bool same = true;

        makeDmrBlock((unsigned int)(r / 3U + 1U) * 3U + (unsigned int)(r % 3U), want);
        for (unsigned int k = 0U; k < 9U && same; k++)
            same = (rec.rx[r * 160U + k] == (int16_t)want[k]);
        if (same)
            right++;
    }

    std::printf("  ids %s, terminator %s, ber %.2f%%, %zu speech blocks, %zu right (12 bursts = 36 blocks sent)\n",
                ids ? "ok" : "MISSING", ended ? "ok" : "MISSING", ber, blocks, right);

    int rc = (ids && ended && ber < 2.0F && blocks >= 35U && right >= 35U) ? 0 : 1;

    std::printf("%s\n", rc == 0 ? "PASS" : "FAIL");
    engine.stop();
    return rc;
}


/* The radio as a subscriber of a repeater: a model of the modem and the repeater (the downlink the real dmr_host sent,
   played in a loop, one 96 sample packet every 4 ms, with the radio's transmit samples played at the same clock like the
   SX1255 bridge does).  The radio keys up on its time slot; where its bursts land against the downlink is measured.
     dmr_interop_tool repeater-tx <downlink.raw> <slot 1|2> <report|noreport> [timing ms] */
static int repeaterTx(const char* downPath, unsigned int slot, bool report, int timingMs, const char* outPath, const char* burstsPath)
{
    FakeHost host;
    RadioEngine engine;
    EngineSettings s;
    std::vector<uint64_t> starts;
    std::mutex startMutex;
    std::vector<std::string> logs;
    std::vector<std::vector<uint8_t>> sentBursts;

    s.mode            = MODE_DMR;
    s.noVocoder       = true;
    s.link.basePort   = host.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 200;
    s.dmr.colorCode   = 1;
    s.dmr.srcId       = 1234;
    s.dmr.dstId       = 9;
    s.dmr.repeater    = true;
    s.dmr.slot        = slot;
    s.dmr.txDelayMs   = 360;
    s.dmr.modeHangMs  = 200;
    s.dmr.txTimingMs  = timingMs;
    s.dmr.txTap       = [&](const uint8_t* b)
    {
        std::lock_guard<std::mutex> l(startMutex);
        sentBursts.push_back(std::vector<uint8_t>(b, b + 33));
    };
    s.dmr.txStart     = [&](uint64_t sym)
    {
        std::lock_guard<std::mutex> l(startMutex);
        starts.push_back(sym);
    };
    engine.events.log = [&](const std::string& t)
    {
        std::lock_guard<std::mutex> l(startMutex);
        logs.push_back(t);
        std::printf("  log: %s\n", t.c_str());
    };

    std::vector<int16_t> down = readRaw(downPath);
    down.resize((down.size() / 1440U) * 1440U);
    if (down.size() < 1440U * 10U)
    {
        std::printf("the downlink recording is too short\n");
        return 2;
    }

    std::string error;

    if (!engine.start(s, error))
    {
        std::printf("start failed: %s\n", error.c_str());
        return 2;
    }
    waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.gotMode; }, 2000);

    std::atomic<bool> stopModem(false);
    std::atomic<long> txStartTick(-1);
    std::atomic<long> tickNow(0);

    std::thread modem([&]()
    {
        size_t played = 0U;
        long tick = 0;
        auto next = std::chrono::steady_clock::now();
        bool ended = false;

        while (!stopModem)
        {
            uint8_t pkt[200];

            std::memset(pkt, 0x00, sizeof(pkt));
            pkt[0] = 0x61;
            pkt[2] = 200;
            pkt[3] = 4;
            std::memcpy(pkt + 4, "SAMP", 4);
            for (unsigned int i = 0U; i < 96U; i++)
            {
                int16_t v = down[((size_t)tick * 96U + i) % down.size()];

                pkt[8 + i * 2U] = (uint8_t)(v & 0xFF);
                pkt[9 + i * 2U] = (uint8_t)((v >> 8) & 0xFF);
            }
            send(host.connFd, pkt, sizeof(pkt), MSG_NOSIGNAL);

            /* the transmit packet that goes on the air in this tick */
            {
                std::lock_guard<std::mutex> l(host.mutex);

                if (host.seots > 0U && played >= host.samples.size())
                    ended = true;
                if (!ended && host.samples.size() >= played + 96U)
                {
                    if (txStartTick < 0)
                        txStartTick = tick;
                    if (report && (played / 96U) % 75U == 0U)
                    {
                        uint8_t tm[16] = {0x61, 0x00, 16, 0x04, 'T', 'X', 'T', 'M'};
                        int32_t off = -96;
                        uint32_t k = (uint32_t)played;

                        for (int b = 0; b < 4; b++)
                        {
                            tm[8 + b]  = (uint8_t)((uint32_t)off >> (24 - 8 * b));
                            tm[12 + b] = (uint8_t)(k >> (24 - 8 * b));
                        }
                        send(host.connFd, tm, sizeof(tm), MSG_NOSIGNAL);
                    }
                    played += 96U;
                }
            }
            tick++;
            tickNow = tick;
            next += std::chrono::milliseconds(4);
            std::this_thread::sleep_until(next);
        }
    });

    sleepMs(1500);

    const unsigned int voiceBursts = 12U;
    std::vector<int16_t> pcm(voiceBursts * 3U * 160U, 0);

    for (unsigned int b = 0U; b < voiceBursts * 3U; b++)
    {
        uint8_t chip[9];

        makeDmrBlock(b + 1U, chip);
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
    waitFor([&host]() { std::lock_guard<std::mutex> l(host.mutex); return host.seots >= 1U; }, 10000);
    sleepMs(500);
    stopModem = true;
    modem.join();

    std::vector<uint64_t> st;
    {
        std::lock_guard<std::mutex> l(startMutex);
        st = starts;
    }
    int rc = 0;
    long t0 = txStartTick;

    if (outPath != NULL)
    {
        std::lock_guard<std::mutex> l(host.mutex);
        FILE* f = std::fopen(outPath, "wb");

        for (size_t i = 0U; f && i < host.samples.size(); i++)
        {
            uint8_t b[2] = {(uint8_t)(host.samples[i] & 0xFF), (uint8_t)((host.samples[i] >> 8) & 0xFF)};
            std::fwrite(b, 1, 2, f);
        }
        if (f)
            std::fclose(f);
    }
    if (burstsPath != NULL)
    {
        FILE* f = std::fopen(burstsPath, "w");

        for (size_t i = 0U; f && i < sentBursts.size(); i++)
        {
            for (int k = 0; k < 33; k++)
                std::fprintf(f, "%02X", sentBursts[i][k]);
            std::fprintf(f, "\n");
        }
        if (f)
            std::fclose(f);
    }

    std::printf("  the radio sent %zu bursts in slot %u, its first sample was on the air at tick %ld\n", st.size(), slot, t0);
    if (st.size() < voiceBursts + 3U || t0 < 0)
    {
        std::printf("FAIL: too few bursts\n");
        return 1;
    }

    /* the sync of a burst of slot 1 of the downlink ends at sample 469 of the 1440 sample cycle (12 CACH symbols, burst
       symbol 77, 24 samples of pulse delay), slot 2 half a cycle later; the uplink sync is meant 31 samples after */
    int worst = 0;
    long spread = 0, lo = 1000000, hi = -1000000;

    for (size_t i = 0U; i < st.size(); i++)
    {
        long r = 96L * t0 + 5L * (long)(st[i] + 77U) + 24L;
        long want = 469L + (slot == 2U ? 720L : 0L) + 31L;
        long err = ((r - want) % 1440L + 1440L + 720L) % 1440L - 720L;

        if (err < lo) lo = err;
        if (err > hi) hi = err;
        if (std::abs((int)err) > std::abs(worst))
            worst = (int)err;
    }
    spread = hi - lo;
    std::printf("  uplink sync against the wanted place (%s): worst %+d samples (%.2f ms), spread %ld samples\n",
                report ? "TX timing report" : "clocks", worst, worst / 24.0, spread);

    int limit = report ? 6 : 480;       /* the clocks alone are good to a few ms only */

    if (std::abs(worst) > limit)
        rc = 1;
    /* consecutive bursts of the slot are 60 ms apart */
    for (size_t i = 1U; i < st.size(); i++)
    {
        long d = (long)(st[i] - st[i - 1U]);

        if (d < 287 || d > 289)
        {
            std::printf("  burst %zu is %ld symbols after the last\n", i, d);
            rc = 1;
        }
    }
    std::printf("%s\n", rc == 0 ? "PASS" : "FAIL");
    engine.stop();
    return rc;
}

/* The radio and the real dmr_host as the two ends of a repeater: this program is the modem of the host (it listens on port
   18000 and plays the gateway on 18400, so dmr_host must be started after it) and, at the same time, the modem of the
   radio.  Samples go both ways in 4 ms steps with no delay, like one RF channel: what the host transmits (its downlink)
   is what the radio receives, what the radio transmits is what the host receives; each side is also given the TX timing
   report of its own modem.  The repeater starts idle: the radio has to wake it up.
     dmr_interop_tool repeater-loop <slot 1|2> */
static int repeaterLoop(unsigned int slot)
{
    FakeHost radioModem;
    const bool radioReport = std::getenv("LOOP_NO_REPORT") == NULL;     /* LOOP_NO_REPORT=1: the radio's modem sends no TX timing report */
    RadioEngine engine;
    EngineSettings s;
    std::vector<std::vector<uint8_t>> sentBursts;
    std::vector<std::string> logs;
    std::mutex burstMutex;

    s.mode            = MODE_DMR;
    s.noVocoder       = true;
    s.link.basePort   = radioModem.port;
    s.link.modem      = 1;
    s.link.txAheadMs  = 200;
    s.dmr.colorCode   = 1;
    s.dmr.srcId       = 1234;
    s.dmr.dstId       = 9;
    s.dmr.group       = true;
    s.dmr.repeater    = true;
    s.dmr.slot        = slot;
    s.dmr.txDelayMs   = 360;
    s.dmr.modeHangMs  = 200;
    /* LOOP_TIMING_MS=30 with LOOP_NO_REPORT=1 puts the first call in the other slot; LOOP_CALLS=2 sends two calls: the
       radio has to see where the repeater put the first and move the second */
    const unsigned int calls = std::getenv("LOOP_CALLS") ? (unsigned int)std::atoi(std::getenv("LOOP_CALLS")) : 1U;
    s.dmr.txTimingMs  = std::getenv("LOOP_TIMING_MS") ? std::atoi(std::getenv("LOOP_TIMING_MS")) : 0;
    s.dmr.txTap       = [&](const uint8_t* b)
    {
        std::lock_guard<std::mutex> l(burstMutex);
        sentBursts.push_back(std::vector<uint8_t>(b, b + 33));
    };
    engine.events.log = [&](const std::string& t)
    {
        std::lock_guard<std::mutex> l(burstMutex);
        logs.push_back(t);
        std::printf("  radio: %s\n", t.c_str());
    };
    engine.events.call = [&](const CallInfo& c)
    {
        std::printf("  radio: call event %d src %u dst %u\n", (int)c.event, c.src, c.dst);
    };

    std::string error;

    if (!engine.start(s, error))
    {
        std::printf("start failed: %s\n", error.c_str());
        return 2;
    }
    waitFor([&radioModem]() { std::lock_guard<std::mutex> l(radioModem.mutex); return radioModem.gotMode; }, 2000);

    int lfd = simListen(18000);
    std::printf("waiting for dmr_host on port 18000...\n");
    int mfd = simAccept(lfd, 30000);
    if (mfd < 0)
    {
        std::printf("dmr_host did not connect\n");
        return 2;
    }

    static SimConn hostC, gwC;
    simConnInit(&hostC, mfd);

    SimPacket reg;
    if (simRead(&hostC, &reg, 10000) != 1 || std::memcmp(reg.data + 8, "DMR", 3) != 0)
    {
        std::printf("no registration for DMR\n");
        return 2;
    }
    int gfd = simConnect(18400, 50);
    if (gfd < 0)
    {
        std::printf("could not connect to the gateway port\n");
        return 2;
    }
    simConnInit(&gwC, gfd);
    {
        const uint8_t comm[1] = {0x00};

        simSend(&hostC, "COMM", comm, 1);
    }

    struct Gw { uint8_t ctl; uint8_t slot; uint8_t burst[33]; uint8_t ids[7]; };
    std::vector<Gw> gw;
    std::vector<int16_t> hostQ;             /* host transmit samples waiting to go on the air */
    size_t hostQAt = 0U, hostPlayed = 0U, radioPlayed = 0U;
    unsigned int radioSeots = 0U;
    bool hostEnded = false;
    std::atomic<bool> callDone(false);

    std::thread caller([&]()
    {
        sleepMs(2500);                      /* the repeater is idle: no downlink */

        const unsigned int voiceBursts = 12U;
        std::vector<int16_t> pcm(voiceBursts * 3U * 160U, 0);

        for (unsigned int b = 0U; b < voiceBursts * 3U; b++)
        {
            uint8_t chip[9];

            makeDmrBlock(b + 1U, chip);
            for (unsigned int k = 0U; k < 9U; k++)
                pcm[b * 160U + k] = (int16_t)chip[k];
        }
        for (unsigned int call = 0U; call < calls; call++)
        {
            if (call > 0U)
                sleepMs(6000);              /* the repeater un-keys after its hang time */
            engine.ptt(true);
            for (size_t pos = 0U; pos < pcm.size(); pos += 160U)
            {
                engine.pushMic(pcm.data() + pos, 160U);
                sleepMs(20);
            }
            sleepMs(50);
            engine.ptt(false);
        }
        callDone = true;
    });

    auto next = std::chrono::steady_clock::now();
    long tick = 0, afterCall = 0;

    while (afterCall < 1500)                /* 6 s after the call */
    {
        /* what the host sent to its modem */
        SimPacket p;
        while (simRead(&hostC, &p, 0) == 1)
        {
            if (std::memcmp(p.data + 4, "SAMP", 4) == 0)
            {
                if (hostEnded)
                {
                    hostEnded = false;
                    hostQ.clear();
                    hostQAt = hostPlayed = 0U;
                }
                for (int i = 8; i + 1 < p.len; i += 2)
                    hostQ.push_back((int16_t)(p.data[i] | (p.data[i + 1] << 8)));
            }
            else if (std::memcmp(p.data + 4, "SEOT", 4) == 0)
                hostEnded = true;
        }

        /* what goes on the air in this step */
        int16_t down[96], up[96];
        bool hostOn = false, radioOn = false;

        for (unsigned int i = 0U; i < 96U; i++)
        {
            down[i] = 0;
            up[i]   = 0;
        }
        if (hostQAt + 96U <= hostQ.size())
        {
            for (unsigned int i = 0U; i < 96U; i++)
                down[i] = hostQ[hostQAt + i];
            hostOn = true;
        }
        {
            std::lock_guard<std::mutex> l(radioModem.mutex);

            if (radioModem.seots > radioSeots && radioPlayed >= radioModem.samples.size())
            {
                radioSeots = radioModem.seots;
                radioModem.samples.clear();
                radioPlayed = 0U;
            }
            if (radioPlayed + 96U <= radioModem.samples.size())
            {
                for (unsigned int i = 0U; i < 96U; i++)
                    up[i] = radioModem.samples[radioPlayed + i];
                radioOn = true;
            }
        }

        uint8_t pkt[200] = {0x61, 0x00, 200, 0x04, 'S', 'A', 'M', 'P'};

        for (unsigned int i = 0U; i < 96U; i++)
        {
            pkt[8 + i * 2U] = (uint8_t)(down[i] & 0xFF);
            pkt[9 + i * 2U] = (uint8_t)((down[i] >> 8) & 0xFF);
        }
        send(radioModem.connFd, pkt, sizeof(pkt), MSG_NOSIGNAL);
        for (unsigned int i = 0U; i < 96U; i++)
        {
            pkt[8 + i * 2U] = (uint8_t)(up[i] & 0xFF);
            pkt[9 + i * 2U] = (uint8_t)((up[i] >> 8) & 0xFF);
        }
        simSendRaw(&hostC, pkt, 200);

        auto timing = [](int fd, bool isHostConn, SimConn* c, size_t k)
        {
            uint8_t tm[16] = {0x61, 0x00, 16, 0x04, 'T', 'X', 'T', 'M'};
            int32_t off = -96;

            for (int b = 0; b < 4; b++)
            {
                tm[8 + b]  = (uint8_t)((uint32_t)off >> (24 - 8 * b));
                tm[12 + b] = (uint8_t)((uint32_t)k >> (24 - 8 * b));
            }
            if (isHostConn)
                simSendRaw(c, tm, 16);
            else
                send(fd, tm, sizeof(tm), MSG_NOSIGNAL);
        };

        if (hostOn)
        {
            if ((hostPlayed / 96U) % 75U == 0U)
                timing(0, true, &hostC, hostPlayed);
            hostQAt += 96U;
            hostPlayed += 96U;
        }
        if (radioOn)
        {
            if (radioReport && (radioPlayed / 96U) % 75U == 0U)
                timing(radioModem.connFd, false, NULL, radioPlayed);
            radioPlayed += 96U;
        }

        simDrain(&gwC, 1, 1, [](const SimPacket* q, void* arg)
        {
            std::vector<Gw>* v = (std::vector<Gw>*)arg;

            if ((std::memcmp(q->data + 4, "DMD1", 4) == 0 || std::memcmp(q->data + 4, "DMD2", 4) == 0) && q->len == 49)
            {
                Gw g;

                g.ctl  = q->data[8];
                g.slot = q->data[7] == '2' ? 2 : 1;
                std::memcpy(g.burst, q->data + 9, 33);
                std::memcpy(g.ids, q->data + 42, 7);
                v->push_back(g);
            }
        }, &gw);

        tick++;
        if (callDone)
            afterCall++;
        next += std::chrono::milliseconds(4);
        std::this_thread::sleep_until(next);
    }
    caller.join();
    engine.stop();

    std::printf("the gateway got %zu bursts\n", gw.size());

    int header = -1, term = -1, voice = 0, voiceOk = 0, wrongSlot = 0;
    size_t voiceSent = 0U;

    {
        std::lock_guard<std::mutex> l(burstMutex);

        for (size_t i = 0U; i < sentBursts.size(); i++)
            (void)i;
        for (size_t i = 0U; i < gw.size(); i++)
        {
            if (gw[i].slot != slot)
                wrongSlot++;
            if ((gw[i].ctl & 0x4FU) == 0x41U && header < 0)
                header = (int)i;
            else if ((gw[i].ctl & 0x4FU) == 0x42U)
                term = (int)i;
            else if ((gw[i].ctl & 0x40U) == 0U)
            {
                voice++;
                for (size_t j = 0U; j < sentBursts.size(); j++)
                {
                    const uint8_t* a = gw[i].burst;
                    const uint8_t* b = sentBursts[j].data();

                    if (std::memcmp(a, b, 12) == 0 && std::memcmp(a + 22, b + 22, 10) == 0 && (a[32] & 0xFCU) == (b[32] & 0xFCU))
                    {
                        voiceOk++;
                        break;
                    }
                }
            }
        }
        voiceSent = 12U;
    }

    bool woke = false, timed = false;
    for (size_t i = 0U; i < logs.size(); i++)
    {
        if (logs[i].find("sending the wake-up") != std::string::npos)
            woke = true;
        if (logs[i].find("timed from the downlink") != std::string::npos)
            timed = true;
    }

    CHECK(woke, "the radio woke the repeater up");
    CHECK(timed, "the radio timed its call from the downlink");
    CHECK(header >= 0, "the voice LC header reached the gateway");
    if (header >= 0)
    {
        unsigned int dst = ((unsigned int)gw[header].ids[0] << 16) | ((unsigned int)gw[header].ids[1] << 8) | gw[header].ids[2];
        unsigned int src = ((unsigned int)gw[header].ids[3] << 16) | ((unsigned int)gw[header].ids[4] << 8) | gw[header].ids[5];

        CHECK(src == 1234U && dst == 9U, "header ids: source %u destination %u", src, dst);
    }
    CHECK(voiceOk >= (int)voiceSent - 1, "voice bursts that match one the radio sent: %d of %zu (%d voice bursts in all)", voiceOk,
          voiceSent, voice);
    CHECK(term >= 0, "the terminator reached the gateway");
    if (calls == 1U)
        CHECK(wrongSlot == 0, "the host put the call in the radio's time slot %u (%d bursts in the other)", slot, wrongSlot);
    else
    {
        /* the slot of each call is that of its first burst */
        std::vector<unsigned int> callSlots;
        bool open = false;

        for (size_t i = 0U; i < gw.size(); i++)
        {
            if (!open && (gw[i].ctl & 0x4FU) == 0x41U)
            {
                callSlots.push_back(gw[i].slot);
                open = true;
            }
            if ((gw[i].ctl & 0x4FU) == 0x42U)
                open = false;
        }
        std::printf("  the slots the repeater took the calls for:");
        for (size_t i = 0U; i < callSlots.size(); i++)
            std::printf(" %u", callSlots[i]);
        std::printf(" (the radio is on slot %u)\n", slot);
        CHECK(callSlots.size() >= calls, "every call reached the gateway (%zu of %u)", callSlots.size(), calls);
        CHECK(!callSlots.empty() && callSlots.back() == slot, "the last call is in the radio's slot");
    }

    std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc == 4 && std::strcmp(argv[1], "capture") == 0)
        return capture(argv[2], argv[3]);
    if (argc == 3 && std::strcmp(argv[1], "repeater-loop") == 0)
        return repeaterLoop((unsigned int)std::atoi(argv[2]));
    if (argc >= 5 && std::strcmp(argv[1], "repeater-tx") == 0)
        return repeaterTx(argv[2], (unsigned int)std::atoi(argv[3]), std::strcmp(argv[4], "report") == 0,
                          argc > 5 ? std::atoi(argv[5]) : 0, argc > 6 ? argv[6] : NULL, argc > 7 ? argv[7] : NULL);
    if (argc == 3 && std::strcmp(argv[1], "replay") == 0)
        return replay(argv[2]);

    std::printf("usage: dmr_interop_tool capture <samples.raw> <bursts.txt> | replay <samples.raw>\n");
    return 2;
}
