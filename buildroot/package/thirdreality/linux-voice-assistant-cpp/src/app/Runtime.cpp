#include <sys/timerfd.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>

#include "app/Application.h"
#include "audio/LibMpvPlayer.h"
#include "entities/EventEntity.h"
#include "entities/MediaPlayerEntity.h"
#include "entities/NetworkSwitchEntity.h"
#include "entities/SelectEntity.h"
#include "entities/UpdateEntity.h"
#include "protocol/ApiServer.h"
#include "satellite/Satellite.h"
#include "sendspin/SendspinHub.h"
#include "tr/ControlListener.h"
#include "tr/MicMuteGpio.h"
#include "tr/ModelDownloader.h"
#include "tr/SoundConfWatcher.h"
#include "util/Log.h"

namespace lva::app {
namespace {
constexpr const char* kTag = "main";
}

void Application::OnPeriodicTick() {
    // RegisterRuntime asserts these dependencies before registering the tick.
    // Shutdown removes event dispatch before releasing any of them.
    if (!state.network_policy.Allowed() && stt_model_downloader)
        stt_model_downloader->RequestCancelAutomatic();
    music_player->PollDrain();
    announce_player->PollDrain();
    satellite->OnLoopTick();
    server->PollTimeSync();
    mic_mute_gpio->Poll();
    sound_watcher->Poll();
    if (update_entity_ptr) update_entity_ptr->OnPeriodicTick();
    network_switch_ptr->OnPeriodicTick(
        (stt_model_downloader && stt_model_downloader->AutomaticBusy()) ||
        (update_entity_ptr && update_entity_ptr->AutomaticBusy()) ||
        satellite->WakeConfigurationBusy());

    const bool want_local = state.preferences.voice_backend.value_or(cli.voice_backend) == "local";
    if (stt_model_downloader && want_local && !use_local_voice && !restart_for_backend) {
        using Phase = lva::tr::ModelDownloader::Phase;
        const auto st = stt_model_downloader->GetStatus();
        if (st.phase == Phase::kReady) {
            state.NotifyHa(kVoiceNotifyId, kVoiceNotifyTitle,
                           state.name +
                               ": the speech model is "
                               "ready. Switching to local speech.");
            restart_for_backend = true;
        } else if (st.phase == Phase::kMissing && !state.network_policy.Allowed()) {
            state.PersistVoiceBackend("ha");
            if (state.broadcast) backend_select_ptr->OnSubscribeStates(state.broadcast);
            state.NotifyHa(kVoiceNotifyId, kVoiceNotifyTitle,
                           state.name +
                               ": automatic speech model download is disabled or was cancelled. "
                               "Speech Processing is set back to Home Assistant; select 3R Speaker "
                               "to explicitly download the missing model.");
        } else if (st.phase == Phase::kFailed) {
            // Local speech cannot work; show HA as the backend
            // again instead of a choice that is not in effect.
            LVA_LOGW(kTag,
                     "STT model download failed (%s); "
                     "voice backend back to ha",
                     st.error.c_str());
            state.PersistVoiceBackend("ha");
            if (state.broadcast) {
                backend_select_ptr->OnSubscribeStates(state.broadcast);
            }
            state.NotifyHa(kVoiceNotifyId, kVoiceNotifyTitle,
                           state.name +
                               ": the speech model "
                               "download failed (" +
                               st.error +
                               "). Speech Processing is set back to "
                               "Home Assistant; pick 3R Speaker to "
                               "try again.");
        }
    }
    if (restart_for_backend && satellite->IsIdle()) {
        // The init script's monitor starts LVA again.
        LVA_LOGI(kTag, "restarting to apply the voice backend");
        restart_for_backend = false;
        server->Stop();
    }
}

namespace {
std::optional<int> ParseControlInteger(const std::string& command, const char* prefix) {
    const std::size_t prefix_len = std::strlen(prefix);
    if (command.compare(0, prefix_len, prefix) != 0) return std::nullopt;

    const char* start = command.c_str() + prefix_len;
    if (*start == '\0') return std::nullopt;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(start, &end, 10);
    if (end == start || *end != '\0' || errno == ERANGE ||
        value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

}  // namespace

bool Application::HandleControl(const std::string& cmd) {
    if (cmd == "TAP_DOWN") {
        if (satellite) satellite->OnTapPressed();
        tap_ha_was_playing = media_player_entity != nullptr && media_player_entity->IsPlaying();
        tap_playback_pending = true;
        return true;
    }
    if (cmd == "TAP") {
        const bool tap_down_seen = tap_playback_pending;
        tap_playback_pending = false;
        const bool ha_was_playing =
            tap_down_seen ? tap_ha_was_playing
                          : media_player_entity != nullptr && media_player_entity->IsPlaying();

        if (satellite && satellite->AbortSpeechIfSpeaking()) {
            return true;
        }
        if (ha_was_playing) {
            media_player_entity->PausePlayback();
        } else {
            media_player_entity->ResumePlayback();
        }
        return true;
    }
    if (cmd == "LED_TOGGLE") {
        sendspin_hub->ToggleLed();
        return true;
    }
    if (cmd == "HOME_SINGLE" || cmd == "HOME_DOUBLE" || cmd == "HOME_TRIPLE") {
        if (home_button_entity != nullptr && state.broadcast) {
            const char* event_type = (cmd == "HOME_SINGLE")   ? "single_press"
                                     : (cmd == "HOME_DOUBLE") ? "double_press"
                                                              : "triple_press";
            home_button_entity->Trigger(event_type, state.broadcast);
        }
        return true;
    }
    if (const auto value = ParseControlInteger(cmd, "CONFIG_SET_VOLUME:")) {
        if (*value < 0 || *value > 100) return false;
        state.PersistVolume(static_cast<double>(*value) / 100.0);
        return true;
    }
    if (const auto value = ParseControlInteger(cmd, "CONFIG_SET_MIC_MUTE:")) {
        if (*value != 0 && *value != 1) return false;
        const bool muted = (*value == 0);
        state.PersistMuted(muted);
        state.PlayMuteToggleSound(muted);
        return true;
    }
    return false;
}

void Application::RegisterRuntime() {
    // Run calls this only after audio, entities, API and voice setup complete.
    assert(music_player && announce_player && satellite && server && mic_mute_gpio &&
           sound_watcher && network_switch_ptr);
    satellite_tick_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (satellite_tick_fd >= 0) {
        itimerspec ts{};
        ts.it_value.tv_sec = 0;
        ts.it_value.tv_nsec = 50'000'000;  // first fire 50 ms
        ts.it_interval.tv_sec = 0;
        ts.it_interval.tv_nsec = 50'000'000;  // every 50 ms
        if (::timerfd_settime(satellite_tick_fd, 0, &ts, nullptr) == 0) {
            server->AddAuxFd(satellite_tick_fd, [this]() {
                std::uint64_t expirations = 0;
                while (::read(satellite_tick_fd, &expirations, sizeof(expirations)) > 0) {
                }
                OnPeriodicTick();
            });
        } else {
            LVA_LOGW(kTag, "timerfd_settime failed; satellite ticks disabled");
            ::close(satellite_tick_fd);
            satellite_tick_fd = -1;
        }
    } else {
        LVA_LOGW(kTag, "timerfd_create failed; satellite ticks disabled");
    }

    // tr-buttond owns the input device and forwards gestures here.
    control_listener = std::make_unique<lva::tr::ControlListener>(
        [this](const std::string& cmd) { return HandleControl(cmd); });
    {
        const int ctl_fd = control_listener->Start();
        if (ctl_fd >= 0) {
            server->AddAuxFd(ctl_fd, [this] { control_listener->OnReadable(); });
        } else {
            LVA_LOGW(kTag, "button control listener disabled");
        }
    }

    // sendspin-cpp delivers its callbacks from loop() on this thread.
    if (state.preferences.sendspin_enabled && !SetSendspinRunning(true)) {
        LVA_LOGW(kTag, "Sendspin start failed; retrying");
    }
    sendspin_tick_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (sendspin_tick_fd >= 0) {
        itimerspec ts{};
        ts.it_value.tv_nsec = 33'000'000;
        ts.it_interval.tv_nsec = 33'000'000;
        if (::timerfd_settime(sendspin_tick_fd, 0, &ts, nullptr) == 0) {
            server->AddAuxFd(sendspin_tick_fd, [this] {
                std::uint64_t expirations = 0;
                while (::read(sendspin_tick_fd, &expirations, sizeof(expirations)) > 0) {
                }
                // Enabled but not running (e.g. PulseAudio was not up yet):
                // retry every 5 s.
                static auto next_retry = std::chrono::steady_clock::now();
                if (state.preferences.sendspin_enabled && !sendspin_hub->IsStarted() &&
                    std::chrono::steady_clock::now() >= next_retry) {
                    next_retry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    if (SetSendspinRunning(true)) {
                        LVA_LOGI(kTag, "Sendspin started on retry");
                    }
                }
                sendspin_hub->Loop();
            });
        } else {
            LVA_LOGE(kTag, "sendspin timerfd_settime failed");
            ::close(sendspin_tick_fd);
            sendspin_tick_fd = -1;
        }
    } else {
        LVA_LOGE(kTag, "sendspin timerfd_create failed");
    }
}

}  // namespace lva::app
