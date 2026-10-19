#include "audio/WhisperStt.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>

#include <malloc.h>

#include "util/Log.h"
#include "whisper.h"

namespace lva::audio {

namespace {

constexpr const char* kTag = "WhisperStt";
constexpr int kSampleRate  = 16000;
constexpr int kFrame       = 160;  // 10 ms

// Buffer caps for lva-whisper-memory.patch.
constexpr int kMaxAudioCtx = 750;
constexpr int kMaxTextCtx  = 128;

void OnWhisperLog(ggml_log_level level, const char* text, void*) {
    if (level == GGML_LOG_LEVEL_ERROR) {
        LVA_LOGE(kTag, "%s", text);
    } else {
        LVA_LOGD(kTag, "%s", text);
    }
}

std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\n");
    return s.substr(b, e - b + 1);
}

// Whisper tiny sometimes loops on short clips ("What time is it? What time
// is it? ..."). If the first sentence repeats, keep only the first one.
std::string CollapseRepeats(const std::string& t) {
    const auto end = t.find_first_of(".?!,");
    if (end == std::string::npos) return t;
    std::string first = Trim(t.substr(0, end + 1));
    const auto next = t.find_first_not_of(' ', end + 1);
    if (next == std::string::npos) return t;

    const std::string body = first.substr(0, first.size() - 1);
    if (body.empty() || t.compare(next, body.size(), body) != 0) return t;

    if (first.back() == ',') first.pop_back();
    return first;
}

// "[BLANK_AUDIO]", "(music)", ... are not speech.
bool IsNonSpeechAnnotation(const std::string& t) {
    if (t.empty()) return true;
    const char open = t.front();
    const char close = t.back();
    return (open == '[' && close == ']') || (open == '(' && close == ')') ||
           (open == '*' && close == '*');
}

}  // namespace

WhisperStt::WhisperStt(Config config) : config_(std::move(config)) {}

WhisperStt::~WhisperStt() { Stop(); }

bool WhisperStt::Start() {
    if (ctx_ != nullptr) return true;
    if (!std::filesystem::exists(config_.model_path)) {
        LVA_LOGE(kTag, "model missing: %s", config_.model_path.c_str());
        return false;
    }
    whisper_log_set(&OnWhisperLog, nullptr);

    // Read by whisper_init_state(); see lva-whisper-memory.patch.
    const std::string audio_cap = std::to_string(kMaxAudioCtx);
    const std::string text_cap  = std::to_string(kMaxTextCtx);
    setenv("WHISPER_MAX_AUDIO_CTX", audio_cap.c_str(), 1);
    setenv("WHISPER_MAX_TEXT_CTX", text_cap.c_str(), 1);
    setenv("WHISPER_MAX_PROMPT_TOKENS", "16", 1);

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu    = false;
    cparams.flash_attn = false;
    // mmap needs a model from align-ggml-model.py; a stock one is copied.
    ctx_ = whisper_init_from_file_mmap_no_state(config_.model_path.c_str(),
                                                cparams);
    if (ctx_ == nullptr) {
        LVA_LOGW(kTag, "model is not mmap-aligned; loading a heap copy");
        ctx_ = whisper_init_from_file_with_params_no_state(
            config_.model_path.c_str(), cparams);
    }
    if (ctx_ == nullptr) {
        LVA_LOGE(kTag, "failed to load %s", config_.model_path.c_str());
        return false;
    }
    // Fail at startup, not on the first utterance, if state allocation fails.
    whisper_state* probe = whisper_init_state(ctx_);
    if (probe == nullptr) {
        LVA_LOGE(kTag, "failed to allocate decoder state");
        whisper_free(ctx_);
        ctx_ = nullptr;
        return false;
    }
    whisper_free_state(probe);
    malloc_trim(0);
    LVA_LOGI(kTag, "started (model=%s, threads=%d)",
             config_.model_path.c_str(), config_.num_threads);
    return true;
}

void WhisperStt::Stop() {
    if (state_ != nullptr) whisper_free_state(state_);
    if (ctx_ != nullptr) whisper_free(ctx_);
    state_ = nullptr;
    ctx_   = nullptr;
    malloc_trim(0);
}

void WhisperStt::Reset() {
    audio_.clear();
    audio_.shrink_to_fit();
    frame_.clear();
    speech_start_ = 0;
    last_speech_  = 0;
    speech_ms_    = 0;
    elapsed_ms_   = 0;
    heard_speech_ = false;
    final_        = false;
    text_.clear();
}

void WhisperStt::AcceptWaveform(const std::int16_t* pcm, std::size_t n) {
    if (ctx_ == nullptr || final_) return;

    const float speech_rms = std::pow(10.0f, config_.speech_dbfs / 20.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const float s = static_cast<float>(pcm[i]) / 32768.0f;
        audio_.push_back(s);
        frame_.push_back(s);
        if (frame_.size() < static_cast<std::size_t>(kFrame)) continue;

        float energy = 0.0f;
        for (const float v : frame_) energy += v * v;
        const float rms = std::sqrt(energy / kFrame);
        frame_.clear();
        elapsed_ms_ += 10;

        const bool in_head = elapsed_ms_ <= config_.ignore_head_ms;
        if (rms >= speech_rms && !in_head) {
            if (speech_ms_ == 0) speech_start_ = audio_.size() - kFrame;
            speech_ms_ += 10;
            if (speech_ms_ >= config_.min_speech_ms) heard_speech_ = true;
            last_speech_ = audio_.size();
        } else if (!heard_speech_) {
            speech_ms_ = 0;
        }

        if (heard_speech_) {
            const auto silence_ms = static_cast<int>(
                (audio_.size() - last_speech_) * 1000 / kSampleRate);
            if (silence_ms >= config_.trailing_silence_ms) {
                Transcribe();
                return;
            }
        } else if (elapsed_ms_ >= config_.no_speech_timeout_ms) {
            LVA_LOGI(kTag, "no speech within %d ms",
                     config_.no_speech_timeout_ms);
            text_.clear();
            final_ = true;
            return;
        }
    }
}

void WhisperStt::InputFinished() {
    if (ctx_ == nullptr || final_) return;
    if (heard_speech_) {
        Transcribe();
    } else {
        text_.clear();
        final_ = true;
    }
}

void WhisperStt::Transcribe() {
    final_ = true;

    // Decode cost scales with clip length, so trim the silence around speech.
    const auto pre  = static_cast<std::size_t>(config_.pre_roll_ms) *
                      kSampleRate / 1000;
    const auto post = static_cast<std::size_t>(kSampleRate / 5);
    const std::size_t begin = speech_start_ > pre ? speech_start_ - pre : 0;
    const std::size_t end   = std::min(audio_.size(), last_speech_ + post);
    if (end <= begin) {
        text_.clear();
        return;
    }
    constexpr std::size_t kMaxClip =
        static_cast<std::size_t>(kMaxAudioCtx - 64) * kSampleRate / 50;
    const std::size_t clip_begin = (end - begin > kMaxClip) ? end - kMaxClip : begin;
    std::vector<float> clip(audio_.begin() + static_cast<long>(clip_begin),
                            audio_.begin() + static_cast<long>(end));
    // Whisper needs at least 1 s of input.
    if (clip.size() < static_cast<std::size_t>(kSampleRate)) {
        clip.resize(kSampleRate, 0.0f);
    }
    const double clip_s = static_cast<double>(clip.size()) / kSampleRate;

    whisper_full_params p =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.n_threads        = config_.num_threads;
    p.language         = "en";
    p.translate        = false;
    // Keep timestamps on: without them greedy decoding loops until
    // max_tokens.
    p.no_context       = true;
    p.print_progress   = false;
    p.print_realtime   = false;
    p.print_special    = false;
    p.print_timestamps = false;
    p.suppress_blank   = true;
    p.suppress_nst     = true;
    p.max_tokens       = std::clamp(static_cast<int>(clip_s * 6.0) + 8, 16, 64);
    p.temperature_inc  = 0.0f;   // a fallback re-decode costs ~1 s
    // Encoder window sized to the clip (50 frames/s); the full 30 s window
    // takes ~8 s on the A113X.
    p.audio_ctx = std::min(kMaxAudioCtx, static_cast<int>(clip_s * 50.0) + 64);

    const auto t0 = std::chrono::steady_clock::now();
    state_ = whisper_init_state(ctx_);
    if (state_ == nullptr) {
        LVA_LOGE(kTag, "failed to allocate decoder state");
        text_.clear();
        return;
    }
    const double init_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    const int rc = whisper_full_with_state(ctx_, state_, p, clip.data(),
                                           static_cast<int>(clip.size()));
    std::string text;
    if (rc == 0) {
        const int n = whisper_full_n_segments_from_state(state_);
        for (int i = 0; i < n; ++i) {
            text += whisper_full_get_segment_text_from_state(state_, i);
        }
    }
    whisper_free_state(state_);
    state_ = nullptr;
    clip.clear();
    clip.shrink_to_fit();
    malloc_trim(0);

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    if (rc != 0) {
        LVA_LOGE(kTag, "whisper_full failed (%d)", rc);
        text_.clear();
        return;
    }
    text = CollapseRepeats(Trim(text));
    if (IsNonSpeechAnnotation(text)) text.clear();
    text_ = std::move(text);

    LVA_LOGI(kTag, "decoded %.1f s (audio_ctx=%d) in %.0f ms "
                   "(state init %.0f ms): '%s'",
             clip_s, p.audio_ctx, ms, init_ms, text_.c_str());
}

}  // namespace lva::audio
