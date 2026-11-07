#include "tr/SupervisorHttpServer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "tr/SysInfo.h"
#include "tr/Supervisor.h"
#include "util/Log.h"

namespace lva::tr {

namespace {

constexpr const char* kTag        = "http";
constexpr const char* kSecretKey  = "ThirdReality";
constexpr std::size_t kMaxHeader  = 16 * 1024;
constexpr std::size_t kMaxBody    = 64 * 1024;
constexpr auto kRequestTimeout = std::chrono::seconds(5);
constexpr auto kSendTimeout = std::chrono::seconds(5);
constexpr auto kOverloadLogInterval = std::chrono::seconds(10);

// One deadline covers both headers and body, so trickled bytes cannot keep
// renewing a per-recv timeout indefinitely. Socket shutdown wakes poll().
ssize_t ReceiveBefore(int fd, char* buffer, std::size_t size,
                      std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) return -1;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            remaining).count();
        pollfd event{fd, POLLIN, 0};
        const int ready = ::poll(&event, 1, static_cast<int>(ms + 1));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;
        const ssize_t n = ::recv(fd, buffer, size, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return n;
    }
}

void SendAll(int fd, const std::string& data) {
    const auto deadline = std::chrono::steady_clock::now() + kSendTimeout;
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            LVA_LOGW(kTag, "HTTP response timed out (fd=%d, sent=%zu/%zu)",
                     fd, sent, data.size());
            return;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            remaining).count();
        pollfd event{fd, POLLOUT, 0};
        const int ready = ::poll(&event, 1, static_cast<int>(ms + 1));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) {
            LVA_LOGW(kTag, "HTTP response incomplete (fd=%d, sent=%zu/%zu): %s",
                     fd, sent, data.size(),
                     ready == 0 ? "send timeout" : std::strerror(errno));
            return;
        }
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent,
                                 MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        LVA_LOGW(kTag, "HTTP response incomplete (fd=%d, sent=%zu/%zu): %s",
                 fd, sent, data.size(),
                 n == 0 ? "send returned zero" : std::strerror(errno));
        return;
    }
}

const char* ReasonPhrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return s;
}

class Md5 {
   public:
    Md5() {
        state_[0] = 0x67452301; state_[1] = 0xEFCDAB89;
        state_[2] = 0x98BADCFE; state_[3] = 0x10325476;
        count_   = 0;
    }
    void Update(const unsigned char* data, std::size_t n) {
        std::size_t idx = static_cast<std::size_t>((count_ >> 3) & 0x3F);
        count_ += static_cast<std::uint64_t>(n) << 3;
        std::size_t part = 64 - idx;
        std::size_t i = 0;
        if (n >= part) {
            std::memcpy(buffer_ + idx, data, part);
            Transform(buffer_);
            for (i = part; i + 63 < n; i += 64) Transform(data + i);
            idx = 0;
        }
        std::memcpy(buffer_ + idx, data + i, n - i);
    }
    std::string HexDigest() {
        unsigned char bits[8];
        for (int i = 0; i < 8; ++i)
            bits[i] = static_cast<unsigned char>((count_ >> (i * 8)) & 0xFF);
        std::size_t idx = static_cast<std::size_t>((count_ >> 3) & 0x3F);
        std::size_t pad_len = (idx < 56) ? (56 - idx) : (120 - idx);
        static const unsigned char kPad[64] = {0x80};
        Update(kPad, pad_len);
        Update(bits, 8);
        unsigned char digest[16];
        for (int i = 0; i < 4; ++i) {
            digest[i * 4 + 0] = static_cast<unsigned char>(state_[i] & 0xFF);
            digest[i * 4 + 1] = static_cast<unsigned char>((state_[i] >> 8) & 0xFF);
            digest[i * 4 + 2] = static_cast<unsigned char>((state_[i] >> 16) & 0xFF);
            digest[i * 4 + 3] = static_cast<unsigned char>((state_[i] >> 24) & 0xFF);
        }
        static const char kHex[] = "0123456789abcdef";
        std::string out(32, '\0');
        for (int i = 0; i < 16; ++i) {
            out[2*i+0] = kHex[(digest[i] >> 4) & 0xF];
            out[2*i+1] = kHex[digest[i] & 0xF];
        }
        return out;
    }

   private:
    static std::uint32_t F(std::uint32_t x, std::uint32_t y, std::uint32_t z)
        { return (x & y) | (~x & z); }
    static std::uint32_t G(std::uint32_t x, std::uint32_t y, std::uint32_t z)
        { return (x & z) | (y & ~z); }
    static std::uint32_t H(std::uint32_t x, std::uint32_t y, std::uint32_t z)
        { return x ^ y ^ z; }
    static std::uint32_t I(std::uint32_t x, std::uint32_t y, std::uint32_t z)
        { return y ^ (x | ~z); }
    static std::uint32_t Rot(std::uint32_t x, int n)
        { return (x << n) | (x >> (32 - n)); }
    static void Step(std::uint32_t (*f)(std::uint32_t,std::uint32_t,std::uint32_t),
                     std::uint32_t& a, std::uint32_t b, std::uint32_t c,
                     std::uint32_t d, std::uint32_t x, int s, std::uint32_t t) {
        a = b + Rot(a + f(b, c, d) + x + t, s);
    }
    void Transform(const unsigned char block[64]) {
        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t x[16];
        for (int i = 0; i < 16; ++i) {
            x[i] = static_cast<std::uint32_t>(block[i*4+0])
                 | (static_cast<std::uint32_t>(block[i*4+1]) << 8)
                 | (static_cast<std::uint32_t>(block[i*4+2]) << 16)
                 | (static_cast<std::uint32_t>(block[i*4+3]) << 24);
        }
        Step(F, a, b, c, d, x[0],  7,  0xD76AA478);
        Step(F, d, a, b, c, x[1],  12, 0xE8C7B756);
        Step(F, c, d, a, b, x[2],  17, 0x242070DB);
        Step(F, b, c, d, a, x[3],  22, 0xC1BDCEEE);
        Step(F, a, b, c, d, x[4],  7,  0xF57C0FAF);
        Step(F, d, a, b, c, x[5],  12, 0x4787C62A);
        Step(F, c, d, a, b, x[6],  17, 0xA8304613);
        Step(F, b, c, d, a, x[7],  22, 0xFD469501);
        Step(F, a, b, c, d, x[8],  7,  0x698098D8);
        Step(F, d, a, b, c, x[9],  12, 0x8B44F7AF);
        Step(F, c, d, a, b, x[10], 17, 0xFFFF5BB1);
        Step(F, b, c, d, a, x[11], 22, 0x895CD7BE);
        Step(F, a, b, c, d, x[12], 7,  0x6B901122);
        Step(F, d, a, b, c, x[13], 12, 0xFD987193);
        Step(F, c, d, a, b, x[14], 17, 0xA679438E);
        Step(F, b, c, d, a, x[15], 22, 0x49B40821);
        Step(G, a, b, c, d, x[1],  5,  0xF61E2562);
        Step(G, d, a, b, c, x[6],  9,  0xC040B340);
        Step(G, c, d, a, b, x[11], 14, 0x265E5A51);
        Step(G, b, c, d, a, x[0],  20, 0xE9B6C7AA);
        Step(G, a, b, c, d, x[5],  5,  0xD62F105D);
        Step(G, d, a, b, c, x[10], 9,  0x02441453);
        Step(G, c, d, a, b, x[15], 14, 0xD8A1E681);
        Step(G, b, c, d, a, x[4],  20, 0xE7D3FBC8);
        Step(G, a, b, c, d, x[9],  5,  0x21E1CDE6);
        Step(G, d, a, b, c, x[14], 9,  0xC33707D6);
        Step(G, c, d, a, b, x[3],  14, 0xF4D50D87);
        Step(G, b, c, d, a, x[8],  20, 0x455A14ED);
        Step(G, a, b, c, d, x[13], 5,  0xA9E3E905);
        Step(G, d, a, b, c, x[2],  9,  0xFCEFA3F8);
        Step(G, c, d, a, b, x[7],  14, 0x676F02D9);
        Step(G, b, c, d, a, x[12], 20, 0x8D2A4C8A);
        Step(H, a, b, c, d, x[5],  4,  0xFFFA3942);
        Step(H, d, a, b, c, x[8],  11, 0x8771F681);
        Step(H, c, d, a, b, x[11], 16, 0x6D9D6122);
        Step(H, b, c, d, a, x[14], 23, 0xFDE5380C);
        Step(H, a, b, c, d, x[1],  4,  0xA4BEEA44);
        Step(H, d, a, b, c, x[4],  11, 0x4BDECFA9);
        Step(H, c, d, a, b, x[7],  16, 0xF6BB4B60);
        Step(H, b, c, d, a, x[10], 23, 0xBEBFBC70);
        Step(H, a, b, c, d, x[13], 4,  0x289B7EC6);
        Step(H, d, a, b, c, x[0],  11, 0xEAA127FA);
        Step(H, c, d, a, b, x[3],  16, 0xD4EF3085);
        Step(H, b, c, d, a, x[6],  23, 0x04881D05);
        Step(H, a, b, c, d, x[9],  4,  0xD9D4D039);
        Step(H, d, a, b, c, x[12], 11, 0xE6DB99E5);
        Step(H, c, d, a, b, x[15], 16, 0x1FA27CF8);
        Step(H, b, c, d, a, x[2],  23, 0xC4AC5665);
        Step(I, a, b, c, d, x[0],  6,  0xF4292244);
        Step(I, d, a, b, c, x[7],  10, 0x432AFF97);
        Step(I, c, d, a, b, x[14], 15, 0xAB9423A7);
        Step(I, b, c, d, a, x[5],  21, 0xFC93A039);
        Step(I, a, b, c, d, x[12], 6,  0x655B59C3);
        Step(I, d, a, b, c, x[3],  10, 0x8F0CCC92);
        Step(I, c, d, a, b, x[10], 15, 0xFFEFF47D);
        Step(I, b, c, d, a, x[1],  21, 0x85845DD1);
        Step(I, a, b, c, d, x[8],  6,  0x6FA87E4F);
        Step(I, d, a, b, c, x[15], 10, 0xFE2CE6E0);
        Step(I, c, d, a, b, x[6],  15, 0xA3014314);
        Step(I, b, c, d, a, x[13], 21, 0x4E0811A1);
        Step(I, a, b, c, d, x[4],  6,  0xF7537E82);
        Step(I, d, a, b, c, x[11], 10, 0xBD3AF235);
        Step(I, c, d, a, b, x[2],  15, 0x2AD7D2BB);
        Step(I, b, c, d, a, x[9],  21, 0xEB86D391);
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    }

    std::uint32_t state_[4];
    std::uint64_t count_;
    unsigned char buffer_[64];
};

std::string Md5HexOf(const std::string& s) {
    Md5 m;
    m.Update(reinterpret_cast<const unsigned char*>(s.data()), s.size());
    return m.HexDigest();
}

std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') { out.push_back(' '); continue; }
        if (in[i] == '%' && i + 2 < in.size()) {
            const auto hex = in.substr(i + 1, 2);
            char* end = nullptr;
            long v = std::strtol(hex.c_str(), &end, 16);
            if (end == hex.c_str() + 2) {
                out.push_back(static_cast<char>(v));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

std::string Base64Decode(const std::string& in) {
    static const int8_t kTbl[256] = {
    };
    static int8_t tbl[256];
    static bool init = false;
    if (!init) {
        for (auto& v : tbl) v = -1;
        const char* alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) {
            tbl[static_cast<unsigned char>(alphabet[i])] = static_cast<int8_t>(i);
        }
        init = true;
    }
    (void)kTbl;
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        const int8_t v = tbl[c];
        if (v < 0) continue;  // skip whitespace etc.
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::string CanonicalSign(const std::map<std::string, std::string>& params) {
    std::string s;
    bool first = true;
    for (const auto& [k, v] : params) {
        if (!first) s.push_back('&');
        s.append(k);
        s.push_back('=');
        s.append(v);
        first = false;
    }
    s.push_back('&');
    s.append(kSecretKey);
    return Md5HexOf(s);
}

}  // namespace

SupervisorHttpServer::SupervisorHttpServer(Supervisor& supervisor,
                                           std::uint16_t port)
    : supervisor_(supervisor), port_(port) {}

SupervisorHttpServer::~SupervisorHttpServer() { Stop(); }

bool SupervisorHttpServer::Start() {
    if (listen_fd_ >= 0) return true;

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LVA_LOGE(kTag, "socket() failed: %s", std::strerror(errno));
        return false;
    }
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port_);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LVA_LOGE(kTag, "bind(:%u) failed: %s", port_, std::strerror(errno));
        ::close(fd);
        return false;
    }
    if (::listen(fd, 8) < 0) {
        LVA_LOGE(kTag, "listen() failed: %s", std::strerror(errno));
        ::close(fd);
        return false;
    }
    listen_fd_ = fd;
    running_.store(true, std::memory_order_relaxed);
    try {
        accept_thread_ = std::thread([this] { AcceptLoop(); });
    } catch (const std::exception& error) {
        LVA_LOGE(kTag, "HTTP accept thread creation failed: %s", error.what());
        running_.store(false, std::memory_order_relaxed);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    LVA_LOGI(kTag, "supervisor HTTP listening on 0.0.0.0:%u", port_);
    return true;
}

void SupervisorHttpServer::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    stopped_.notify_all();
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
    }
    // AcceptLoop keeps using listen_fd_; do not close/reuse it before joining.
    if (accept_thread_.joinable()) accept_thread_.join();
    {
        std::lock_guard lock(workers_mutex_);
        for (auto& worker : workers_) {
            if (worker.fd >= 0) ::shutdown(worker.fd, SHUT_RDWR);
        }
    }
    for (auto& worker : workers_) {
        if (worker.thread.joinable()) worker.thread.join();
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    LVA_LOGI(kTag, "supervisor HTTP stopped");
}

bool SupervisorHttpServer::WaitForActionDelay(const char* action) {
    std::unique_lock lock(workers_mutex_);
    if (stopped_.wait_for(lock, std::chrono::seconds(3), [this] {
        return !running_.load(std::memory_order_relaxed);
    })) {
        LVA_LOGW(kTag, "%s cancelled: server stopping", action);
        return false;
    }
    return true;
}

void SupervisorHttpServer::AcceptLoop() {
    ::pthread_setname_np(::pthread_self(), "lva-http-accept");
    auto next_overload_log = std::chrono::steady_clock::time_point::min();
    std::size_t rejected_since_log = 0;
    while (running_.load(std::memory_order_relaxed)) {
        sockaddr_in peer{};
        socklen_t   plen = sizeof(peer);
        const int   conn = ::accept4(listen_fd_,
                                     reinterpret_cast<sockaddr*>(&peer),
                                     &plen, SOCK_CLOEXEC);
        if (conn < 0) {
            if (!running_.load(std::memory_order_relaxed)) break;
            if (errno == EINTR) continue;
            LVA_LOGW(kTag, "accept() failed: %s", std::strerror(errno));
            continue;
        }
        std::lock_guard lock(workers_mutex_);
        if (!running_.load(std::memory_order_relaxed)) {
            ::close(conn);
            break;
        }
        Worker* available = nullptr;
        for (auto& worker : workers_) {
            if (worker.fd < 0) {
                available = &worker;
                break;
            }
        }
        if (!available) {
            ++rejected_since_log;
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_overload_log) {
                char address[INET_ADDRSTRLEN]{};
                const char* ip = ::inet_ntop(AF_INET, &peer.sin_addr,
                                             address, sizeof(address));
                LVA_LOGW(kTag, "HTTP connection limit (%zu) reached; rejecting "
                         "peer=%s:%u (%zu rejected since last log)",
                         kMaxConnections, ip ? ip : "unknown",
                         static_cast<unsigned>(ntohs(peer.sin_port)),
                         rejected_since_log);
                rejected_since_log = 0;
                next_overload_log = now + kOverloadLogInterval;
            }
            // Do not block the accept loop on a response to an excess client.
            ::close(conn);
            continue;
        }
        // For a joinable slot, fd becomes -1 only in the worker's final
        // mutex-protected scope. It never locks workers_mutex_ again, so
        // joining under this mutex cannot deadlock. Preserve this invariant.
        if (available->thread.joinable()) available->thread.join();
        available->fd = conn;
        try {
            available->thread = std::thread([this, conn, available] {
                ::pthread_setname_np(::pthread_self(), "lva-http");
                try {
                    HandleConnection(conn);
                } catch (const std::exception& error) {
                    LVA_LOGE(kTag, "HTTP connection failed: %s", error.what());
                } catch (...) {
                    LVA_LOGE(kTag, "HTTP connection failed: unknown exception");
                }
                // Serialize close with Stop's shutdown so an fd cannot be
                // reused while Stop still considers it an active client.
                std::lock_guard lock(workers_mutex_);
                ::close(conn);
                available->fd = -1;
            });
        } catch (const std::exception& error) {
            LVA_LOGE(kTag, "HTTP connection thread creation failed: %s", error.what());
            ::close(conn);
            available->fd = -1;
        }
    }
}

void SupervisorHttpServer::HandleConnection(int conn_fd) {
    std::string buf;
    buf.reserve(1024);
    const auto deadline = std::chrono::steady_clock::now() + kRequestTimeout;

    while (buf.find("\r\n\r\n") == std::string::npos) {
        char tmp[2048];
        const ssize_t n = ReceiveBefore(conn_fd, tmp, sizeof(tmp), deadline);
        if (n <= 0) return;
        buf.append(tmp, static_cast<std::size_t>(n));
        if (buf.size() > kMaxHeader) {
            SendText(conn_fd, 400, "Request header too large");
            return;
        }
    }
    const auto split = buf.find("\r\n\r\n");
    std::string headers_part = buf.substr(0, split);
    std::string body         = buf.substr(split + 4);

    std::istringstream iss(headers_part);
    std::string request_line;
    std::getline(iss, request_line);
    if (!request_line.empty() && request_line.back() == '\r')
        request_line.pop_back();

    std::istringstream rl(request_line);
    std::string method, path, http_ver;
    rl >> method >> path >> http_ver;
    if (method.empty() || path.empty()) {
        SendText(conn_fd, 400, "Malformed request");
        return;
    }

    std::map<std::string, std::string> hdrs;
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = Lower(line.substr(0, colon));
        std::string v = line.substr(colon + 1);
        const auto first = v.find_first_not_of(" \t");
        if (first != std::string::npos) v = v.substr(first);
        hdrs[k] = v;
    }

    std::size_t cl = 0;
    if (auto it = hdrs.find("content-length"); it != hdrs.end()) {
        cl = static_cast<std::size_t>(std::strtoull(it->second.c_str(),
                                                    nullptr, 10));
    }
    if (cl > kMaxBody) {
        SendText(conn_fd, 400, "Request body too large");
        return;
    }
    while (body.size() < cl) {
        char tmp[2048];
        const ssize_t n = ReceiveBefore(conn_fd, tmp,
            std::min(sizeof(tmp), cl - body.size()), deadline);
        if (n <= 0) return;  // Never execute a truncated/timed-out request.
        body.append(tmp, static_cast<std::size_t>(n));
    }
    body.resize(cl);

    std::string clean_path = path;
    if (auto q = clean_path.find('?'); q != std::string::npos) {
        clean_path = clean_path.substr(0, q);
    }

    if (method == "OPTIONS") {
        SendJson(conn_fd, 200, "{\"ok\":true}");
    } else if (method == "GET") {
        HandleGet(conn_fd, clean_path);
    } else if (method == "POST") {
        const std::string ct = hdrs.count("content-type")
            ? hdrs["content-type"] : std::string();
        HandlePost(conn_fd, clean_path, ct, body);
    } else {
        SendJson(conn_fd, 405, "{\"error\":\"Method not allowed\"}");
    }
}

void SupervisorHttpServer::HandleGet(int conn_fd, const std::string& path) {
    using nlohmann::json;
    if (path == "/api/wifi/status") {
        DeviceInfo d = ReadDeviceInfo();
        json j = {
            {"connected", d.status == "connected" ||
                          d.status == "connected_no_internet"},
            {"ssid",       d.ssid},
            {"ip_address", d.ip},
            {"mac_address",d.mac_address},
            {"message",    ""},
        };
        SendJson(conn_fd, 200, j.dump());
        return;
    }
    if (path == "/api/system/info") {
        DeviceInfo d = ReadDeviceInfo();
        json j = {
            {"Device Model", "ThirdReality HA Speaker"},
            {"Device Name", d.name},
            {"WIFI Connected", d.status == "connected" ||
                               d.status == "connected_no_internet"},
            {"SSID",        d.ssid},
            {"Ip Address",  d.ip},
            {"Mac Address", d.mac_address},
            {"Version",     d.firmware_version},
        };
        SendJson(conn_fd, 200, j.dump());
        return;
    }
    if (path == "/api/ota/status") {
        OtaState s = supervisor_.GetOtaState();
        json j = {
            {"ota_id",      s.ota_id},
            {"ota_status",  s.status},
            {"progress",    s.progress},
            {"start_time",  s.start_time},
            {"finish_time", s.finish_time},
            {"message",     s.message},
        };
        SendJson(conn_fd, 200, j.dump());
        return;
    }
    SendText(conn_fd, 404, "Not Found");
}

void SupervisorHttpServer::HandlePost(int conn_fd, const std::string& path,
                                      const std::string& content_type,
                                      const std::string& body) {
    using nlohmann::json;
    if (path != "/api/system/command") {
        SendJson(conn_fd, 404, "{\"error\":\"Not found\"}");
        return;
    }

    std::map<std::string, std::string> params;
    std::string signature;

    if (content_type.find("application/json") != std::string::npos) {
        json j = json::parse(body, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                const std::string key = it.key();
                std::string val = it.value().is_string()
                    ? it.value().get<std::string>()
                    : it.value().dump();
                if (key == "_sig") signature = val;
                else                params[key] = std::move(val);
            }
        }
    } else {
        // form-encoded: a=b&c=d&_sig=...
        std::size_t i = 0;
        while (i < body.size()) {
            const auto amp = body.find('&', i);
            const std::string pair = body.substr(i,
                amp == std::string::npos ? body.size() - i : amp - i);
            const auto eq = pair.find('=');
            if (eq != std::string::npos) {
                const std::string k = pair.substr(0, eq);
                const std::string v = pair.substr(eq + 1);
                if (k == "_sig") signature = v;
                else             params[k] = v;
            }
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
    }

    if (params.find("command") == params.end()) {
        SendJson(conn_fd, 400, "{\"error\":\"Command is required\"}");
        return;
    }
    if (signature.empty()) {
        SendJson(conn_fd, 400, "{\"error\":\"Signature is required\"}");
        return;
    }
    const std::string expected = CanonicalSign(params);
    if (Lower(expected) != Lower(signature)) {
        LVA_LOGW(kTag, "signature mismatch (got %s, want %s)",
                 signature.c_str(), expected.c_str());
        SendJson(conn_fd, 401, "{\"error\":\"Unauthorized: Invalid signature\"}");
        return;
    }

    const std::string command = params["command"];

    // param payload: URL-encoded base64 of a JSON object
    std::map<std::string, std::string> param_dict;
    if (auto it = params.find("param"); it != params.end() && !it->second.empty()) {
        try {
            const std::string decoded_url = UrlDecode(it->second);
            const std::string decoded_b64 = Base64Decode(decoded_url);
            json j = json::parse(decoded_b64, nullptr, false);
            if (!j.is_discarded() && j.is_object()) {
                for (auto kit = j.begin(); kit != j.end(); ++kit) {
                    param_dict[kit.key()] = kit.value().is_string()
                        ? kit.value().get<std::string>()
                        : kit.value().dump();
                }
            }
        } catch (...) {
            SendJson(conn_fd, 400, "{\"error\":\"param decode error\"}");
            return;
        }
    }

    LVA_LOGI(kTag, "command=%s", command.c_str());

    if (command == "reboot") {
        SendJson(conn_fd, 200, "{\"success\":true}");
        // Queue the response before FIN, then reuse this worker for the delay.
        ::shutdown(conn_fd, SHUT_WR);
        if (WaitForActionDelay("reboot")) Supervisor::PerformReboot();
        return;
    }
    if (command == "factory_reset") {
        SendJson(conn_fd, 200, "{\"success\":true}");
        ::shutdown(conn_fd, SHUT_WR);
        if (WaitForActionDelay("factory_reset")) Supervisor::PerformFactoryReset();
        return;
    }
    if (command == "ota") {
        OtaRelease release;
        release.source = RequestSource::Explicit;
        release.url          = param_dict.count("url")     ? param_dict["url"]     : "";
        release.version      = param_dict.count("version") ? param_dict["version"] : "";
        release.expected_md5 = param_dict.count("md5")     ? param_dict["md5"]     : "";
        for (const char* req : { "url", "version", "md5" }) {
            if (param_dict.find(req) == param_dict.end()) {
                std::string err = std::string("{\"error\":\"Missing required field: ") +
                                  req + "\"}";
                SendJson(conn_fd, 400, err);
                return;
            }
        }
        const std::string ota_id = supervisor_.StartOtaUpdateAsync(release);
        if (ota_id.empty()) {
            SendJson(conn_fd, 409,
                     "{\"success\":false,\"error\":\"OTA already running\"}");
            return;
        }
        json j = { {"success", true}, {"ota_id", ota_id} };
        SendJson(conn_fd, 200, j.dump());
        return;
    }

    json j = { {"error", std::string("Unsupported command: ") + command} };
    SendJson(conn_fd, 400, j.dump());
}

void SupervisorHttpServer::SendJson(int conn_fd, int status,
                                    const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << ' ' << ReasonPhrase(status) << "\r\n"
        << "Content-Type: application/json; charset=utf-8\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        << "Access-Control-Allow-Headers: Content-Type\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    SendAll(conn_fd, oss.str());
}

void SupervisorHttpServer::SendText(int conn_fd, int status,
                                    const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << ' ' << ReasonPhrase(status) << "\r\n"
        << "Content-Type: text/plain; charset=utf-8\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    SendAll(conn_fd, oss.str());
}

}  // namespace lva::tr
