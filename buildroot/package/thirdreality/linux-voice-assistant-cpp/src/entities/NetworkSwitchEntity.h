#pragma once
#include <future>
#include <functional>
#include "entities/Entity.h"
namespace lva::state { class ServerState; }
namespace lva::entities {
class NetworkSwitchEntity final : public Entity {
 public:
    using ApplyLauncher = std::function<std::future<int>()>;
    NetworkSwitchEntity(std::uint32_t key, state::ServerState& state,
                        ApplyLauncher launcher = {});
    void OnListEntities(const ResponseSink&) override;
    void OnSubscribeStates(const ResponseSink&) override;
    void OnCommand(const google::protobuf::MessageLite&, std::uint32_t,
                   const ResponseSink&) override;
    void OnPeriodicTick(bool automatic_requests_busy);
 private:
    void FailApply(const char* reason);
    state::ServerState& state_;
    bool applied_;
    bool pending_ = false;
    bool script_done_ = false;
    std::future<int> apply_;
    ApplyLauncher launcher_;
};
}
