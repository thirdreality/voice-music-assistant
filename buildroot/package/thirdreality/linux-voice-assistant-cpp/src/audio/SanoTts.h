#pragma once

#include <memory>
#include <string>

#include "audio/ITextToSpeech.h"

namespace lva::audio {

// Neural TTS: eSpeak-NG text -> Piper phoneme ids, then the sanoTTS
// piperlite "amy" voice (duration + acoustic + decoder, third_party/sanotts)
// -> 22.05 kHz PCM. The voice files (fp32 front, int8 decoder) are mmapped
// and used in place. The decoder runs in fixed-size chunks so peak memory
// does not grow with the reply length.
//
// eSpeak-NG has process-global state: one instance only, all calls from the
// same thread.
class SanoTts final : public ITextToSpeech {
   public:
    struct Config {
        std::string espeak_data_dir;  // espeak-ng-data (G2P only)
        std::string voice_dir;        // front_f32.bin, dec_q8.bin
        float       speed   = 1.0f;   // >1 faster
        int         threads = 2;      // decoder threads, 1-4
    };

    explicit SanoTts(Config config);
    ~SanoTts() override;

    bool Start() override;
    void Stop() override;
    bool Synthesize(const std::string& text,
                    std::vector<std::int16_t>& out_pcm) override;
    int SampleRate() const override;

    // Takes effect from the next Synthesize(). Same thread as Synthesize().
    void SetSpeed(float speed);

   private:
    struct Impl;
    Config                config_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lva::audio
