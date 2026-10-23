// In-process Sendspin (Music Assistant) player.
//
// Owns the sendspin-cpp client, the PulseAudio sink, the Sendspin
// identity/settings files and the music LED effect. The HA media player
// (MediaPlayerEntity) treats it as one media source next to mpv, following
// ESPHome's sendspin media_source + speaker_source split.
//
// Threads: every method and callback runs on the main loop, except the
// audio path, which stays on the sendspin-cpp sync thread.

#pragma once

#include <functional>
#include <memory>
#include <string>

namespace lva::sendspin {

class SendspinHub {
   public:
    struct Config {
        std::string name;              // friendly name shown in MA
        std::string product_name;
        std::string manufacturer;
        std::string software_version;
        std::string mac_address;       // lowercase, colon-separated; may be empty
        int         initial_volume = 100;  // 0-100
    };

    struct Callbacks {
        // A stream started (true) or ended / was dropped (false) on this
        // speaker.
        std::function<void(bool active)> on_active_changed;
        // The Sendspin server changed the player volume / mute.
        std::function<void(int percent)> on_volume_request;
        std::function<void(bool muted)>  on_mute_request;
    };

    SendspinHub(Config config, Callbacks callbacks);
    ~SendspinHub();

    SendspinHub(const SendspinHub&)            = delete;
    SendspinHub& operator=(const SendspinHub&) = delete;

    bool Start();
    void Stop();  // blocking; sends client/goodbye
    bool IsStarted() const;

    // Call from the main loop every ~33 ms.
    void Loop();

    // True while a stream plays on this speaker.
    bool IsActive() const;
    bool IsGroupPlaying() const;

    // Group-wide controller commands.
    void Play();
    void Pause();

    // Leaves the group (this speaker only). Sent only while the group
    // plays, like ESPHome's SendspinHub::leave_group().
    void LeaveGroup();

    // Another media source takes the speaker: drop the stream's audio and
    // leave the group (sent whenever a stream is open, whatever the cached
    // group state says).
    void Deactivate();
    // The other source is done. If the yielded stream is still open once
    // the server had time to act on the leave (it kept streaming), play it
    // again. Checked from Loop().
    void Reactivate();
    // Another source started again before the grace period ran out.
    void CancelReactivate();

    void ConnectTo(const std::string& url);

    // Local volume / mute, reported to the server. Volume updates are
    // ignored while muted (LVA mutes by setting volume 0).
    void SetVolume(int percent);
    void SetMuted(bool muted);

    // Two independent duck requests (voice turn, HA announcement); the
    // stream is ducked while either is set.
    void SetDucked(bool ducked);
    void SetAnnouncementDucked(bool ducked);

    void ToggleLed();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lva::sendspin
