#include "entities/MediaPlayerEntity.h"

#include <algorithm>
#include <cstring>

#include "audio/LibMpvPlayer.h"
#include "audio/MicroWakeWord.h"
#include "audio/WebRtcProcessor.h"
#include "protocol/MessageRegistry.h"
#include "satellite/Satellite.h"
#include "sendspin/SendspinHub.h"
#include "state/ServerState.h"
#include "util/Log.h"

#include "api.pb.h"

namespace lva::entities {

namespace {

constexpr const char* kTag         = "media";
constexpr const char* kObjectId    = "linux_voice_assistant_media_player";
constexpr const char* kDisplayName = "Media Player";

// aioesphomeapi MediaPlayerEntityFeature. HA ignores this below API 1.11
// and uses the same fixed set; keep them equal.
constexpr std::uint32_t kFeaturePause         = 1u << 0;
constexpr std::uint32_t kFeatureVolumeSet     = 1u << 2;
constexpr std::uint32_t kFeatureVolumeMute    = 1u << 3;
constexpr std::uint32_t kFeaturePlayMedia     = 1u << 9;
constexpr std::uint32_t kFeatureStop          = 1u << 12;
constexpr std::uint32_t kFeaturePlay          = 1u << 14;
constexpr std::uint32_t kFeatureBrowseMedia   = 1u << 17;
constexpr std::uint32_t kFeatureMediaAnnounce = 1u << 20;
constexpr std::uint32_t kFeatureFlags =
    kFeaturePause | kFeatureVolumeSet | kFeatureVolumeMute |
    kFeaturePlayMedia | kFeatureStop | kFeaturePlay | kFeatureBrowseMedia |
    kFeatureMediaAnnounce;

constexpr const char* kSendspinUriPrefix = "sendspin://";

bool IsActiveState(lva::audio::PlayerState state) {
    return state == lva::audio::PlayerState::kLoading ||
           state == lva::audio::PlayerState::kPlaying;
}

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
    if (IsActiveState(music_state)) last_source_ = Source::kMpv;
    const bool music_finished =
        music_state == lva::audio::PlayerState::kIdle &&
        last_music_state_ != lva::audio::PlayerState::kIdle;
    last_music_state_ = music_state;
    if (sendspin_ != nullptr) {
        if (music_finished) {
            // mpv took the speaker from an open Sendspin stream; give it
            // back if the server never ended that stream.
            sendspin_->Reactivate();
        } else if (IsActiveState(music_state)) {
            sendspin_->CancelReactivate();
        }
    }

    // Duck Sendspin under an HA announcement, like ESPHome's on_announcement
    // mixer ducking. Voice turns duck through the satellite.
    if (sendspin_ != nullptr) {
        sendspin_->SetAnnouncementDucked(CommandPlayer() != player_);
    }

    // Same priority as ESPHome's speaker_source: the active media source
    // decides, Sendspin being one of them. Sendspin has no paused state on
    // the wire; a stopped group reads as idle.
    PlayState new_local = PlayState::kIdle;
    if (IsActiveState(music_state) || IsActiveState(announce_state) ||
        SendspinActive()) {
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
    if (SendspinActive()) return true;
    auto* command_player = CommandPlayer();
    return command_player != player_ && is_playing(command_player);
}

bool MediaPlayerEntity::SendspinActive() const {
    return sendspin_ != nullptr && sendspin_->IsActive();
}

// speaker_source's target: the active source, else the last one.
bool MediaPlayerEntity::SendspinIsCommandTarget() const {
    if (sendspin_ == nullptr) return false;
    if (CommandPlayer() != player_) return false;  // HA announcement first
    return SendspinActive() || last_source_ == Source::kSendspin;
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
    if (SendspinActive()) sendspin_->Pause();
    const auto state = command_player != nullptr
        ? command_player->State()
        : lva::audio::PlayerState::kIdle;
    OnPlayerStateChanged(state);
}

void MediaPlayerEntity::ResumePlayback() {
    if (player_ != nullptr &&
        player_->State() == lva::audio::PlayerState::kPaused &&
        last_source_ != Source::kSendspin) {
        player_->Resume();
        return;
    }
    if (sendspin_ != nullptr) sendspin_->Play();
}

void MediaPlayerEntity::OnSendspinActiveChanged(bool active) {
    if (active) {
        // Sendspin takes the media pipeline; stop mpv music like the
        // orchestrator stops the active source before play_uri().
        last_source_ = Source::kSendspin;
        if (player_ != nullptr &&
            player_->State() != lva::audio::PlayerState::kIdle) {
            LVA_LOGI(kTag, "sendspin stream: stopping mpv music");
            player_->Stop();
        }
    }
    if (state_.webrtc_processor != nullptr) {
        state_.webrtc_processor->SetExternalFarEndActive(active);
    }
    lva::audio::MicroWakeWord::SetFarEndTelemetry(active);
    const auto music_state = player_ != nullptr
        ? player_->State()
        : lva::audio::PlayerState::kIdle;
    OnPlayerStateChanged(music_state);
}

void MediaPlayerEntity::OnSendspinMuteRequest(bool muted) {
    if (muted == muted_) return;
    LVA_LOGI(kTag, "sendspin: mute -> %d", muted ? 1 : 0);
    ApplyMute(muted);
    BroadcastState();
}

// LVA mutes by setting the volume to 0. Sendspin gets a real mute flag
// first, so the server keeps the pre-mute volume (the hub ignores volume
// updates while muted).
void MediaPlayerEntity::ApplyMute(bool mute) {
    if (mute == muted_) return;
    muted_ = mute;
    if (sendspin_ != nullptr) sendspin_->SetMuted(mute);
    if (mute) {
        pre_mute_volume_ = state_.volume.load(std::memory_order_relaxed);
        state_.PersistVolume(0.0);
    } else {
        state_.PersistVolume(pre_mute_volume_);
    }
}

void MediaPlayerEntity::PlaySendspinUri(const std::string& uri) {
    if (sendspin_ == nullptr) {
        LVA_LOGW(kTag, "PLAY_MEDIA %s: Sendspin disabled", uri.c_str());
        return;
    }
    const std::string target = uri.substr(std::strlen(kSendspinUriPrefix));
    if (target.empty()) {
        LVA_LOGW(kTag, "PLAY_MEDIA: invalid URI '%s'", uri.c_str());
        return;
    }
    if (player_ != nullptr &&
        player_->State() != lva::audio::PlayerState::kIdle) {
        player_->Stop();
    }
    if (target == "current") {
        sendspin_->Play();
    } else {
        sendspin_->ConnectTo("ws://" + target);
    }
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
                // Back to the last media source, like speaker_source's
                // fallback to last_source. With no paused mpv media to
                // resume, PLAY goes to the Sendspin group.
                auto* command_player = CommandPlayer();
                const bool mpv_paused =
                    player_ != nullptr &&
                    player_->State() == lva::audio::PlayerState::kPaused;
                if (command_player == player_ && sendspin_ != nullptr &&
                    (last_source_ == Source::kSendspin || !mpv_paused)) {
                    LVA_LOGI(kTag, "command: PLAY (sendspin group)");
                    sendspin_->Play();
                    break;  // state follows the stream
                }
                if (command_player == nullptr ||
                    command_player->State() !=
                        lva::audio::PlayerState::kPaused) {
                    LVA_LOGI(kTag, "command: PLAY ignored (nothing to resume)");
                    break;
                }
                LVA_LOGI(kTag, "command: PLAY (resume)");
                command_player->Resume();
                play_state_ = PlayState::kPlaying;
                state_changed = true;
                break;
            }
            case ::MEDIA_PLAYER_COMMAND_PAUSE: {
                if (SendspinIsCommandTarget()) {
                    // Group wide, as in ESPHome's sendspin media source.
                    LVA_LOGI(kTag, "command: PAUSE (sendspin group)");
                    sendspin_->Pause();
                    break;
                }
                LVA_LOGI(kTag, "command: PAUSE");
                auto* command_player = CommandPlayer();
                if (command_player != nullptr) command_player->Pause();
                play_state_ = PlayState::kPaused;
                state_changed = true;
                break;
            }
            case ::MEDIA_PLAYER_COMMAND_STOP: {
                if (SendspinIsCommandTarget()) {
                    // This speaker only: leave the group.
                    LVA_LOGI(kTag, "command: STOP (leave sendspin group)");
                    sendspin_->LeaveGroup();
                    break;
                }
                LVA_LOGI(kTag, "command: STOP");
                auto* command_player = CommandPlayer();
                if (command_player != nullptr) command_player->Stop();
                play_state_ = PlayState::kIdle;
                state_changed = true;
                break;
            }
            case ::MEDIA_PLAYER_COMMAND_MUTE:
                if (!muted_) {
                    ApplyMute(true);
                    state_changed = true;
                }
                LVA_LOGI(kTag, "command: MUTE");
                break;
            case ::MEDIA_PLAYER_COMMAND_UNMUTE:
                if (muted_) {
                    ApplyMute(false);
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
        if (!announcement &&
            cmd.media_url().rfind(kSendspinUriPrefix, 0) == 0) {
            PlaySendspinUri(cmd.media_url());  // state follows the stream
        } else {
            if (!announcement && sendspin_ != nullptr) {
                // mpv takes the media pipeline: drop Sendspin audio and
                // leave the group (ESPHome stops the active source first).
                sendspin_->Deactivate();
            }
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
    }

    if (state_changed) {
        EmitState(sink);
    }
}

}  // namespace lva::entities
