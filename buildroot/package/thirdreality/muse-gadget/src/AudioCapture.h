#pragma once
#include <alsa/asoundlib.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
namespace muse {
// The speaker's four-channel loopback layout is mic0, mic1, ref0, ref1.
void SelectMicrophoneChannel(const int16_t* interleaved, size_t frames, int16_t* mono);

class AudioCapture {
 public:
    using Callback = std::function<bool(const int16_t*, size_t)>;
    explicit AudioCapture(std::string device);
    ~AudioCapture();
    bool Start(Callback callback);
    void Stop();
    bool active() const { return active_; }
 private:
    void Run(Callback callback);
    std::string device_;
    snd_pcm_t* pcm_{nullptr};
    std::atomic<bool> active_{false};
    std::thread thread_;
};
}  // namespace muse
