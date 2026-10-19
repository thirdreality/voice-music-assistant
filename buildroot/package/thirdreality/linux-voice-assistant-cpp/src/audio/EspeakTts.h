#pragma once

#include <string>

#include "audio/ITextToSpeech.h"

namespace lva::audio {

// eSpeak-NG TTS. eSpeak-NG has process-global state: one instance only, and
// all calls from the same thread.
class EspeakTts final : public ITextToSpeech {
   public:
    struct Config {
        std::string data_dir;  // espeak-ng-data
        std::string voice    = "en-us";
        int         rate_wpm = 165;
        int         pitch    = 50;
    };

    explicit EspeakTts(Config config);
    ~EspeakTts() override;

    bool Start() override;
    void Stop() override;
    bool Synthesize(const std::string& text,
                    std::vector<std::int16_t>& out_pcm) override;
    int SampleRate() const override { return sample_rate_; }

   private:
    Config config_;
    bool   started_     = false;
    int    sample_rate_ = 0;
};

}  // namespace lva::audio
