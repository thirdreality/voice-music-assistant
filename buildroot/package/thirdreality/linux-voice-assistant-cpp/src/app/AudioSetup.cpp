#include <algorithm>
#include <array>
#include <filesystem>

#include "app/Application.h"
#include "audio/AudioCapture.h"
#include "audio/LibMpvPlayer.h"
#include "audio/PcmRingBuffer.h"
#include "audio/WebRtcProcessor.h"
#include "tr/MicMuteGpio.h"
#include "tr/ModelDownloader.h"
#include "util/Log.h"

namespace lva::app {
namespace {
constexpr const char* kTag = "main";
}

void Application::PrepareSpeechModel() {
    // The backend is fixed for the life of the process; switching it from
    // HA restarts LVA. The STT tap must be registered before capture starts.
    const std::string wanted_backend = state.preferences.voice_backend.value_or(cli.voice_backend);
    const bool stt_model_is_default = (cli.stt_model == kSttModelPath);
    if (stt_model_is_default) {
        stt_model_downloader = std::make_unique<lva::tr::ModelDownloader>(
            lva::tr::ModelSpec{
                .url = cli.stt_model_url,
                .path = kSttModelPath,
                .size = kSttModelSize,
                .sha256 = kSttModelSha256,
            },
            state.network_policy);
    }
    // Restoring a preference is an automatic request. HA-only users never
    // prefetch a speech model; a new explicit selection can download once.
    if (wanted_backend == "local" && stt_model_downloader) {
        stt_model_downloader->Start();
    }
    use_local_voice = (wanted_backend == "local");
    if (use_local_voice && !SttModelReady()) {
        LVA_LOGW(kTag,
                 "local STT model %s not found; using ha voice "
                 "backend%s",
                 cli.stt_model.c_str(), stt_model_downloader ? " until it is downloaded" : "");
        use_local_voice = false;
    }
    local_stt_ring =
        use_local_voice ? std::make_unique<lva::audio::PcmRingBuffer>(16'000 * 4) : nullptr;
}

bool Application::SttModelReady() const {
    return stt_model_downloader ? stt_model_downloader->Installed()
                                : std::filesystem::exists(cli.stt_model);
}

bool Application::SetupAudio() {
    mic_mute_gpio = std::make_unique<lva::tr::MicMuteGpio>(state);
    if (mic_mute_gpio->Available()) {
        mic_mute_gpio->ReadAndApplyOnce();
    }

    lva::audio::LibMpvPlayer::Options mpv_opts;
    mpv_opts.audio_device = cli.audio_device;
    mpv_opts.cache_secs = 10;
    mpv_opts.short_sound_safe = false;
    music_player = std::make_unique<lva::audio::LibMpvPlayer>(mpv_opts);

    lva::audio::LibMpvPlayer::Options tts_opts;
    tts_opts.audio_device = cli.audio_device;
    tts_opts.cache_secs = 1;
    tts_opts.short_sound_safe = true;
    announce_player = std::make_unique<lva::audio::LibMpvPlayer>(tts_opts);

    wake_ring = std::make_unique<lva::audio::PcmRingBuffer>(16'000 * 2);

    // Shadow detector input: WebRTC linear-AEC output.
    linear_wake_ring = std::make_unique<lva::audio::PcmRingBuffer>(16'000 * 2);

    satellite_ring = std::make_unique<lva::audio::PcmRingBuffer>(16'000 * 2);

    PrepareSpeechModel();

    lva::audio::AudioCapture::Options cap_opts;
    const bool use_alsa_backend = (cli.capture_backend == "alsa");
    if (use_alsa_backend) {
        cap_opts.backend = lva::audio::AudioCapture::Backend::kAlsa;
        cap_opts.alsa_device = cli.capture_alsa_device;
        cap_opts.alsa_channels = 4;
        cap_opts.mic_channel = static_cast<unsigned>(std::max(0, cli.capture_mic_channel));
        cap_opts.frames_per_read = 160;  // one WebRTC frame
        // "a,b" / "a" / "" / "none"
        std::array<int, 2> refs = {-1, -1};
        if (!cli.capture_ref_channels.empty() && cli.capture_ref_channels != "none") {
            std::size_t comma = cli.capture_ref_channels.find(',');
            try {
                refs[0] = std::stoi(cli.capture_ref_channels.substr(0, comma));
                if (comma != std::string::npos) {
                    refs[1] = std::stoi(cli.capture_ref_channels.substr(comma + 1));
                }
            } catch (...) {
                LVA_LOGW(kTag,
                         "could not parse --capture-ref-channels '%s'; "
                         "AEC disabled",
                         cli.capture_ref_channels.c_str());
                refs = {-1, -1};
            }
        }
        cap_opts.ref_channels = refs;
    } else if (cli.capture_backend == "pulse") {
        cap_opts.backend = lva::audio::AudioCapture::Backend::kPulse;
        cap_opts.frames_per_read = 1600;
        cap_opts.buffer_latency_us = 100'000;
    } else {
        LVA_LOGE(kTag, "unknown --capture-backend '%s'; expected alsa|pulse",
                 cli.capture_backend.c_str());
        return false;
    }

    audio_capture = std::make_unique<lva::audio::AudioCapture>(cap_opts, *satellite_ring);
    audio_capture->AddWakeTap(*wake_ring);
    audio_capture->AddLinearWakeTap(*linear_wake_ring);
    if (local_stt_ring) audio_capture->AddTap(*local_stt_ring);

    // Set when a first-boot default is filled in, so sound.json is saved
    // once with every setting.
    const bool mic_defaults_applied = state.preferences.mic_defaults_applied();
    prefs_dirty = preferences_corrupt || mic_defaults_applied;
    if (mic_defaults_applied) {
        LVA_LOGI(kTag, "applied first-boot mic defaults: agc=%d, ns=%d, vol=%d",
                 state.preferences.mic_auto_gain, state.preferences.mic_noise_suppression,
                 state.preferences.mic_volume);
    }

    const bool aec_enabled = use_alsa_backend && (cap_opts.ref_channels[0] >= 0);
    LVA_LOGI(kTag, "capture backend=%s aec=%s", use_alsa_backend ? "alsa" : "pulse",
             aec_enabled ? "enabled" : "disabled");

    webrtc_processor = std::make_unique<lva::audio::WebRtcProcessor>(
        state.preferences.mic_auto_gain, state.preferences.mic_noise_suppression, aec_enabled);
    audio_capture->SetProcessor(webrtc_processor.get());
    audio_capture->SetMicVolumeSource(&state.mic_volume_live);

    return true;
}

}  // namespace lva::app
