#include "modem_link.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace openmt
{

static uint64_t nowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static const uint8_t SETMODE[] = { 0x61, 0x00, 0x05, 0x01, 0x02 };
static const uint8_t SETIDLE[] = { 0x61, 0x00, 0x05, 0x01, 0x03 };

ModemLink::ModemLink() :
    cfg_(),
    fd_(-1),
    open_(false),
    modeHeld_(false),
    reader_(),
    writeMutex_(),
    rxPackets_(0U),
    rxSampPackets_(0U),
    rxSampCount_(0U),
    rxAbsSum_(0U),
    samplesFn_(),
    duplexFn_(),
    closedFn_(),
    count_(0U),
    playEnd_(0U)
{
    std::memset(pkt_, 0x00, sizeof(pkt_));
}

ModemLink::~ModemLink()
{
    close();
}

void ModemLink::setHandlers(const SamplesFn& samples, const DuplexFn& duplex, const ClosedFn& closed)
{
    samplesFn_ = samples;
    duplexFn_  = duplex;
    closedFn_  = closed;
}

void ModemLink::setLogHandler(const std::function<void(const std::string&)>& log)
{
    logFn_ = log;
}

void ModemLink::setTimingHandler(const TimingFn& timing)
{
    timingFn_ = timing;
}

bool ModemLink::open(const Config& cfg, std::string& error)
{
    if (open_)
    {
        error = "already connected";
        return false;
    }

    cfg_ = cfg;

    struct addrinfo hints;
    struct addrinfo* res = NULL;
    char port[16];

    std::memset(&hints, 0x00, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::snprintf(port, sizeof(port), "%u", cfg_.basePort + (cfg_.modem > 0U ? cfg_.modem - 1U : 0U));

    int rc = getaddrinfo(cfg_.host.c_str(), port, &hints, &res);
    if (rc != 0 || res == NULL)
    {
        error = std::string("cannot resolve ") + cfg_.host + ": " + gai_strerror(rc);
        return false;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0)
    {
        error = std::string("socket: ") + std::strerror(errno);
        freeaddrinfo(res);
        return false;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    /* connect with a time limit: a host that is not there must not hang the program */
    struct timeval tv;
    tv.tv_sec  = 4;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0)
    {
        error = std::string("connect to ") + cfg_.host + ":" + port + " failed: " + std::strerror(errno);
        ::close(fd);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    tv.tv_sec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    fd_ = fd;
    open_ = true;
    modeHeld_ = false;

    if (!registerMode())
    {
        error = "could not register with Modem_Host";
        close();
        return false;
    }

    rxPackets_ = rxSampPackets_ = rxSampCount_ = rxAbsSum_ = 0U;
    otherTypes_.clear();
    reader_ = std::thread(&ModemLink::readLoop, this);
    watcher_ = std::thread(&ModemLink::watchLoop, this);
    return true;
}

void ModemLink::close()
{
    bool wasOpen = open_.exchange(false);

    if (fd_ >= 0)
        shutdown(fd_, SHUT_RDWR);               /* wakes the reader */

    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id())
        reader_.join();
    if (watcher_.joinable() && watcher_.get_id() != std::this_thread::get_id())
        watcher_.join();

    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
    modeHeld_ = false;
    (void)wasOpen;
}

bool ModemLink::isOpen() const
{
    return open_;
}

bool ModemLink::writeAll(const uint8_t* data, size_t length)
{
    std::lock_guard<std::mutex> lock(writeMutex_);
    size_t sent = 0U;

    while (sent < length && fd_ >= 0)
    {
        ssize_t n = send(fd_, data + sent, length - sent, MSG_NOSIGNAL);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        sent += (size_t)n;
    }
    return sent == length;
}

bool ModemLink::readFully(uint8_t* data, size_t length)
{
    size_t got = 0U;

    while (got < length)
    {
        ssize_t n = recv(fd_, data + got, length - got, 0);

        if (n == 0)
            return false;
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

/* The MODE packet that registers this client: name at 8, modem type at 19, TX level at 25 (as the hosts send it) */
bool ModemLink::registerMode()
{
    uint8_t buffer[84];

    std::memset(buffer, 0x00, sizeof(buffer));
    buffer[0] = 0x61;
    buffer[1] = 0x00;
    buffer[2] = (uint8_t)sizeof(buffer);
    buffer[3] = 0x04;
    std::memcpy(buffer + 4, "MODE", 4);
    std::strncpy((char*)buffer + 8, cfg_.modeName.c_str(), 10);
    std::strncpy((char*)buffer + 19, cfg_.modemType.c_str(), 5);
    buffer[25] = cfg_.txLevel;

    return writeAll(buffer, sizeof(buffer));
}

bool ModemLink::requestMode()
{
    if (!open_)
        return false;
    modeHeld_ = true;
    return writeAll(SETMODE, sizeof(SETMODE));
}

bool ModemLink::releaseMode()
{
    if (!open_ || !modeHeld_)
        return false;
    modeHeld_ = false;
    return writeAll(SETIDLE, sizeof(SETIDLE));
}

bool ModemLink::forceIdle()
{
    if (!open_)
        return false;
    modeHeld_ = false;
    return writeAll(SETIDLE, sizeof(SETIDLE));
}

bool ModemLink::modeHeld() const
{
    return modeHeld_;
}

void ModemLink::txBegin()
{
    count_   = 0U;
    playEnd_ = nowMs();
    std::memset(pkt_, 0x00, sizeof(pkt_));
}

void ModemLink::sendPacket(const char* type)
{
    uint64_t now = nowMs();

    if (playEnd_ < now)
        playEnd_ = now;

    while (cfg_.txAheadMs > 0U && open_ && playEnd_ - now > cfg_.txAheadMs)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        now = nowMs();
    }

    pkt_[0] = 0x61;
    pkt_[1] = 0x00;
    pkt_[2] = (uint8_t)PacketBytes;
    pkt_[3] = 0x04;
    std::memcpy(pkt_ + 4, type, 4);

    if (open_)
        writeAll(pkt_, sizeof(pkt_));

    if (std::memcmp(type, "SAMP", 4) == 0)
        playEnd_ += (SamplesPerPacket * 1000U) / 24000U;

    count_ = 0U;
    std::memset(pkt_ + 8, 0x00, sizeof(pkt_) - 8U);
}

void ModemLink::txSamples(const int16_t* samples, size_t count)
{
    for (size_t i = 0U; i < count; i++)
    {
        uint8_t* p = pkt_ + 8 + count_ * 2U;

        p[0] = (uint8_t)(samples[i] & 0xFF);
        p[1] = (uint8_t)((samples[i] >> 8) & 0xFF);
        count_++;

        if (count_ == SamplesPerPacket)
            sendPacket("SAMP");
    }
}

void ModemLink::txEnd()
{
    if (count_ > 0U)
        sendPacket("SAMP");

    /* the modem un-keys when it gets the end and drops what it has not yet sent out: let it play the last samples first */
    if (cfg_.txAheadMs > 0U)
    {
        const uint64_t until = playEnd_ + cfg_.txDrainMs;

        while (open_ && nowMs() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    sendPacket("SEOT");
}

void ModemLink::readLoop()
{
    uint8_t buffer[2048];

    while (open_)
    {
        if (!readFully(buffer, 1))
            break;

        if (buffer[0] != 0x61)
            continue;

        if (!readFully(buffer + 1, 3))
            break;

        unsigned int len = (unsigned int)((buffer[1] << 8) + buffer[2]);

        if (len < 5U || len > sizeof(buffer))
            continue;

        if (len > 4U && !readFully(buffer + 4, len - 4U))
            break;

        if (buffer[3] != 4 || len < 8U)
            continue;

        rxPackets_++;

        if (std::memcmp(buffer + 4, "COMM", 4) == 0)
        {
            if (len > 8U && buffer[8] == 0x00 && duplexFn_)
                duplexFn_(true);
            else if (len > 8U && buffer[8] == 0x01 && duplexFn_)
                duplexFn_(false);
        }
        else if (std::memcmp(buffer + 4, "TXTM", 4) == 0)
        {
            if (len >= 16U && timingFn_)
            {
                int32_t offset = (int32_t)(((uint32_t)buffer[8] << 24) | ((uint32_t)buffer[9] << 16) |
                                           ((uint32_t)buffer[10] << 8) | (uint32_t)buffer[11]);
                uint32_t k = ((uint32_t)buffer[12] << 24) | ((uint32_t)buffer[13] << 16) | ((uint32_t)buffer[14] << 8) |
                             (uint32_t)buffer[15];

                timingFn_(offset, k);
            }
        }
        else if (std::memcmp(buffer + 4, "SAMP", 4) == 0 && samplesFn_)
        {
            int16_t smp[1024];
            unsigned int count = (len - 8U) / 2U;

            if (count > 1024U)
                count = 1024U;

            for (unsigned int i = 0U; i < count; i++)
                smp[i] = (int16_t)(buffer[8 + i * 2U] | (buffer[9 + i * 2U] << 8));

            uint64_t sum = 0U;

            for (unsigned int i = 0U; i < count; i++)
                sum += (uint64_t)(smp[i] < 0 ? -(int)smp[i] : (int)smp[i]);
            rxSampPackets_++;
            rxSampCount_ += count;
            rxAbsSum_ += sum;

            samplesFn_(smp, count);
        }
        else if (std::memcmp(buffer + 4, "SAMP", 4) != 0)
        {
            std::string t((const char*)buffer + 4, 4U);
            std::lock_guard<std::mutex> lock(typeMutex_);

            if (otherTypes_.find(t) == std::string::npos && otherTypes_.size() < 40U)
                otherTypes_ += (otherTypes_.empty() ? "" : " ") + t;
        }
    }

    bool wasOpen = open_.exchange(false);
    if (wasOpen && closedFn_)
        closedFn_();
}

/* What Modem_Host delivers, reported when it changes: the first samples (with their level), samples that have stopped, and
   once at the start when nothing has come.  This is how a silent receiver is told from a deaf one. */
void ModemLink::watchLoop()
{
    uint64_t lastCount = 0U, lastAbs = 0U, windowStart = 0U;
    unsigned int quiet = 0U, ticks = 0U;
    bool flowing = false, told = false;

    auto say = [this](const std::string& text)
    {
        if (logFn_)
            logFn_(text);
    };

    while (open_)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        ticks++;

        uint64_t count = rxSampCount_, sum = rxAbsSum_;

        if (count != lastCount)
        {
            if (!flowing)
            {
                char text[160];

                std::snprintf(text, sizeof(text), "Modem_Host: samples are arriving (average level %.0f)",
                              (double)(sum - lastAbs) / (double)(count - lastCount));
                say(text);
                flowing = true;
                windowStart = count;
            }
            quiet = 0U;
            lastCount = count;
            lastAbs = sum;
        }
        else if (flowing && ++quiet >= 8U)           /* 2 s */
        {
            char text[160];

            std::snprintf(text, sizeof(text), "Modem_Host: samples stopped after %llu samples (%.1f s)",
                          (unsigned long long)(count - windowStart), (double)(count - windowStart) / 24000.0);
            say(text);
            flowing = false;
            lastAbs = sum;
        }

        if (!told && ticks == 20U && count == 0U)    /* 5 s */
        {
            std::string others;
            {
                std::lock_guard<std::mutex> lock(typeMutex_);
                others = otherTypes_;
            }
            say("Modem_Host: no samples have arrived in 5 s (the modem may send none while there is no signal); packets of other types: " +
                (others.empty() ? std::string("none") : others) + "; " + std::to_string((unsigned long long)rxPackets_.load()) +
                " packets in all");
            told = true;
        }
    }
}

}
