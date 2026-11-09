
#pragma once

#include <string>
#include <vector>

namespace lva::tr {

enum class LedState {
    Idle,        // pipeline finished
    Listening,   // wake fired; mic streaming to HA
    Thinking,    // STT done, waiting on intent / TTS
    Speaking,    // TTS playing
    Error,       // pipeline error
    Muted,       // mic muted
    Unmuted,     // mic unmuted
};

void Show(LedState state);
void ShowAnimations(bool to_idle, const std::vector<std::string>& paths);

// Brief volume feedback, coalesced/throttled on the background worker.
void ShowVolumeChanged();

}  // namespace lva::tr
