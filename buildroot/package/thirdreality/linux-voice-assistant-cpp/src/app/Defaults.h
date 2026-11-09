#pragma once
#include <cstdint>

namespace lva::app {

inline constexpr const char* kVersion = "0.0.1";

inline constexpr double kDefaultMicroWakeSensitivity = 0.85;
inline constexpr double kDefaultOpenWakeSensitivity = 0.5;
inline constexpr double kDefaultStopSensitivity = 0.5;

// The on-device STT model is downloaded to /data instead of shipping in the
// rootfs. It is an asset of the v1.2.4 GitHub release; later firmware keeps
// this URL until the model changes. Never delete or replace that asset.
inline constexpr const char* kSttModelUrl =
    "https://github.com/thirdreality/voice-music-assistant/releases/download/"
    "v1.2.4/ggml-tiny.en-q5_1.bin";
inline constexpr std::uintmax_t kSttModelSize = 32168608;
inline constexpr const char* kSttModelSha256 =
    "6f98f0dacb0ed1ab5c1d7eed51d132ca139b5aca8c843a06926362d49e8cb0d0";
inline constexpr const char* kSttModelPath = "/data/models/stt/ggml-tiny.en-q5_1.bin";

inline constexpr const char* kVoiceBackendHa = "Home Assistant";
inline constexpr const char* kVoiceBackendLocal = "3R Speaker";

inline constexpr int kDefaultTtsRate = 165;  // words per minute = speed 1.0

inline constexpr const char* kTtsVoiceDir = "/usr/share/thirdreality/tts/amy";

inline constexpr const char* kVoiceNotifyId = "lva_voice_model";
inline constexpr const char* kVoiceNotifyTitle = "Local speech";

// Speech Rate (wpm) -> TTS speed factor.
inline float TtsSpeed(int rate_wpm) {
    return static_cast<float>(rate_wpm) / static_cast<float>(kDefaultTtsRate);
}

}  // namespace lva::app
