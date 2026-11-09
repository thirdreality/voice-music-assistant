#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "state/Preferences.h"

namespace lva::state {

// The main loop only queues value snapshots. File locking, JSON merge, fsync
// and rename run on one worker. Stop() flushes the final snapshot before join.
class PreferencesWriter {
   public:
    ~PreferencesWriter();
    bool Start(std::filesystem::path path, const Preferences& initial);
    bool Queue(const Preferences& snapshot, bool force);
    // Recheck disk before recovery; do not manufacture dirty fields from a
    // fallback/default snapshot if an external writer has already repaired it.
    bool Recover(const Preferences& snapshot);
    std::uint64_t Revision() const;
    bool CanApplyExternal(const std::string& key, std::uint64_t revision) const;
    bool Stop();

   private:
    void Run();

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::thread thread_;
    bool stopping_ = true;
    bool failed_ = false;
    bool recovery_pending_ = false;
    bool recovery_writing_ = false;
    std::uint64_t revision_ = 0;
    std::filesystem::path path_;
    Preferences latest_;
    std::set<std::string> pending_;
    std::set<std::string> writing_;
    std::chrono::steady_clock::time_point first_change_{};
    std::chrono::steady_clock::time_point last_change_{};
    std::chrono::steady_clock::time_point retry_at_{};
};

}  // namespace lva::state
