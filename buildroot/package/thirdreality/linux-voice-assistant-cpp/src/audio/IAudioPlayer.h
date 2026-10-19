
#pragma once

#include <functional>
#include <string>

namespace lva::audio {

enum class PlayerState {
    kIdle,
    kLoading,   // mpv is fetching/buffering the URL
    kPlaying,
    kPaused,
    kError,
};

class IAudioPlayer {
   public:
    virtual ~IAudioPlayer() = default;

    virtual void Play(const std::string& url,
                      std::function<void()> done_callback = {}) = 0;

    virtual void Pause()  = 0;
    virtual void Resume() = 0;

    virtual void Stop() = 0;

    virtual void SetGain(double normalized) = 0;

    virtual PlayerState State() const = 0;
};

}  // namespace lva::audio
