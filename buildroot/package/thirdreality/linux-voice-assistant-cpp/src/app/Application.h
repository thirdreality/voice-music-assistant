#pragma once

#include <atomic>
#include <memory>

#include "app/CliOptions.h"
#include "protocol/MdnsPublisher.h"
#include "state/ServerState.h"

namespace lva::audio {
class LibMpvPlayer;
class PcmRingBuffer;
class AudioCapture;
class WebRtcProcessor;
class WakeWordEngine;
}  // namespace lva::audio
namespace lva::satellite {
class Satellite;
class LocalStt;
class LocalTts;
}  // namespace lva::satellite
namespace lva::sendspin {
class SendspinHub;
}
namespace lva::proto {
class ApiServer;
}
namespace lva::tr {
class MicMuteGpio;
class ModelDownloader;
class SoundConfWatcher;
class Supervisor;
class SupervisorHttpServer;
class ControlListener;
}  // namespace lva::tr
namespace lva::entities {
class MediaPlayerEntity;
class MuteSwitchEntity;
class SelectEntity;
class UpdateEntity;
class NetworkSwitchEntity;
class EventEntity;
}  // namespace lva::entities

namespace lva::app {

// Owns every runtime service. Callbacks borrow this stable, non-movable object.
// Shutdown stops dispatch/producers before releasing their dependencies.
class Application {
   public:
    explicit Application(CliOptions options);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    int Run();

   private:
    bool InitializeState();
    bool SetupAudio();
    void PrepareSpeechModel();
    bool SttModelReady() const;
    std::string DownloadNotice() const;
    void RegisterEntities();
    void SetupSendspin();
    bool SetSendspinRunning(bool run);
    bool StartApi();
    void LoadWakeModels();
    void StartVoice();
    void StartWakeDetection();
    void RegisterRuntime();
    void OnPeriodicTick();
    bool HandleControl(const std::string& command);
    void BindPreferenceEffects();
    void Shutdown();

    CliOptions cli;
    lva::state::ServerState state;
    bool stopped = false;
    bool preferences_corrupt = false;
    bool prefs_dirty = false;
    bool use_openwakeword = false;
    bool use_local_voice = false;
    bool restart_for_backend = false;
    bool sendspin_active = false;
    bool tap_playback_pending = false;
    bool tap_ha_was_playing = false;
    std::atomic<std::int64_t> last_wake_dispatch_ns{0};
    std::filesystem::path wakeword_dir;

    std::unique_ptr<lva::tr::MicMuteGpio> mic_mute_gpio;
    std::unique_ptr<lva::audio::LibMpvPlayer> music_player, announce_player;
    std::unique_ptr<lva::audio::PcmRingBuffer> wake_ring, linear_wake_ring, satellite_ring,
        local_stt_ring;
    std::unique_ptr<lva::tr::ModelDownloader> stt_model_downloader;
    std::unique_ptr<lva::audio::WebRtcProcessor> webrtc_processor;
    std::unique_ptr<lva::audio::AudioCapture> audio_capture;
    std::unique_ptr<lva::sendspin::SendspinHub> sendspin_hub;
    lva::proto::MdnsPublisher sendspin_mdns, mdns;
    std::unique_ptr<lva::proto::ApiServer> server;
    std::unique_ptr<lva::audio::WakeWordEngine> wakeword_engine, linear_wakeword_engine;
    std::unique_ptr<lva::satellite::Satellite> satellite;
    std::unique_ptr<lva::satellite::LocalStt> local_stt;
    std::unique_ptr<lva::satellite::LocalTts> local_tts;
    std::unique_ptr<lva::tr::SoundConfWatcher> sound_watcher;
    std::unique_ptr<lva::tr::Supervisor> supervisor;
    std::unique_ptr<lva::tr::SupervisorHttpServer> supervisor_http;
    std::unique_ptr<lva::tr::ControlListener> control_listener;

    // Observers into state.entities; never used after the entity owners clear.
    lva::entities::MediaPlayerEntity* media_player_entity = nullptr;
    lva::entities::MuteSwitchEntity* mute_switch_entity = nullptr;
    lva::entities::SelectEntity* backend_select_ptr = nullptr;
    lva::entities::UpdateEntity* update_entity_ptr = nullptr;
    lva::entities::NetworkSwitchEntity* network_switch_ptr = nullptr;
    lva::entities::EventEntity* home_button_entity = nullptr;
    int satellite_tick_fd = -1;
    int sendspin_tick_fd = -1;
};

}  // namespace lva::app
