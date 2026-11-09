#include "app/Application.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>

#include "app/Signals.h"
#include "audio/AudioCapture.h"
#include "audio/LibMpvPlayer.h"
#include "audio/PcmRingBuffer.h"
#include "audio/WakeWordEngine.h"
#include "audio/WebRtcProcessor.h"
#include "entities/EventEntity.h"
#include "entities/MediaPlayerEntity.h"
#include "entities/MuteSwitchEntity.h"
#include "entities/NetworkSwitchEntity.h"
#include "entities/SelectEntity.h"
#include "entities/UpdateEntity.h"
#include "protocol/ApiServer.h"
#include "satellite/LocalStt.h"
#include "satellite/LocalTts.h"
#include "satellite/Satellite.h"
#include "sendspin/SendspinHub.h"
#include "tr/ControlListener.h"
#include "tr/DeviceEffects.h"
#include "tr/MicMuteGpio.h"
#include "tr/ModelDownloader.h"
#include "tr/SoundConfWatcher.h"
#include "tr/Supervisor.h"
#include "tr/SupervisorHttpServer.h"
#include "tr/SysInfo.h"
#include "tr/SystemVolume.h"
#ifdef LVA_DIAG_LOG
#include "util/MemProbe.h"
#endif
#include "util/Log.h"

namespace lva::app {
namespace {
constexpr const char* kTag = "main";
}

namespace {
CliOptions ConfigureLogging(CliOptions options) {
    lva::log::SetLevel(options.debug ? lva::log::Level::kDebug : lva::log::Level::kInfo);
    if (options.debug) lva::log::SetLogFile("/usr/share/thirdreality/log/lva.log", 5u << 20);
#ifdef LVA_DIAG_LOG
    lva::memprobe::Start();
#endif
    LVA_LOGI(kTag,
             "linux-voice-assistant-cpp %s starting "
             "(name=%s host=%s port=%u prefs=%s)",
             kVersion, options.device_name.c_str(), options.host.c_str(),
             static_cast<unsigned>(options.port), options.preferences_file.c_str());

    return options;
}

std::string ReadMacFromDeviceJson(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};
    std::ifstream f(path);
    if (!f) return {};
    nlohmann::json j;
    try {
        f >> j;
    } catch (...) {
        return {};
    }
    if (!j.is_object()) return {};
    if (auto it = j.find("device"); it != j.end() && it->is_object()) {
        if (auto mit = it->find("macAddress"); mit != it->end() && mit->is_string()) {
            return mit->get<std::string>();
        }
    }
    return {};
}

}  // namespace

Application::Application(CliOptions options) : cli(ConfigureLogging(std::move(options))) {
    BindPreferenceEffects();
}

Application::~Application() { Shutdown(); }

int Application::Run() {
    try {
        if (!InitializeState()) return 1;
        if (!SetupAudio()) return 2;
        RegisterEntities();
        if (!StartApi()) return 1;
        LoadWakeModels();
        StartVoice();
        StartWakeDetection();
        RegisterRuntime();
        const int rc = server->Run();
        Shutdown();
        return rc;
    } catch (const std::exception& error) {
        LVA_LOGE(kTag, "application failed: %s", error.what());
    } catch (...) {
        LVA_LOGE(kTag, "application failed: unknown exception");
    }
    Shutdown();
    return 1;
}

void Application::Shutdown() {
    if (stopped) return;
    stopped = true;
    // No event callbacks may run after this point. Signal targets are cleared
    // while their objects are alive, including on partial startup failure.
    SetSignalServer(nullptr);
    SetSignalWakeEngine(nullptr);
    if (server) server->Stop();
    server.reset();
    control_listener.reset();
    for (int* fd : {&satellite_tick_fd, &sendspin_tick_fd}) {
        if (*fd >= 0) ::close(*fd);
        *fd = -1;
    }
    if (supervisor_http) supervisor_http->Stop();
    supervisor_http.reset();
    mdns.Stop();
    sendspin_mdns.Stop();
    sendspin_active = false;
    if (media_player_entity) media_player_entity->SetSendspin(nullptr);
    if (sendspin_hub) sendspin_hub->Stop();
    if (wakeword_engine) wakeword_engine->Stop();
    if (linear_wakeword_engine) linear_wakeword_engine->Stop();
    if (satellite) satellite->StopLocalVoice();
    if (local_stt) local_stt->Stop();
    if (local_tts) local_tts->Stop();
    if (stt_model_downloader) stt_model_downloader->Cancel();
    if (audio_capture) audio_capture->Stop();

    state.on_client_authenticated = {};
    state.on_client_disconnected = {};
    state.handle_voice_message = {};
    state.SetPreferenceEffects({});
    // Stop clears completion/drain callbacks before destroying their targets.
    if (music_player) music_player->Stop();
    if (announce_player) announce_player->Stop();
    if (media_player_entity) media_player_entity->SetVoiceActions({});
    satellite.reset();
    // Entity destruction cancels OTA checks and detaches player state callbacks.
    state.entities.clear();
    media_player_entity = nullptr;
    mute_switch_entity = nullptr;
    backend_select_ptr = nullptr;
    update_entity_ptr = nullptr;
    network_switch_ptr = nullptr;
    home_button_entity = nullptr;
    supervisor.reset();
    if (!state.StopPreferencesWriter())
        LVA_LOGW(kTag, "final preferences flush failed; latest changes may not be saved");
    lva::tr::StopDeviceEffects();
    // Remaining owners are destroyed normally after all producers have stopped.
    const int signo = ShutdownSignal();
    if (signo != 0) LVA_LOGI(kTag, "exiting after signal %d", signo);
}

bool Application::InitializeState() {
    state.name = cli.device_name;
    state.friendly_name = cli.device_name;
    {
        const lva::tr::DeviceInfo dev = lva::tr::ReadDeviceInfo();
        if (!dev.firmware_version.empty()) {
            state.version = dev.firmware_version;
        } else {
            LVA_LOGW(kTag,
                     "device.firmwareVersion missing; "
                     "falling back to package version %s",
                     kVersion);
            state.version = kVersion;
        }
        if (!dev.mac_address.empty()) {
            state.mac_address = dev.mac_address;
        }
    }
    state.esphome_version = "2025.9.0";
    if (state.mac_address.empty()) {
        state.mac_address = ReadMacFromDeviceJson("/data/conf/device.json");
    }
    std::transform(state.mac_address.begin(), state.mac_address.end(), state.mac_address.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    state.preferences_path = cli.preferences_file;
    use_openwakeword = (cli.wakeword_type == "open");
    wakeword_dir = use_openwakeword ? "/usr/share/thirdreality/wakewords/openwakeword"
                                    : "/usr/share/thirdreality/wakewords/microwakeword";
    state.wakeword_dir = wakeword_dir;
    state.preferences =
        lva::state::Preferences::LoadFromFile(cli.preferences_file, nullptr, &preferences_corrupt);
    if (!state.StartPreferencesWriter() || !lva::tr::StartDeviceEffects()) {
        LVA_LOGE(kTag, "failed to start background settings/hardware workers");
        return false;
    }

    if (state.preferences.volume.has_value()) {
        state.volume.store(*state.preferences.volume, std::memory_order_relaxed);
        lva::tr::SetSystemVolumeSilent(static_cast<int>(*state.preferences.volume * 100.0 + 0.5));
    }
    state.muted.store(state.preferences.is_mic_muted(), std::memory_order_relaxed);
    state.mic_volume_live.store(state.preferences.mic_volume, std::memory_order_relaxed);
    {
        const double delay_s =
            state.preferences.continue_conversation_delay.value_or(cli.continue_conversation_delay);
        state.continue_conversation_delay_ns = static_cast<std::int64_t>(delay_s * 1e9);
    }

    return true;
}

}  // namespace lva::app
