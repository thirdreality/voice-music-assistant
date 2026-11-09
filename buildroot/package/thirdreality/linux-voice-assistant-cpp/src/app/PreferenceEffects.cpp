#include "app/Application.h"
#include "audio/LibMpvPlayer.h"
#include "audio/MicroWakeWord.h"
#include "audio/OpenWakeWord.h"
#include "audio/WakeWordEngine.h"
#include "audio/WebRtcProcessor.h"
#include "entities/MediaPlayerEntity.h"
#include "entities/MuteSwitchEntity.h"
#include "satellite/Satellite.h"
#include "sendspin/SendspinHub.h"
#include "tr/LedRing.h"
#include "tr/MicMuteGpio.h"
#include "tr/SystemVolume.h"
#include "util/Log.h"

namespace lva::app {

void Application::BindPreferenceEffects() {
    state.SetPreferenceEffects({
        .volume_changed =
            [this](double volume) {
                const int percent = static_cast<int>(volume * 100.0 + 0.5);
                lva::tr::SetSystemVolume(percent);
                lva::tr::ShowVolumeChanged();
                if (sendspin_active) sendspin_hub->SetVolume(percent);
                if (media_player_entity) media_player_entity->BroadcastState();
            },
        .muted_changed =
            [this](bool muted) {
                if (mic_mute_gpio) mic_mute_gpio->SyncToHardware(muted);
                if (muted && satellite) satellite->OnMuted();
                if (mute_switch_entity) mute_switch_entity->BroadcastState();
                if (media_player_entity) media_player_entity->BroadcastState();
            },
        .mute_sound =
            [this](bool muted) {
                if (announce_player)
                    announce_player->Play(
                        muted ? "/usr/share/thirdreality/sounds/mute_switch_on.flac"
                              : "/usr/share/thirdreality/sounds/mute_switch_off.flac",
                        {});
            },
        .wake_sensitivity_changed =
            [this](int n, double value) {
                if (wakeword_engine) {
                    const auto micro_models = wakeword_engine->SnapshotModels();
                    const std::size_t target_idx = static_cast<std::size_t>(n - 1);
                    std::size_t wake_idx = 0;
                    bool applied = false;
                    for (const auto& m : micro_models) {
                        if (m->config().id == "stop") continue;
                        if (wake_idx == target_idx) {
                            m->SetProbabilityCutoff(static_cast<float>(value));
                            applied = true;
                            break;
                        }
                        ++wake_idx;
                    }
                    if (!applied) {
                        const auto open_models = wakeword_engine->SnapshotOpenModels();
                        for (const auto& m : open_models) {
                            if (wake_idx == target_idx) {
                                m->SetProbabilityCutoff(static_cast<float>(value));
                                break;
                            }
                            ++wake_idx;
                        }
                    }
                }
            },
        .stop_sensitivity_changed =
            [this](double value) {
                if (wakeword_engine) {
                    const auto micro_models = wakeword_engine->SnapshotModels();
                    for (const auto& m : micro_models) {
                        if (m->config().id == "stop") {
                            m->SetProbabilityCutoff(static_cast<float>(value));
                            break;
                        }
                    }
                }
            },
        .mic_levels_changed =
            [this](int gain, int noise) {
                if (webrtc_processor) webrtc_processor->SetLevels(gain, noise);
            },
    });
}

}  // namespace lva::app
