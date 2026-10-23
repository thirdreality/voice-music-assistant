
#pragma once

#include <cstdint>
#include <string>

#include "audio/IAudioPlayer.h"
#include "entities/Entity.h"

namespace lva::state { class ServerState; }
namespace lva::sendspin { class SendspinHub; }

namespace lva::entities {

class MediaPlayerEntity final : public Entity {
   public:
    MediaPlayerEntity(std::uint32_t key,
                      lva::state::ServerState& state,
                      lva::audio::IAudioPlayer* player);

    void OnListEntities(const ResponseSink& sink) override;
    void OnSubscribeStates(const ResponseSink& sink) override;
    void OnCommand(const ::google::protobuf::MessageLite& request,
                   std::uint32_t request_msg_type_id,
                   const ResponseSink& sink) override;

    void BroadcastState();
    bool IsPlaying() const;
    void PausePlayback();
    // TAP when nothing plays: resume paused mpv music, else ask the
    // Sendspin group to play.
    void ResumePlayback();

    // Sendspin is a second media source next to the mpv music player, as
    // in ESPHome's speaker_source media player. Main loop only.
    void SetSendspin(lva::sendspin::SendspinHub* hub) { sendspin_ = hub; }
    void OnSendspinActiveChanged(bool active);
    // Mute requested by the Sendspin server.
    void OnSendspinMuteRequest(bool muted);

   private:
    enum class PlayState { kIdle, kPlaying, kPaused };
    enum class Source { kNone, kMpv, kSendspin };
    void EmitState(const ResponseSink& sink) const;
    void OnPlayerStateChanged(lva::audio::PlayerState s);
    lva::audio::IAudioPlayer* CommandPlayer() const;
    bool SendspinActive() const;
    // HA command / TAP target: an HA announcement keeps priority, then the
    // Sendspin stream.
    bool SendspinIsCommandTarget() const;
    void ApplyMute(bool mute);
    void PlaySendspinUri(const std::string& uri);

    lva::state::ServerState&  state_;
    lva::audio::IAudioPlayer* player_;     // not owned
    lva::sendspin::SendspinHub* sendspin_ = nullptr;  // not owned
    // Last source that played on the media pipeline; PLAY goes back to it.
    Source     last_source_     = Source::kNone;
    lva::audio::PlayerState last_music_state_ = lva::audio::PlayerState::kIdle;
    PlayState  play_state_      = PlayState::kIdle;
    bool       muted_           = false;
    double     pre_mute_volume_ = 1.0;
};

}  // namespace lva::entities
