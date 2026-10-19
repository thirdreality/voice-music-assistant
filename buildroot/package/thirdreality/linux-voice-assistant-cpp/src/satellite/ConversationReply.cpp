#include "satellite/ConversationReply.h"

#include <nlohmann/json.hpp>

namespace lva::satellite {

namespace {

const nlohmann::json* Child(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    const auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}

}  // namespace

ConversationReply ParseConversationReply(const std::string& data) {
    ConversationReply out;
    const auto root =
        nlohmann::json::parse(data, nullptr, /*allow_exceptions=*/false);
    const auto* outer = Child(root, "response");
    if (outer == nullptr) return out;

    if (const auto* id = Child(*outer, "conversation_id");
        id != nullptr && id->is_string()) {
        out.conversation_id = id->get<std::string>();
    }
    if (const auto* cont = Child(*outer, "continue_conversation");
        cont != nullptr && cont->is_boolean()) {
        out.continue_conversation = cont->get<bool>();
    }

    const nlohmann::json* node = Child(*outer, "response");
    for (const char* key : {"speech", "plain", "speech"}) {
        if (node == nullptr) return out;
        node = Child(*node, key);
    }
    if (node != nullptr && node->is_string()) {
        out.speech = node->get<std::string>();
    }
    return out;
}

}  // namespace lva::satellite
