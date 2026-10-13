#pragma once
#include <cstdint>
#include <vector>
namespace muse {
std::vector<uint8_t> StreamingWavHeader(uint32_t sample_rate);
}
