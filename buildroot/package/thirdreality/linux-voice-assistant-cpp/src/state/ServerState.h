
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/message_lite.h>

#include "entities/Entity.h"
#include "state/Preferences.h"
#include "state/PreferencesWriter.h"
#include "tr/NetworkPolicy.h"

namespace lva::state {

class ServerState {
   public:
    explicit ServerState(std::filesystem::path network_policy_path = "/data/conf/network.json")
        : network_policy(std::move(network_policy_path)) {}
    lva::tr::NetworkPolicy network_policy;
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

    // Runtime side effects are supplied by the application, never discovered
    // through concrete service pointers. Invoked on the main loop only.
    struct PreferenceEffects {
        std::function<void(double)> volume_changed;
        std::function<void(bool)> muted_changed;
        std::function<void(bool)> mute_sound;
        std::function<void(int, double)> wake_sensitivity_changed;
        std::function<void(double)> stop_sensitivity_changed;
        std::function<void(int, int)> mic_levels_changed;
    };
    void SetPreferenceEffects(PreferenceEffects effects) { effects_ = std::move(effects); }

    using VoiceMessageHandler = std::function<bool(
        std::uint32_t, const ::google::protobuf::MessageLite&)>;
    VoiceMessageHandler handle_voice_message;

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

    bool StartPreferencesWriter() {
        return preferences_writer_.Start(preferences_path, preferences);
    }

    // True acknowledges queuing, not durable persistence. The worker reports
    // write failures and retries; StopPreferencesWriter flushes on exit.
    bool SavePreferences(bool force = false) {
        return preferences_writer_.Queue(preferences, force);
    }
    bool RequestPreferencesRecovery() {
        return preferences_writer_.Recover(preferences);
    }
    std::uint64_t PreferencesRevision() const {
        return preferences_writer_.Revision();
    }
    bool CanApplyExternalPreference(const std::string& key, std::uint64_t revision) const {
        return preferences_writer_.CanApplyExternal(key, revision);
    }
    bool StopPreferencesWriter() {
        return preferences_writer_.Stop();
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
    PreferenceEffects effects_;
    PreferencesWriter preferences_writer_;
    struct PendingNotification {
        std::string title;
        std::string message;
    };

    std::map<std::string, PendingNotification> pending_notifications_;
};

}  // namespace lva::state
