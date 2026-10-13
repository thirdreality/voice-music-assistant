#include "AudioCapture.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <cerrno>
namespace muse {
void SelectMicrophoneChannel(const int16_t* interleaved, size_t frames, int16_t* mono) {
    for (size_t i = 0; i < frames; ++i) mono[i] = interleaved[i * 4];
}

AudioCapture::AudioCapture(std::string device) : device_(std::move(device)) {}
AudioCapture::~AudioCapture() { Stop(); }
bool AudioCapture::Start(Callback callback) {
    if (active_) return false;
    // A callback can stop capture before the key is released. Reap that worker
    // and close its PCM handle before assigning another thread.
    Stop();
    int rc = snd_pcm_open(&pcm_, device_.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        std::fprintf(stderr, "[muse] AudioCapture open(%s) failed: %s\n", device_.c_str(), snd_strerror(rc));
        return false;
    }
    // Match the existing Linux voice capture: four interleaved channels on
    // LOOPBACK-A, with mic0 selected below. Mono hardware configuration treats
    // the four-channel DMA data as mono and yields four times the sample count.
    // Explicit parameters and a 500 ms buffer avoid this driver's -EIO path.
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_uframes_t period = 160, buffer = 8000;
    unsigned int rate = 16000;
    if ((rc = snd_pcm_hw_params_any(pcm_, hw)) < 0 ||
        (rc = snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (rc = snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (rc = snd_pcm_hw_params_set_channels(pcm_, hw, 4)) < 0 ||
        (rc = snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr)) < 0 ||
        (rc = snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, nullptr)) < 0 ||
        (rc = snd_pcm_hw_params_set_buffer_size_near(pcm_, hw, &buffer)) < 0 ||
        (rc = snd_pcm_hw_params(pcm_, hw)) < 0) {
        std::fprintf(stderr, "[muse] AudioCapture hw_params failed: %s\n", snd_strerror(rc));
        snd_pcm_close(pcm_); pcm_ = nullptr; return false;
    }
    if (rate != 16000) {
        std::fprintf(stderr, "[muse] AudioCapture requires 16000 Hz, got %u\n", rate);
        snd_pcm_close(pcm_); pcm_ = nullptr; return false;
    }
    std::fprintf(stderr, "[muse] AudioCapture params rate=%u channels=4 mic_channel=0 period=%lu buffer=%lu\n",
                 rate, (unsigned long)period, (unsigned long)buffer);
    rc = snd_pcm_prepare(pcm_);
    if (rc < 0) {
        std::fprintf(stderr, "[muse] AudioCapture prepare failed: %s\n", snd_strerror(rc));
        snd_pcm_close(pcm_); pcm_ = nullptr; return false;
    }
    active_ = true;
    thread_ = std::thread(&AudioCapture::Run, this, std::move(callback));
    return true;
}
void AudioCapture::Stop() {
    active_ = false;
    if (pcm_) snd_pcm_abort(pcm_);
    if (thread_.joinable()) thread_.join();
    if (pcm_) { snd_pcm_close(pcm_); pcm_ = nullptr; }
}
void AudioCapture::Run(Callback callback) {
    std::array<int16_t, 320 * 4> interleaved{};
    std::array<int16_t, 320> frames{};
    bool logged_first = false;
    int recover_fails = 0;
    while (active_) {
        const snd_pcm_sframes_t n = snd_pcm_readi(pcm_, interleaved.data(), frames.size());
        if (n == -EPIPE || n == -EIO || n == -ESTRPIPE) {
            if (++recover_fails > 50) { std::fprintf(stderr, "[muse] AudioCapture: too many recoveries, giving up\n"); break; }
            const int rc = snd_pcm_recover(pcm_, (int)n, 1);
            if (rc < 0) { std::fprintf(stderr, "[muse] AudioCapture recover failed: %s\n", snd_strerror(rc)); break; }
            snd_pcm_start(pcm_);
            continue;
        }
        if (n < 0) { std::fprintf(stderr, "[muse] AudioCapture readi failed: %s\n", snd_strerror((int)n)); break; }
        if (!logged_first) { std::fprintf(stderr, "[muse] AudioCapture first read %ld frames\n", (long)n); logged_first = true; }
        SelectMicrophoneChannel(interleaved.data(), static_cast<size_t>(n), frames.data());
        if (!callback(frames.data(), static_cast<size_t>(n))) break;
    }
    active_ = false;
}
}  // namespace muse
