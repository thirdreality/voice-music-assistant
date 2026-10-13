#pragma once

#include <nlohmann/json.hpp>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace muse {
// Correlate NDJSON assistant events to this upload, never to other app chats.
class ReplyTracker {
 public:
    // session empty + accept_parentless true = main chat (official behavior):
    // speak the next parentless assistant reply following this turn.
    explicit ReplyTracker(std::string session = "", bool accept_parentless = false)
        : requested_session_(std::move(session)), accept_parentless_(accept_parentless) {}
    bool SetAcknowledgement(const nlohmann::json& ack);
    bool Feed(const std::string& bytes);
    std::vector<std::string> TakeReady();
    bool agent_busy() const { return agent_busy_; }
 private:
    struct Message { std::string text; bool done{false}; bool taken{false}; };
    bool Event(const nlohmann::json& event);
    std::string requested_session_, session_, pending_line_;
    bool accept_parentless_{false};
    std::deque<nlohmann::json> before_ack_;
    std::set<std::string> roots_;
    std::map<std::string, Message> messages_;
    std::vector<std::string> order_;
    uint64_t last_seq_{0};
    bool agent_busy_{false};
};
}
