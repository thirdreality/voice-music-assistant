#pragma once

#include <cstdint>
#include <string>

namespace lva::audio {

// Speech-to-text engine fed 16 kHz mono int16 PCM. Inference runs on the
// caller's thread.
class ISpeechToText {
   public:
    virtual ~ISpeechToText() = default;

    virtual bool Start() = 0;
    virtual void Stop() = 0;  // safe when already stopped

    virtual void AcceptWaveform(const std::int16_t* pcm, std::size_t n) = 0;
    virtual void InputFinished() = 0;

    virtual std::string Result() const = 0;  // UTF-8
    virtual bool IsFinal() const = 0;

    // Prepare for the next utterance; the model stays loaded.
    virtual void Reset() = 0;
};

}  // namespace lva::audio
