// Kept apart from ServerState.cpp: api.pb.h clashes with <cmath>'s log().
#include "state/ServerState.h"

#include "protocol/MessageRegistry.h"

#include "api.pb.h"

namespace lva::state {

namespace {

void FillNotificationRequest(const std::string& id,
                             const std::string& title,
                             const std::string& message,
                             ::HomeassistantActionRequest* req) {
    req->set_service("persistent_notification.create");
    auto add = [req](const char* key, const std::string& value) {
        auto* kv = req->add_data();
        kv->set_key(key);
        kv->set_value(value);
    };
    add("notification_id", id);
    add("title", title);
    add("message", message);
}

}  // namespace

void ServerState::NotifyHa(const std::string& id, const std::string& title,
                           const std::string& message) {
    pending_notifications_[id] = PendingNotification{title, message};
    if (!broadcast || !has_voice_client ||
        !has_voice_client()) {
        return;
    }

    ::HomeassistantActionRequest req;
    FillNotificationRequest(id, title, message, &req);
    if (broadcast(lva::proto::kIdHomeassistantActionRequest, req)) {
        pending_notifications_.erase(id);
    }
}

void ServerState::FlushPendingNotifications() {
    if (pending_notifications_.empty() || !broadcast ||
        !has_voice_client || !has_voice_client()) {
        return;
    }

    for (auto it = pending_notifications_.begin();
         it != pending_notifications_.end();) {
        ::HomeassistantActionRequest req;
        FillNotificationRequest(it->first, it->second.title,
                                it->second.message, &req);
        if (broadcast(lva::proto::kIdHomeassistantActionRequest, req)) {
            it = pending_notifications_.erase(it);
        } else {
            break;
        }
    }
}

}  // namespace lva::state
