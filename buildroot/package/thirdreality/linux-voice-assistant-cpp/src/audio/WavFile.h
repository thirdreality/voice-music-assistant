#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lva::audio {

// 16-bit mono PCM with a 44-byte header.
bool WriteWavS16Mono(const std::string& path,
                     const std::vector<std::int16_t>& pcm,
                     int sample_rate);

}  // namespace lva::audio
