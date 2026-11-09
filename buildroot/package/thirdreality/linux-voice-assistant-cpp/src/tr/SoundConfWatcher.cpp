#include "tr/SoundConfWatcher.h"

#include <cerrno>
#include <cmath>
#include <cstring>

#include "state/Preferences.h"
#include "state/ServerState.h"
#include "util/Log.h"

namespace lva::tr {

namespace {

constexpr const char* kTag = "soundwatch";

constexpr int kPollIntervalMs = 500;

}  // namespace

SoundConfWatcher::SoundConfWatcher(lva::state::ServerState& state,
                                   std::filesystem::path path)
    : state_(state), path_(std::move(path)) {}

void SoundConfWatcher::Poll() {
    const auto now = std::chrono::steady_clock::now();
    if (last_check_.time_since_epoch().count() != 0) {
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_check_).count();
        if (elapsed_ms < kPollIntervalMs) return;
    }
    last_check_ = now;

    struct stat st{};
    if (::stat(path_.c_str(), &st) != 0) {
        // May not exist on first boot.
        return;
    }
    if (have_stamp_ && st.st_mtim.tv_sec  == last_mtime_.tv_sec &&
        st.st_mtim.tv_nsec == last_mtime_.tv_nsec) {
        invalid_reads_ = 0;
        return;  // unchanged
    }
    // Keep the old stamp on deferral, so even an otherwise unchanged file is
    // retried after the pending save completes (or after a failed save).
    if (ReloadAndApply()) {
        last_mtime_ = st.st_mtim;
        have_stamp_ = true;
    }
}

bool SoundConfWatcher::ReloadAndApply() {
    const auto revision = state_.PreferencesRevision();
    bool loaded = false;
    bool corrupt = false;
    auto fresh = lva::state::Preferences::LoadFromFile(path_, &loaded, &corrupt);
    // A failed/partial external write must not apply fallback defaults (in
    // particular unmuting the microphone) as if they were an intentional edit.
    if (!loaded) {
        if (corrupt) {
            if (invalid_reads_ < 3) ++invalid_reads_;
            if (invalid_reads_ == 3) state_.RequestPreferencesRecovery();
        } else {
            invalid_reads_ = 0;
        }
        return false;
    }
    invalid_reads_ = 0;

    // A delayed save may not have reached disk yet. Do not apply stale disk
    // values over newer HA/button/Sendspin changes still being persisted.
    const bool volume_ready = state_.CanApplyExternalPreference("volume", revision);
    if (fresh.volume.has_value() && volume_ready) {
        const double on_disk = *fresh.volume;
        const double current =
            state_.volume.load(std::memory_order_relaxed);
        if (std::abs(on_disk - current) > 1e-4) {
            LVA_LOGI(kTag,
                     "external volume change detected: %.3f -> %.3f",
                     current, on_disk);
            state_.PersistVolume(on_disk);
        }
    }

    const bool on_disk_muted  = fresh.is_mic_muted();
    const bool current_muted  = state_.muted.load(std::memory_order_relaxed);
    const bool mute_ready = state_.CanApplyExternalPreference("mic_mute", revision);
    if (on_disk_muted != current_muted && mute_ready) {
        LVA_LOGI(kTag,
                 "external mic_mute change detected: %d -> %d",
                 current_muted ? 1 : 0, on_disk_muted ? 1 : 0);
        state_.PersistMuted(on_disk_muted);
        state_.PlayMuteToggleSound(on_disk_muted);
    }
    return volume_ready && mute_ready;
}

}  // namespace lva::tr
