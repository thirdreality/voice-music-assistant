#include "protocol/ApiServer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <utility>

#include "state/ServerState.h"
#include "util/Log.h"

namespace lva::proto {

namespace {

constexpr const char* kTag = "api";

bool SetNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    return true;
}

}  // namespace

ApiServer::ApiServer(lva::state::ServerState& state, Options options)
    : options_(std::move(options)), state_(state) {}

ApiServer::~ApiServer() {
    state_.broadcast = nullptr;
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (epoll_fd_  >= 0) ::close(epoll_fd_);
    if (wake_fd_   >= 0) ::close(wake_fd_);
}

void ApiServer::Stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
    const int fd = wake_fd_;
    if (fd >= 0) {
        const std::uint64_t value = 1;
        ssize_t r;
        do {
            r = ::write(fd, &value, sizeof(value));
        } while (r < 0 && errno == EINTR);
        (void)r;
    }
}

void ApiServer::AddAuxFd(int fd, std::function<void()> on_readable) {
    if (epoll_fd_ < 0) {
        LVA_LOGE(kTag, "AddAuxFd called before Start()");
        return;
    }
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LVA_LOGE(kTag, "epoll_ctl(aux fd=%d) failed: %s", fd,
                 std::strerror(errno));
        return;
    }
    aux_handlers_.emplace(fd, std::move(on_readable));
    LVA_LOGD(kTag, "registered aux fd=%d", fd);
}

bool ApiServer::Start() {
    const bool bind_any =
        options_.bind_address == "0.0.0.0" || options_.bind_address.empty();

    // On the wildcard address, listen dual-stack (IPv6 socket with
    // IPV6_V6ONLY=0) so HA can connect over either family. avahi also
    // advertises the device's IPv6 addresses, and HA may pick one of them.
    if (bind_any) {
        listen_fd_ = ::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) {
            LVA_LOGW(kTag, "IPv6 socket() failed (%s), falling back to IPv4",
                     std::strerror(errno));
        }
    }
    const bool dual_stack = listen_fd_ >= 0;
    if (!dual_stack) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    }
    if (listen_fd_ < 0) {
        LVA_LOGE(kTag, "socket() failed: %s", std::strerror(errno));
        return false;
    }

    int one = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        LVA_LOGW(kTag, "setsockopt(SO_REUSEADDR) failed: %s", std::strerror(errno));
    }

    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    if (dual_stack) {
        int zero = 0;
        if (::setsockopt(listen_fd_, IPPROTO_IPV6, IPV6_V6ONLY, &zero,
                         sizeof(zero)) < 0) {
            LVA_LOGE(kTag, "setsockopt(IPV6_V6ONLY=0) failed: %s",
                     std::strerror(errno));
            return false;
        }
        auto* a6 = reinterpret_cast<sockaddr_in6*>(&addr);
        a6->sin6_family = AF_INET6;
        a6->sin6_port   = htons(options_.port);
        a6->sin6_addr   = in6addr_any;
        addr_len = sizeof(sockaddr_in6);
    } else {
        auto* a4 = reinterpret_cast<sockaddr_in*>(&addr);
        a4->sin_family = AF_INET;
        a4->sin_port   = htons(options_.port);
        if (bind_any) {
            a4->sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (::inet_pton(AF_INET, options_.bind_address.c_str(),
                               &a4->sin_addr) != 1) {
            LVA_LOGE(kTag, "inet_pton(%s) failed", options_.bind_address.c_str());
            return false;
        }
        addr_len = sizeof(sockaddr_in);
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), addr_len) < 0) {
        LVA_LOGE(kTag, "bind(%s:%u) failed: %s", options_.bind_address.c_str(),
                 options_.port, std::strerror(errno));
        return false;
    }

    if (::listen(listen_fd_, options_.listen_backlog) < 0) {
        LVA_LOGE(kTag, "listen() failed: %s", std::strerror(errno));
        return false;
    }

    if (!SetNonBlocking(listen_fd_)) {
        LVA_LOGE(kTag, "fcntl(O_NONBLOCK) on listen fd failed: %s",
                 std::strerror(errno));
        return false;
    }

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        LVA_LOGE(kTag, "epoll_create1 failed: %s", std::strerror(errno));
        return false;
    }

    wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0) {
        LVA_LOGE(kTag, "eventfd failed: %s", std::strerror(errno));
        return false;
    }

    {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = listen_fd_;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
            LVA_LOGE(kTag, "epoll_ctl(listen) failed: %s", std::strerror(errno));
            return false;
        }
    }

    {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = wake_fd_;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) < 0) {
            LVA_LOGE(kTag, "epoll_ctl(wakefd) failed: %s", std::strerror(errno));
            return false;
        }
    }

    LVA_LOGI(kTag, "listening on %s:%u%s (device_name=%s)",
             dual_stack ? "[::]" : options_.bind_address.c_str(),
             options_.port, dual_stack ? " (IPv4+IPv6)" : "",
             state_.name.c_str());

    state_.broadcast = [this](std::uint32_t msg_type_id,
                              const ::google::protobuf::MessageLite& msg) {
        for (auto& [fd, conn] : connections_) {
            (void)fd;
            if (conn) {
                conn->SendSerializedMessage(msg_type_id, msg);
            }
        }
    };

    return true;
}

bool ApiServer::AcceptOne() {
    sockaddr_storage peer{};
    socklen_t peer_len = sizeof(peer);
    const int fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&peer),
                             &peer_len, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        if (errno == EINTR) return true;
        LVA_LOGE(kTag, "accept4 failed: %s", std::strerror(errno));
        return false;
    }

    int one = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
        LVA_LOGW(kTag, "setsockopt(TCP_NODELAY) failed: %s", std::strerror(errno));
    }

    char ip_str[INET6_ADDRSTRLEN] = {0};
    unsigned port = 0;
    if (peer.ss_family == AF_INET6) {
        const auto* p6 = reinterpret_cast<const sockaddr_in6*>(&peer);
        if (IN6_IS_ADDR_V4MAPPED(&p6->sin6_addr)) {
            // Show IPv4 clients on the dual-stack socket as a.b.c.d.
            ::inet_ntop(AF_INET, &p6->sin6_addr.s6_addr[12], ip_str,
                        sizeof(ip_str));
        } else {
            ::inet_ntop(AF_INET6, &p6->sin6_addr, ip_str, sizeof(ip_str));
        }
        port = ntohs(p6->sin6_port);
    } else {
        const auto* p4 = reinterpret_cast<const sockaddr_in*>(&peer);
        ::inet_ntop(AF_INET, &p4->sin_addr, ip_str, sizeof(ip_str));
        port = ntohs(p4->sin_port);
    }
    char peer_label[80];
    std::snprintf(peer_label, sizeof(peer_label), "%s:%u", ip_str, port);

    auto conn = std::make_unique<Connection>(fd, state_,
                                             std::string(peer_label));

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LVA_LOGE(kTag, "epoll_ctl(client) failed: %s", std::strerror(errno));
        return true;
    }

    connections_.emplace(fd, std::move(conn));
    return true;
}

void ApiServer::EraseConnection(int fd) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    auto it = connections_.find(fd);
    if (it != connections_.end()) {
        connections_.erase(it);
    } else {
        ::close(fd);
    }
    if (connections_.empty() && state_.on_client_disconnected) {
        state_.on_client_disconnected();
    }
}

int ApiServer::Run() {
    constexpr int kMaxEvents = 16;
    epoll_event events[kMaxEvents];

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        const int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            LVA_LOGE(kTag, "epoll_wait failed: %s", std::strerror(errno));
            return 1;
        }

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;

            if (fd == wake_fd_) {
                std::uint64_t v = 0;
                while (::read(wake_fd_, &v, sizeof(v)) > 0) {
                }
                continue;
            }

            if (fd == listen_fd_) {
                while (AcceptOne()) {
                }
                continue;
            }

            if (auto it = aux_handlers_.find(fd); it != aux_handlers_.end()) {
                try {
                    it->second();
                } catch (...) {
                    LVA_LOGW(kTag, "aux fd %d handler threw", fd);
                }
                continue;
            }

            auto it = connections_.find(fd);
            if (it == connections_.end()) {
                continue;
            }
            Connection& conn = *it->second;

            const std::uint32_t mask = events[i].events;
            bool keep_alive = true;
            if (mask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                keep_alive = false;
            } else {
                if (mask & EPOLLIN) {
                    keep_alive = conn.OnReadable();
                }
                if (keep_alive && (mask & EPOLLOUT)) {
                    keep_alive = conn.OnWritable();
                }
                if (keep_alive && conn.closed()) {
                    keep_alive = false;
                }
            }
            if (!keep_alive) {
                EraseConnection(fd);
            }
        }
    }

    LVA_LOGI(kTag, "stopping (cleaning up %zu connection(s))",
             connections_.size());
    while (!connections_.empty()) {
        EraseConnection(connections_.begin()->first);
    }
    return 0;
}

}  // namespace lva::proto
