#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "audio/SanoTts.h"

namespace lva::satellite {

// Main-thread API. The single worker owns all SanoTts calls, including
// initialization, speed changes and shutdown. At most one pending job/result.
class LocalTts {
   public:
    struct Result {
        std::uint64_t generation;
        std::string path;  // Empty on failure. Caller owns a successful file.
    };

    explicit LocalTts(lva::audio::SanoTts::Config config);
    ~LocalTts();
    LocalTts(const LocalTts&) = delete;
    LocalTts& operator=(const LocalTts&) = delete;

    bool Start();
    void Stop();
    void SetSpeed(float speed);
    std::uint64_t Submit(std::string text);
    // After a timeout, reject new jobs until the in-flight call returns.
    void Cancel(bool timed_out = false);
    // Recheck completion/progress and cancel atomically under mutex_. A result
    // or fresh progress published after the caller's poll wins over timeout.
    bool TryCancelStalled(std::uint64_t generation,
                          std::chrono::steady_clock::duration timeout);
    bool Busy() const;
    // Only real synthesis progress for this generation renews the watchdog.
    std::optional<std::chrono::steady_clock::time_point> ProgressAt(
        std::uint64_t generation) const;
    std::optional<Result> TakeResult();

   private:
    struct Request {
        std::uint64_t generation;
        std::string text;
    };
    void ThreadMain();
    void ReportProgress(std::uint64_t generation);
    void DiscardResult();  // mutex_ held.
    void CancelLocked(bool timed_out);  // mutex_ held.

    lva::audio::SanoTts::Config config_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<bool> cancel_current_{false};
    bool stopping_ = true;
    bool busy_ = false;
    bool recovering_ = false;
    float speed_;
    std::uint64_t generation_ = 0;
    std::chrono::steady_clock::time_point progress_at_{};
    std::optional<Request> pending_;
    std::optional<Result> result_;
};

}  // namespace lva::satellite
