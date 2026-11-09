#include "tr/LedRing.h"

#include <string>
#include <utility>

#include "tr/DeviceEffects.h"
#include "util/Log.h"

namespace lva::tr {

namespace {

constexpr const char* kTag    = "led";
constexpr const char* kAnimDir = "/usr/share/thirdreality/animation/";

const char* AnimationFor(LedState s) {
    switch (s) {
        case LedState::Listening: return "active-waking.animation";
        case LedState::Thinking:  return "active-thinking.animation";
        case LedState::Speaking:  return "active-talking.animation";
        case LedState::Idle:      return "active-ending.animation";
        case LedState::Error:     return "active-ending.animation";
        case LedState::Muted:     return "mics-off_on.animation";
        case LedState::Unmuted:   return "none.animation";
    }
    return nullptr;
}

bool IsTerminal(LedState s) {
    switch (s) {
        case LedState::Idle:
        case LedState::Error:
        case LedState::Muted:
        case LedState::Unmuted:
            return true;
        default:
            return false;
    }
}

}  // namespace

void ShowAnimations(bool to_idle, const std::vector<std::string>& paths) {
    std::string array_arg = "array:string:";
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (i != 0) array_arg += ',';
        array_arg += paths[i];
    }
    // Sendspin rewrites dynamic animation files in place; identical paths do
    // not imply identical contents, so only coalesce pending requests here.
    QueueLedAnimation(to_idle, std::move(array_arg), false, false);
}

void Show(LedState state) {
    const char* anim_name = AnimationFor(state);
    if (anim_name == nullptr) {
        LVA_LOGW(kTag, "unknown LED state");
        return;
    }
    QueueLedAnimation(IsTerminal(state), std::string("array:string:") + kAnimDir + anim_name);
}

void ShowVolumeChanged() {
    QueueLedAnimation(false, std::string("array:string:") + kAnimDir +
                             "volume-changed.animation", true);
}

}  // namespace lva::tr
