
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lva::state {

class Preferences {
   public:
    enum class Format {
        kPreferences,  // legacy plain JSON; volume normalized 0..1
        kSound,        // /data/conf/sound.json; volume 0..100; extra keys
    };

    std::vector<std::optional<std::string>> active_wake_words;

    std::optional<double> volume;

    // 0 = disabled, 1 = enabled. Anything outside is treated as 0.
    int thinking_sound = 0;

    // Seconds; nullopt = CLI default.
    std::optional<double> continue_conversation_delay;

    std::optional<double> wake_word_1_sensitivity;
    std::optional<double> wake_word_2_sensitivity;
    std::optional<double> stop_word_sensitivity;

    // WebRTC AGC / NS levels, 0..N. 0 = off.
    int mic_auto_gain         = 0;
    int mic_noise_suppression = 0;

    int mic_volume = 1600;

    // "ha" or "local"; nullopt = CLI default.
    std::optional<std::string> voice_backend;

    // Local TTS speed in words per minute; nullopt = built-in default.
    std::optional<int>         tts_rate;

    // sound.json-only fields. Round-tripped verbatim.
    int mic_gain = 30;   // analog gain knob, 0..100, hardware-defined
    int mic_mute = 1;    // 0 = muted, 1 = unmuted (matches GPIO 438)

    Format format() const noexcept { return format_; }
    bool is_mic_muted() const noexcept { return mic_mute == 0; }
    void set_mic_muted(bool muted) noexcept { mic_mute = muted ? 0 : 1; }

    static Preferences ForPath(const std::filesystem::path& storage_path);

    static Preferences LoadFromFile(const std::filesystem::path& storage_path);

    bool SaveToFile(const std::filesystem::path& storage_path) const;

   private:
    Format format_ = Format::kPreferences;
    std::string raw_data_text_;
};

}  // namespace lva::state
