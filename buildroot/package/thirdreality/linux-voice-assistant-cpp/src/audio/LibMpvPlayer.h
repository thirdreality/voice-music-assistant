
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>

#include "audio/IAudioPlayer.h"

struct mpv_handle;

namespace lva::audio {

class LibMpvPlayer final : public IAudioPlayer {
   public:
    struct Options {
        // PulseAudio sink to send to. Empty = mpv's default.
        std::string audio_device;
        int  cache_secs       = 2;
        bool short_sound_safe = true;
    };

    explicit LibMpvPlayer(const Options& opts);
    ~LibMpvPlayer() override;

    LibMpvPlayer(const LibMpvPlayer&)            = delete;
    LibMpvPlayer& operator=(const LibMpvPlayer&) = delete;

    bool        Play(const std::string& url,
                     DoneCallback done_callback = {}) override;
    void        Pause()  override;
    void        Resume() override;
    void        Stop()   override;
    void        SetGain(double normalized) override;
    PlayerState State() const override;

    int  WakeupFd() const noexcept { return wakeup_read_fd_; }

    void DrainEvents();

    void PollDrain();

    int  DrainFd() const noexcept { return drain_fd_; }

    void SetStateChangedCallback(
        StateChangedCallback on_state_changed) override;

   private:
    static void OnMpvWakeupTrampoline(void* opaque);
    void        ReadDrainWakeupFd();

    void        FinishPlayback();
    void        CancelDrain();
    void        SuspendDrain();
    void        ResumeDrain();

    void        SetState(PlayerState new_state);
    bool        FailPlay();

    mpv_handle* mpv_      = nullptr;
    int  wakeup_read_fd_  = -1;
    int  wakeup_write_fd_ = -1;
    int  drain_fd_        = -1;

    std::unordered_set<std::int64_t> requested_entry_ids_;
    std::int64_t event_entry_id_ = -1;
    bool playback_ok_ = true;  // Accumulate errors across redirected entries.

    std::atomic<PlayerState> state_{PlayerState::kIdle};

    std::function<void(PlayerState)> on_state_changed_;

    std::mutex                   cb_mutex_;
    DoneCallback                done_callback_;

    DoneCallback                               drain_callback_;
    bool                                        drain_ok_ = true;
    bool                                        drain_pending_ = false;
    std::int64_t                                drain_delay_ns_ = 0;
    std::int64_t                                drain_remaining_ns_ = 0;
    bool                                        drain_paused_ = false;

    double user_volume_ = 1.0;
};

}  // namespace lva::audio
