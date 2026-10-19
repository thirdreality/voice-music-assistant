
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lva::audio {

// UTF-8 text -> 16-bit mono PCM at SampleRate().
class ITextToSpeech {
   public:
    virtual ~ITextToSpeech() = default;

    virtual bool Start() = 0;

    virtual void Stop() = 0;

    // out_pcm is cleared on failure.
    virtual bool Synthesize(const std::string& text,
                            std::vector<std::int16_t>& out_pcm) = 0;

    virtual int SampleRate() const = 0;
};

}  // namespace lva::audio
