#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace muse {
struct SpeechAudio {
    uint32_t rate{0};
    uint8_t channels{0};
    std::vector<uint8_t> pcm;
};
class WyomingTts {
 public:
    WyomingTts(std::string host, uint16_t port, std::string voice = "");
    bool Synthesize(const std::string& text, std::atomic<bool>& cancel,
                    SpeechAudio& audio, std::string& error);
 private:
    std::string host_, voice_;
    uint16_t port_;
};
// Use the existing PulseAudio server, never open its ALSA hardware directly.
bool PlaySpeech(const SpeechAudio& audio, std::atomic<bool>& cancel, std::string& error);
}
