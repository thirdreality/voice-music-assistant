#include "app/Application.h"
#include "app/Signals.h"
#include "audio/AudioCapture.h"
#include "audio/LibMpvPlayer.h"
#include "protocol/ApiServer.h"
#include "util/Log.h"

namespace lva::app {
namespace {
constexpr const char* kTag = "main";
}

bool Application::StartApi() {
    lva::proto::ApiServer::Options opts;
    opts.bind_address = cli.host;
    opts.port = cli.port;

    server = std::make_unique<lva::proto::ApiServer>(state, opts);
    SetSignalServer(server.get());

    InstallSignalHandlers();

    if (!server->Start()) {
        LVA_LOGE(kTag, "ApiServer::Start failed");
        SetSignalServer(nullptr);
        return false;
    }

    {
        lva::proto::MdnsPublisher::Options mdns_opts;
        mdns_opts.name = state.name;
        mdns_opts.port = cli.port;
        mdns_opts.mac = state.mac_address;
        mdns_opts.version = state.esphome_version;
        if (!mdns.Start(mdns_opts)) {
            LVA_LOGW(kTag,
                     "mDNS registration failed; device won't "
                     "be auto-discovered by HA");
        }
    }

    if (music_player->WakeupFd() >= 0) {
        server->AddAuxFd(music_player->WakeupFd(), [this] { music_player->DrainEvents(); });
    }
    if (announce_player->WakeupFd() >= 0) {
        server->AddAuxFd(announce_player->WakeupFd(), [this] { announce_player->DrainEvents(); });
    }
    if (music_player->DrainFd() >= 0) {
        server->AddAuxFd(music_player->DrainFd(), [this] { music_player->PollDrain(); });
    }
    if (announce_player->DrainFd() >= 0) {
        server->AddAuxFd(announce_player->DrainFd(), [this] { announce_player->PollDrain(); });
    }

    if (!audio_capture->Start()) {
        LVA_LOGW(kTag,
                 "audio capture failed to start; mic-using "
                 "features (wake word) will be unavailable");
    }

    return true;
}

}  // namespace lva::app
