#include <algorithm>
#include <utility>
#include <vector>

#include "app/Application.h"
#include "audio/LibMpvPlayer.h"
#include "audio/WebRtcProcessor.h"
#include "entities/EventEntity.h"
#include "entities/MediaPlayerEntity.h"
#include "entities/MuteSwitchEntity.h"
#include "entities/NetworkSwitchEntity.h"
#include "entities/NumberEntity.h"
#include "entities/SelectEntity.h"
#include "entities/SendspinSwitchEntity.h"
#include "entities/ThinkingSoundEntity.h"
#include "entities/UpdateEntity.h"
#include "satellite/LocalTts.h"
#include "sendspin/SendspinHub.h"
#include "tr/ModelDownloader.h"
#include "tr/Supervisor.h"
#include "util/Log.h"

namespace lva::app {
namespace {
constexpr const char* kTag = "main";

// Publish observers only after the owning container has accepted the entity.
// A failed vector allocation destroys the temporary without leaving a dangling
// observer for Application::Shutdown().
template <class T>
T* RegisterEntity(lva::state::ServerState& state, std::unique_ptr<T> entity) {
    auto* observer = entity.get();
    state.entities.push_back(std::move(entity));
    return observer;
}

}  // namespace

std::string Application::DownloadNotice() const {
    const std::string name = state.name;
    if (!stt_model_downloader) {
        return name +
               ": the speech model is missing; using Home "
               "Assistant for voice.";
    }
    const auto st = stt_model_downloader->GetStatus();
    return name + ": the speech model is still downloading (" + std::to_string(st.percent) +
           "%). The speaker keeps using "
           "Home Assistant for voice and switches to local speech "
           "automatically when the download finishes.";
}

void Application::RegisterEntities() {
    std::uint32_t next_key = 0;

    {
        auto media = std::make_unique<lva::entities::MediaPlayerEntity>(
            next_key++, state, music_player.get(), announce_player.get(), *webrtc_processor);
        media_player_entity = RegisterEntity(state, std::move(media));
    }

    SetupSendspin();

    {
        auto mute = std::make_unique<lva::entities::MuteSwitchEntity>(next_key++, state);
        mute_switch_entity = RegisterEntity(state, std::move(mute));
    }
    state.entities.push_back(
        std::make_unique<lva::entities::ThinkingSoundEntity>(next_key++, state));

    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++, lva::entities::NumberEntity::Config{
                        .object_id = "wake_word_1_sensitivity",
                        .display_name = "Wake Word 1 Sensitivity",
                        .icon = "",
                        .min_value = 0.0,
                        .max_value = 1.0,
                        .step = 0.001,
                        .mode_enum = 1,  // BOX
                        .getter =
                            [this] {
                                const double fallback = use_openwakeword
                                                            ? kDefaultOpenWakeSensitivity
                                                            : kDefaultMicroWakeSensitivity;
                                return state.preferences.wake_word_1_sensitivity.value_or(fallback);
                            },
                        .setter = [this](double v) { state.PersistWakeWordSensitivity(1, v); },
                    }));

    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++, lva::entities::NumberEntity::Config{
                        .object_id = "wake_word_2_sensitivity",
                        .display_name = "Wake Word 2 Sensitivity",
                        .icon = "",
                        .min_value = 0.0,
                        .max_value = 1.0,
                        .step = 0.001,
                        .mode_enum = 1,
                        .getter =
                            [this] {
                                const double fallback = use_openwakeword
                                                            ? kDefaultOpenWakeSensitivity
                                                            : kDefaultMicroWakeSensitivity;
                                return state.preferences.wake_word_2_sensitivity.value_or(fallback);
                            },
                        .setter = [this](double v) { state.PersistWakeWordSensitivity(2, v); },
                    }));

    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++, lva::entities::NumberEntity::Config{
                        .object_id = "stop_word_sensitivity",
                        .display_name = "Stop Word Sensitivity",
                        .icon = "mdi:hand-back-left",
                        .min_value = 0.0,
                        .max_value = 1.0,
                        .step = 0.001,
                        .mode_enum = 1,
                        .getter =
                            [this] {
                                return state.preferences.stop_word_sensitivity.value_or(
                                    kDefaultStopSensitivity);
                            },
                        .setter = [this](double v) { state.PersistStopWordSensitivity(v); },
                    }));

    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++, lva::entities::NumberEntity::Config{
                        .object_id = "continue_conversation_delay",
                        .display_name = "Continue Conversation Delay",
                        .icon = "mdi:timer-sand",
                        .min_value = 0.0,
                        .max_value = 10.0,
                        .step = 0.1,
                        .mode_enum = 1,  // BOX
                        .getter =
                            [this, default_ccd = cli.continue_conversation_delay] {
                                return state.preferences.continue_conversation_delay.value_or(
                                    default_ccd);
                            },
                        .setter = [this](double v) { state.PersistContinueConversationDelay(v); },
                    }));

    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++,
        lva::entities::NumberEntity::Config{
            .object_id = "mic_gain",
            .display_name = "Mic Auto Gain",
            .icon = "mdi:microphone-plus",
            .min_value = 0.0,
            .max_value = 31.0,
            .step = 1.0,
            .mode_enum = 0,  // AUTO
            .getter = [this] { return static_cast<double>(state.preferences.mic_auto_gain); },
            .setter = [this](double v) { state.PersistMicAutoGain(static_cast<int>(v)); },
        }));

    {
        static const std::vector<std::string> kNoiseOptions = {
            "Off", "Low", "Medium", "High", "Max",
        };
        state.entities.push_back(std::make_unique<lva::entities::SelectEntity>(
            next_key++, lva::entities::SelectEntity::Config{
                            .object_id = "mic_noise",
                            .display_name = "Mic Noise Suppression",
                            .icon = "mdi:waveform",
                            .options = kNoiseOptions,
                            .getter = [this]() -> std::string {
                                const int idx =
                                    std::clamp(state.preferences.mic_noise_suppression, 0, 4);
                                return kNoiseOptions[static_cast<std::size_t>(idx)];
                            },
                            .setter =
                                [this](const std::string& label) {
                                    for (std::size_t i = 0; i < kNoiseOptions.size(); ++i) {
                                        if (kNoiseOptions[i] == label) {
                                            state.PersistMicNoiseSuppression(static_cast<int>(i));
                                            return;
                                        }
                                    }
                                    LVA_LOGW(kTag, "mic_noise: unknown label '%s'", label.c_str());
                                },
                        }));
    }

    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++,
        lva::entities::NumberEntity::Config{
            .object_id = "mic_volume",
            .display_name = "Mic Volume",
            .icon = "mdi:microphone-settings",
            .min_value = 1.0,
            .max_value = 4000.0,
            .step = 1.0,
            .mode_enum = 0,  // AUTO
            .getter = [this] { return static_cast<double>(state.preferences.mic_volume); },
            .setter = [this](double v) { state.PersistMicVolume(static_cast<int>(v)); },
        }));

    // Created later if the local backend is in use; the rate entity below
    // applies changes to it.

    // Set when HA picks another voice backend; main loop restarts LVA.
    const auto backend_label = [this]() -> std::string {
        return state.preferences.voice_backend.value_or(cli.voice_backend) == "local"
                   ? kVoiceBackendLocal
                   : kVoiceBackendHa;
    };
    auto backend_select = std::make_unique<lva::entities::SelectEntity>(
        next_key++,
        lva::entities::SelectEntity::Config{
            .object_id = "speech_processing",
            .display_name = "Speech Processing",
            .icon = "mdi:account-voice",
            .options = {kVoiceBackendHa, kVoiceBackendLocal},
            .getter = backend_label,
            .setter =
                [this](const std::string& label) {
                    std::string backend;
                    if (label == kVoiceBackendHa)
                        backend = "ha";
                    else if (label == kVoiceBackendLocal)
                        backend = "local";
                    else {
                        LVA_LOGW(kTag, "speech_processing: unknown label '%s'", label.c_str());
                        return;
                    }
                    state.PersistVoiceBackend(backend);
                    const bool want_local = (backend == "local");
                    if (want_local && !SttModelReady()) {
                        // Keep the HA backend until the model is in place. A
                        // failed download is retried; if it fails again the
                        // main loop sets the select back to Home Assistant.
                        if (stt_model_downloader)
                            stt_model_downloader->Start(lva::tr::RequestSource::Explicit);
                        state.NotifyHa(kVoiceNotifyId, kVoiceNotifyTitle, DownloadNotice());
                        return;
                    }
                    // Picking the running backend again cancels a pending
                    // switch, e.g. back to HA right after the model finished.
                    restart_for_backend = (want_local != use_local_voice);
                    if (restart_for_backend) {
                        LVA_LOGI(kTag, "voice backend -> %s; restart pending", backend.c_str());
                    }
                },
        });
    backend_select_ptr = RegisterEntity(state, std::move(backend_select));

    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++, lva::entities::NumberEntity::Config{
                        .object_id = "speech_rate",
                        .display_name = "Speech Rate (Local TTS)",
                        .icon = "mdi:speedometer",
                        .min_value = 100.0,
                        .max_value = 250.0,
                        .step = 5.0,
                        .mode_enum = 2,  // SLIDER
                        .getter =
                            [this] {
                                return static_cast<double>(
                                    state.preferences.tts_rate.value_or(kDefaultTtsRate));
                            },
                        .setter =
                            [this](double v) {
                                const int rate = static_cast<int>(v);
                                state.PersistTtsRate(rate);
                                if (local_tts) local_tts->SetSpeed(TtsSpeed(rate));
                            },
                    }));

    state.entities.push_back(std::make_unique<lva::entities::SendspinSwitchEntity>(
        next_key++, [this] { return state.preferences.sendspin_enabled; },
        [this](bool on) {
            state.PersistSendspinEnabled(on);
            if (on == sendspin_hub->IsStarted()) return;
            if (on && !SetSendspinRunning(true)) {
                LVA_LOGW(kTag, "Sendspin start failed; retrying");
            } else if (!on) {
                SetSendspinRunning(false);
            }
        }));

    const std::uint32_t home_button_entity_key = next_key++;
    {
        auto ev = std::make_unique<lva::entities::EventEntity>(
            home_button_entity_key,
            lva::entities::EventEntity::Config{
                .object_id = "thirdreality_home_button",
                .display_name = "Home Button",
                .icon = "mdi:gesture-tap-button",
                .event_types = {"single_press", "double_press", "triple_press"},
            });
        home_button_entity = RegisterEntity(state, std::move(ev));
    }

    supervisor = std::make_unique<lva::tr::Supervisor>(state.network_policy);
    {
        auto upd = std::make_unique<lva::entities::UpdateEntity>(next_key++, state, *supervisor);
        update_entity_ptr = RegisterEntity(state, std::move(upd));
    }

    auto network_switch = std::make_unique<lva::entities::NetworkSwitchEntity>(next_key++, state);
    network_switch_ptr = RegisterEntity(state, std::move(network_switch));

    LVA_LOGI(kTag, "registered %zu entities", state.entities.size());

    if (update_entity_ptr != nullptr) {
        state.on_client_authenticated = [this] { update_entity_ptr->TriggerCheck(); };
    }
}

}  // namespace lva::app
