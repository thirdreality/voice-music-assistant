#include "app/Application.h"
#include "entities/MediaPlayerEntity.h"
#include "sendspin/SendspinHub.h"
#include "util/Log.h"

namespace lva::app {
namespace {
constexpr const char* kTag = "main";
}

void Application::SetupSendspin() {
    // In-process Sendspin player: a second media source behind the HA
    // media player (ESPHome sendspin hub + media_source).
    sendspin_hub = std::make_unique<lva::sendspin::SendspinHub>(
        lva::sendspin::SendspinHub::Config{
            .name = state.friendly_name,
            .product_name = "Voice & Music Assistant",
            .manufacturer = "ThirdReality",
            .software_version = state.version,
            .mac_address = state.mac_address,
            .initial_volume =
                static_cast<int>(state.volume.load(std::memory_order_relaxed) * 100.0 + 0.5),
        },
        lva::sendspin::SendspinHub::Callbacks{
            // sendspin_active is cleared before Stop(), so callbacks fired
            // while stopping (e.g. at shutdown) are dropped here.
            .on_active_changed =
                [this](bool active) {
                    if (!sendspin_active) return;
                    media_player_entity->OnSendspinActiveChanged(active);
                },
            .on_volume_request =
                [this](int percent) {
                    if (!sendspin_active) return;
                    state.PersistVolume(static_cast<double>(percent) / 100.0);
                },
            .on_mute_request =
                [this](bool muted) {
                    if (!sendspin_active) return;
                    media_player_entity->OnSendspinMuteRequest(muted);
                },
        });
}

bool Application::SetSendspinRunning(bool run) {
    if (run) {
        if (!sendspin_hub->Start()) return false;
        sendspin_active = true;
        media_player_entity->SetSendspin(sendspin_hub.get());
        lva::proto::MdnsPublisher::Options opts;
        opts.name = state.name;
        opts.type = "_sendspin._tcp";
        opts.port = 8928;  // SendspinClientConfig::DEFAULT_SERVER_PORT
        opts.txt = {"path=/sendspin"};
        if (!sendspin_mdns.Start(opts)) {
            LVA_LOGW(kTag, "_sendspin._tcp mDNS registration failed");
        }
        return true;
    }
    sendspin_mdns.Stop();
    sendspin_active = false;
    media_player_entity->SetSendspin(nullptr);
    sendspin_hub->Stop();
    media_player_entity->OnSendspinActiveChanged(false);
    return false;
}

}  // namespace lva::app
