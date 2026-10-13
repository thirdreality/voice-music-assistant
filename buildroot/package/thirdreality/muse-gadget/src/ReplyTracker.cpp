#include "ReplyTracker.h"
#include <stdexcept>

namespace muse {
namespace {
std::string String(const nlohmann::json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) return "";
    return object[key].get<std::string>();
}
}
bool ReplyTracker::SetAcknowledgement(const nlohmann::json& ack) {
    const auto& value = ack.contains("result") && ack["result"].is_object() ? ack["result"] : ack;
    if (!requested_session_.empty()) {
        session_ = String(value, "session_id");
        if (session_ != requested_session_)
            throw std::runtime_error("Muse did not acknowledge the isolated voice session");
    }
    for (const char* key : {"message_id", "reply_to_message_id"}) {
        const std::string id = String(value, key);
        if (!id.empty()) roots_.insert(id);
    }
    if (roots_.empty()) return false;
    for (const auto& event : before_ack_) Event(event);
    before_ack_.clear();
    return true;
}
bool ReplyTracker::Feed(const std::string& bytes) {
    pending_line_ += bytes;
    bool activity = false;
    size_t at = 0;
    while (true) {
        const size_t end = pending_line_.find('\n', at);
        if (end == std::string::npos) break;
        if (end - at > 65536) throw std::runtime_error("Muse event line too large");
        if (end != at) {
            const auto event = nlohmann::json::parse(pending_line_.substr(at, end - at));
            if (roots_.empty()) {
                if (before_ack_.size() == 128) before_ack_.pop_front();
                before_ack_.push_back(event);
            } else {
                activity = Event(event) || activity;
            }
        }
        at = end + 1;
    }
    pending_line_.erase(0, at);
    if (pending_line_.size() > 65536) throw std::runtime_error("Muse event line too large");
    return activity;
}
bool ReplyTracker::Event(const nlohmann::json& event) {
    if (String(event, "type") != "event") return false;
    uint64_t seq = 0;
    if (event.contains("seq") && event["seq"].is_number_integer()) {
        const auto number = event["seq"].get<int64_t>();
        if (number > 0) seq = static_cast<uint64_t>(number);
    }
    if (seq && seq <= last_seq_) return false;
    if (seq) last_seq_ = seq;
    const std::string kind = String(event, "event");
    const auto payload = event.value("payload", nlohmann::json::object());
    if (!payload.is_object()) return false;
    if (kind == "agent.status" || kind == "task.status") {
        if (payload.contains("activity_code")) {
            const auto code = String(payload, "activity_code");
            agent_busy_ = !code.empty() && code != "online" && code != "idle";
        } else if (payload.contains("status")) {
            const auto status = String(payload, "status");
            agent_busy_ = !status.empty() && status != "completed" && status != "failed";
        }
        return true;
    }
    if (kind != "delta.message_start" && kind != "delta.text_append" &&
        kind != "delta.message_done" && kind != "message.assistant") return false;
    std::string id = String(payload, "message_id");
    if (id.empty()) id = String(event, "message_id");
    if (id.empty()) id = String(payload, "id");
    if (id.empty()) return false;
    const auto event_session = String(payload, "session_id");
    if (!session_.empty() && !event_session.empty() && event_session != session_) return false;
    auto it = messages_.find(id);
    if (it == messages_.end()) {
        auto parent = String(payload, "reply_to_message_id");
        if (parent.empty()) parent = String(payload, "parent_message_id");
        // Parentless events are accepted only in the explicitly isolated voice
        // session acknowledged by the server, never in arbitrary app chats.
        const bool session_ok = session_.empty() || event_session.empty() || event_session == session_;
        const bool matched = roots_.count(parent) || messages_.count(parent) ||
            (parent.empty() && session_ok &&
             ((!session_.empty() && event_session == session_) || accept_parentless_));
        if (!matched) return false;
        if (messages_.size() >= 16) throw std::runtime_error("too many Muse reply messages");
        it = messages_.emplace(id, Message{}).first;
        order_.push_back(id);
    }
    auto& message = it->second;
    if (message.taken) return false;
    if (kind == "delta.text_append" && !message.done) {
        message.text += String(payload, "text");
    } else if (kind == "delta.message_done" || kind == "message.assistant") {
        if (kind == "message.assistant" && payload.contains("display_text_ready") &&
            payload["display_text_ready"].is_boolean() && !payload["display_text_ready"].get<bool>()) return false;
        for (const char* key : {"display_text", "content", "transcript"}) {
            if (payload.contains(key) && payload[key].is_string()) {
                const auto text = payload[key].get<std::string>();
                if (!text.empty()) message.text = text;
                break;
            }
        }
        message.done = true;
    }
    if (message.text.size() > 16384) throw std::runtime_error("Muse reply text too large");
    return true;
}
std::vector<std::string> ReplyTracker::TakeReady() {
    std::vector<std::string> result;
    for (const auto& id : order_) {
        auto& message = messages_.at(id);
        if (message.done && !message.taken && !message.text.empty()) {
            result.push_back(message.text);
            message.taken = true;
        }
    }
    return result;
}
}
