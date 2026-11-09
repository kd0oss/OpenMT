#include "rf_spectrum.h"
#include "iq_monitor.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cmath>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>

namespace openmt
{

static long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static const double CHANNEL_HALF_HZ = 6250.0;
static const double NOISE_FROM_HZ   = 10000.0;
static const double HANN_ENBW_DB    = 1.76;            /* the noise bandwidth of the bridge's Hann window is 1.5 bins */

bool parseRfFrame(const uint8_t* d, size_t length, RfFrame& f, std::vector<uint8_t>* raw)
{
    if (length < 20U || std::memcmp(d, "SPEC", 4) != 0 || d[4] != 1U)
        return false;

    const unsigned int n = (unsigned int)d[6] | ((unsigned int)d[7] << 8);

    if (n < 64U || n > 4096U || length < 20U + n)
        return false;

    f.centreHz = (uint32_t)d[8] | ((uint32_t)d[9] << 8) | ((uint32_t)d[10] << 16) | ((uint32_t)d[11] << 24);
    f.spanHz   = (uint32_t)d[12] | ((uint32_t)d[13] << 8) | ((uint32_t)d[14] << 16) | ((uint32_t)d[15] << 24);
    if (f.spanHz == 0U)
        return false;
    f.overload       = (d[5] & 1U) != 0U;
    f.adcPeak        = d[16];
    f.adcClipPercent = ((double)d[18] + 256.0 * (double)d[19]) / 100.0;
    f.binHz          = (double)f.spanHz / (double)n;
    f.db.resize(n);
    for (unsigned int k = 0U; k < n; k++)
        f.db[k] = -0.5F * (float)d[20 + k];
    if (raw != NULL)
        raw->assign(d + 20, d + 20 + n);

    /* the channel power, the strongest bin in the channel, the noise floor */
    double lin = 0.0;
    double peak = -200.0;
    double peakHz = 0.0;
    std::vector<float> outside;

    for (unsigned int k = 0U; k < n; k++)
    {
        double hz = ((double)k - (double)n / 2.0) * f.binHz;

        if (std::fabs(hz) <= CHANNEL_HALF_HZ)
        {
            lin += std::pow(10.0, f.db[k] / 10.0);
            if (f.db[k] > peak)
            {
                peak   = f.db[k];
                peakHz = hz;
            }
        }
        else if (std::fabs(hz) >= NOISE_FROM_HZ)
        {
            outside.push_back(f.db[k]);
        }
    }
    f.channelDb    = lin > 0.0 ? 10.0 * std::log10(lin) - HANN_ENBW_DB : -200.0;
    f.peakDb       = peak;
    f.peakOffsetHz = peakHz;
    if (!outside.empty())
    {
        std::nth_element(outside.begin(), outside.begin() + (long)(outside.size() / 2U), outside.end());
        f.noiseDb = outside[outside.size() / 2U];
    }
    f.snrDb = f.peakDb - f.noiseDb;
    return true;
}

SpectrumClient::SpectrumClient() :
    fd_(-1),
    running_(false),
    wanted_(false),
    iqWanted_(false),
    iqSink_(nullptr),
    lastMs_(0),
    frames_(0),
    nTargets_(0U),
    automatic_(false),
    lockedPort_(0U)
{
    std::memset(targets_, 0, sizeof(targets_));
}

SpectrumClient::~SpectrumClient()
{
    stop();
}

std::string SpectrumClient::target() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return target_;
}

bool SpectrumClient::start(const std::string& host, unsigned int port, std::string& error)
{
    std::vector<unsigned int> ports;

    if (port != 0U)
        ports.push_back(port);
    stop();
    if (ports.empty())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target_.clear();
        return true;
    }
    automatic_ = false;
    return startAuto(host, ports, error);
}

bool SpectrumClient::startAuto(const std::string& host, const std::vector<unsigned int>& ports, std::string& error)
{
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target_.clear();
    }
    nTargets_ = 0U;
    lockedPort_ = 0U;
    automatic_ = ports.size() > 1U;
    if (ports.empty())
        return true;

    for (size_t i = 0U; i < ports.size() && i < MaxTargets; i++)
    {
        struct addrinfo hints;
        struct addrinfo* res = NULL;
        char portText[16];

        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        std::snprintf(portText, sizeof(portText), "%u", ports[i]);
        if (getaddrinfo(host.c_str(), portText, &hints, &res) != 0 || res == NULL)
        {
            error = "cannot find " + host + " for the RF spectrum";
            return false;
        }
        if (res->ai_addrlen > sizeof(targets_[i].addr))
        {
            freeaddrinfo(res);
            error = "address too long";
            return false;
        }
        std::memcpy(targets_[i].addr, res->ai_addr, res->ai_addrlen);
        targets_[i].len  = res->ai_addrlen;
        targets_[i].port = ports[i];
        freeaddrinfo(res);
        nTargets_++;
    }

    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0)
    {
        error = "cannot open the RF spectrum socket";
        return false;
    }
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 200000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.clear();
        last_ = RfFrame();
        lastMs_ = 0;
    }
    frames_ = 0;
    host_ = host;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!automatic_)
            target_ = host + ":" + std::to_string(ports[0]);
        else
        {
            target_ = host + ": trying ports";
            for (size_t i = 0U; i < nTargets_; i++)
                target_ += (i == 0U ? " " : ", ") + std::to_string(targets_[i].port);
        }
    }
    running_ = true;
    thread_ = std::thread(&SpectrumClient::run, this);
    return true;
}

void SpectrumClient::stop()
{
    running_ = false;
    if (thread_.joinable())
        thread_.join();
    if (fd_ >= 0)
    {
        close(fd_);
        fd_ = -1;
    }
}

void SpectrumClient::setWanted(bool wanted)
{
    wanted_ = wanted;
}

void SpectrumClient::setIqSink(IqMonitor* sink)
{
    iqSink_ = sink;
}

void SpectrumClient::setIqWanted(bool wanted)
{
    iqWanted_ = wanted;
}

void SpectrumClient::run()
{
    long long lastAsk = 0;
    long long lastIqAsk = 0;
    long long lastAnyMs = nowMs();
    uint8_t buffer[8192];

    while (running_)
    {
        const bool askSpec = wanted_ && nowMs() - lastAsk >= 1000;
        const bool askIq = iqWanted_ && iqSink_ != nullptr && nowMs() - lastIqAsk >= 1000;

        /* no answer for 5 s from the one that answered: ask them all again */
        if (automatic_ && lockedPort_ != 0U && (wanted_ || iqWanted_) && nowMs() - lastAnyMs > 5000)
        {
            lockedPort_ = 0U;
            std::lock_guard<std::mutex> lock(mutex_);
            target_ = host_ + ": trying ports";
            for (size_t i = 0U; i < nTargets_; i++)
                target_ += (i == 0U ? " " : ", ") + std::to_string(targets_[i].port);
        }
        for (size_t i = 0U; i < nTargets_ && (askSpec || askIq); i++)
        {
            if (lockedPort_ != 0U && targets_[i].port != lockedPort_)
                continue;
            if (askSpec)
                (void)sendto(fd_, "SPEC", 4, 0, (const struct sockaddr*)targets_[i].addr, (socklen_t)targets_[i].len);
            if (askIq)
                (void)sendto(fd_, "IQST", 4, 0, (const struct sockaddr*)targets_[i].addr, (socklen_t)targets_[i].len);
        }
        if (askSpec)
            lastAsk = nowMs();
        if (askIq)
            lastIqAsk = nowMs();

        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        ssize_t r = recvfrom(fd_, buffer, sizeof(buffer), 0, (struct sockaddr*)&from, &fromLen);

        if (r <= 0)
            continue;                                   /* the receive timeout: look at running_ and the request timer */

        /* the answer comes from the port that was asked: that is the bridge to stay with */
        const unsigned int fromPort = (unsigned int)ntohs(from.sin_port);

        if (automatic_ && lockedPort_ != 0U && fromPort != lockedPort_)
            continue;                                   /* another bridge that answered too: one is enough */

        bool good = false;

        if (r >= 4 && buffer[0] == 'I' && buffer[1] == 'Q')
        {
            IqMonitor* sink = iqSink_;
            IqFrame iq;

            if (sink != nullptr && parseIqFrame(buffer, (size_t)r, iq))
            {
                sink->feed(iq);
                good = true;
            }
        }
        else
        {
            RfFrame f;
            std::vector<uint8_t> raw;

            if (parseRfFrame(buffer, (size_t)r, f, &raw))
            {
                std::lock_guard<std::mutex> lock(mutex_);

                last_   = f;
                lastMs_ = nowMs();
                history_.push_back(raw);
                while (history_.size() > HistoryRows)
                    history_.pop_front();
                frames_++;
                good = true;
            }
        }
        if (!good)
            continue;
        lastAnyMs = nowMs();
        if (automatic_ && lockedPort_ == 0U)
        {
            for (size_t i = 0U; i < nTargets_; i++)
            {
                if (targets_[i].port == fromPort)
                {
                    lockedPort_ = fromPort;
                    std::lock_guard<std::mutex> lock(mutex_);
                    target_ = host_ + ":" + std::to_string(fromPort);
                    break;
                }
            }
        }
    }
}

RfSpectrumSnapshot SpectrumClient::snapshot()
{
    RfSpectrumSnapshot s;
    std::lock_guard<std::mutex> lock(mutex_);

    s.valid = lastMs_ != 0 && nowMs() - lastMs_ < 2000;
    s.frame = last_;
    s.history.assign(history_.begin(), history_.end());
    return s;
}

}
