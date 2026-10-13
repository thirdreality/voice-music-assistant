#include "WavWriter.h"
#include <algorithm>
namespace muse {
namespace {
void PutLe(uint8_t* out, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}
}
std::vector<uint8_t> StreamingWavHeader(uint32_t rate) {
    std::vector<uint8_t> h(44);
    std::copy_n(reinterpret_cast<const uint8_t*>("RIFF"), 4, h.begin());
    PutLe(h.data() + 4, UINT32_MAX, 4);
    std::copy_n(reinterpret_cast<const uint8_t*>("WAVEfmt "), 8, h.begin() + 8);
    PutLe(h.data() + 16, 16, 4); PutLe(h.data() + 20, 1, 2);
    PutLe(h.data() + 22, 1, 2); PutLe(h.data() + 24, rate, 4);
    PutLe(h.data() + 28, rate * 2, 4); PutLe(h.data() + 32, 2, 2);
    PutLe(h.data() + 34, 16, 2);
    std::copy_n(reinterpret_cast<const uint8_t*>("data"), 4, h.begin() + 36);
    PutLe(h.data() + 40, UINT32_MAX, 4);
    return h;
}
}  // namespace muse
