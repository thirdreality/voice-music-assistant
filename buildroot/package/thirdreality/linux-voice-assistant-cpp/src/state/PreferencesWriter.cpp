#include "state/PreferencesWriter.h"

#include <algorithm>
#include <exception>
#include <utility>
#include <pthread.h>

#include "util/Log.h"

namespace lva::state {
namespace {
constexpr const char* kTag = "prefs-writer";
constexpr auto kDebounce = std::chrono::milliseconds(300);
constexpr auto kMaxDelay = std::chrono::seconds(1);
constexpr auto kRetryDelay = std::chrono::seconds(1);
}

PreferencesWriter::~PreferencesWriter() { Stop(); }

bool PreferencesWriter::Start(std::filesystem::path path, const Preferences& initial) {
    std::lock_guard lock(mutex_);
    if (thread_.joinable()) return !stopping_;
    path_ = std::move(path);
    latest_ = initial;
    stopping_ = false;
    failed_ = false;
    try {
        thread_ = std::thread([this] {
            try {
                Run();
            } catch (const std::exception& error) {
                LVA_LOGE(kTag, "worker failed: %s", error.what());
                std::lock_guard lock(mutex_);
                pending_.insert(writing_.begin(), writing_.end());
                recovery_pending_ = recovery_pending_ || recovery_writing_;
                recovery_writing_ = false;
                writing_.clear();
                stopping_ = true;
                failed_ = true;
                ++revision_;
            } catch (...) {
                LVA_LOGE(kTag, "worker failed: unknown exception");
                std::lock_guard lock(mutex_);
                pending_.insert(writing_.begin(), writing_.end());
                recovery_pending_ = recovery_pending_ || recovery_writing_;
                recovery_writing_ = false;
                writing_.clear();
                stopping_ = true;
                failed_ = true;
                ++revision_;
            }
        });
    } catch (const std::exception& error) {
        stopping_ = true;
        LVA_LOGE(kTag, "thread creation failed: %s", error.what());
        return false;
    }
    return true;
}

bool PreferencesWriter::Queue(const Preferences& snapshot, bool force) {
    std::lock_guard lock(mutex_);
    if (stopping_) return false;
    const auto keys = force ? Preferences::AllFields() : snapshot.ChangedFields(latest_);
    // Keep the full snapshot/diff baseline current even when only metadata or
    // other non-diff fields changed. Recovery also needs this latest snapshot.
    latest_ = snapshot;
    if (keys.empty()) return true;
    const auto now = std::chrono::steady_clock::now();
    if (pending_.empty() && !recovery_pending_) first_change_ = now;
    pending_.insert(keys.begin(), keys.end());
    last_change_ = now;
    changed_.notify_one();
    return true;
}

bool PreferencesWriter::Recover(const Preferences& snapshot) {
    std::lock_guard lock(mutex_);
    if (stopping_) return false;
    latest_ = snapshot;
    if (recovery_pending_ || recovery_writing_) return true;
    const auto now = std::chrono::steady_clock::now();
    if (pending_.empty()) first_change_ = now;
    recovery_pending_ = true;
    last_change_ = now;
    changed_.notify_one();
    return true;
}

std::uint64_t PreferencesWriter::Revision() const {
    std::lock_guard lock(mutex_);
    return revision_;
}

bool PreferencesWriter::CanApplyExternal(const std::string& key,
                                         std::uint64_t revision) const {
    std::lock_guard lock(mutex_);
    return revision == revision_ && !recovery_pending_ && !recovery_writing_ &&
           pending_.count(key) == 0 && writing_.count(key) == 0;
}

bool PreferencesWriter::Stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_one();
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    return !failed_ && pending_.empty() && writing_.empty() &&
           !recovery_pending_ && !recovery_writing_;
}

void PreferencesWriter::Run() {
    ::pthread_setname_np(::pthread_self(), "lva-prefs");
    unsigned failures = 0;
    for (;;) {
        Preferences snapshot;
        std::set<std::string> keys;
        bool recover_corrupt;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] {
                return stopping_ || !pending_.empty() || recovery_pending_;
            });
            if (pending_.empty() && !recovery_pending_) return;
            if (!stopping_) {
                // Mic mute is applied in hardware immediately. Submit its
                // persistence promptly too, instead of waiting for debounce.
                const auto scheduled = (recovery_pending_ || pending_.count("mic_mute") != 0)
                    ? first_change_ : std::min(last_change_ + kDebounce,
                                              first_change_ + kMaxDelay);
                const auto due = std::max(retry_at_, scheduled);
                if (std::chrono::steady_clock::now() < due) {
                    changed_.wait_until(lock, due);
                    continue;  // Recompute after new changes or spurious wakeup.
                }
            }
            snapshot = latest_;
            keys = std::move(pending_);
            pending_.clear();
            writing_ = keys;
            recover_corrupt = recovery_pending_ || failures >= 3;
            recovery_pending_ = false;
            recovery_writing_ = recover_corrupt;
        }
        bool saved = false;
        try {
            saved = snapshot.SaveFields(path_, keys, recover_corrupt);
        } catch (const std::exception& error) {
            if (Preferences::ShouldLogWarning(path_)) {
                LVA_LOGW(kTag, "save exception: %s", error.what());
            }
        } catch (...) {
            if (Preferences::ShouldLogWarning(path_)) {
                LVA_LOGW(kTag, "save exception: unknown exception");
            }
        }
        bool final_failure;
        {
            std::lock_guard lock(mutex_);
            ++revision_;  // Even a failed fsync may have changed the file.
            writing_.clear();
            recovery_writing_ = false;
            final_failure = !saved && stopping_;
            if (!saved) {
                const auto now = std::chrono::steady_clock::now();
                if (pending_.empty() && !recovery_pending_) first_change_ = last_change_ = now;
                pending_.insert(keys.begin(), keys.end());
                recovery_pending_ = recovery_pending_ || recover_corrupt;
                retry_at_ = now + kRetryDelay;
                if (failures < 3) ++failures;
            } else {
                retry_at_ = {};
                failures = 0;
            }
        }
        if (!saved) {
            if (Preferences::ShouldLogWarning(path_)) {
                LVA_LOGW(kTag, "save to %s failed (%zu fields)%s", path_.c_str(),
                         keys.size(), final_failure ? "; final flush failed" : "; retrying");
            }
            if (final_failure) return;  // Do not retry forever during shutdown.
        } else {
            LVA_LOGD(kTag, "saved %zu changed fields to %s%s", keys.size(), path_.c_str(),
                     recover_corrupt ? " (recovery checked)" : "");
        }
    }
}

}  // namespace lva::state
