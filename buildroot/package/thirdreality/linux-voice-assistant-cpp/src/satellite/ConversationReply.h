#pragma once

#include <string>

namespace lva::satellite {

struct ConversationReply {
    std::string speech;
    std::string conversation_id;
    bool        continue_conversation = false;
};

// Parses the conversation.process response_data:
//   {"response": {"response": {"speech": {"plain": {"speech": "..."}}},
//                 "conversation_id": "...", "continue_conversation": bool}}
// Separate TU: nlohmann/json clashes with the `log` extension in api.pb.h.
ConversationReply ParseConversationReply(const std::string& data);

}  // namespace lva::satellite
