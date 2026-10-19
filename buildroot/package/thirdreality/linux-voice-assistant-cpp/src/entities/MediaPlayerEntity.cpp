#include "entities/MediaPlayerEntity.h"

#include <algorithm>

#include "audio/LibMpvPlayer.h"
#include "audio/WebRtcProcessor.h"
#include "protocol/MessageRegistry.h"
#include "satellite/Satellite.h"
#include "state/ServerState.h"
#include "util/Log.h"

#include "api.pb.h"

namespace lva::entities {

namespace {

constexpr const char* kTag         = "media";
constexpr const char* kObjectId    = "linux_voice_assistant_media_player";
constexpr const char* kDisplayName = "Media Player";

constexpr std::uint32_t kFeatureFlags =
    (1u <<  0) | (1u <<  1) | (1u <<  2) | (1u <<  3) |
    (1u << 11) | (1u << 14) | (1u << 23);

}  // namespace

MediaPlayerEntity::MediaPlayerEntity(std::uint32_t key,
                                     lva::state::ServerState& state,
                                     lva::audio::IAudioPlayer* player)
    : Entity(key, kObjectId, kDisplayName), state_(state), player_(player) {
    pre_mute_volume_ = state_.volume.load(std::memory_order_relaxed);
    if (player_ != nullptr) {
        auto* mpv = dynamic_cast<lva::audio::LibMpvPlayer*>(player_);
        if (mpv != nullptr) {
            mpv->SetStateChangedCallback(
                [this](lva::audio::PlayerState s) {
                    OnPlayerStateChanged(s);
                });
        }
    }

    if (state_.announce_player != nullptr &&
        state_.announce_player != player_) {
        auto* mpv = dynamic_cast<lva::audio::LibMpvPlayer*>(
            state_.announce_player);
        if (mpv != nullptr) {
            mpv->SetStateChangedCallback(
                [this](lva::audio::PlayerState s) {
                    OnPlayerStateChanged(s);
                });
        }
    }
}

void MediaPlayerEntity::OnPlayerStateChanged(lva::audio::PlayerState s) {
    // AEC runs only while music or TTS plays (see SetFarEndActive). Check
    // both players; either one may have triggered this callback.
    if (state_.webrtc_processor != nullptr) {
        auto is_active = [](lva::audio::PlayerState st) {
            return st == lva::audio::PlayerState::kLoading ||
                   st == lva::audio::PlayerState::kPlaying;
        };
        bool far_end = is_active(s);
        if (state_.music_player != nullptr) {
            far_end = far_end || is_active(state_.music_player->State());
        }
        if (state_.announce_player != nullptr) {
            far_end = far_end || is_active(state_.announce_player->State());
        }
        state_.webrtc_processor->SetFarEndActive(far_end);
    }

    const auto music_state = state_.music_player != nullptr
        ? state_.music_player->State()
        : lva::audio::PlayerState::kIdle;
    const auto announce_state = state_.announce_player != nullptr
        ? state_.announce_player->State()
        : lva::audio::PlayerState::kIdle;
    const auto is_playing = [](lva::audio::PlayerState state) {
        return state == lva::audio::PlayerState::kLoading ||
               state == lva::audio::PlayerState::kPlaying;
    };

    PlayState new_local = PlayState::kIdle;
    if (is_playing(music_state) || is_playing(announce_state)) {
        new_local = PlayState::kPlaying;
    } else if (music_state == lva::audio::PlayerState::kPaused ||
               announce_state == lva::audio::PlayerState::kPaused) {
        new_local = PlayState::kPaused;
    }
    if (new_local == play_state_) {
        return;  // no-op, don't spam HA
    }
    play_state_ = new_local;

    BroadcastState();
}

bool MediaPlayerEntity::IsPlaying() const {
    const auto is_playing = [](lva::audio::IAudioPlayer* player) {
        if (player == nullptr) return false;
        const auto state = player->State();
        return state == lva::audio::PlayerState::kLoading ||
               state == lva::audio::PlayerState::kPlaying;
    };
    if (is_playing(player_)) return true;
    auto* command_player = CommandPlayer();
    return command_player != player_ && is_playing(command_player);
}

lva::audio::IAudioPlayer* MediaPlayerEntity::CommandPlayer() const {
    auto* announce = state_.announce_player;
    if (announce != nullptr) {
        const auto announce_state = announce->State();
        const bool announce_active =
            announce_state == lva::audio::PlayerState::kLoading ||
            announce_state == lva::audio::PlayerState::kPlaying ||
            announce_state == lva::audio::PlayerState::kPaused;
        const bool satellite_owns_announce =
            state_.satellite != nullptr &&
            state_.satellite->OwnsAnnouncePlayback();
        if (announce_active && !satellite_owns_announce) return announce;
    }
    return player_;
}

void MediaPlayerEntity::PausePlayback() {
    const auto is_playing = [](lva::audio::IAudioPlayer* player) {
        if (player == nullptr) return false;
        const auto state = player->State();
        return state == lva::audio::PlayerState::kLoading ||
               state == lva::audio::PlayerState::kPlaying;
    };
    auto* command_player = CommandPlayer();
    if (is_playing(player_)) player_->Pause();
    if (command_player != player_ && is_playing(command_player)) {
        command_player->Pause();
    }
    const auto state = command_player != nullptr
        ? command_player->State()
        : lva::audio::PlayerState::kIdle;
    OnPlayerStateChanged(state);
}

void MediaPlayerEntity::BroadcastState() {
    if (!state_.broadcast) return;

    ::MediaPlayerStateResponse resp;
    resp.set_key(key());
    switch (play_state_) {
        case PlayState::kIdle:
            resp.set_state(::MEDIA_PLAYER_STATE_IDLE);
            break;
        case PlayState::kPlaying:
            resp.set_state(::MEDIA_PLAYER_STATE_PLAYING);
            break;
        case PlayState::kPaused:
            resp.set_state(::MEDIA_PLAYER_STATE_PAUSED);
            break;
    }
    resp.set_volume(static_cast<float>(
        state_.volume.load(std::memory_order_relaxed)));
    resp.set_muted(muted_);
    state_.broadcast(lva::proto::kIdMediaPlayerStateResponse, resp);
    LVA_LOGD(kTag, "broadcast state -> %d", static_cast<int>(play_state_));
}

void MediaPlayerEntity::OnListEntities(const ResponseSink& sink) {
    ::ListEntitiesMediaPlayerResponse resp;
    resp.set_object_id(object_id());
    resp.set_key(key());
    resp.set_name(name());
    resp.set_supports_pause(true);
    resp.set_feature_flags(kFeatureFlags);
    sink(lva::proto::kIdListEntitiesMediaPlayerResponse, resp);
}

void MediaPlayerEntity::EmitState(const ResponseSink& sink) const {
    ::MediaPlayerStateResponse resp;
    resp.set_key(key());
    switch (play_state_) {
        case PlayState::kIdle:
            resp.set_state(::MEDIA_PLAYER_STATE_IDLE);
            break;
        case PlayState::kPlaying:
            resp.set_state(::MEDIA_PLAYER_STATE_PLAYING);
            break;
        case PlayState::kPaused:
            resp.set_state(::MEDIA_PLAYER_STATE_PAUSED);
            break;
    }
    resp.set_volume(static_cast<float>(
        state_.volume.load(std::memory_order_relaxed)));
    resp.set_muted(muted_);
    sink(lva::proto::kIdMediaPlayerStateResponse, resp);
}

void MediaPlayerEntity::OnSubscribeStates(const ResponseSink& sink) {
    EmitState(sink);
}

void MediaPlayerEntity::OnCommand(
    const ::google::protobuf::MessageLite& request,
    std::uint32_t request_msg_type_id,
    const ResponseSink& sink) {
    if (request_msg_type_id != lva::proto::kIdMediaPlayerCommandRequest) return;
    const auto& cmd = static_cast<const ::MediaPlayerCommandRequest&>(request);
    if (cmd.key() != key()) return;

    bool state_changed = false;

    if (cmd.has_volume()) {
        const double v = std::clamp(static_cast<double>(cmd.volume()),
                                    0.0, 1.0);
        LVA_LOGI(kTag, "command: volume -> %.3f", v);
        state_.PersistVolume(v);
        if (!muted_) {
            pre_mute_volume_ = v;
        }
        state_changed = true;
    }

    if (cmd.has_command()) {
        const auto command = cmd.command();
        switch (command) {
            case ::MEDIA_PLAYER_COMMAND_PLAY: {
                LVA_LOGI(kTag, "command: PLAY (resume)");
                auto* command_player = CommandPlayer();
                if (command_player != nullptr) command_player->Resume();
                play_state_ = PlayState::kPlaying;
                state_changed = true;
                break;
            }
            case ::MEDIA_PLAYER_COMMAND_PAUSE: {
                LVA_LOGI(kTag, "command: PAUSE");
                auto* command_player = CommandPlayer();
                if (command_player != nullptr) command_player->Pause();
                play_state_ = PlayState::kPaused;
                state_changed = true;
                break;
            }
            case ::MEDIA_PLAYER_COMMAND_STOP: {
                LVA_LOGI(kTag, "command: STOP");
                auto* command_player = CommandPlayer();
                if (command_player != nullptr) command_player->Stop();
                play_state_ = PlayState::kIdle;
                state_changed = true;
                break;
            }
            case ::MEDIA_PLAYER_COMMAND_MUTE:
                if (!muted_) {
                    pre_mute_volume_ = state_.volume.load(
                        std::memory_order_relaxed);
                    state_.PersistVolume(0.0);
                    muted_ = true;
                    state_changed = true;
                }
                LVA_LOGI(kTag, "command: MUTE");
                break;
            case ::MEDIA_PLAYER_COMMAND_UNMUTE:
                if (muted_) {
                    state_.PersistVolume(pre_mute_volume_);
                    muted_ = false;
                    state_changed = true;
                }
                LVA_LOGI(kTag, "command: UNMUTE -> %.3f", pre_mute_volume_);
                break;
            default:
                LVA_LOGD(kTag, "command: ignoring unsupported subcommand %d",
                         static_cast<int>(command));
                break;
        }
    }

    if (cmd.has_media_url()) {
        const bool announcement =
            cmd.has_announcement() && cmd.announcement();
        LVA_LOGI(kTag, "command: PLAY_MEDIA url=%s announcement=%d",
                 cmd.media_url().c_str(), announcement ? 1 : 0);
        lva::audio::IAudioPlayer* target =
            announcement && state_.announce_player != nullptr
                ? state_.announce_player
                : player_;
        if (announcement && state_.satellite != nullptr) {
            state_.satellite->PrepareForMediaAnnouncement();
        }
        if (target != nullptr) {
            target->Play(cmd.media_url());
        }
        play_state_ = PlayState::kPlaying;
        state_changed = true;
    }

    if (state_changed) {
        EmitState(sink);
    }
}

}  // namespace lva::entities
