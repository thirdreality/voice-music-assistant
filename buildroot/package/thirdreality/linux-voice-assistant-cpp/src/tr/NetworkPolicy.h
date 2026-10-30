#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <curl/curl.h>

namespace lva::tr {

enum class RequestSource { Automatic, Explicit, LocalModel };

// Loaded before any worker is created. The initialized marker distinguishes
// an upgrade's missing config from a deleted config after an explicit choice.
class NetworkPolicy {
 public:
    explicit NetworkPolicy(std::filesystem::path path = "/data/conf/network.json");
    bool Allowed() const { return allowed_.load(std::memory_order_acquire); }
    std::uint64_t Generation() const { return generation_.load(std::memory_order_acquire); }
    bool Save(bool allow);
 private:
    bool StoreLocked(bool allow);
    std::filesystem::path path_;
    std::atomic<bool> allowed_{false};
    std::atomic<std::uint64_t> generation_{0};
};

// One instance per transfer; it must outlive curl_easy_perform. LocalModel
// can resolve hostnames but only opens private/link-local/loopback sockets
// while background internet is disabled. Each redirect uses this check too.
class NetworkRequest {
 public:
    NetworkRequest(const NetworkPolicy& policy, RequestSource source,
                   const std::atomic<bool>* cancel = nullptr);
    bool Cancelled() const;
    void Configure(CURL* curl);
    static bool IsLocalAddress(const sockaddr* address);
 private:
    static curl_socket_t Open(void*, curlsocktype, curl_sockaddr*);
    static int Progress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t);
    const NetworkPolicy& policy_;
    RequestSource source_;
    std::uint64_t generation_;
    const std::atomic<bool>* cancel_;
};

} // namespace lva::tr
