// Kept apart from ServerState.cpp: api.pb.h clashes with <cmath>'s log().
#include "state/ServerState.h"

#include "protocol/MessageRegistry.h"

#include "api.pb.h"

namespace lva::state {

void ServerState::NotifyHa(const std::string& id, const std::string& title,
                           const std::string& message) const {
    if (!broadcast) return;
    ::HomeassistantActionRequest req;
    req.set_service("persistent_notification.create");
    auto add = [&req](const char* key, const std::string& value) {
        auto* kv = req.add_data();
        kv->set_key(key);
        kv->set_value(value);
    };
    add("notification_id", id);
    add("title", title);
    add("message", message);
    broadcast(lva::proto::kIdHomeassistantActionRequest, req);
}

}  // namespace lva::state
