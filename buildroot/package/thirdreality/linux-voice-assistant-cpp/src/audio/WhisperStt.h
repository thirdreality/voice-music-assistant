#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "audio/ISpeechToText.h"

struct whisper_context;
struct whisper_state;

namespace lva::audio {

// whisper.cpp STT. Not streaming: audio is buffered behind an energy VAD and
// the utterance is decoded in one pass once speech is followed by silence.
// The decoder state is allocated only for the duration of each decode.
class WhisperStt final : public ISpeechToText {
   public:
    struct Config {
        std::string model_path;
        int         num_threads = 4;

        // Energy VAD on 10 ms frames.
        float speech_dbfs          = -55.0f;
        int   min_speech_ms        = 150;
        int   trailing_silence_ms  = 800;
        int   no_speech_timeout_ms = 5000;
        int   pre_roll_ms          = 300;
        // The wake chime tail is ~-20 dBFS here and must not start speech.
        int   ignore_head_ms       = 600;
    };

    explicit WhisperStt(Config config);
    ~WhisperStt() override;

    bool Start() override;
    void Stop() override;
    void AcceptWaveform(const std::int16_t* pcm, std::size_t n) override;
    void InputFinished() override;
    std::string Result() const override { return text_; }
    bool IsFinal() const override { return final_; }
    void Reset() override;

   private:
    void Transcribe();

    Config           config_;
    whisper_context* ctx_   = nullptr;
    whisper_state*   state_ = nullptr;

    std::vector<float> audio_;
    std::vector<float> frame_;
    std::size_t speech_start_  = 0;
    std::size_t last_speech_   = 0;
    int         speech_ms_     = 0;
    int         elapsed_ms_    = 0;
    bool        heard_speech_  = false;
    bool        final_         = false;
    std::string text_;
};

}  // namespace lva::audio
