
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <google/protobuf/message_lite.h>
#include <map>
#include <string>
#include <vector>

#include "audio/ExternalWakeWord.h"

namespace lva::audio    { class WakeWordEngine; class PcmRingBuffer; class IAudioPlayer; class ITextToSpeech; }
namespace lva::state    { class ServerState; }

namespace lva::satellite {

class LocalStt;

// On-device STT/TTS; HA only resolves the intent via conversation.process.
// Pointers are non-owning.
struct LocalVoiceConfig {
    LocalStt*                  stt = nullptr;
    lva::audio::ITextToSpeech* tts = nullptr;
    std::string conversation_agent;  // empty = HA default
    std::string language = "en";
};

class Satellite {
   public:
    Satellite(lva::state::ServerState& state,
              lva::audio::PcmRingBuffer& mic_ring,
              lva::audio::WakeWordEngine& engine,
              lva::audio::IAudioPlayer* announce_player);
    ~Satellite();

    Satellite(const Satellite&)            = delete;
    Satellite& operator=(const Satellite&) = delete;

    void OnWakeDetected(const std::string& model_id, float prob);

    void OnStopDetected();

    void OnTapPressed();
    bool AbortSpeechIfSpeaking();
    void PrepareForMediaAnnouncement();
    bool OwnsAnnouncePlayback() const;

    bool HandleMessage(std::uint32_t msg_type_id,
                       const ::google::protobuf::MessageLite& msg);

    void OnLoopTick();

    void OnDisconnected();

    void OnMuted();

    // Call once at startup, before the first wake.
    void EnableLocalVoice(LocalVoiceConfig cfg);

   private:
    void StartPipeline(const std::string& wake_word_phrase);
    void OpenStreamToHa(const std::string& wake_word_phrase);
    void StopAudioStreaming();
    // Open the mic and ask HA to start listening (continued conversation).
    void BeginListening();
    void PumpAudioToHa();
    void OnVoiceEvent(int event_type,
                      const std::vector<std::pair<std::string, std::string>>& data);
    void PlayTts();
    void FinishSpeech(bool success);
    void PlayAnnounce(const std::string& media_id,
                      const std::string& preannounce_media_id,
                      const std::string& text,
                      bool start_conversation);

    // Local voice backend; main thread only.
    bool LocalVoiceEnabled() const { return local_.stt != nullptr; }
    void BeginLocalListening();
    void PollLocalVoice();
    void SendConversationRequest(const std::string& text);
    void OnActionResponse(std::uint32_t call_id, bool success,
                          const std::string& error_message,
                          const std::string& response_data);
    std::string SynthesizeToFile(const std::string& text);
    void SpeakLocal(const std::string& text);
    void FinishLocalTurn();
    void CancelLocalTurn();

    void ProcessStopDetected();

    void StartTimerRing();
    void StopTimerRing();
    void OnTimerSoundEof();
    void Duck();
    void Unduck();


    struct PendingWake {
        std::string model_id;
        float       prob = 0.0f;
    };

    lva::state::ServerState&    state_;
    lva::audio::PcmRingBuffer&  mic_ring_;
    lva::audio::WakeWordEngine& engine_;
    lva::audio::IAudioPlayer*   announce_player_ = nullptr;

    std::atomic<bool> pending_wake_{false};
    PendingWake       pending_wake_data_;

    std::atomic<bool> pending_stop_{false};

    // Atomic: also touched from the wake-word thread and the mute path.
    std::atomic<bool> is_streaming_audio_{false};
    std::atomic<bool> pipeline_active_{false};
    std::atomic<bool> continue_conversation_{false};
    std::atomic<bool> speech_active_{false};
    std::atomic<bool> wake_sound_pending_{false};
    std::atomic<bool> tap_started_during_speech_{false};
    std::atomic<bool> tap_down_pending_{false};
    std::string tts_url_;
    bool   tts_played_         = false;

    std::atomic<std::int64_t> continue_listen_at_ns_{0};

    bool                                  timer_ringing_ = false;
    std::chrono::steady_clock::time_point timer_ring_start_{};

    bool   ducked_           = false;

    std::atomic<std::int64_t> refractory_until_ns_{0};

    std::atomic<std::int64_t> stop_guard_until_ns_{0};

    std::map<std::string, lva::audio::ExternalWakeWordInfo>
        external_wake_words_;

    enum class LocalPhase { kIdle, kListening, kWaitingIntent };
    LocalVoiceConfig local_;
    LocalPhase       local_phase_       = LocalPhase::kIdle;
    std::uint32_t    next_call_id_      = 1;
    std::uint32_t    pending_call_id_   = 0;
    std::chrono::steady_clock::time_point intent_deadline_{};
    std::string      conversation_id_;
    // Local reply playing: do not send AnnounceFinished to HA.
    bool             local_speech_      = false;
    unsigned         tts_file_seq_      = 0;

    std::filesystem::path ResolveWakeWordConfig(const std::string& id);
};

}  // namespace lva::satellite
