// Unit test for ParseConversationReply(). Exit code 0 = pass.

#include <cstdio>
#include <string>

#include "satellite/ConversationReply.h"

namespace {

int g_failures = 0;

void Expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

}  // namespace

int main() {
    using lva::satellite::ParseConversationReply;

    {
        const auto r = ParseConversationReply(R"({"response": {
            "response": {"speech": {"plain": {"speech": "Turned on the light",
                                              "extra_data": null}},
                         "response_type": "action_done"},
            "conversation_id": "01JABC",
            "continue_conversation": false}})");
        Expect(r.speech == "Turned on the light", "full reply: speech");
        Expect(r.conversation_id == "01JABC", "full reply: conversation_id");
        Expect(!r.continue_conversation, "full reply: continue=false");
    }
    {
        const auto r = ParseConversationReply(R"({"response": {
            "response": {"speech": {"plain": {"speech": "Which room?"}}},
            "continue_conversation": true}})");
        Expect(r.speech == "Which room?", "follow-up: speech");
        Expect(r.continue_conversation, "follow-up: continue=true");
        Expect(r.conversation_id.empty(), "follow-up: no conversation_id");
    }
    {
        const auto r = ParseConversationReply(R"({"response": {
            "response": {"speech": {}}, "conversation_id": "x"}})");
        Expect(r.speech.empty(), "no plain speech: empty");
        Expect(r.conversation_id == "x", "no plain speech: id kept");
    }
    for (const char* bad : {"", "not json", "[]", "{}", R"({"response": 5})",
                            R"({"response": {"response": "str"}})"}) {
        const auto r = ParseConversationReply(bad);
        Expect(r.speech.empty() && r.conversation_id.empty() &&
                   !r.continue_conversation,
               bad);
    }

    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
