#include <utility>

#include "app/Application.h"
#include "audio/LibMpvPlayer.h"
#include "audio/MicroWakeWord.h"
#include "audio/PcmRingBuffer.h"
#include "audio/SanoTts.h"
#include "audio/WakeWordEngine.h"
#include "audio/WhisperStt.h"
#include "entities/MediaPlayerEntity.h"
#include "satellite/LocalStt.h"
#include "satellite/LocalTts.h"
#include "satellite/Satellite.h"
#include "sendspin/SendspinHub.h"
#include "tr/ModelDownloader.h"
#include "tr/SoundConfWatcher.h"
#include "tr/Supervisor.h"
#include "tr/SupervisorHttpServer.h"
#include "util/Log.h"

namespace lva::app {
namespace {
constexpr const char* kTag = "main";
}

void Application::StartVoice() {
    satellite = std::make_unique<lva::satellite::Satellite>(state, *satellite_ring,
                                                            *wakeword_engine, announce_player.get(),
                                                            *music_player, [this](bool ducked) {
                                                                if (sendspin_active)
                                                                    sendspin_hub->SetDucked(ducked);
                                                            });
    media_player_entity->SetVoiceActions({
        .owns_announcement = [this] { return satellite->OwnsAnnouncePlayback(); },
        .prepare_announcement = [this] { satellite->PrepareForMediaAnnouncement(); },
    });
    state.handle_voice_message = [this](std::uint32_t id,
                                        const ::google::protobuf::MessageLite& message) {
        return satellite->HandleMessage(id, message);
    };

    if (use_local_voice) {
        lva::audio::WhisperStt::Config wcfg;
        wcfg.model_path = cli.stt_model;
        wcfg.num_threads = cli.stt_threads;

        lva::satellite::LocalStt::Options stt_opts;
        stt_opts.dump_wav_path = cli.stt_dump_wav;
        local_stt = std::make_unique<lva::satellite::LocalStt>(
            std::make_unique<lva::audio::WhisperStt>(wcfg), *local_stt_ring, stt_opts);
        local_stt->Start();

        lva::audio::SanoTts::Config tts_cfg;
        tts_cfg.espeak_data_dir = cli.tts_data_dir;
        tts_cfg.voice_dir = kTtsVoiceDir;
        tts_cfg.speed = TtsSpeed(state.preferences.tts_rate.value_or(kDefaultTtsRate));
        tts_cfg.threads = cli.tts_threads;
        local_tts = std::make_unique<lva::satellite::LocalTts>(tts_cfg);
        if (!local_tts->Start()) {
            LVA_LOGW(kTag, "local TTS unavailable; replies will be silent");
            local_tts.reset();
        }

        lva::satellite::LocalVoiceConfig voice_cfg;
        voice_cfg.stt = local_stt.get();
        voice_cfg.tts = local_tts.get();
        voice_cfg.conversation_agent = cli.conversation_agent;
        satellite->EnableLocalVoice(std::move(voice_cfg));
    }
    LVA_LOGI(kTag, "voice backend=%s", use_local_voice ? "local" : "ha");

    // Independent shadow engine; deliberately not in wakeword_engine,
    // HA only manages the primary one.
    linear_wakeword_engine = std::make_unique<lva::audio::WakeWordEngine>(*linear_wake_ring);

    if (!use_openwakeword) {
        for (const auto& primary_model : wakeword_engine->SnapshotModels()) {
            if (!primary_model) continue;

            const auto json_path = wakeword_dir / (primary_model->config().id + ".json");

            auto shadow_model = lva::audio::MicroWakeWord::FromConfig(json_path);

            if (shadow_model && shadow_model->Ok()) {
                // Linear-AEC output only exists during playback, where real
                // wakes score far below the primary cutoff.
                constexpr float kShadowProbabilityCutoff = 0.40f;
                shadow_model->SetProbabilityCutoff(kShadowProbabilityCutoff);
                shadow_model->SetSourceTag("linear_aec");

                LVA_LOGI(kTag,
                         "shadow model loaded: id=%s cutoff=%.3f "
                         "(primary cutoff=%.3f)",
                         primary_model->config().id.c_str(), shadow_model->ProbabilityCutoff(),
                         primary_model->ProbabilityCutoff());

                linear_wakeword_engine->AddModel(std::move(shadow_model));
            } else {
                LVA_LOGW(kTag, "shadow model failed to load: %s",
                         primary_model->config().id.c_str());
            }
        }
    }

    state.on_client_disconnected = [this] { satellite->OnDisconnected(); };

    sound_watcher = std::make_unique<lva::tr::SoundConfWatcher>(state, cli.preferences_file);

    supervisor_http = std::make_unique<lva::tr::SupervisorHttpServer>(*supervisor);
    if (!supervisor_http->Start()) {
        LVA_LOGW(kTag,
                 "supervisor HTTP failed to start; OTA over "
                 "the legacy supervisor API will not be "
                 "available (HA UI Update entity still works)");
    }
}

}  // namespace lva::app
