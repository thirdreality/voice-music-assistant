// Runs 16 kHz mono WAVs (e.g. --stt-dump-wav output) through WhisperStt.
// Usage: stt_wav_test <model.bin> <a.wav> [b.wav ...]

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "audio/ISpeechToText.h"
#include "audio/WhisperStt.h"
#include "util/Log.h"

namespace {

bool ReadWav(const char* path, std::vector<std::int16_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    unsigned char hdr[44];
    if (std::fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        std::memcmp(hdr, "RIFF", 4) != 0) {
        std::fclose(f);
        return false;
    }
    std::int16_t buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, sizeof(buf[0]), 4096, f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    std::fclose(f);
    return true;
}

long AnonKb() {
    FILE* f = std::fopen("/proc/self/smaps_rollup", "r");
    if (f == nullptr) return -1;
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::sscanf(line, "Anonymous: %ld kB", &kb) == 1) break;
    }
    std::fclose(f);
    return kb;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <model.bin> <wav>...\n", argv[0]);
        return 2;
    }
    lva::log::SetLevel(lva::log::Level::kInfo);

    lva::audio::WhisperStt::Config cfg;
    cfg.model_path = argv[1];
    auto stt = std::make_unique<lva::audio::WhisperStt>(cfg);

    const auto t0 = std::chrono::steady_clock::now();
    if (!stt->Start()) return 1;
    std::printf("[load] %.2f s anon=%ld kB\n",
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t0).count(),
                AnonKb());

    int failures = 0;
    for (int i = 2; i < argc; ++i) {
        std::vector<std::int16_t> pcm;
        if (!ReadWav(argv[i], pcm)) {
            std::fprintf(stderr, "cannot read %s\n", argv[i]);
            ++failures;
            continue;
        }
        stt->Reset();
        const auto s0 = std::chrono::steady_clock::now();
        constexpr std::size_t kChunk = 1600;
        for (std::size_t off = 0; off < pcm.size() && !stt->IsFinal();
             off += kChunk) {
            stt->AcceptWaveform(pcm.data() + off,
                                std::min(kChunk, pcm.size() - off));
        }
        if (!stt->IsFinal()) stt->InputFinished();
        std::printf("[stt] %s -> '%s' (%.2f s) anon=%ld kB\n", argv[i],
                    stt->Result().c_str(),
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - s0).count(),
                    AnonKb());
    }
    stt->Stop();
    return failures == 0 ? 0 : 1;
}
