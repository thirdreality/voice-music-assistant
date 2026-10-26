#include "satellite/Satellite.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include <malloc.h>
#include <optional>
#include <utility>
#include <vector>

#include "audio/ExternalWakeWord.h"
#include "audio/IAudioPlayer.h"
#include "audio/ITextToSpeech.h"
#include "audio/MicroWakeWord.h"
#include "audio/OpenWakeWord.h"
#include "audio/PcmRingBuffer.h"
#include "audio/WakeWordEngine.h"
#include "audio/WakeWordScanner.h"
#include "audio/WavFile.h"
#include "protocol/MessageRegistry.h"
#include "satellite/ConversationReply.h"
#include "satellite/LocalStt.h"
#include "state/ServerState.h"
#include "tr/LedRing.h"
#include "sendspin/SendspinHub.h"
#include "util/Log.h"

#include "api.pb.h"

namespace lva::satellite {

namespace {

constexpr const char* kTag = "satellite";

constexpr std::size_t kAudioPumpSamples = 1024;

constexpr double kDuckFactor = 0.2;

constexpr int kMaxPumpsPerTick = 4;

constexpr int kEvtError          = 0;
constexpr int kEvtRunStart       = 1;
constexpr int kEvtRunEnd         = 2;
constexpr int kEvtSttEnd         = 4;
constexpr int kEvtIntentStart    = 5;
constexpr int kEvtIntentEnd      = 6;
constexpr int kEvtSttVadEnd      = 12;
constexpr int kEvtIntentProgress = 100;
constexpr int kEvtTtsEnd         = 8;

constexpr const char* kThinkingSoundPath =
    "/usr/share/thirdreality/sounds/processing.wav";

constexpr const char* kWakeupSoundPath =
    "/usr/share/thirdreality/sounds/wake_word_triggered.flac";

constexpr const char* kTimerFinishedSoundPath =
    "/usr/share/thirdreality/sounds/timer_finished.flac";

constexpr int kTimerMaxRingSeconds = 900;

constexpr const char* kTtsDir = "/tmp/lva-tts";
// LLM conversation agents can take several seconds.
constexpr auto kIntentTimeout = std::chrono::seconds(15);

// HA sends RUN_START right after VoiceAssistantRequest(start=true). If it
// does not, the run was never created and the mic would stream forever.
constexpr auto kHaStartTimeout = std::chrono::seconds(8);
// Longest gap between HA voice events (e.g. a slow LLM intent).
constexpr auto kHaEventTimeout = std::chrono::seconds(60);

constexpr const char* kLocalSttUnavailable =
    "Local speech recognition is still loading.";
constexpr const char* kLocalNoConnection =
    "I am not connected to Home Assistant.";
constexpr const char* kLocalIntentFailed =
    "Sorry, something went wrong.";


}  // namespace

Satellite::Satellite(lva::state::ServerState& state,
                     lva::audio::PcmRingBuffer& mic_ring,
                     lva::audio::WakeWordEngine& engine,
                     lva::audio::IAudioPlayer* announce_player)
    : state_(state), mic_ring_(mic_ring), engine_(engine),
      announce_player_(announce_player) {}

Satellite::~Satellite() = default;

void Satellite::OnWakeDetected(const std::string& model_id, float prob) {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now_ns < refractory_until_ns_.load(std::memory_order_relaxed)) {
        return;  // fast-path drop on ww-engine thread
    }
    pending_wake_data_.model_id = model_id;
    pending_wake_data_.prob     = prob;
    pending_wake_.store(true, std::memory_order_release);
}

void Satellite::OnStopDetected() {
    // Wake-word thread: only post the event; OnLoopTick handles it.
    pending_stop_.store(true, std::memory_order_release);
}

void Satellite::ProcessStopDetected() {
    if (!pipeline_active_ && !timer_ringing_) return;

    // Ignore stop words during the wake chime, except to stop a timer.
    if (!timer_ringing_) {
        const auto now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now()
                    .time_since_epoch()).count();

        const auto guard_until_ns =
            stop_guard_until_ns_.load(
                std::memory_order_relaxed);

        if (guard_until_ns != 0 &&
            now_ns < guard_until_ns) {
            const auto remaining_ms =
                (guard_until_ns - now_ns) / 1'000'000LL;

            LVA_LOGD(
                kTag,
                "stop-word guard suppressed detection "
                "(remaining_ms=%lld)",
                static_cast<long long>(remaining_ms));
            return;
        }
    }

    if (timer_ringing_) {
        LVA_LOGI(kTag, "stop word → stopping timer ring");
        StopTimerRing();
    } else if (announce_player_) {
        LVA_LOGI(kTag, "stop word → stopping TTS");
        continue_conversation_ = false;
        continue_listen_at_ns_.store(0, std::memory_order_relaxed);
        announce_player_->Stop();
        if (speech_active_) {
            FinishSpeech(false);
        } else {
            wake_sound_pending_ = false;
            CancelLocalTurn();
            pipeline_active_ = false;
            Unduck();
            lva::tr::Show(lva::tr::LedState::Idle);
        }
    }
}

void Satellite::OnTapPressed() {
    tap_started_during_speech_.store(speech_active_,
                                     std::memory_order_release);
    tap_down_pending_.store(true, std::memory_order_release);
}

bool Satellite::AbortSpeechIfSpeaking() {
    const bool tap_down_seen =
        tap_down_pending_.exchange(false, std::memory_order_acq_rel);
    const bool started_during_speech = tap_down_seen
        ? tap_started_during_speech_.exchange(false,
                                               std::memory_order_acq_rel)
        : speech_active_.load(std::memory_order_acquire);
    if (!started_during_speech) {
        return false;
    }
    if (!speech_active_) {
        continue_conversation_ = false;
        continue_listen_at_ns_.store(0, std::memory_order_relaxed);
        CancelLocalTurn();
        if (is_streaming_audio_) {
            StopAudioStreaming();
            if (state_.broadcast) {
                ::VoiceAssistantRequest req;
                req.set_start(false);
                state_.broadcast(lva::proto::kIdVoiceAssistantRequest, req);
            }
        }
        pipeline_active_ = false;
        Unduck();
        lva::tr::Show(lva::tr::LedState::Idle);
        return true;
    }
    if (announce_player_ == nullptr) return true;

    LVA_LOGI(kTag, "tap during speech → stopping speech");
    continue_conversation_ = false;
    continue_listen_at_ns_.store(0, std::memory_order_relaxed);
    announce_player_->Stop();
    FinishSpeech(false);
    return true;
}

void Satellite::PrepareForMediaAnnouncement() {
    if (timer_ringing_) {
        StopTimerRing();
    }
    if (wake_sound_pending_.exchange(false, std::memory_order_acq_rel)) {
        if (announce_player_ != nullptr) announce_player_->Stop();
        CancelLocalTurn();
        pipeline_active_ = false;
        Unduck();
        lva::tr::Show(lva::tr::LedState::Idle);
    }
    if (!speech_active_ || announce_player_ == nullptr) return;

    LVA_LOGI(kTag, "media announcement replacing speech");
    continue_conversation_ = false;
    continue_listen_at_ns_.store(0, std::memory_order_relaxed);
    announce_player_->Stop();
    FinishSpeech(false);
}

bool Satellite::OwnsAnnouncePlayback() const {
    return pipeline_active_.load(std::memory_order_acquire) || timer_ringing_;
}

void Satellite::StartPipeline(const std::string& wake_word_phrase) {
    const auto now = std::chrono::steady_clock::now();
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    const bool barge_in = pipeline_active_ && speech_active_ &&
                          announce_player_ != nullptr;
    if (!barge_in &&
        now_ns < refractory_until_ns_.load(std::memory_order_relaxed)) {
        const auto remaining_ms =
            (refractory_until_ns_.load(std::memory_order_relaxed) - now_ns)
            / 1'000'000;
        LVA_LOGD(kTag, "wake suppressed (refractory %lld ms remaining)",
                 static_cast<long long>(remaining_ms));
        return;
    }
    if (pipeline_active_) {
        if (barge_in) {
            LVA_LOGI(kTag,
                     "wake during speech -> stopping speech and restarting");
            continue_conversation_ = false;
            continue_listen_at_ns_.store(0, std::memory_order_relaxed);
            announce_player_->Stop();
            FinishSpeech(false);
        } else {
            LVA_LOGD(kTag, "wake during active pipeline; ignoring");
            return;
        }
    }
    if (timer_ringing_) {
        LVA_LOGI(kTag, "wake during timer ring — stopping ring");
        StopTimerRing();
        refractory_until_ns_.store(now_ns + 2'000'000'000LL,
                                   std::memory_order_relaxed);
        return;
    }
    LVA_LOGI(kTag, "starting voice pipeline (wake='%s')",
             wake_word_phrase.c_str());

    refractory_until_ns_.store(now_ns + 2'000'000'000LL,
                               std::memory_order_relaxed);

    constexpr std::int64_t kStopGuardNs =
        1'000'000'000LL;

    stop_guard_until_ns_.store(
        now_ns + kStopGuardNs,
        std::memory_order_relaxed);

    LVA_LOGD(
        kTag,
        "stop-word guard armed: 1000 ms");

    pipeline_active_       = true;
    is_streaming_audio_    = false;  // not yet — wakeup sound first
    DisarmHaWatchdog();
    tts_url_.clear();
    tts_played_            = false;
    continue_conversation_ = false;
    speech_active_         = false;

    lva::tr::Show(lva::tr::LedState::Listening);
    Duck();

    if (announce_player_ != nullptr) {
        const std::string phrase_copy = wake_word_phrase;
        wake_sound_pending_ = true;
        announce_player_->Play(kWakeupSoundPath, [this, phrase_copy] {
            if (!wake_sound_pending_.exchange(false,
                                               std::memory_order_acq_rel)) {
                return;
            }
            OpenStreamToHa(phrase_copy);
        });
    } else {
        OpenStreamToHa(wake_word_phrase);
    }
}

void Satellite::OpenStreamToHa(const std::string& wake_word_phrase) {
    if (!pipeline_active_) {
        LVA_LOGD(kTag, "OpenStreamToHa: pipeline no longer active; skip");
        return;
    }
    if (LocalVoiceEnabled()) {
        BeginLocalListening();
        return;
    }
    is_streaming_audio_ = true;

    {
        std::int16_t scratch[1024];
        std::size_t total_drained = 0;
        while (true) {
            const std::size_t got = mic_ring_.Read(scratch,
                                                   sizeof(scratch) /
                                                   sizeof(scratch[0]));
            if (got == 0) break;
            total_drained += got;
        }
        if (total_drained > 0) {
            LVA_LOGD(kTag, "drained %zu samples (%.0f ms) before stream",
                     total_drained,
                     total_drained * 1000.0 / 16000.0);
        }
    }

    if (!state_.broadcast) {
        LVA_LOGW(kTag, "no broadcast hook; can't notify HA");
        return;
    }
    ::VoiceAssistantRequest req;
    req.set_start(true);
    req.set_wake_word_phrase(wake_word_phrase);
    state_.broadcast(lva::proto::kIdVoiceAssistantRequest, req);
    ArmHaWatchdog(kHaStartTimeout);
}

void Satellite::PumpAudioToHa() {
    std::vector<std::int16_t> chunk(kAudioPumpSamples);

    // Drain while idle, so an overrun only means streaming fell behind.
    if (!is_streaming_audio_ || !state_.broadcast) {
        while (mic_ring_.Read(chunk.data(), chunk.size()) > 0) {
        }
        return;
    }

    for (int i = 0; i < kMaxPumpsPerTick; ++i) {
        const std::size_t got =
            mic_ring_.Read(chunk.data(), chunk.size());
        if (got == 0) break;

        ::VoiceAssistantAudio audio_msg;
        audio_msg.set_data(reinterpret_cast<const char*>(chunk.data()),
                           got * sizeof(std::int16_t));
        state_.broadcast(lva::proto::kIdVoiceAssistantAudio, audio_msg);
    }
}

void Satellite::StopAudioStreaming() {
    if (!is_streaming_audio_) return;
    is_streaming_audio_ = false;
    LVA_LOGD(kTag, "stopped streaming audio to HA");
}

void Satellite::BeginListening() {
    if (LocalVoiceEnabled()) {
        pipeline_active_ = true;
        BeginLocalListening();
        return;
    }
    // Drain stale mic audio captured during TTS playback before re-streaming.
    {
        std::int16_t scratch[1024];
        while (mic_ring_.Read(scratch, 1024) > 0) {}
    }
    lva::tr::Show(lva::tr::LedState::Listening);
    is_streaming_audio_ = true;
    pipeline_active_    = true;
    if (state_.broadcast) {
        ::VoiceAssistantRequest req;
        req.set_start(true);
        state_.broadcast(lva::proto::kIdVoiceAssistantRequest, req);
    }
    ArmHaWatchdog(kHaStartTimeout);
}

void Satellite::PlayTts() {
    if (tts_url_.empty()) {
        LVA_LOGW(kTag, "PlayTts called with empty url");
        return;
    }
    if (tts_played_) {
        LVA_LOGD(kTag, "PlayTts: already played; skip");
        return;
    }
    if (announce_player_ == nullptr) {
        LVA_LOGW(kTag, "PlayTts: no announce player");
        return;
    }

    tts_played_ = true;
    DisarmHaWatchdog();  // FinishSpeech ends the turn from here
    LVA_LOGI(kTag, "playing TTS: %s", tts_url_.c_str());
    lva::tr::Show(lva::tr::LedState::Speaking);

    if (timer_ringing_) StopTimerRing();
    speech_active_ = true;
    announce_player_->Play(tts_url_, [this]() {
        FinishSpeech(false);
    });
}

void Satellite::FinishSpeech(bool success) {
    if (!speech_active_.exchange(false, std::memory_order_acq_rel)) return;

    const bool was_local = std::exchange(local_speech_, false);
    if (state_.broadcast && !was_local) {
        ::VoiceAssistantAnnounceFinished done;
        done.set_success(success);
        state_.broadcast(lva::proto::kIdVoiceAssistantAnnounceFinished, done);
    }
    if (continue_conversation_) {
        LVA_LOGI(kTag, "speech done — continuing conversation after %lld ms",
                 static_cast<long long>(
                     state_.continue_conversation_delay_ns / 1'000'000));
        lva::tr::Show(lva::tr::LedState::Thinking);
        const auto now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        continue_listen_at_ns_.store(
            now_ns + state_.continue_conversation_delay_ns,
            std::memory_order_release);
    } else {
        pipeline_active_ = false;
        lva::tr::Show(lva::tr::LedState::Idle);
        Unduck();
        ::malloc_trim(0);
    }
}

void Satellite::PlayAnnounce(const std::string& media_id,
                             const std::string& preannounce_media_id,
                             const std::string& text,
                             bool start_conversation) {
    if (announce_player_ == nullptr) return;

    // Speak announcement text locally; media without text plays as-is.
    std::string play_id = media_id;
    if (LocalVoiceEnabled() && local_.tts != nullptr && !text.empty()) {
        const std::string path = SynthesizeToFile(text);
        if (!path.empty()) play_id = path;
    }
    if (play_id.empty()) return;

    if (timer_ringing_) StopTimerRing();
    pipeline_active_ = true;
    DisarmHaWatchdog();
    Duck();
    continue_conversation_ = start_conversation;

    speech_active_ = true;
    local_speech_  = false;
    auto play_main = [this, play_id]() {
        announce_player_->Play(play_id, [this]() {
            FinishSpeech(true);
        });
    };

    if (!preannounce_media_id.empty()) {
        announce_player_->Play(preannounce_media_id,
                               std::move(play_main));
    } else {
        play_main();
    }
}

void Satellite::Duck() {
    if (ducked_) return;
    ducked_ = true;
    if (state_.sendspin != nullptr) state_.sendspin->SetDucked(true);
    if (state_.music_player) {
        state_.music_player->SetGain(kDuckFactor);
    }
}

void Satellite::Unduck() {
    if (!ducked_) return;
    ducked_ = false;
    if (state_.sendspin != nullptr) state_.sendspin->SetDucked(false);
    if (state_.music_player) {
        state_.music_player->SetGain(1.0);
    }
}

void Satellite::StartTimerRing() {
    if (timer_ringing_) return;  // already ringing
    if (speech_active_ && announce_player_ != nullptr) {
        continue_conversation_ = false;
        continue_listen_at_ns_.store(0, std::memory_order_relaxed);
        announce_player_->Stop();
        FinishSpeech(false);
    }
    timer_ringing_    = true;
    timer_ring_start_ = std::chrono::steady_clock::now();
    if (announce_player_ != nullptr) {
        announce_player_->Play(kTimerFinishedSoundPath, [this] {
            OnTimerSoundEof();
        });
    }
    Duck();
    lva::tr::Show(lva::tr::LedState::Speaking);
}

void Satellite::StopTimerRing() {
    if (!timer_ringing_) return;
    timer_ringing_ = false;
    if (announce_player_ != nullptr) {
        announce_player_->Stop();
    }
    Unduck();
    lva::tr::Show(lva::tr::LedState::Idle);
}

void Satellite::OnTimerSoundEof() {
    if (!timer_ringing_) return;  // user cancelled

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - timer_ring_start_).count();
    if (elapsed >= kTimerMaxRingSeconds) {
        LVA_LOGI(kTag, "timer ring auto-stopped after %lld s",
                 static_cast<long long>(elapsed));
        StopTimerRing();
        return;
    }
    if (announce_player_ != nullptr) {
        announce_player_->Play(kTimerFinishedSoundPath, [this] {
            OnTimerSoundEof();
        });
    }
}

void Satellite::OnVoiceEvent(
    int event_type,
    const std::vector<std::pair<std::string, std::string>>& data) {
    // HA is alive and driving this run.
    if (ha_deadline_ != std::chrono::steady_clock::time_point{}) {
        ArmHaWatchdog(kHaEventTimeout);
    }
    switch (event_type) {
        case kEvtRunStart:
            LVA_LOGD(kTag, "RUN_START");
            // Some pipelines deliver a TTS URL here; capture if present.
            for (const auto& [k, v] : data) {
                if (k == "url") tts_url_ = v;
            }
            tts_played_           = false;
            continue_conversation_ = false;
            break;

        case kEvtError: {
            std::string code, message;
            for (const auto& [k, v] : data) {
                if      (k == "code")    code    = v;
                else if (k == "message") message = v;
            }
            LVA_LOGW(kTag, "voice ERROR event: code=%s message=%s",
                     code.empty()    ? "(none)" : code.c_str(),
                     message.empty() ? "(none)" : message.c_str());
            lva::tr::Show(lva::tr::LedState::Error);
            StopAudioStreaming();
            DisarmHaWatchdog();
            pipeline_active_       = false;
            continue_conversation_ = false;
            wake_sound_pending_    = false;
            Unduck();
            break;
        }

        case kEvtSttVadEnd:
        case kEvtSttEnd:
            LVA_LOGD(kTag, "STT_(VAD_)END — stop streaming");
            StopAudioStreaming();
            lva::tr::Show(lva::tr::LedState::Thinking);
            break;

        case kEvtIntentStart:
            LVA_LOGD(kTag, "INTENT_START");
            if (state_.preferences.thinking_sound != 0 &&
                announce_player_ != nullptr) {
                LVA_LOGI(kTag, "playing thinking sound");
                announce_player_->Play(kThinkingSoundPath, {});
            }
            break;

        case kEvtIntentEnd:
            for (const auto& [k, v] : data) {
                if (k == "continue_conversation" && v == "1") {
                    continue_conversation_ = true;
                }
            }
            LVA_LOGD(kTag, "INTENT_END (continue=%d)",
                     continue_conversation_ ? 1 : 0);
            break;

        case kEvtIntentProgress:
            for (const auto& [k, v] : data) {
                if (k == "tts_start_streaming" && v == "1") {
                    LVA_LOGD(kTag, "INTENT_PROGRESS: early TTS start");
                    PlayTts();
                }
            }
            break;

        case kEvtTtsEnd:
            for (const auto& [k, v] : data) {
                if (k == "url") tts_url_ = v;
            }
            LVA_LOGI(kTag, "TTS_END — tts_url=%s", tts_url_.c_str());
            PlayTts();
            break;

        case kEvtRunEnd:
            for (const auto& [k, v] : data) {
                if (k == "continue_conversation" && v == "1") {
                    continue_conversation_ = true;
                }
            }
            LVA_LOGI(kTag, "RUN_END (continue=%d tts_played=%d)",
                     continue_conversation_ ? 1 : 0,
                     tts_played_ ? 1 : 0);
            StopAudioStreaming();
            if (!tts_played_) {
                if (continue_conversation_) {
                    lva::tr::Show(lva::tr::LedState::Listening);
                } else {
                    DisarmHaWatchdog();
                    pipeline_active_ = false;
                    lva::tr::Show(lva::tr::LedState::Idle);
                    Unduck();
                }
            }
            break;

        default:
            LVA_LOGD(kTag, "voice event %d", event_type);
            break;
    }
}

void Satellite::OnLoopTick() {
    if (pending_wake_.load(std::memory_order_acquire)) {
        const PendingWake snap = pending_wake_data_;
        pending_wake_.store(false, std::memory_order_relaxed);
        StartPipeline(snap.model_id);
    }

    if (pending_stop_.exchange(false, std::memory_order_acq_rel)) {
        ProcessStopDetected();
    }

    // Open the mic for a continued conversation once the settle delay elapses.
    const auto listen_at = continue_listen_at_ns_.load(std::memory_order_acquire);
    if (listen_at != 0) {
        const auto now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_ns >= listen_at) {
            continue_listen_at_ns_.store(0, std::memory_order_relaxed);
            if (state_.muted.load(std::memory_order_relaxed)) {
                LVA_LOGD(kTag, "continued conversation skipped: muted");
                pipeline_active_ = false;
                lva::tr::Show(lva::tr::LedState::Idle);
                Unduck();
            } else {
                LVA_LOGI(kTag, "settle delay elapsed — listening");
                BeginListening();
            }
        }
    }

    PollLocalVoice();

    CheckHaWatchdog();

    PumpAudioToHa();
}

void Satellite::ArmHaWatchdog(std::chrono::steady_clock::duration timeout) {
    ha_deadline_ = std::chrono::steady_clock::now() + timeout;
}

void Satellite::CheckHaWatchdog() {
    if (ha_deadline_ == std::chrono::steady_clock::time_point{}) return;
    if (!pipeline_active_ || speech_active_) {
        DisarmHaWatchdog();
        return;
    }
    if (std::chrono::steady_clock::now() < ha_deadline_) return;
    ResetHaPipeline("no voice event from HA");
}

void Satellite::ResetHaPipeline(const char* reason) {
    LVA_LOGW(kTag, "resetting voice pipeline: %s", reason);
    DisarmHaWatchdog();
    StopAudioStreaming();
    // Ask HA to drop the run in case it exists but stopped talking to us.
    if (state_.broadcast) {
        ::VoiceAssistantRequest req;
        req.set_start(false);
        state_.broadcast(lva::proto::kIdVoiceAssistantRequest, req);
    }
    pipeline_active_       = false;
    continue_conversation_ = false;
    wake_sound_pending_    = false;
    continue_listen_at_ns_.store(0, std::memory_order_relaxed);
    Unduck();
    lva::tr::Show(lva::tr::LedState::Idle);
}

void Satellite::OnDisconnected() {
    LVA_LOGI(kTag, "HA disconnected — resetting pipeline state");
    StopAudioStreaming();
    DisarmHaWatchdog();
    CancelLocalTurn();
    StopTimerRing();
    pipeline_active_       = false;
    continue_conversation_ = false;
    speech_active_         = false;
    wake_sound_pending_    = false;
    continue_listen_at_ns_.store(0, std::memory_order_relaxed);
    Unduck();
    lva::tr::Show(lva::tr::LedState::Idle);
    if (announce_player_) announce_player_->Stop();
    if (state_.music_player) state_.music_player->Stop();
    tts_url_.clear();
    tts_played_ = false;
}

void Satellite::OnMuted() {
    StopAudioStreaming();
    DisarmHaWatchdog();
    CancelLocalTurn();
    continue_listen_at_ns_.store(0, std::memory_order_relaxed);
    if (pipeline_active_ && announce_player_) {
        announce_player_->Stop();
        pipeline_active_       = false;
        continue_conversation_ = false;
        speech_active_         = false;
        wake_sound_pending_    = false;
        Unduck();
        lva::tr::Show(lva::tr::LedState::Idle);
    }
}

void Satellite::EnableLocalVoice(LocalVoiceConfig cfg) {
    local_ = std::move(cfg);
    LVA_LOGI(kTag, "local voice backend enabled (stt=%s tts=%s agent=%s)",
             local_.stt != nullptr ? "yes" : "no",
             local_.tts != nullptr ? "yes" : "no",
             local_.conversation_agent.empty()
                 ? "(default)" : local_.conversation_agent.c_str());
}

void Satellite::BeginLocalListening() {
    if (!local_.stt->Ready()) {
        LVA_LOGW(kTag, "local STT not ready yet");
        SpeakLocal(kLocalSttUnavailable);
        return;
    }
    LVA_LOGI(kTag, "local STT listening");
    lva::tr::Show(lva::tr::LedState::Listening);
    local_.stt->BeginUtterance();
    local_phase_ = LocalPhase::kListening;
}

void Satellite::PollLocalVoice() {
    if (!LocalVoiceEnabled()) return;

    if (local_phase_ == LocalPhase::kListening) {
        auto text = local_.stt->TakeResult();
        if (!text) return;
        if (text->empty()) {
            LVA_LOGI(kTag, "local STT heard nothing; ending turn");
            local_phase_ = LocalPhase::kIdle;
            continue_conversation_ = false;
            FinishLocalTurn();
            return;
        }
        lva::tr::Show(lva::tr::LedState::Thinking);
        if (state_.preferences.thinking_sound != 0 &&
            announce_player_ != nullptr) {
            announce_player_->Play(kThinkingSoundPath, {});
        }
        SendConversationRequest(*text);
        return;
    }

    if (local_phase_ == LocalPhase::kWaitingIntent &&
        std::chrono::steady_clock::now() >= intent_deadline_) {
        LVA_LOGW(kTag, "conversation.process timed out (call_id=%u); check "
                       "that 'Allow the device to perform Home Assistant "
                       "actions' is enabled for this device in HA",
                 pending_call_id_);
        pending_call_id_ = 0;
        local_phase_     = LocalPhase::kIdle;
        continue_conversation_ = false;
        SpeakLocal(kLocalIntentFailed);
    }
}

void Satellite::SendConversationRequest(const std::string& text) {
    if (!state_.broadcast) {
        LVA_LOGW(kTag, "no HA connection; cannot process '%s'", text.c_str());
        local_phase_ = LocalPhase::kIdle;
        continue_conversation_ = false;
        SpeakLocal(kLocalNoConnection);
        return;
    }

    ::HomeassistantActionRequest req;
    req.set_service("conversation.process");
    auto add = [&req](const char* key, const std::string& value) {
        auto* kv = req.add_data();
        kv->set_key(key);
        kv->set_value(value);
    };
    add("text", text);
    add("language", local_.language);
    if (!local_.conversation_agent.empty()) {
        add("agent_id", local_.conversation_agent);
    }
    if (!conversation_id_.empty()) {
        add("conversation_id", conversation_id_);
    }

    pending_call_id_ = next_call_id_++;
    if (next_call_id_ == 0) next_call_id_ = 1;  // 0 means "no response"
    req.set_call_id(pending_call_id_);
    req.set_wants_response(true);

    local_phase_     = LocalPhase::kWaitingIntent;
    intent_deadline_ = std::chrono::steady_clock::now() + kIntentTimeout;
    LVA_LOGI(kTag, "conversation.process '%s' (call_id=%u)", text.c_str(),
             pending_call_id_);
    state_.broadcast(lva::proto::kIdHomeassistantActionRequest, req);
}

void Satellite::OnActionResponse(std::uint32_t call_id, bool success,
                                 const std::string& error_message,
                                 const std::string& response_data) {
    if (call_id == 0 || call_id != pending_call_id_ ||
        local_phase_ != LocalPhase::kWaitingIntent) {
        LVA_LOGD(kTag, "ignoring action response call_id=%u", call_id);
        return;
    }
    pending_call_id_ = 0;
    local_phase_     = LocalPhase::kIdle;

    if (!success) {
        LVA_LOGW(kTag, "conversation.process failed: %s",
                 error_message.c_str());
        continue_conversation_ = false;
        SpeakLocal(kLocalIntentFailed);
        return;
    }

    const ConversationReply reply = ParseConversationReply(response_data);
    if (!reply.conversation_id.empty()) {
        conversation_id_ = reply.conversation_id;
    }
    continue_conversation_ = reply.continue_conversation;
    LVA_LOGI(kTag, "conversation reply (continue=%d): '%s'",
             reply.continue_conversation ? 1 : 0, reply.speech.c_str());

    if (reply.speech.empty()) {
        FinishLocalTurn();
        return;
    }
    SpeakLocal(reply.speech);
}

std::string Satellite::SynthesizeToFile(const std::string& text) {
    if (local_.tts == nullptr) return {};

    std::vector<std::int16_t> pcm;
    const auto t0 = std::chrono::steady_clock::now();
    if (!local_.tts->Synthesize(text, pcm)) return {};
    const double synth_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    std::error_code ec;
    std::filesystem::create_directories(kTtsDir, ec);
    // Two files, so a new reply never overwrites one mpv is still reading.
    const std::string path = std::string(kTtsDir) + "/reply-" +
                             std::to_string(tts_file_seq_++ % 2) + ".wav";
    if (!lva::audio::WriteWavS16Mono(path, pcm, local_.tts->SampleRate())) {
        LVA_LOGW(kTag, "failed to write %s", path.c_str());
        return {};
    }
    LVA_LOGI(kTag, "local TTS: %.2f s audio in %.0f ms -> %s",
             static_cast<double>(pcm.size()) / local_.tts->SampleRate(),
             synth_s * 1000.0, path.c_str());
    return path;
}

void Satellite::SpeakLocal(const std::string& text) {
    const std::string path = SynthesizeToFile(text);
    if (path.empty() || announce_player_ == nullptr) {
        FinishLocalTurn();
        return;
    }
    if (timer_ringing_) StopTimerRing();
    lva::tr::Show(lva::tr::LedState::Speaking);
    speech_active_ = true;
    local_speech_  = true;
    announce_player_->Play(path, [this]() { FinishSpeech(true); });
}

void Satellite::FinishLocalTurn() {
    if (continue_conversation_) {
        const auto now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        continue_listen_at_ns_.store(
            now_ns + state_.continue_conversation_delay_ns,
            std::memory_order_release);
        return;
    }
    conversation_id_.clear();
    pipeline_active_ = false;
    lva::tr::Show(lva::tr::LedState::Idle);
    Unduck();
    ::malloc_trim(0);
}

void Satellite::CancelLocalTurn() {
    if (!LocalVoiceEnabled()) return;
    if (local_phase_ != LocalPhase::kIdle) {
        LVA_LOGI(kTag, "local turn cancelled");
    }
    local_.stt->Cancel();
    local_phase_     = LocalPhase::kIdle;
    pending_call_id_ = 0;
    conversation_id_.clear();
}

std::filesystem::path Satellite::ResolveWakeWordConfig(
    const std::string& id) {
    const auto local_path = state_.wakeword_dir / (id + ".json");
    if (std::filesystem::exists(local_path)) {
        return local_path;
    }

    const auto it = external_wake_words_.find(id);
    if (it == external_wake_words_.end()) {
        return {};
    }
    return lva::audio::DownloadExternalWakeWord(it->second,
                                                state_.wakeword_dir);
}

bool Satellite::HandleMessage(std::uint32_t msg_type_id,
                              const ::google::protobuf::MessageLite& msg) {
    using namespace lva::proto;

    switch (msg_type_id) {
        case kIdVoiceAssistantConfigurationRequest: {
            const auto& cfg_req =
                static_cast<const ::VoiceAssistantConfigurationRequest&>(msg);

            ::VoiceAssistantConfigurationResponse resp;
            resp.set_max_active_wake_words(2);

            for (const auto& info :
                 lva::audio::ScanAvailableWakeWords(state_.wakeword_dir)) {
                auto* ww = resp.add_available_wake_words();
                ww->set_id(info.id);
                ww->set_wake_word(info.wake_word);
                for (const auto& lang : info.trained_languages) {
                    ww->add_trained_languages(lang);
                }
            }

            external_wake_words_.clear();
            for (int i = 0; i < cfg_req.external_wake_words_size(); ++i) {
                const auto& eww = cfg_req.external_wake_words(i);
                if (eww.model_type() != "micro") {
                    LVA_LOGD(kTag, "skipping external wake word '%s' "
                                   "(type=%s, not micro)",
                             eww.id().c_str(), eww.model_type().c_str());
                    continue;
                }

                lva::audio::ExternalWakeWordInfo info;
                info.id         = eww.id();
                info.wake_word  = eww.wake_word();
                info.model_type = eww.model_type();
                info.model_size = eww.model_size();
                info.model_hash = eww.model_hash();
                info.url        = eww.url();
                for (int l = 0; l < eww.trained_languages_size(); ++l) {
                    info.trained_languages.push_back(eww.trained_languages(l));
                }

                auto* ww = resp.add_available_wake_words();
                ww->set_id(info.id);
                ww->set_wake_word(info.wake_word);
                for (const auto& lang : info.trained_languages) {
                    ww->add_trained_languages(lang);
                }

                external_wake_words_.emplace(eww.id(), std::move(info));
            }

            // Snapshot under the engine lock; SetActive* may run concurrently.
            {
                const auto micro = engine_.SnapshotModels();
                for (const auto& m : micro) {
                    if (m->config().id != "stop")
                        resp.add_active_wake_words(m->config().id);
                }
                const auto open = engine_.SnapshotOpenModels();
                for (const auto& m : open) {
                    resp.add_active_wake_words(m->config().id);
                }
            }

            if (state_.broadcast) {
                state_.broadcast(kIdVoiceAssistantConfigurationResponse,
                                 resp);
            }
            LVA_LOGI(kTag, "ConfigurationRequest: %d available "
                           "(%zu external), %d active",
                     resp.available_wake_words_size(),
                     external_wake_words_.size(),
                     resp.active_wake_words_size());
            return true;
        }

        case kIdVoiceAssistantSetConfiguration: {
            const auto& set_req =
                static_cast<const ::VoiceAssistantSetConfiguration&>(msg);
            std::string ids;
            for (int i = 0; i < set_req.active_wake_words_size(); ++i) {
                if (i > 0) ids += ",";
                ids += set_req.active_wake_words(i);
            }
            LVA_LOGI(kTag, "SetConfiguration: active=[%s]", ids.c_str());

            std::vector<std::shared_ptr<lva::audio::MicroWakeWord>> new_micro;
            std::vector<std::shared_ptr<lva::audio::OpenWakeWord>>  new_open;
            int load_failures = 0;
            for (int i = 0; i < set_req.active_wake_words_size(); ++i) {
                const auto& ww_id = set_req.active_wake_words(i);
                const auto json_path = ResolveWakeWordConfig(ww_id);
                if (json_path.empty()) {
                    ++load_failures;
                    LVA_LOGW(kTag, "SetConfiguration: cannot resolve '%s'",
                             ww_id.c_str());
                    continue;
                }
                const std::string type =
                    lva::audio::ReadWakeWordType(json_path.parent_path(),
                                                 ww_id);

                bool loaded = false;
                if (type == "open") {
                    auto model = lva::audio::OpenWakeWord::FromConfig(json_path);
                    if (model && model->Ok()) {
                        new_open.push_back(std::move(model));
                        loaded = true;
                    }
                } else {
                    // "" or "micro" or anything else → try micro.
                    auto model = lva::audio::MicroWakeWord::FromConfig(json_path);
                    if (model && model->Ok()) {
                        new_micro.push_back(std::move(model));
                        loaded = true;
                    }
                }
                if (!loaded) {
                    ++load_failures;
                    LVA_LOGW(kTag, "SetConfiguration: failed to load '%s'"
                                   " (type=%s)",
                             ww_id.c_str(),
                             type.empty() ? "?" : type.c_str());
                }
            }

            if (!new_micro.empty() || !new_open.empty()) {
                std::size_t slot = 0;
                auto apply_default = [&](float cutoff) {
                    if (slot == 0 &&
                        !state_.preferences.wake_word_1_sensitivity.has_value()) {
                        state_.preferences.wake_word_1_sensitivity = cutoff;
                    } else if (slot == 1 &&
                        !state_.preferences.wake_word_2_sensitivity.has_value()) {
                        state_.preferences.wake_word_2_sensitivity = cutoff;
                    }
                    ++slot;
                };
                for (const auto& m : new_micro) {
                    if (slot >= 2) break;
                    apply_default(m->config().probability_cutoff);
                }
                for (const auto& m : new_open) {
                    if (slot >= 2) break;
                    apply_default(m->config().probability_cutoff);
                }

                // Persist loaded ids in slot order (micro, then open), before
                // the std::move below empties the vectors.
                std::vector<std::optional<std::string>> selected;
                selected.reserve(new_micro.size() + new_open.size());
                for (const auto& m : new_micro) {
                    selected.emplace_back(m->config().id);
                }
                for (const auto& m : new_open) {
                    selected.emplace_back(m->config().id);
                }
                state_.preferences.active_wake_words = std::move(selected);

                engine_.SetActiveModels(std::move(new_micro));
                engine_.SetActiveOpenModels(std::move(new_open));
                state_.SavePreferences();
                if (state_.broadcast) {
                    const auto sink = [&](std::uint32_t id,
                                          const ::google::protobuf::MessageLite& m) {
                        state_.broadcast(id, m);
                    };
                    for (const auto& entity : state_.entities) {
                        entity->OnSubscribeStates(sink);
                    }
                }
            } else {
                LVA_LOGW(kTag, "SetConfiguration: no valid models "
                               "(%d failures); keeping current set",
                         load_failures);
            }
            return true;
        }

        case kIdVoiceAssistantEventResponse: {
            const auto& ev =
                static_cast<const ::VoiceAssistantEventResponse&>(msg);
            std::vector<std::pair<std::string, std::string>> data;
            data.reserve(ev.data_size());
            for (int i = 0; i < ev.data_size(); ++i) {
                const auto& kv = ev.data(i);
                data.emplace_back(kv.name(), kv.value());
            }
            OnVoiceEvent(static_cast<int>(ev.event_type()), data);
            return true;
        }

        case kIdVoiceAssistantTimerEventResponse: {
            const auto& timer_msg =
                static_cast<const ::VoiceAssistantTimerEventResponse&>(msg);
            const int evt = static_cast<int>(timer_msg.event_type());
            if (evt == 3) {
                LVA_LOGI(kTag, "TIMER_FINISHED — ringing");
                StartTimerRing();
            } else if (evt == 2) {
                LVA_LOGI(kTag, "TIMER_CANCELLED — stopping ring");
                StopTimerRing();
            }
            return true;
        }

        case kIdVoiceAssistantAnnounceRequest: {
            const auto& ann =
                static_cast<const ::VoiceAssistantAnnounceRequest&>(msg);
            LVA_LOGI(kTag, "AnnounceRequest: media_id=%s preannounce=%s",
                     ann.media_id().c_str(),
                     ann.preannounce_media_id().c_str());
            PlayAnnounce(ann.media_id(), ann.preannounce_media_id(),
                         ann.text(), ann.start_conversation());
            return true;
        }

        case kIdVoiceAssistantResponse: {
            const auto& r = static_cast<const ::VoiceAssistantResponse&>(msg);
            if (r.error() && pipeline_active_ && !speech_active_ &&
                !LocalVoiceEnabled()) {
                ResetHaPipeline("HA could not start the pipeline");
            }
            return true;
        }

        case kIdHomeassistantActionResponse: {
            const auto& r =
                static_cast<const ::HomeassistantActionResponse&>(msg);
            OnActionResponse(r.call_id(), r.success(), r.error_message(),
                             r.response_data());
            return true;
        }

        default:
            return false;
    }
}

}  // namespace lva::satellite
