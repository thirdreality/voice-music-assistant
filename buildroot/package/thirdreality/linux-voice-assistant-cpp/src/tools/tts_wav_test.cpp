// Synthesizes text with SanoTts and writes 16-bit WAVs.
// Usage: tts_wav_test <threads> <out_prefix> <text> [text ...]
// Writes <out_prefix>-N.wav; prints timing and memory per utterance.
// TTS_ESPEAK_DIR / TTS_VOICE_DIR override the installed paths.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "audio/SanoTts.h"
#include "audio/WavFile.h"

namespace {

long StatusKb(const char* key) {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return -1;
    char line[256];
    long kb = -1;
    const std::size_t n = std::strlen(key);
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, key, n) == 0) kb = std::atol(line + n);
    }
    std::fclose(f);
    return kb;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <threads> <out_prefix> <text>...\n",
                     argv[0]);
        return 2;
    }
    lva::audio::SanoTts::Config cfg;
    cfg.espeak_data_dir = "/usr/share/thirdreality/tts/espeak-ng-data";
    cfg.voice_dir       = "/usr/share/thirdreality/tts/amy";
    cfg.threads         = std::atoi(argv[1]);
    if (const char* d = std::getenv("TTS_ESPEAK_DIR")) cfg.espeak_data_dir = d;
    if (const char* d = std::getenv("TTS_VOICE_DIR")) cfg.voice_dir = d;

    lva::audio::SanoTts tts(cfg);
    if (!tts.Start()) return 1;
    std::printf("started: rss=%ld kB anon=%ld kB\n", StatusKb("VmRSS:"),
                StatusKb("RssAnon:"));

    for (int i = 3; i < argc; ++i) {
        std::vector<std::int16_t> pcm;
        if (!tts.Synthesize(argv[i], pcm)) return 1;
        const std::string path =
            std::string(argv[2]) + "-" + std::to_string(i - 3) + ".wav";
        if (!lva::audio::WriteWavS16Mono(path, pcm, tts.SampleRate())) {
            return 1;
        }
        std::printf("%s: %.2f s, hwm=%ld kB anon=%ld kB\n", path.c_str(),
                    static_cast<double>(pcm.size()) / tts.SampleRate(),
                    StatusKb("VmHWM:"), StatusKb("RssAnon:"));
    }
    return 0;
}
