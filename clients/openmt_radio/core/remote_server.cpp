#include "remote_server.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace openmt
{

static const int            PROTOCOL_VERSION   = 1;
static const size_t         MAX_MESSAGE        = 256U * 1024U;
static const size_t         DROP_AUDIO_ABOVE   = 48U * 1024U;       /* bytes waiting for a slow client: audio is dropped above this */
static const size_t         DROP_CLIENT_ABOVE  = 2U * 1024U * 1024U;
static const char* const    WS_GUID            = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Helpers                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

static std::string hexOf(const unsigned char* data, size_t n)
{
    static const char* digits = "0123456789abcdef";
    std::string out;

    for (size_t i = 0U; i < n; i++)
    {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15U];
    }
    return out;
}

static bool randomBytes(unsigned char* out, size_t n)
{
    return RAND_bytes(out, (int)n) == 1;
}

/* a frame of a WebSocket, as a server sends it (no mask) */
static std::string wsFrame(unsigned int opcode, const std::string& payload)
{
    std::string f;
    size_t n = payload.size();

    f += (char)(0x80U | (opcode & 15U));
    if (n < 126U)
        f += (char)n;
    else if (n < 65536U)
    {
        f += (char)126;
        f += (char)((n >> 8) & 0xFFU);
        f += (char)(n & 0xFFU);
    }
    else
    {
        f += (char)127;
        for (int shift = 56; shift >= 0; shift -= 8)
            f += (char)((n >> shift) & 0xFFU);
    }
    f += payload;
    return f;
}

static std::string closeFrame(unsigned int code, const std::string& reason)
{
    std::string p;

    p += (char)((code >> 8) & 0xFFU);
    p += (char)(code & 0xFFU);
    p += reason.substr(0, 100);
    return wsFrame(0x8U, p);
}

static std::string acceptKey(const std::string& key)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dl = 0U;
    std::string in = key + WS_GUID;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();

    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(ctx, in.data(), in.size());
    EVP_DigestFinal_ex(ctx, digest, &dl);
    EVP_MD_CTX_free(ctx);

    unsigned char b64[64];
    int n = EVP_EncodeBlock(b64, digest, (int)dl);

    return std::string((const char*)b64, (size_t)n);
}

static std::string lower(std::string s)
{
    for (size_t i = 0U; i < s.size(); i++)
        s[i] = (char)std::tolower((unsigned char)s[i]);
    return s;
}

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");

    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1U);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Tokens, MACs, mu-law                                                                                         */
/* ------------------------------------------------------------------------------------------------------------ */

static const char TOKEN_ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";       /* 32 symbols: no I, O, 0, 1 */

std::string RemoteServer::generateToken()
{
    unsigned char r[24];
    std::string t;

    if (!randomBytes(r, sizeof(r)))
        return std::string();
    for (size_t i = 0U; i < 24U; i++)
        t += TOKEN_ALPHABET[r[i] & 31U];
    return t;
}

std::string RemoteServer::normalizeToken(const std::string& text)
{
    std::string t;

    for (size_t i = 0U; i < text.size(); i++)
    {
        unsigned char c = (unsigned char)text[i];

        if (std::isalnum(c))
            t += (char)std::toupper(c);
    }
    return t;
}

std::string RemoteServer::formatToken(const std::string& token)
{
    std::string t;

    for (size_t i = 0U; i < token.size(); i++)
    {
        if (i > 0U && i % 4U == 0U)
            t += '-';
        t += token[i];
    }
    return t;
}

std::string RemoteServer::authMac(const std::string& token, char prefix, const std::string& nonce, const std::string& cnonce)
{
    std::string msg;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int ml = 0U;

    msg += prefix;
    msg += nonce;
    msg += cnonce;
    HMAC(EVP_sha256(), token.data(), (int)token.size(), (const unsigned char*)msg.data(), msg.size(), md, &ml);
    return hexOf(md, ml);
}

uint8_t RemoteServer::ulawEncode(int16_t sample)
{
    const int BIAS = 0x84, CLIP = 32635;
    int s = sample;
    int sign = (s >> 8) & 0x80;

    if (sign != 0)
        s = -s;
    if (s > CLIP)
        s = CLIP;
    s += BIAS;

    int exponent = 7;

    for (int mask = 0x4000; (s & mask) == 0 && exponent > 0; mask >>= 1)
        exponent--;

    int mantissa = (s >> (exponent + 3)) & 0x0F;

    return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

int16_t RemoteServer::ulawDecode(uint8_t code)
{
    code = (uint8_t)~code;

    int sign = code & 0x80;
    int exponent = (code >> 4) & 7;
    int mantissa = code & 15;
    int s = (((mantissa << 3) + 0x84) << exponent) - 0x84;

    return (int16_t)(sign != 0 ? -s : s);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Certificate                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------ */

bool RemoteServer::generateCertificate(const std::string& certFile, const std::string& keyFile, std::string& error)
{
    EVP_PKEY* pkey = NULL;
    EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    X509* x = NULL;
    bool ok = false;

    if (kctx != NULL && EVP_PKEY_keygen_init(kctx) > 0 && EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1) > 0 &&
        EVP_PKEY_keygen(kctx, &pkey) > 0)
    {
        x = X509_new();
        if (x != NULL)
        {
            unsigned char serial[8];

            randomBytes(serial, sizeof(serial));
            serial[0] &= 0x7FU;
            BIGNUM* bn = BN_bin2bn(serial, (int)sizeof(serial), NULL);

            if (bn != NULL)
            {
                BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x));
                BN_free(bn);
            }
            X509_set_version(x, 2);
            X509_gmtime_adj(X509_getm_notBefore(x), -3600);
            X509_gmtime_adj(X509_getm_notAfter(x), 3650L * 24L * 3600L);
            X509_set_pubkey(x, pkey);

            X509_NAME* name = X509_get_subject_name(x);

            X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)"OpenMT Radio", -1, -1, 0);
            X509_set_issuer_name(x, name);
            ok = X509_sign(x, pkey, EVP_sha256()) > 0;
        }
    }

    if (ok)
    {
        int kfd = ::open(keyFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        FILE* kf = kfd >= 0 ? fdopen(kfd, "w") : NULL;
        FILE* cf = std::fopen(certFile.c_str(), "w");

        if (kf == NULL || cf == NULL)
        {
            error = "cannot write " + std::string(kf == NULL ? keyFile : certFile);
            ok = false;
        }
        else
        {
            fchmod(kfd, 0600);
            ok = PEM_write_PrivateKey(kf, pkey, NULL, NULL, 0, NULL, NULL) == 1 && PEM_write_X509(cf, x) == 1;
            if (!ok)
                error = "cannot write the certificate";
        }
        if (kf != NULL)
            std::fclose(kf);
        if (cf != NULL)
            std::fclose(cf);
    }
    else if (error.empty())
        error = "cannot make a certificate";

    if (x != NULL)
        X509_free(x);
    if (pkey != NULL)
        EVP_PKEY_free(pkey);
    if (kctx != NULL)
        EVP_PKEY_CTX_free(kctx);
    return ok;
}

std::string RemoteServer::certFingerprint(const std::string& certFile)
{
    FILE* f = std::fopen(certFile.c_str(), "r");

    if (f == NULL)
        return std::string();

    X509* x = PEM_read_X509(f, NULL, NULL, NULL);

    std::fclose(f);
    if (x == NULL)
        return std::string();

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int ml = 0U;
    std::string out;

    if (X509_digest(x, EVP_sha256(), md, &ml) == 1)
    {
        static const char* digits = "0123456789ABCDEF";

        for (unsigned int i = 0U; i < ml; i++)
        {
            if (i > 0U)
                out += ':';
            out += digits[md[i] >> 4];
            out += digits[md[i] & 15U];
        }
    }
    X509_free(x);
    return out;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Clients                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

struct RemoteServer::Client
{
    uint64_t           id = 0U;
    int                fd = -1;
    SSL*               ssl = NULL;
    std::string        peer;
    std::string        ip;
    std::thread        thread;
    std::atomic<bool>  done;
    std::atomic<bool>  dead;
    std::atomic<bool>  authed;
    int                wakeR = -1;
    int                wakeW = -1;

    std::mutex              qmu;
    std::deque<std::string> q;
    size_t                  qBytes = 0U;
    uint16_t                seq = 0U;

    std::atomic<bool>  audioRx;
    std::atomic<bool>  audioTx;
    std::atomic<int>   codec;                   /* 0 pcm16, 1 ulaw */
    std::atomic<int>   monitorMs;               /* 0 = not subscribed */

    /* only the client's own thread */
    std::string        wbuf;
    size_t             woff = 0U;
    std::string        rbuf;
    bool               wantWrite = false;
    bool               closing = false;
    long long          closeAt = 0;

    Client() : done(false), dead(false), authed(false), audioRx(false), audioTx(false), codec(0), monitorMs(0) {}

    /* the wake pipe stays open as long as anything can hold this client: other threads call wake() */
    ~Client()
    {
        if (wakeR >= 0)
            ::close(wakeR);
        if (wakeW >= 0)
            ::close(wakeW);
    }

    void wake()
    {
        if (wakeW >= 0)
        {
            char b = 1;

            if (::write(wakeW, &b, 1) < 0)
            {
                /* the pipe is full: the thread is awake anyway */
            }
        }
    }

    /* false when the frame was not queued */
    bool enqueue(const std::string& frame, bool droppable)
    {
        {
            std::lock_guard<std::mutex> lock(qmu);

            if (dead)
                return false;
            if (droppable && qBytes > DROP_AUDIO_ABOVE)
                return false;
            if (qBytes > DROP_CLIENT_ABOVE)
            {
                dead = true;                        /* does not keep up with the events: drop it */
            }
            else
            {
                qBytes += frame.size();
                q.push_back(frame);
            }
        }
        wake();
        return !dead;
    }
};

struct RemoteServer::Impl
{
    SSL_CTX*                              ctx = NULL;
    int                                   listenFd = -1;
    std::thread                           acceptThread;
    std::thread                           watchThread;

    std::mutex                            mu;               /* clients, state, nextId */
    std::vector<std::shared_ptr<Client> > clients;
    Json                                  state;
    uint64_t                              nextId = 1U;

    std::mutex                            pttMu;
    long long                             pttOwner = 0;     /* 0 none, -1 the window, else a client id */
    long long                             pttKeepalive = 0;
    long long                             pttStart = 0;
    std::string                           pttWho;

    struct Fail { unsigned int count = 0U; long long until = 0; };
    std::mutex                            failMu;
    std::map<std::string, Fail>           fails;
};

RemoteServer::RemoteServer() :
    impl_(new Impl()),
    cfg_(),
    host_(NULL),
    running_(false),
    boundPort_(0U)
{
}

RemoteServer::~RemoteServer()
{
    stop();
}

void RemoteServer::log(const std::string& text)
{
    if (host_ != NULL)
        host_->remoteLog(text);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Start and stop                                                                                               */
/* ------------------------------------------------------------------------------------------------------------ */

bool RemoteServer::start(const RemoteConfig& config, RemoteHost* host, std::string& error)
{
    if (running_)
    {
        error = "already running";
        return false;
    }
    if (host == NULL)
    {
        error = "no host";
        return false;
    }

    cfg_ = config;
    cfg_.token = normalizeToken(config.token);
    host_ = host;

    if (cfg_.token.size() < 16U)
    {
        error = "the pairing token must have at least 16 characters";
        return false;
    }

    std::signal(SIGPIPE, SIG_IGN);

    if (cfg_.tls)
    {
        SSL_library_init();
        SSL_load_error_strings();
        impl_->ctx = SSL_CTX_new(TLS_server_method());
        if (impl_->ctx == NULL)
        {
            error = "cannot start TLS";
            return false;
        }
        SSL_CTX_set_min_proto_version(impl_->ctx, TLS1_2_VERSION);
        SSL_CTX_set_options(impl_->ctx, SSL_OP_NO_COMPRESSION);
        SSL_CTX_set_mode(impl_->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        if (SSL_CTX_use_certificate_chain_file(impl_->ctx, cfg_.certFile.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(impl_->ctx, cfg_.keyFile.c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(impl_->ctx) != 1)
        {
            error = "cannot use the certificate " + cfg_.certFile + " and key " + cfg_.keyFile;
            SSL_CTX_free(impl_->ctx);
            impl_->ctx = NULL;
            return false;
        }
    }

    struct addrinfo hints;
    struct addrinfo* res = NULL;
    char portText[16];

    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    std::snprintf(portText, sizeof(portText), "%u", cfg_.port);

    if (getaddrinfo(cfg_.bindAddress.c_str(), portText, &hints, &res) != 0 || res == NULL)
    {
        error = "cannot use the address " + cfg_.bindAddress;
        if (impl_->ctx != NULL)
        {
            SSL_CTX_free(impl_->ctx);
            impl_->ctx = NULL;
        }
        return false;
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    if (fd >= 0)
    {
        int one = 1;

        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (::bind(fd, res->ai_addr, res->ai_addrlen) != 0 || ::listen(fd, 8) != 0)
        {
            error = std::string("cannot listen on port ") + portText + ": " + std::strerror(errno);
            ::close(fd);
            fd = -1;
        }
    }
    else
        error = std::string("cannot open a socket: ") + std::strerror(errno);
    freeaddrinfo(res);

    if (fd < 0)
    {
        if (impl_->ctx != NULL)
        {
            SSL_CTX_free(impl_->ctx);
            impl_->ctx = NULL;
        }
        return false;
    }

    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);

    boundPort_ = cfg_.port;
    if (getsockname(fd, (struct sockaddr*)&ss, &sl) == 0)
    {
        if (ss.ss_family == AF_INET)
            boundPort_ = ntohs(((struct sockaddr_in*)&ss)->sin_port);
        else if (ss.ss_family == AF_INET6)
            boundPort_ = ntohs(((struct sockaddr_in6*)&ss)->sin6_port);
    }

    impl_->listenFd = fd;
    running_ = true;
    impl_->acceptThread = std::thread(&RemoteServer::acceptLoop, this);
    impl_->watchThread = std::thread(&RemoteServer::watchdogLoop, this);
    return true;
}

void RemoteServer::stop()
{
    if (!running_ && impl_->listenFd < 0)
        return;

    running_ = false;
    if (impl_->acceptThread.joinable())
        impl_->acceptThread.join();
    if (impl_->watchThread.joinable())
        impl_->watchThread.join();
    if (impl_->listenFd >= 0)
    {
        ::close(impl_->listenFd);
        impl_->listenFd = -1;
    }

    std::vector<std::shared_ptr<Client> > all;

    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        all.swap(impl_->clients);
    }
    for (size_t i = 0U; i < all.size(); i++)
    {
        all[i]->dead = true;
        all[i]->wake();
    }
    for (size_t i = 0U; i < all.size(); i++)
        if (all[i]->thread.joinable())
            all[i]->thread.join();

    releasePtt("the server stopped");
    if (impl_->ctx != NULL)
    {
        SSL_CTX_free(impl_->ctx);
        impl_->ctx = NULL;
    }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Accepting                                                                                                    */
/* ------------------------------------------------------------------------------------------------------------ */

static std::string peerOf(int fd, std::string& ip)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    char host[INET6_ADDRSTRLEN] = "?";
    unsigned int port = 0U;

    if (getpeername(fd, (struct sockaddr*)&ss, &sl) == 0)
    {
        if (ss.ss_family == AF_INET)
        {
            inet_ntop(AF_INET, &((struct sockaddr_in*)&ss)->sin_addr, host, sizeof(host));
            port = ntohs(((struct sockaddr_in*)&ss)->sin_port);
        }
        else if (ss.ss_family == AF_INET6)
        {
            inet_ntop(AF_INET6, &((struct sockaddr_in6*)&ss)->sin6_addr, host, sizeof(host));
            port = ntohs(((struct sockaddr_in6*)&ss)->sin6_port);
        }
    }
    ip = host;
    return ip + ":" + std::to_string(port);
}

void RemoteServer::acceptLoop()
{
    while (running_)
    {
        struct pollfd p;

        p.fd = impl_->listenFd;
        p.events = POLLIN;
        p.revents = 0;
        if (::poll(&p, 1, 200) > 0 && (p.revents & POLLIN) != 0)
        {
            int fd = ::accept(impl_->listenFd, NULL, NULL);

            if (fd >= 0)
            {
                std::shared_ptr<Client> c(new Client());
                bool refuse = false;

                c->fd = fd;
                c->peer = peerOf(fd, c->ip);

                {
                    std::lock_guard<std::mutex> lock(impl_->failMu);
                    std::map<std::string, Impl::Fail>::iterator it = impl_->fails.find(c->ip);

                    if (it != impl_->fails.end() && it->second.until > nowMs())
                        refuse = true;              /* too many wrong tokens from this address: it waits */
                }

                int pipefd[2] = {-1, -1};

                if (!refuse && ::pipe(pipefd) != 0)
                    refuse = true;
                if (!refuse)
                {
                    std::lock_guard<std::mutex> lock(impl_->mu);

                    if (impl_->clients.size() >= (size_t)cfg_.maxClients * 2U + 4U)
                        refuse = true;
                }

                if (refuse)
                {
                    if (pipefd[0] >= 0)
                    {
                        ::close(pipefd[0]);
                        ::close(pipefd[1]);
                    }
                    ::close(fd);
                }
                else
                {
                    int one = 1;

                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    ::fcntl(pipefd[0], F_SETFL, ::fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);
                    ::fcntl(pipefd[1], F_SETFL, ::fcntl(pipefd[1], F_GETFL) | O_NONBLOCK);
                    c->wakeR = pipefd[0];
                    c->wakeW = pipefd[1];
                    {
                        std::lock_guard<std::mutex> lock(impl_->mu);
                        c->id = impl_->nextId++;
                        impl_->clients.push_back(c);
                    }
                    c->thread = std::thread(&RemoteServer::clientMain, this, c);
                }
            }
        }

        /* the clients that have finished */
        std::vector<std::shared_ptr<Client> > finished;

        {
            std::lock_guard<std::mutex> lock(impl_->mu);

            for (size_t i = 0U; i < impl_->clients.size();)
            {
                if (impl_->clients[i]->done)
                {
                    finished.push_back(impl_->clients[i]);
                    impl_->clients.erase(impl_->clients.begin() + (long)i);
                }
                else
                    i++;
            }
        }
        for (size_t i = 0U; i < finished.size(); i++)
            if (finished[i]->thread.joinable())
                finished[i]->thread.join();
    }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Push to talk                                                                                                 */
/* ------------------------------------------------------------------------------------------------------------ */

static Json pttJson(long long owner, const std::string& who, const std::string& reason)
{
    Json e = Json::object();

    e.set("type", "ptt");
    e.set("owner", owner == 0 ? "none" : (owner < 0 ? "local" : "remote"));
    e.set("who", who);
    if (!reason.empty())
        e.set("reason", reason);
    return e;
}

void RemoteServer::releasePtt(const char* reason)
{
    std::lock_guard<std::mutex> lock(impl_->pttMu);

    if (impl_->pttOwner > 0)
    {
        std::string err;

        host_->remotePtt(false, err);
        impl_->pttOwner = 0;
        impl_->pttWho.clear();
        log(std::string("remote: push to talk released (") + reason + ")");
        broadcast(pttJson(0, "", reason));
    }
}

void RemoteServer::watchdogLoop()
{
    while (running_)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const char* reason = NULL;

        {
            std::lock_guard<std::mutex> lock(impl_->pttMu);

            if (impl_->pttOwner > 0)
            {
                long long now = nowMs();

                if (now - impl_->pttKeepalive > (long long)cfg_.pttKeepaliveMs)
                    reason = "no keepalive from the client";
                else if (cfg_.maxTxSeconds > 0U && now - impl_->pttStart > (long long)cfg_.maxTxSeconds * 1000LL)
                    reason = "time limit";
            }
        }
        if (reason != NULL)
            releasePtt(reason);
    }
}

bool RemoteServer::localPtt(bool down)
{
    std::lock_guard<std::mutex> lock(impl_->pttMu);

    if (down)
    {
        if (impl_->pttOwner > 0)
            return false;
        if (impl_->pttOwner == 0)
        {
            impl_->pttOwner = -1;
            impl_->pttWho = "desktop";
            broadcast(pttJson(-1, "desktop", ""));
        }
        return true;
    }
    if (impl_->pttOwner < 0)
    {
        impl_->pttOwner = 0;
        impl_->pttWho.clear();
        broadcast(pttJson(0, "", "desktop released"));
    }
    return true;
}

std::string RemoteServer::pttOwner() const
{
    std::lock_guard<std::mutex> lock(impl_->pttMu);

    return impl_->pttOwner == 0 ? "none" : (impl_->pttOwner < 0 ? "local" : "remote");
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Sending                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

void RemoteServer::sendEvent(const std::shared_ptr<Client>& c, const Json& message)
{
    c->enqueue(wsFrame(0x1U, message.dump()), false);
}

void RemoteServer::reply(uint64_t client, long long id, bool ok, const Json& body)
{
    std::shared_ptr<Client> target;

    {
        std::lock_guard<std::mutex> lock(impl_->mu);

        for (size_t i = 0U; i < impl_->clients.size(); i++)
            if (impl_->clients[i]->id == client)
                target = impl_->clients[i];
    }
    if (!target)
        return;

    Json m = Json::object();

    m.set("type", "reply");
    m.set("id", id);
    m.set("ok", ok);
    m.set(ok ? "result" : "error", body);
    sendEvent(target, m);
}

void RemoteServer::broadcast(const Json& event)
{
    std::vector<std::shared_ptr<Client> > all;

    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        all = impl_->clients;
    }

    std::string frame = wsFrame(0x1U, event.dump());

    for (size_t i = 0U; i < all.size(); i++)
        if (all[i]->authed)
            all[i]->enqueue(frame, false);
}

void RemoteServer::setState(const Json& state)
{
    Json s = state;

    s.set("type", "state");
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->state = s;
    }
    broadcast(s);
}

unsigned int RemoteServer::clients() const
{
    unsigned int n = 0U;
    std::lock_guard<std::mutex> lock(impl_->mu);

    for (size_t i = 0U; i < impl_->clients.size(); i++)
        if (impl_->clients[i]->authed)
            n++;
    return n;
}

unsigned int RemoteServer::monitorSubscribers() const
{
    unsigned int n = 0U;
    std::lock_guard<std::mutex> lock(impl_->mu);

    for (size_t i = 0U; i < impl_->clients.size(); i++)
        if (impl_->clients[i]->authed && impl_->clients[i]->monitorMs > 0)
            n++;
    return n;
}

unsigned int RemoteServer::monitorIntervalMs() const
{
    int best = 0;
    std::lock_guard<std::mutex> lock(impl_->mu);

    for (size_t i = 0U; i < impl_->clients.size(); i++)
    {
        int m = impl_->clients[i]->monitorMs;

        if (impl_->clients[i]->authed && m > 0 && (best == 0 || m < best))
            best = m;
    }
    return (unsigned int)best;
}

void RemoteServer::broadcastMonitor(const Json& event)
{
    std::vector<std::shared_ptr<Client> > all;

    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        all = impl_->clients;
    }

    std::string frame = wsFrame(0x1U, event.dump());

    for (size_t i = 0U; i < all.size(); i++)
        if (all[i]->authed && all[i]->monitorMs > 0)
            all[i]->enqueue(frame, true);
}

void RemoteServer::sendRxAudio(const int16_t* pcm8k, size_t count)
{
    std::vector<std::shared_ptr<Client> > all;

    {
        std::lock_guard<std::mutex> lock(impl_->mu);

        for (size_t i = 0U; i < impl_->clients.size(); i++)
            if (impl_->clients[i]->authed && impl_->clients[i]->audioRx)
                all.push_back(impl_->clients[i]);
    }
    if (all.empty() || count == 0U)
        return;

    std::string pcm16, ulaw;

    pcm16.reserve(count * 2U);
    ulaw.reserve(count);
    for (size_t i = 0U; i < count; i++)
    {
        pcm16 += (char)(pcm8k[i] & 0xFF);
        pcm16 += (char)((pcm8k[i] >> 8) & 0xFF);
        ulaw += (char)ulawEncode(pcm8k[i]);
    }

    /* frames of 160 samples (20 ms), whatever the mode hands over at a time (P25 gives 180 ms) */
    const size_t FRAME = 160U;

    for (size_t i = 0U; i < all.size(); i++)
    {
        Client& c = *all[i];
        const int codec = c.codec.load();
        const std::string& samples = codec == 1 ? ulaw : pcm16;
        const size_t bytesPer = codec == 1 ? 1U : 2U;

        for (size_t first = 0U; first < count; first += FRAME)
        {
            size_t n = std::min(FRAME, count - first);
            std::string payload;
            unsigned int seq;

            {
                std::lock_guard<std::mutex> lock(c.qmu);
                seq = c.seq++;
            }
            payload += (char)0x01;
            payload += (char)codec;
            payload += (char)((seq >> 8) & 0xFFU);
            payload += (char)(seq & 0xFFU);
            payload.append(samples, first * bytesPer, n * bytesPer);
            c.enqueue(wsFrame(0x2U, payload), true);
        }
    }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* One client                                                                                                   */
/* ------------------------------------------------------------------------------------------------------------ */

namespace
{
    /* reading and writing through the TLS session or the plain socket: n > 0 data, 0 would block (or timed out), -1 closed or failed */
    struct Io
    {
        int  fd;
        SSL* ssl;
        bool wantWrite;

        int rd(void* buf, size_t len)
        {
            if (ssl != NULL)
            {
                ERR_clear_error();
                int n = SSL_read(ssl, buf, (int)len);

                if (n > 0)
                    return n;
                int e = SSL_get_error(ssl, n);

                if (e == SSL_ERROR_WANT_READ)
                    return 0;
                if (e == SSL_ERROR_WANT_WRITE)
                {
                    wantWrite = true;
                    return 0;
                }
                return -1;
            }
            ssize_t n = ::recv(fd, buf, len, 0);

            if (n > 0)
                return (int)n;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                return 0;
            return -1;
        }

        int wr(const void* buf, size_t len)
        {
            if (ssl != NULL)
            {
                ERR_clear_error();
                int n = SSL_write(ssl, buf, (int)len);

                if (n > 0)
                    return n;
                int e = SSL_get_error(ssl, n);

                if (e == SSL_ERROR_WANT_WRITE)
                {
                    wantWrite = true;
                    return 0;
                }
                if (e == SSL_ERROR_WANT_READ)
                    return 0;
                return -1;
            }
            ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL);

            if (n > 0)
                return (int)n;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            {
                wantWrite = true;
                return 0;
            }
            return -1;
        }
    };
}

void RemoteServer::clientMain(std::shared_ptr<Client> client)
{
    Client& c = *client;
    Io io;
    const long long started = nowMs();
    bool upgraded = false;
    std::string nonce;
    std::string msg;            /* the message being assembled from fragments */
    unsigned int msgOp = 0U;
    long long lastRx = nowMs();

    io.fd = c.fd;
    io.ssl = NULL;
    io.wantWrite = false;

    /* a blocking socket with short time outs for the TLS and HTTP handshakes */
    {
        struct timeval tv;

        tv.tv_sec = 0;
        tv.tv_usec = 300000;
        setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    bool ok = true;

    if (cfg_.tls)
    {
        c.ssl = SSL_new(impl_->ctx);
        if (c.ssl == NULL)
            ok = false;
        else
        {
            SSL_set_fd(c.ssl, c.fd);
            for (;;)
            {
                ERR_clear_error();
                int r = SSL_accept(c.ssl);

                if (r == 1)
                    break;
                int e = SSL_get_error(c.ssl, r);

                if ((e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) || nowMs() - started > (long long)cfg_.authTimeoutMs ||
                    !running_ || c.dead)
                {
                    ok = false;
                    break;
                }
            }
            io.ssl = c.ssl;
        }
    }

    /* the HTTP request that asks for the WebSocket */
    if (ok)
    {
        std::string req;

        while (ok && req.find("\r\n\r\n") == std::string::npos)
        {
            char b[1024];
            int n = io.rd(b, sizeof(b));

            if (n < 0 || req.size() > 8192U || nowMs() - started > (long long)cfg_.authTimeoutMs || !running_ || c.dead)
                ok = false;
            else if (n > 0)
                req.append(b, (size_t)n);
        }

        if (ok)
        {
            std::map<std::string, std::string> headers;
            size_t eol = req.find("\r\n");
            std::string requestLine = req.substr(0U, eol);
            size_t pos = eol + 2U;

            while (pos < req.size())
            {
                size_t e = req.find("\r\n", pos);
                std::string line = req.substr(pos, e == std::string::npos ? std::string::npos : e - pos);

                pos = e == std::string::npos ? req.size() : e + 2U;
                size_t colon = line.find(':');

                if (colon != std::string::npos)
                    headers[lower(trim(line.substr(0U, colon)))] = trim(line.substr(colon + 1U));
            }

            bool pathOk = requestLine.compare(0U, 11U, "GET /openmt") == 0 &&
                          (requestLine.size() == 11U || requestLine[11] == ' ' || requestLine[11] == '?');
            bool upgradeOk = lower(headers["upgrade"]) == "websocket" && lower(headers["connection"]).find("upgrade") != std::string::npos &&
                             !headers["sec-websocket-key"].empty() && headers["sec-websocket-version"] == "13";
            std::string response;

            bool pageOk = !cfg_.webPage.empty() && (requestLine.compare(0U, 6U, "GET / ") == 0 ||
                                                    requestLine.compare(0U, 17U, "GET /index.html ") == 0);

            if (pageOk && !upgradeOk)
            {
                response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                           std::to_string((unsigned long long)cfg_.webPage.size()) +
                           "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
                           "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                           "connect-src 'self' wss:; base-uri 'none'; form-action 'none'\r\n"
                           "Connection: close\r\n\r\n" + cfg_.webPage;
                ok = false;
            }
            else if (!pathOk || !upgradeOk)
            {
                response = "HTTP/1.1 426 Upgrade Required\r\nContent-Length: 0\r\nSec-WebSocket-Version: 13\r\nConnection: close\r\n\r\n";
                ok = false;
            }
            else
            {
                response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                           acceptKey(headers["sec-websocket-key"]) + "\r\n\r\n";
            }

            size_t off = 0U;
            long long until = nowMs() + 3000;

            while (off < response.size() && nowMs() < until)
            {
                int n = io.wr(response.data() + off, response.size() - off);

                if (n < 0)
                {
                    ok = false;
                    break;
                }
                off += (size_t)n;
            }
            if (off < response.size())
                ok = false;
            upgraded = ok;
        }
    }

    if (ok)
    {
        unsigned char r[16];

        randomBytes(r, sizeof(r));
        nonce = hexOf(r, sizeof(r));

        int fl = ::fcntl(c.fd, F_GETFL);

        ::fcntl(c.fd, F_SETFL, fl | O_NONBLOCK);

        Json hello = Json::object();

        hello.set("type", "hello");
        hello.set("protocol", PROTOCOL_VERSION);
        hello.set("server", cfg_.serverName);
        hello.set("nonce", nonce);
        sendEvent(client, hello);
    }
    else if (!upgraded)
        c.dead = true;

    /* ------------------------------------------------------------------ the frames */
    auto closeWith = [&](unsigned int code, const std::string& reason)
    {
        if (!c.closing)
        {
            c.closing = true;
            c.closeAt = nowMs() + 1000;
            c.enqueue(closeFrame(code, reason), false);
        }
    };

    auto handleText = [&](const std::string& text)
    {
        Json m;
        std::string perr;

        if (!Json::parse(text, m, perr) || !m.isObject())
        {
            Json e = Json::object();

            e.set("type", "error");
            e.set("error", "the message is not a JSON object");
            sendEvent(client, e);
            return;
        }

        if (!c.authed)
        {
            if (m["type"].asString() != "auth")
            {
                closeWith(1008, "authenticate first");
                return;
            }

            std::string response = m["response"].asString();
            std::string cnonce = m["cnonce"].asString();
            std::string want = authMac(cfg_.token, 'C', nonce, cnonce);
            bool good = cnonce.size() >= 16U && cnonce.size() <= 128U && response.size() == want.size() &&
                        CRYPTO_memcmp(response.data(), want.data(), want.size()) == 0;

            if (!good)
            {
                {
                    std::lock_guard<std::mutex> lock(impl_->failMu);
                    Impl::Fail& f = impl_->fails[c.ip];

                    if (++f.count >= 5U)
                    {
                        f.until = nowMs() + 60000;
                        f.count = 0U;
                    }
                }
                log("remote: " + c.peer + " sent a wrong token");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                Json e = Json::object();

                e.set("type", "auth_failed");
                sendEvent(client, e);
                closeWith(1008, "wrong token");
                return;
            }

            unsigned int n = clients();

            if (n >= cfg_.maxClients)
            {
                Json e = Json::object();

                e.set("type", "auth_failed");
                e.set("error", "too many clients");
                sendEvent(client, e);
                closeWith(1013, "too many clients");
                return;
            }

            {
                std::lock_guard<std::mutex> lock(impl_->failMu);
                impl_->fails.erase(c.ip);
            }
            c.authed = true;

            Json ok2 = Json::object();

            ok2.set("type", "auth_ok");
            ok2.set("protocol", PROTOCOL_VERSION);
            ok2.set("server", cfg_.serverName);
            ok2.set("proof", authMac(cfg_.token, 'S', nonce, cnonce));
            ok2.set("clientId", (unsigned long long)c.id);
            sendEvent(client, ok2);

            Json st;

            {
                std::lock_guard<std::mutex> lock(impl_->mu);
                st = impl_->state;
            }
            if (st.isObject())
                sendEvent(client, st);
            log("remote: " + c.peer + " connected");
            return;
        }

        /* ------------------------------------------------------------ the commands of an authenticated client */
        RemoteRequest req;

        req.client = c.id;
        req.id = m["id"].asInt(0);
        req.cmd = m["cmd"].asString();
        req.args = m;
        req.peer = c.peer;

        if (req.cmd.empty())
        {
            reply(c.id, req.id, false, Json("no \"cmd\""));
            return;
        }

        if (req.cmd == "ping")
        {
            Json r = Json::object();

            r.set("pong", true);
            reply(c.id, req.id, true, r);
        }
        else if (req.cmd == "get_state")
        {
            Json st;

            {
                std::lock_guard<std::mutex> lock(impl_->mu);
                st = impl_->state;
            }
            reply(c.id, req.id, true, st);
        }
        else if (req.cmd == "ptt")
        {
            bool down = m["down"].asBool(false);
            std::string err;
            bool good = true;

            {
                std::lock_guard<std::mutex> lock(impl_->pttMu);

                if (down)
                {
                    if (impl_->pttOwner < 0)
                    {
                        good = false;
                        err = "the desktop holds the push to talk";
                    }
                    else if (impl_->pttOwner > 0 && impl_->pttOwner != (long long)c.id)
                    {
                        good = false;
                        err = "busy: another client holds the push to talk";
                    }
                    else if (impl_->pttOwner == 0)
                    {
                        if (host_->remotePtt(true, err))
                        {
                            impl_->pttOwner = (long long)c.id;
                            impl_->pttStart = nowMs();
                            impl_->pttWho = c.peer;
                            impl_->pttKeepalive = nowMs();
                            broadcast(pttJson(1, c.peer, ""));
                        }
                        else
                            good = false;
                    }
                    else
                        impl_->pttKeepalive = nowMs();
                }
                else if (impl_->pttOwner == (long long)c.id)
                {
                    host_->remotePtt(false, err);
                    err.clear();
                    impl_->pttOwner = 0;
                    impl_->pttWho.clear();
                    broadcast(pttJson(0, "", "released"));
                }
            }
            if (good)
                reply(c.id, req.id, true, Json::object());
            else
                reply(c.id, req.id, false, Json(err.empty() ? std::string("cannot transmit") : err));
        }
        else if (req.cmd == "ptt_keepalive")
        {
            std::lock_guard<std::mutex> lock(impl_->pttMu);

            if (impl_->pttOwner == (long long)c.id)
                impl_->pttKeepalive = nowMs();
            if (req.id != 0)
                reply(c.id, req.id, true, Json::object());
        }
        else if (req.cmd == "dtmf")
        {
            std::string digit = m["digit"].asString();
            std::string err;

            if (digit.size() != 1U)
                reply(c.id, req.id, false, Json("\"digit\" must be one character"));
            else if (host_->remoteDtmf(digit[0], err))
                reply(c.id, req.id, true, Json::object());
            else
                reply(c.id, req.id, false, Json(err.empty() ? std::string("the digit was not sent") : err));
        }
        else if (req.cmd == "audio_start")
        {
            std::string codec = m["codec"].asString("pcm16");

            if (codec != "pcm16" && codec != "ulaw")
                reply(c.id, req.id, false, Json("codec must be pcm16 or ulaw"));
            else
            {
                c.codec = codec == "ulaw" ? 1 : 0;
                c.audioRx = m["rx"].asBool(true);
                c.audioTx = m["tx"].asBool(true);
                Json r = Json::object();

                r.set("codec", codec);
                reply(c.id, req.id, true, r);
            }
        }
        else if (req.cmd == "audio_stop")
        {
            c.audioRx = false;
            c.audioTx = false;
            reply(c.id, req.id, true, Json::object());
        }
        else if (req.cmd == "monitor_start")
        {
            long long ms = m["intervalMs"].asInt(500);

            c.monitorMs = (int)std::max(100LL, std::min(5000LL, ms));
            reply(c.id, req.id, true, Json::object());
        }
        else if (req.cmd == "monitor_stop")
        {
            c.monitorMs = 0;
            reply(c.id, req.id, true, Json::object());
        }
        else
            host_->remoteCommand(req);
    };

    auto handleBinary = [&](const std::string& data)
    {
        if (!c.authed || data.size() < 5U || (unsigned char)data[0] != 0x02U || !c.audioTx)
            return;

        {
            std::lock_guard<std::mutex> lock(impl_->pttMu);

            if (impl_->pttOwner != (long long)c.id)
                return;                                 /* only the one who holds the key is heard */
            impl_->pttKeepalive = nowMs();
        }

        int codec = (unsigned char)data[1];
        int16_t pcm[2048];
        size_t n = 0U;

        if (codec == 1)
        {
            for (size_t i = 4U; i < data.size() && n < 2048U; i++)
                pcm[n++] = ulawDecode((uint8_t)data[i]);
        }
        else if (codec == 0)
        {
            for (size_t i = 4U; i + 1U < data.size() && n < 2048U; i += 2U)
                pcm[n++] = (int16_t)((unsigned char)data[i] | ((unsigned char)data[i + 1U] << 8));
        }
        if (n > 0U)
            host_->remoteMic(pcm, n);
    };

    /* returns false on a protocol error */
    auto parseFrames = [&]() -> bool
    {
        size_t off = 0U;

        for (;;)
        {
            const std::string& b = c.rbuf;

            if (b.size() - off < 2U)
                break;

            unsigned char b0 = (unsigned char)b[off], b1 = (unsigned char)b[off + 1U];
            bool fin = (b0 & 0x80U) != 0U;
            unsigned int op = b0 & 15U;
            bool masked = (b1 & 0x80U) != 0U;
            uint64_t len = b1 & 0x7FU;
            size_t hdr = 2U;

            if ((b0 & 0x70U) != 0U || !masked)
                return false;                           /* reserved bits, or a client that does not mask */
            if (len == 126U)
            {
                if (b.size() - off < 4U)
                    break;
                len = ((uint64_t)(unsigned char)b[off + 2U] << 8) | (unsigned char)b[off + 3U];
                hdr = 4U;
            }
            else if (len == 127U)
            {
                if (b.size() - off < 10U)
                    break;
                len = 0U;
                for (int i = 0; i < 8; i++)
                    len = (len << 8) | (unsigned char)b[off + 2U + (size_t)i];
                hdr = 10U;
            }
            if (len > MAX_MESSAGE)
                return false;
            if (op >= 8U && (!fin || len > 125U))
                return false;
            if (b.size() - off < hdr + 4U + len)
                break;

            unsigned char mask[4];

            std::memcpy(mask, b.data() + off + hdr, 4U);

            std::string payload(b, off + hdr + 4U, (size_t)len);

            for (size_t i = 0U; i < payload.size(); i++)
                payload[i] = (char)((unsigned char)payload[i] ^ mask[i & 3U]);
            off += hdr + 4U + (size_t)len;

            if (op == 0x8U)
            {
                closeWith(1000, "");
                continue;
            }
            if (op == 0x9U)
            {
                c.enqueue(wsFrame(0xAU, payload), false);
                continue;
            }
            if (op == 0xAU)
                continue;
            if (op == 0x1U || op == 0x2U)
            {
                if (msgOp != 0U)
                    return false;                       /* a new message inside a fragmented one */
                msgOp = op;
                msg.clear();
            }
            else if (op == 0x0U)
            {
                if (msgOp == 0U)
                    return false;
            }
            else
                return false;

            msg += payload;
            if (msg.size() > MAX_MESSAGE)
                return false;
            if (fin)
            {
                unsigned int doneOp = msgOp;

                msgOp = 0U;
                if (doneOp == 0x1U)
                    handleText(msg);
                else
                    handleBinary(msg);
                msg.clear();
            }
        }
        c.rbuf.erase(0U, off);
        return true;
    };

    /* ------------------------------------------------------------------ the loop */
    while (upgraded && running_ && !c.dead)
    {
        struct pollfd p[2];
        bool haveOut;

        {
            std::lock_guard<std::mutex> lock(c.qmu);
            haveOut = !c.q.empty();
        }
        haveOut = haveOut || c.woff < c.wbuf.size();

        p[0].fd = c.fd;
        p[0].events = (short)(POLLIN | ((haveOut || io.wantWrite) ? POLLOUT : 0));
        p[0].revents = 0;
        p[1].fd = c.wakeR;
        p[1].events = POLLIN;
        p[1].revents = 0;
        ::poll(p, 2, 200);
        io.wantWrite = false;                       /* set again by the read or write that needs the socket to be writable */

        if ((p[1].revents & POLLIN) != 0)
        {
            char drain[64];

            while (::read(c.wakeR, drain, sizeof(drain)) > 0)
            {
            }
        }

        /* reading */
        bool dropped = false;

        if ((p[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0 || (io.ssl != NULL && SSL_pending(io.ssl) > 0))
        {
            for (int rounds = 0; rounds < 16; rounds++)
            {
                char buf[4096];
                int n = io.rd(buf, sizeof(buf));

                if (n < 0)
                {
                    dropped = true;
                    break;
                }
                if (n == 0)
                    break;
                c.rbuf.append(buf, (size_t)n);
                lastRx = nowMs();
            }
            if (!dropped && !c.rbuf.empty() && !parseFrames())
            {
                closeWith(1002, "protocol error");
                c.rbuf.clear();
            }
        }

        /* writing */
        for (int rounds = 0; rounds < 16 && !dropped; rounds++)
        {
            if (c.woff >= c.wbuf.size())
            {
                c.wbuf.clear();
                c.woff = 0U;

                std::lock_guard<std::mutex> lock(c.qmu);

                while (!c.q.empty() && c.wbuf.size() < 65536U)
                {
                    c.wbuf += c.q.front();
                    c.qBytes -= c.q.front().size();
                    c.q.pop_front();
                }
            }
            if (c.wbuf.empty())
                break;

            int n = io.wr(c.wbuf.data() + c.woff, c.wbuf.size() - c.woff);

            if (n < 0)
            {
                dropped = true;
                break;
            }
            if (n == 0)
                break;
            c.woff += (size_t)n;
        }
        if (dropped)
            break;

        long long now = nowMs();

        if (!c.authed && now - started > (long long)cfg_.authTimeoutMs)
            closeWith(1008, "authentication timed out");
        if (c.authed && cfg_.idleTimeoutMs > 0U && now - lastRx > (long long)cfg_.idleTimeoutMs)
            closeWith(1001, "idle");
        if (c.closing)
        {
            bool empty;

            {
                std::lock_guard<std::mutex> lock(c.qmu);
                empty = c.q.empty();
            }
            if ((empty && c.woff >= c.wbuf.size()) || now > c.closeAt)
                break;
        }
    }

    /* ------------------------------------------------------------------ the end */
    c.dead = true;
    if (c.authed)
    {
        std::string who = c.peer;
        bool held;

        {
            std::lock_guard<std::mutex> lock(impl_->pttMu);
            held = impl_->pttOwner == (long long)c.id;
        }
        if (held)
            releasePtt("the client went away");
        c.authed = false;
        log("remote: " + who + " disconnected");
    }
    if (c.ssl != NULL)
    {
        SSL_shutdown(c.ssl);
        SSL_free(c.ssl);
        c.ssl = NULL;
    }
    ::close(c.fd);
    c.done = true;
}

}
