#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <unordered_set>

#include <pulse/pulseaudio.h>

namespace lva::audio {

// Observes PulseAudio sink-input lifecycle and reports whether the official
// sendspin-client currently owns an active playback stream.
//
// ThirdReality's Sendspin client creates its PulseAudio sink input only while
// actual Sendspin audio is playing and removes it when playback stops. This
// lets the voice assistant gate hardware-loopback AEC without modifying the
// Sendspin process itself.
class SendspinPlaybackMonitor {
   public:
    using Callback = std::function<void(bool active)>;

    explicit SendspinPlaybackMonitor(Callback callback);
    ~SendspinPlaybackMonitor();

    SendspinPlaybackMonitor(const SendspinPlaybackMonitor&) = delete;
    SendspinPlaybackMonitor& operator=(const SendspinPlaybackMonitor&) = delete;

    bool Start();
    void Stop();

   private:
    static void OnContextState(pa_context* ctx, void* userdata);
    static void OnSubscription(pa_context* ctx,
                               pa_subscription_event_type_t event_type,
                               uint32_t index,
                               void* userdata);
    static void OnSinkInputInfo(pa_context* ctx,
                                const pa_sink_input_info* info,
                                int eol,
                                void* userdata);

    void ThreadMain();
    void QuerySinkInput(unsigned index);
    void PublishState();
    void ClearState();

    Callback callback_;

    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};

    pa_mainloop* mainloop_ = nullptr;
    pa_context*  context_  = nullptr;

    std::unordered_set<unsigned> sendspin_inputs_;
    bool published_active_ = false;
};

}  // namespace lva::audio
