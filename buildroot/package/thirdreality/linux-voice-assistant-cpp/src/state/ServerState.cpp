#include "state/ServerState.h"

#include <algorithm>
#include <cmath>

#include "util/Log.h"

namespace lva::state {

namespace {
constexpr const char* kTag = "state";

double Clamp01(double v) noexcept {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}
}  // namespace

void ServerState::PersistVolume(double new_volume) {
    const double clamped = Clamp01(new_volume);
    const bool runtime_changed =
        std::abs(volume.load(std::memory_order_relaxed) - clamped) > 1e-4;
    const bool prefs_changed =
        !preferences.volume.has_value() ||
        std::abs(*preferences.volume - clamped) > 1e-4;
    if (!runtime_changed && !prefs_changed) {
        return;
    }
    volume.store(clamped, std::memory_order_relaxed);
    preferences.volume = clamped;
    if (effects_.volume_changed) effects_.volume_changed(clamped);
    if (!SavePreferences()) {
        LVA_LOGW(kTag, "PersistVolume(%.4f): save failed", clamped);
    }
}

void ServerState::PersistMuted(bool new_muted) {
    const bool runtime_changed =
        muted.load(std::memory_order_relaxed) != new_muted;
    const bool prefs_changed = preferences.is_mic_muted() != new_muted;
    if (!runtime_changed && !prefs_changed) return;

    muted.store(new_muted, std::memory_order_relaxed);
    preferences.set_mic_muted(new_muted);
    if (effects_.muted_changed) effects_.muted_changed(new_muted);
    if (!SavePreferences()) {
        LVA_LOGW(kTag, "PersistMuted(%d): save failed", new_muted ? 1 : 0);
    }
}

void ServerState::PlayMuteToggleSound(bool muted) {
    if (effects_.mute_sound) effects_.mute_sound(muted);
}

void ServerState::PersistSendspinEnabled(bool enabled) {
    if (preferences.sendspin_enabled == enabled) return;
    preferences.sendspin_enabled = enabled;
    if (!SavePreferences()) {
        LVA_LOGW(kTag, "PersistSendspinEnabled(%d): save failed",
                 enabled ? 1 : 0);
    }
}

void ServerState::PersistThinkingSound(bool enabled) {
    const int desired = enabled ? 1 : 0;
    if (preferences.thinking_sound == desired) return;
    preferences.thinking_sound = desired;
    if (!SavePreferences()) {
        LVA_LOGW(kTag, "PersistThinkingSound(%d): save failed", desired);
    }
}

void ServerState::PersistContinueConversationDelay(double seconds) {
    const double clamped = std::clamp(seconds, 0.0, 10.0);
    if (preferences.continue_conversation_delay.has_value() &&
        std::abs(*preferences.continue_conversation_delay - clamped) < 1e-4) {
        return;
    }
    preferences.continue_conversation_delay = clamped;
    continue_conversation_delay_ns =
        static_cast<std::int64_t>(clamped * 1e9);
    if (!SavePreferences()) {
        LVA_LOGW(kTag, "PersistContinueConversationDelay(%.3f): save failed",
                 clamped);
    }
}

void ServerState::PersistWakeWordSensitivity(int n, double value) {
    auto& slot = (n == 1) ? preferences.wake_word_1_sensitivity
                          : preferences.wake_word_2_sensitivity;
    if (slot.has_value() && std::abs(*slot - value) < 1e-4) return;
    slot = value;
    if (effects_.wake_sensitivity_changed) effects_.wake_sensitivity_changed(n, value);
    if (!SavePreferences()) {
        LVA_LOGW(kTag, "PersistWakeWordSensitivity(%d, %.4f): save failed",
                 n, value);
    }
}

void ServerState::PersistStopWordSensitivity(double value) {
    auto& slot = preferences.stop_word_sensitivity;
    if (slot.has_value() && std::abs(*slot - value) < 1e-4) return;
    slot = value;
    if (effects_.stop_sensitivity_changed) effects_.stop_sensitivity_changed(value);
    if (!SavePreferences()) {
        LVA_LOGW(kTag, "PersistStopWordSensitivity(%.4f): save failed", value);
    }
}

void ServerState::PersistMicAutoGain(int value) {
    const int clamped = std::clamp(value, 0, 31);
    if (preferences.mic_auto_gain == clamped) return;
    preferences.mic_auto_gain = clamped;
    if (effects_.mic_levels_changed)
        effects_.mic_levels_changed(clamped, preferences.mic_noise_suppression);
    if (!SavePreferences()) {
        LVA_LOGW(kTag, "PersistMicAutoGain(%d): save failed", clamped);
    }
}

void ServerState::PersistMicNoiseSuppression(int value) {
    const int clamped = std::clamp(value, 0, 4);
    if (preferences.mic_noise_suppression == clamped) return;
    preferences.mic_noise_suppression = clamped;
    if (effects_.mic_levels_changed)
        effects_.mic_levels_changed(preferences.mic_auto_gain, clamped);
    if (!SavePreferences()) {
        LVA_LOGW(kTag, "PersistMicNoiseSuppression(%d): save failed", clamped);
    }
}

void ServerState::PersistMicVolume(int value) {
    const int clamped = std::clamp(value, 1, 4000);
    if (preferences.mic_volume == clamped) return;
    preferences.mic_volume = clamped;
    mic_volume_live.store(clamped, std::memory_order_relaxed);
    if (!SavePreferences()) {
        LVA_LOGW(kTag, "PersistMicVolume(%d): save failed", clamped);
    }
}

void ServerState::PersistVoiceBackend(const std::string& backend) {
    if (preferences.voice_backend == backend) return;
    preferences.voice_backend = backend;
    if (!SavePreferences()) {
        LVA_LOGW(kTag, "PersistVoiceBackend(%s): save failed",
                 backend.c_str());
    }
}

void ServerState::PersistTtsRate(int rate_wpm) {
    if (preferences.tts_rate == rate_wpm) return;
    preferences.tts_rate = rate_wpm;
    if (!SavePreferences()) {
        LVA_LOGW(kTag, "PersistTtsRate(%d): save failed", rate_wpm);
    }
}

}  // namespace lva::state
