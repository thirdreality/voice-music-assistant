
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
    using DoneCallback = std::function<void(bool ok)>;
    using StateChangedCallback = std::function<void(PlayerState)>;

    virtual ~IAudioPlayer() = default;

    // True means the request was submitted, not that media has loaded.
    // On synchronous failure, return false without invoking done_callback.
    // On asynchronous completion, report true only if playback completed
    // successfully (including all redirected entries). Explicit Stop() or
    // replacement cancels the old callback.
    virtual bool Play(const std::string& url,
                      DoneCallback done_callback = {}) = 0;

    virtual void Pause()  = 0;
    virtual void Resume() = 0;

    virtual void Stop() = 0;

    virtual void SetGain(double normalized) = 0;

    virtual PlayerState State() const = 0;

    // Single observer; an empty callback detaches it. Deliver on the main loop,
    // and do not retain/invoke the previous observer after replacement returns.
    virtual void SetStateChangedCallback(StateChangedCallback callback) = 0;
};

}  // namespace lva::audio
