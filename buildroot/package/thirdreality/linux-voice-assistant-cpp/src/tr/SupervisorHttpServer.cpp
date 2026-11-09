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
#include "util/Md5.h"

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
    return lva::util::Md5Hex(s);
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
