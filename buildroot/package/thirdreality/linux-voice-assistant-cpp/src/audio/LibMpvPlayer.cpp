#include "audio/LibMpvPlayer.h"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <utility>

#include <mpv/client.h>

#include "util/Log.h"

namespace lva::audio {

namespace {

constexpr const char* kTag = "mpv";

constexpr std::int64_t kDefaultAudioBufferMs    = 200;
constexpr std::int64_t kShortSoundAudioBufferMs = 800;
constexpr std::int64_t kDrainSafetyMarginMs     = 100;

double Clamp01(double v) noexcept {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

void SetMpvOptionString(mpv_handle* mpv, const char* name, const char* value) {
    const int rc = mpv_set_option_string(mpv, name, value);
    if (rc < 0) {
        LVA_LOGW(kTag, "set_option_string(%s=%s) failed: %s",
                 name, value, mpv_error_string(rc));
    }
}

}  // namespace

LibMpvPlayer::LibMpvPlayer(const Options& opts) {
    mpv_ = mpv_create();
    if (mpv_ == nullptr) {
        LVA_LOGE(kTag, "mpv_create returned NULL — libmpv missing or failing");
        return;
    }

    SetMpvOptionString(mpv_, "audio-display", "no");
    SetMpvOptionString(mpv_, "vid",            "no");
    SetMpvOptionString(mpv_, "video",          "no");
    SetMpvOptionString(mpv_, "input-default-bindings", "no");
    SetMpvOptionString(mpv_, "input-vo-keyboard",      "no");
    SetMpvOptionString(mpv_, "terminal", "no");

    SetMpvOptionString(mpv_, "cache",                "yes");
    SetMpvOptionString(mpv_, "cache-pause",          "yes");
    SetMpvOptionString(mpv_, "cache-pause-wait",     "10");
    SetMpvOptionString(mpv_, "demuxer-max-bytes",    "2MiB");
    SetMpvOptionString(mpv_, "demuxer-max-back-bytes", "512KiB");
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", opts.cache_secs);
        SetMpvOptionString(mpv_, "cache-secs", buf);
    }

    SetMpvOptionString(mpv_, "audio-stream-silence", "yes");

    if (opts.short_sound_safe) {
        SetMpvOptionString(mpv_, "audio-buffer", "0.8");
    }

    const std::int64_t audio_buffer_ms =
        opts.short_sound_safe ? kShortSoundAudioBufferMs
                              : kDefaultAudioBufferMs;
    drain_delay_ns_ = (audio_buffer_ms + kDrainSafetyMarginMs) * 1'000'000;

    if (!opts.audio_device.empty()) {
        SetMpvOptionString(mpv_, "audio-device", opts.audio_device.c_str());
    }

    SetMpvOptionString(mpv_, "msg-level", "all=warn");

    if (const int rc = mpv_initialize(mpv_); rc < 0) {
        LVA_LOGE(kTag, "mpv_initialize failed: %s", mpv_error_string(rc));
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return;
    }

    const int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd < 0) {
        LVA_LOGE(kTag, "eventfd: %s", std::strerror(errno));
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return;
    }
    wakeup_read_fd_  = efd;
    wakeup_write_fd_ = efd;  // eventfd is r/w from same fd

    drain_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (drain_fd_ < 0) {
        LVA_LOGW(kTag, "timerfd_create: %s; EOF completions will not drain",
                 std::strerror(errno));
        drain_delay_ns_ = 0;
    }

    mpv_set_wakeup_callback(mpv_, &OnMpvWakeupTrampoline, this);

    DrainEvents();

    LVA_LOGI(kTag, "libmpv initialized (cache_secs=%d device=%s)",
             opts.cache_secs,
             opts.audio_device.empty() ? "default" : opts.audio_device.c_str());
}

LibMpvPlayer::~LibMpvPlayer() {
    CancelDrain();
    if (mpv_ != nullptr) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }
    if (wakeup_read_fd_ >= 0) {
        ::close(wakeup_read_fd_);
        wakeup_read_fd_  = -1;
        wakeup_write_fd_ = -1;
    }
    if (drain_fd_ >= 0) {
        ::close(drain_fd_);
        drain_fd_ = -1;
    }
}

void LibMpvPlayer::OnMpvWakeupTrampoline(void* opaque) {
    auto* self = static_cast<LibMpvPlayer*>(opaque);
    if (self == nullptr || self->wakeup_write_fd_ < 0) return;
    const std::uint64_t value = 1;
    ssize_t r;
    do {
        r = ::write(self->wakeup_write_fd_, &value, sizeof(value));
    } while (r < 0 && errno == EINTR);
    (void)r;  // best-effort
}

void LibMpvPlayer::ReadDrainWakeupFd() {
    std::uint64_t v = 0;
    while (::read(wakeup_read_fd_, &v, sizeof(v)) > 0) {
    }
}

void LibMpvPlayer::PollDrain() {
    if (drain_fd_ < 0) return;
    std::uint64_t expirations = 0;
    const ssize_t n = ::read(drain_fd_, &expirations, sizeof(expirations));
    if (n != static_cast<ssize_t>(sizeof(expirations))) return;
    if (!drain_pending_) return;
    if (drain_paused_) return;
    FinishPlayback();
}

void LibMpvPlayer::FinishPlayback() {
    drain_pending_ = false;
    drain_paused_  = false;
    drain_remaining_ns_ = 0;
    SetState(PlayerState::kIdle);

    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        cb = std::move(drain_callback_);
        drain_callback_ = nullptr;
    }
    if (!cb) return;
    try {
        cb();
    } catch (...) {
        // Callback errors must never break the player.
    }
}

void LibMpvPlayer::CancelDrain() {
    drain_pending_ = false;
    drain_paused_  = false;
    drain_remaining_ns_ = 0;
    if (drain_fd_ >= 0) {
        itimerspec disarmed{};
        ::timerfd_settime(drain_fd_, 0, &disarmed, nullptr);
        std::uint64_t expirations = 0;
        while (::read(drain_fd_, &expirations, sizeof(expirations)) > 0) {
        }
    }
    std::lock_guard<std::mutex> lk(cb_mutex_);
    drain_callback_ = nullptr;
}

void LibMpvPlayer::SuspendDrain() {
    if (!drain_pending_ || drain_paused_ || drain_fd_ < 0) return;

    itimerspec left{};
    if (::timerfd_gettime(drain_fd_, &left) == 0) {
        drain_remaining_ns_ = static_cast<std::int64_t>(left.it_value.tv_sec) *
                                  1'000'000'000LL +
                              left.it_value.tv_nsec;
    } else {
        drain_remaining_ns_ = 0;
    }

    drain_paused_ = true;
    itimerspec disarmed{};
    ::timerfd_settime(drain_fd_, 0, &disarmed, nullptr);
    std::uint64_t expirations = 0;
    while (::read(drain_fd_, &expirations, sizeof(expirations)) > 0) {
    }
    LVA_LOGD(kTag, "drain suspended with %lld ms left",
             static_cast<long long>(drain_remaining_ns_ / 1'000'000));
}

void LibMpvPlayer::ResumeDrain() {
    if (!drain_pending_ || !drain_paused_) return;
    drain_paused_ = false;

    if (drain_remaining_ns_ <= 0 || drain_fd_ < 0) {
        FinishPlayback();
        return;
    }

    itimerspec due{};
    due.it_value.tv_sec  = drain_remaining_ns_ / 1'000'000'000LL;
    due.it_value.tv_nsec = drain_remaining_ns_ % 1'000'000'000LL;
    if (::timerfd_settime(drain_fd_, 0, &due, nullptr) < 0) {
        LVA_LOGW(kTag, "timerfd_settime: %s; completing now",
                 std::strerror(errno));
        FinishPlayback();
        return;
    }
    LVA_LOGD(kTag, "drain resumed with %lld ms left",
             static_cast<long long>(drain_remaining_ns_ / 1'000'000));
}

void LibMpvPlayer::SetState(PlayerState new_state) {
    const PlayerState prev =
        state_.exchange(new_state, std::memory_order_relaxed);
    if (prev != new_state && on_state_changed_) {
        try {
            on_state_changed_(new_state);
        } catch (...) {
            // Listener errors must never break the caller.
        }
    }
}

void LibMpvPlayer::DrainEvents() {
    if (mpv_ == nullptr) return;
    ReadDrainWakeupFd();

    while (true) {
        mpv_event* ev = mpv_wait_event(mpv_, 0);
        if (ev == nullptr || ev->event_id == MPV_EVENT_NONE) {
            break;
        }
        switch (ev->event_id) {
            case MPV_EVENT_START_FILE: {
                const auto* start = static_cast<mpv_event_start_file*>(ev->data);
                event_entry_id_ = start ? start->playlist_entry_id : -1;
                if (requested_entry_ids_.count(event_entry_id_) == 0) break;
                if (State() != PlayerState::kPaused)
                    SetState(PlayerState::kLoading);
                LVA_LOGD(kTag, "event: START_FILE");
                break;
            }
            case MPV_EVENT_FILE_LOADED:
                if (requested_entry_ids_.count(event_entry_id_) == 0) break;
                if (State() != PlayerState::kPaused)
                    SetState(PlayerState::kPlaying);
                LVA_LOGD(kTag, "event: FILE_LOADED");
                break;
            case MPV_EVENT_END_FILE: {
                auto* end = static_cast<mpv_event_end_file*>(ev->data);
                if (end && end->playlist_entry_id == event_entry_id_)
                    event_entry_id_ = -1;
                if (!end || requested_entry_ids_.erase(end->playlist_entry_id) == 0)
                    break;
                const int reason = static_cast<int>(end->reason);
                LVA_LOGD(kTag, "event: END_FILE (reason=%d)", reason);

                if (reason == MPV_END_FILE_REASON_REDIRECT &&
                    end->playlist_insert_num_entries > 0) {
                    for (int i = 0; i < end->playlist_insert_num_entries; ++i)
                        requested_entry_ids_.insert(end->playlist_insert_id + i);
                    break;
                }
                if (!requested_entry_ids_.empty() &&
                    (reason == MPV_END_FILE_REASON_EOF ||
                     reason == MPV_END_FILE_REASON_ERROR ||
                     reason == MPV_END_FILE_REASON_REDIRECT)) {
                    if (State() != PlayerState::kPaused)
                        SetState(PlayerState::kLoading);
                    break;
                }

                if (reason != MPV_END_FILE_REASON_EOF &&
                    reason != MPV_END_FILE_REASON_ERROR &&
                    reason != MPV_END_FILE_REASON_REDIRECT) {
                    requested_entry_ids_.clear();
                    SetState(PlayerState::kIdle);
                    break;
                }

                {
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    drain_callback_ = std::move(done_callback_);
                    done_callback_  = nullptr;
                }

                if (reason == MPV_END_FILE_REASON_ERROR ||
                    reason == MPV_END_FILE_REASON_REDIRECT ||
                    drain_delay_ns_ <= 0) {
                    if (reason == MPV_END_FILE_REASON_ERROR) {
                        LVA_LOGW(kTag,
                                 "playback ended with an error; completing "
                                 "without draining");
                    }
                    FinishPlayback();
                    break;
                }

                if (state_.load(std::memory_order_relaxed) ==
                    PlayerState::kPaused) {
                    drain_pending_      = true;
                    drain_paused_       = true;
                    drain_remaining_ns_ = drain_delay_ns_;
                    LVA_LOGD(kTag,
                             "END_FILE while paused: holding %lld ms until "
                             "resume",
                             static_cast<long long>(drain_delay_ns_ / 1'000'000));
                    break;
                }

                if (drain_fd_ < 0) {
                    FinishPlayback();
                    break;
                }
                itimerspec due{};
                due.it_value.tv_sec  = drain_delay_ns_ / 1'000'000'000LL;
                due.it_value.tv_nsec = drain_delay_ns_ % 1'000'000'000LL;
                if (::timerfd_settime(drain_fd_, 0, &due, nullptr) < 0) {
                    LVA_LOGW(kTag, "timerfd_settime: %s; completing now",
                             std::strerror(errno));
                    FinishPlayback();
                    break;
                }
                drain_pending_ = true;
                LVA_LOGD(kTag, "END_FILE: draining %lld ms before completing",
                         static_cast<long long>(drain_delay_ns_ / 1'000'000));
                break;
            }
            case MPV_EVENT_LOG_MESSAGE: {
                auto* m = static_cast<mpv_event_log_message*>(ev->data);
                if (m && m->level && m->text) {
                    LVA_LOGD(kTag, "[mpv:%s] %s", m->level, m->text);
                }
                break;
            }
            case MPV_EVENT_SHUTDOWN:
                LVA_LOGI(kTag, "event: SHUTDOWN");
                SetState(PlayerState::kIdle);
                return;
            default:
                LVA_LOGD(kTag, "event: %s", mpv_event_name(ev->event_id));
                break;
        }
    }
}

void LibMpvPlayer::Play(const std::string& url,
                        std::function<void()> done_callback) {
    if (mpv_ == nullptr) {
        LVA_LOGW(kTag, "Play(%s) ignored: mpv not initialized",
                 url.c_str());
        return;
    }
    CancelDrain();
    requested_entry_ids_.clear();
    {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        done_callback_ = std::move(done_callback);
    }

    const char* args[] = {"loadfile", url.c_str(), "replace", nullptr};
    mpv_node result{};
    std::int64_t requested_entry_id = -1;
    const int rc = mpv_command_ret(mpv_, args, &result);
    if (rc >= 0 && result.format == MPV_FORMAT_NODE_MAP && result.u.list) {
        for (int i = 0; i < result.u.list->num; ++i) {
            const auto& value = result.u.list->values[i];
            if (std::strcmp(result.u.list->keys[i], "playlist_entry_id") == 0 &&
                value.format == MPV_FORMAT_INT64) {
                requested_entry_id = value.u.int64;
                break;
            }
        }
    }
    mpv_free_node_contents(&result);
    if (rc < 0 || requested_entry_id < 0) {
        LVA_LOGE(kTag, "loadfile(%s) failed: %s", url.c_str(),
                 rc < 0 ? mpv_error_string(rc) : "missing playlist entry ID");
        SetState(PlayerState::kIdle);
        std::lock_guard<std::mutex> lk(cb_mutex_);
        done_callback_ = nullptr;
        return;
    }
    requested_entry_ids_.insert(requested_entry_id);
    int no = 0;
    if (const int rc = mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &no);
        rc < 0) {
        LVA_LOGW(kTag, "clear pause failed: %s", mpv_error_string(rc));
    }
    SetState(PlayerState::kLoading);
    LVA_LOGI(kTag, "Play: %s", url.c_str());
}

void LibMpvPlayer::Pause() {
    if (mpv_ == nullptr) return;
    int yes = 1;
    if (const int rc = mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &yes);
        rc < 0) {
        LVA_LOGW(kTag, "pause set failed: %s", mpv_error_string(rc));
        return;
    }
    SetState(PlayerState::kPaused);
    SuspendDrain();
}

void LibMpvPlayer::Resume() {
    if (mpv_ == nullptr) return;
    int no = 0;
    if (const int rc = mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &no);
        rc < 0) {
        LVA_LOGW(kTag, "resume set failed: %s", mpv_error_string(rc));
        return;
    }
    SetState(PlayerState::kPlaying);
    // Continue the tail that a pause held back.
    ResumeDrain();
}

void LibMpvPlayer::Stop() {
    if (mpv_ == nullptr) return;
    const bool draining = drain_pending_;
    CancelDrain();
    {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        done_callback_ = nullptr;
    }
    const char* args[] = {"stop", nullptr};
    if (const int rc = mpv_command(mpv_, args); rc < 0) {
        LVA_LOGW(kTag, "stop failed: %s", mpv_error_string(rc));
        if (!draining) return;
    }
    requested_entry_ids_.clear();
    SetState(PlayerState::kIdle);
}

void LibMpvPlayer::SetGain(double normalized) {
    user_volume_ = Clamp01(normalized);
    if (mpv_ == nullptr) return;
    double percent = user_volume_ * 100.0;
    if (const int rc = mpv_set_property(mpv_, "volume",
                                        MPV_FORMAT_DOUBLE, &percent);
        rc < 0) {
        LVA_LOGW(kTag, "volume set failed: %s", mpv_error_string(rc));
    }
}

PlayerState LibMpvPlayer::State() const {
    return state_.load(std::memory_order_relaxed);
}

void LibMpvPlayer::SetStateChangedCallback(
    std::function<void(PlayerState)> on_state_changed) {
    on_state_changed_ = std::move(on_state_changed);
}

}  // namespace lva::audio
