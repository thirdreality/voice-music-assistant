
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace webrtc { class AudioProcessing; class StreamConfig; }

namespace lva::audio {

class WebRtcProcessor {
   public:
    static constexpr int kSampleRate     = 16'000;
    static constexpr int kFrameSamples   = 160;   // 10 ms @ 16 kHz

    WebRtcProcessor(int agc_level, int ns_level, bool aec_enabled);
    ~WebRtcProcessor();

    WebRtcProcessor(const WebRtcProcessor&)            = delete;
    WebRtcProcessor& operator=(const WebRtcProcessor&) = delete;

    void SetLevels(int agc_level, int ns_level);

    // AEC must run only during playback: otherwise the loopback ref is
    // just crosstalk and AEC3 gates near-end speech as echo. Main thread.
    void SetFarEndActive(bool active);

    // Sendspin playback; either source keeps AEC on.
    void SetExternalFarEndActive(bool active);

    bool Process(std::int16_t* buf, std::size_t n);

    // False when AEC is inactive.
    bool GetLinearAecOutput(std::int16_t* buf, std::size_t n);

    // steady_clock ns until which the shadow detector is suppressed while
    // AEC3 converges after turning on. 0 = no guard.
    std::int64_t LinearGuardUntilNs() const noexcept {
        return linear_guard_until_ns_.load(std::memory_order_relaxed);
    }

    bool ProcessReverse(std::int16_t* buf, std::size_t n);

    // Removes the Sendspin component still correlated with the ref after
    // AEC. Only active during external playback.
    void SuppressSendspinResidual(std::int16_t* mic,
                                  const std::int16_t* ref,
                                  std::size_t n);

    // Call after an xrun, when mic and ref are misaligned.
    void ResetEcho();

    int  agc_level()   const { return agc_level_; }
    int  ns_level()    const { return ns_level_; }
    bool aec_enabled() const { return aec_enabled_; }

   private:
    // Caller must hold apm_mutex_.
    void RebuildLocked();
    // Caller must hold apm_mutex_.
    void ApplyConfigLocked(bool echo_on);
    // Caller must hold apm_mutex_.
    void ReconcileGateLocked();

    // Capture thread vs. SetLevels() on the main thread.
    std::mutex apm_mutex_;

    int  agc_level_   = 0;
    int  ns_level_    = 0;
    bool aec_enabled_ = false;

    // Desired (main thread) vs. applied (capture thread) echo state.
    std::atomic<bool> far_end_active_{false};
    std::atomic<bool> external_far_end_active_{false};
    bool              echo_on_applied_ = false;

    std::atomic<std::int64_t> linear_guard_until_ns_{0};

    void* apm_ = nullptr;
    std::unique_ptr<webrtc::StreamConfig> stream_cfg_;
};

}  // namespace lva::audio
