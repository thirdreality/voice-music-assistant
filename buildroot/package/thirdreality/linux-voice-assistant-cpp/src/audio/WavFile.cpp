#include "audio/WavFile.h"

#include <cstdio>
#include <cstring>

namespace lva::audio {

namespace {

void PutLe16(unsigned char* p, std::uint16_t v) {
    p[0] = static_cast<unsigned char>(v & 0xff);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xff);
}

void PutLe32(unsigned char* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xff);
    }
}

}  // namespace

bool WriteWavS16Mono(const std::string& path,
                     const std::vector<std::int16_t>& pcm,
                     int sample_rate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;

    const auto data_bytes =
        static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
    const auto rate = static_cast<std::uint32_t>(sample_rate);

    unsigned char h[44];
    std::memcpy(h, "RIFF", 4);
    PutLe32(h + 4, 36 + data_bytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    PutLe32(h + 16, 16);         // fmt chunk length
    PutLe16(h + 20, 1);          // PCM
    PutLe16(h + 22, 1);          // mono
    PutLe32(h + 24, rate);
    PutLe32(h + 28, rate * 2);   // byte rate
    PutLe16(h + 32, 2);          // block align
    PutLe16(h + 34, 16);         // bits per sample
    std::memcpy(h + 36, "data", 4);
    PutLe32(h + 40, data_bytes);

    bool ok = std::fwrite(h, 1, sizeof(h), f) == sizeof(h);
    if (ok && !pcm.empty()) {
        ok = std::fwrite(pcm.data(), sizeof(std::int16_t), pcm.size(), f) ==
             pcm.size();
    }
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

}  // namespace lva::audio
