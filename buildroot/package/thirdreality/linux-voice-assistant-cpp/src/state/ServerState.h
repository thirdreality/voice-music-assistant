
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/message_lite.h>

#include "entities/Entity.h"
#include "state/Preferences.h"

namespace lva::audio    { class AudioCapture; class WebRtcProcessor; class IAudioPlayer; class WakeWordEngine; }
namespace lva::satellite { class Satellite; }
namespace lva::tr       { class MicMuteGpio; class Supervisor; }
namespace lva::entities { class MediaPlayerEntity; class MuteSwitchEntity; }
namespace lva::sendspin { class SendspinHub; }

namespace lva::state {

class ServerState {
   public:
    std::string name;            // e.g. "3RSPK-A8E291152F53"
    std::string friendly_name;   // human-readable fallback to name
    std::string mac_address;     // colon-separated, lowercase
    std::string version;         // our project version
    std::string esphome_version; // pinned to api.proto's contract

    Preferences preferences;
    std::filesystem::path preferences_path;
    std::filesystem::path wakeword_dir;

    std::atomic<bool>   muted{false};
    std::atomic<double> volume{1.0};
    std::atomic<int>    mic_volume_live{100}; // 1-4000 (percent), read by capture thread

    std::int64_t continue_conversation_delay_ns = 500'000'000LL;

    class ::lva::audio::AudioCapture* audio_capture = nullptr;

    class ::lva::satellite::Satellite* satellite = nullptr;

    class ::lva::audio::WebRtcProcessor* webrtc_processor = nullptr;

    class ::lva::audio::WakeWordEngine* wakeword_engine = nullptr;

    class ::lva::tr::MicMuteGpio* mic_mute_gpio = nullptr;

    class ::lva::entities::MediaPlayerEntity* media_player_entity = nullptr;

    class ::lva::entities::MuteSwitchEntity* mute_switch_entity = nullptr;

    class ::lva::audio::IAudioPlayer* announce_player = nullptr;

    class ::lva::audio::IAudioPlayer* music_player = nullptr;

    // In-process Sendspin player; null while disabled.
    class ::lva::sendspin::SendspinHub* sendspin = nullptr;

    class ::lva::tr::Supervisor* supervisor = nullptr;

    using BroadcastFn = std::function<bool(
        std::uint32_t msg_type_id,
        const ::google::protobuf::MessageLite& msg)>;
    BroadcastFn broadcast;

    using ConnectionPredicate = std::function<bool()>;
    ConnectionPredicate has_voice_client;

    using ConnectionEventFn = std::function<void()>;
    ConnectionEventFn on_client_authenticated;

    ConnectionEventFn on_client_disconnected;

    std::vector<std::unique_ptr<entities::Entity>> entities;

    bool SavePreferences() const {
        return preferences.SaveToFile(preferences_path);
    }

    void PersistVolume(double new_volume);

    void PersistMuted(bool new_muted);

    void PlayMuteToggleSound(bool muted);

    void PersistThinkingSound(bool enabled);

    void PersistContinueConversationDelay(double seconds);

    // n is 1-based.
    void PersistWakeWordSensitivity(int n, double value);
    void PersistStopWordSensitivity(double value);

    // Each clamps to its valid range.
    void PersistMicAutoGain(int value);
    void PersistMicNoiseSuppression(int value);
    void PersistMicVolume(int value);

    // "ha" or "local".
    void PersistVoiceBackend(const std::string& backend);

    void PersistSendspinEnabled(bool enabled);

    // Shows a notification in HA (persistent_notification.create). The id
    // replaces an earlier notification with the same id. If HA is offline,
    // the latest undelivered notification is replayed after authentication.
    void NotifyHa(const std::string& id, const std::string& title,
                  const std::string& message);

    void FlushPendingNotifications();

    // Words per minute for the local TTS.
    void PersistTtsRate(int rate_wpm);

   private:
    struct PendingNotification {
        std::string title;
        std::string message;
    };

    std::map<std::string, PendingNotification> pending_notifications_;
};

}  // namespace lva::state
