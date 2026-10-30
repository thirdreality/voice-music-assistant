#include "tr/NetworkPolicy.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fstream>
#include <cstring>
#include <nlohmann/json.hpp>
#include "util/Log.h"

namespace lva::tr {
namespace {
// Same lock name as network-policy.sh; custom paths isolate host tests.
std::filesystem::path LockFor(const std::filesystem::path& path) {
    if (path == "/data/conf/network.json") return "/tmp/ha_network_policy.lock";
    return path.string() + ".lock";
}
struct Lock {
    int fd;
    explicit Lock(const std::filesystem::path& path)
        : fd(::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600)) {
        if (fd >= 0 && ::flock(fd, LOCK_EX) != 0) { ::close(fd); fd = -1; }
    }
    ~Lock() { if (fd >= 0) ::close(fd); }
};
}

NetworkPolicy::NetworkPolicy(std::filesystem::path path) : path_(std::move(path)) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    Lock lock(LockFor(path_));
    if (lock.fd < 0) return;
    const auto marker = path_.parent_path() / "network.initialized";
    if (!std::filesystem::exists(path_, ec)) {
        if (!std::filesystem::exists(marker, ec)) StoreLocked(true);
        return;
    }
    try {
        std::ifstream file(path_);
        const auto j = nlohmann::json::parse(file);
        allowed_ = j.at("allow_background_internet").get<bool>();
    } catch (...) {
        LVA_LOGW("network", "invalid policy; background internet disabled");
    }
    // Existing configs (including corrupt ones) also count as initialized.
    const int fd = ::open(marker.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (fd >= 0) { ::fsync(fd); ::close(fd); }
}

bool NetworkPolicy::Save(bool allow) {
    Lock lock(LockFor(path_));
    return lock.fd >= 0 && StoreLocked(allow);
}

bool NetworkPolicy::StoreLocked(bool allow) {
    const std::string body = nlohmann::json{
        {"allow_background_internet", allow}}.dump() + "\n";
    std::string name = path_.string() + ".XXXXXX";
    const int fd = ::mkstemp(name.data());
    if (fd < 0) return false;
    const bool written = ::write(fd, body.data(), body.size()) ==
                         static_cast<ssize_t>(body.size()) && ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (!written || !closed || ::rename(name.c_str(), path_.c_str()) != 0) {
        ::unlink(name.c_str());
        return false;
    }
    const auto marker = path_.parent_path() / "network.initialized";
    const int mfd = ::open(marker.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    bool durable = mfd >= 0;
    if (mfd >= 0) { durable = ::fsync(mfd) == 0; ::close(mfd); }
    const int dfd = ::open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { durable = (::fsync(dfd) == 0) && durable; ::close(dfd); }
    else durable = false;
    // Publish the persisted value even if a directory fsync failed. The UI
    // still receives failure, but memory must not disagree with the file.
    if (Allowed() != allow) {
        allowed_.store(allow, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    return durable;
}

NetworkRequest::NetworkRequest(const NetworkPolicy& policy, RequestSource source,
                               const std::atomic<bool>* cancel)
    : policy_(policy), source_(source), generation_(policy.Generation()), cancel_(cancel) {}

bool NetworkRequest::Cancelled() const {
    if (cancel_ && cancel_->load(std::memory_order_acquire)) return true;
    if (source_ == RequestSource::Explicit) return false;
    // An off/on cycle must not revive an earlier request or its retries.
    return generation_ != policy_.Generation() ||
           (source_ == RequestSource::Automatic && !policy_.Allowed());
}

bool NetworkRequest::IsLocalAddress(const sockaddr* address) {
    if (address->sa_family == AF_INET) {
        const auto ip = ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr);
        return (ip >> 24) == 10 || (ip >> 24) == 127 ||
               (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8 ||
               (ip >> 16) == 0xa9fe;
    }
    if (address->sa_family == AF_INET6) {
        const auto& ip = reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(&ip)) {
            sockaddr_in v4{}; v4.sin_family = AF_INET;
            std::memcpy(&v4.sin_addr, &ip.s6_addr[12], sizeof(v4.sin_addr));
            return IsLocalAddress(reinterpret_cast<sockaddr*>(&v4));
        }
        return IN6_IS_ADDR_LOOPBACK(&ip) || IN6_IS_ADDR_LINKLOCAL(&ip) ||
               (ip.s6_addr[0] & 0xfe) == 0xfc;
    }
    return false;
}

curl_socket_t NetworkRequest::Open(void* opaque, curlsocktype, curl_sockaddr* address) {
    const auto& self = *static_cast<NetworkRequest*>(opaque);
    if (self.Cancelled()) return CURL_SOCKET_BAD;
    if (self.source_ != RequestSource::Explicit && !self.policy_.Allowed() &&
        !IsLocalAddress(&address->addr)) return CURL_SOCKET_BAD;
    return ::socket(address->family, address->socktype | SOCK_CLOEXEC, address->protocol);
}

int NetworkRequest::Progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<NetworkRequest*>(opaque)->Cancelled() ? 1 : 0;
}

void NetworkRequest::Configure(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, &Open);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, this);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &Progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (source_ != RequestSource::Explicit) {
        // A proxy would hide a public destination behind a private socket.
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    }
}
} // namespace lva::tr
