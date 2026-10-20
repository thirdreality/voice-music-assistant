#include "audio/EspeakTts.h"

#include <filesystem>

#include "espeak-ng/speak_lib.h"
#include "util/Log.h"

namespace lva::audio {

namespace {

constexpr const char* kTag = "EspeakTts";

int OnSynth(short* wav, int num_samples, espeak_EVENT* events) {
    if (wav == nullptr || num_samples <= 0 || events == nullptr) return 0;
    auto* out = static_cast<std::vector<std::int16_t>*>(events->user_data);
    if (out != nullptr) {
        out->insert(out->end(), wav, wav + num_samples);
    }
    return 0;
}

}  // namespace

EspeakTts::EspeakTts(Config config) : config_(std::move(config)) {}

EspeakTts::~EspeakTts() { Stop(); }

bool EspeakTts::Start() {
    if (started_) return true;

    if (!std::filesystem::exists(
            std::filesystem::path(config_.data_dir) / "phontab")) {
        LVA_LOGE(kTag, "espeak-ng data missing under %s",
                 config_.data_dir.c_str());
        return false;
    }

    // Without DONT_EXIT a bad data dir calls exit(1).
    const int rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0,
                                       config_.data_dir.c_str(),
                                       espeakINITIALIZE_DONT_EXIT);
    if (rate <= 0) {
        LVA_LOGE(kTag, "espeak_Initialize failed (data=%s)",
                 config_.data_dir.c_str());
        return false;
    }
    espeak_SetSynthCallback(&OnSynth);

    if (espeak_SetVoiceByName(config_.voice.c_str()) != EE_OK) {
        LVA_LOGE(kTag, "voice '%s' not found", config_.voice.c_str());
        espeak_Terminate();
        return false;
    }
    espeak_SetParameter(espeakRATE, config_.rate_wpm, 0);
    espeak_SetParameter(espeakPITCH, config_.pitch, 0);

    sample_rate_ = rate;
    started_     = true;
    LVA_LOGI(kTag, "started (voice=%s, rate=%d Hz, wpm=%d)",
             config_.voice.c_str(), sample_rate_, config_.rate_wpm);
    return true;
}

void EspeakTts::Stop() {
    if (!started_) return;
    espeak_Terminate();
    started_     = false;
    sample_rate_ = 0;
    LVA_LOGI(kTag, "stopped");
}

bool EspeakTts::SetVoice(const std::string& voice, int rate_wpm) {
    if (started_ && espeak_SetVoiceByName(voice.c_str()) != EE_OK) {
        LVA_LOGW(kTag, "voice '%s' not found; keeping '%s'", voice.c_str(),
                 config_.voice.c_str());
        return false;
    }
    config_.voice    = voice;
    config_.rate_wpm = rate_wpm;
    if (started_) {
        espeak_SetParameter(espeakRATE, config_.rate_wpm, 0);
        espeak_SetParameter(espeakPITCH, config_.pitch, 0);
        LVA_LOGI(kTag, "voice=%s wpm=%d", config_.voice.c_str(),
                 config_.rate_wpm);
    }
    return true;
}

bool EspeakTts::Synthesize(const std::string& text,
                           std::vector<std::int16_t>& out_pcm) {
    out_pcm.clear();
    if (!started_ || text.empty()) return false;

    const espeak_ERROR rc =
        espeak_Synth(text.c_str(), text.size() + 1, 0, POS_CHARACTER, 0,
                     espeakCHARS_UTF8, nullptr, &out_pcm);
    if (rc != EE_OK) {
        LVA_LOGE(kTag, "espeak_Synth failed (%d)", static_cast<int>(rc));
        out_pcm.clear();
        return false;
    }
    espeak_Synchronize();

    if (out_pcm.empty()) {
        LVA_LOGW(kTag, "synthesis produced no audio");
        return false;
    }
    return true;
}

}  // namespace lva::audio
