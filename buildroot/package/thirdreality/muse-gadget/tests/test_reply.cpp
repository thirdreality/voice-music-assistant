#include "ReplyTracker.h"
#include <cstdlib>
#include <iostream>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL " << message << '\n'; std::exit(1); }
}
std::string Event(const char* kind, const char* id, const char* parent, const char* text, int seq) {
    nlohmann::json payload{{"message_id", id}};
    if (parent) payload["reply_to_message_id"] = parent;
    if (text) payload["text"] = text;
    return nlohmann::json{{"type", "event"}, {"seq", seq}, {"event", kind}, {"payload", payload}}.dump() + "\n";
}
}
int main() {
    muse::ReplyTracker tracker;
    // A fast assistant can finish before the HTTP acknowledgement arrives.
    const auto start = Event("delta.message_start", "reply-a", "note-1", nullptr, 1);
    tracker.Feed(start.substr(0, 7));
    tracker.Feed(start.substr(7));
    tracker.Feed(Event("delta.text_append", "reply-a", nullptr, "Hello.", 2));
    tracker.Feed(Event("delta.message_done", "reply-a", nullptr, nullptr, 3));
    Check(tracker.TakeReady().empty(), "no playback before upload correlation");
    Check(tracker.SetAcknowledgement({{"result", {{"message_id", "note-1"}}}}), "nested upload acknowledgement");
    Check(tracker.TakeReady() == std::vector<std::string>{"Hello."}, "fragmented pre-ack reply must be replayed");
    tracker.Feed(Event("delta.message_done", "reply-a", nullptr, nullptr, 3));
    tracker.Feed(nlohmann::json{{"type", "event"}, {"seq", 4}, {"event", "message.assistant"},
        {"payload", {{"message_id", "reply-a"}, {"display_text", "Hello."}}}}.dump() + "\n");
    Check(tracker.TakeReady().empty(), "done plus full-message event must not speak twice");
    tracker.Feed(Event("delta.message_start", "foreign", "another-note", nullptr, 5));
    tracker.Feed(Event("delta.text_append", "foreign", nullptr, "Do not speak this.", 6));
    tracker.Feed(Event("delta.message_done", "foreign", nullptr, nullptr, 7));
    tracker.Feed(Event("delta.message_start", "unbound", nullptr, nullptr, 8));
    tracker.Feed(Event("delta.text_append", "unbound", nullptr, "Do not speak this either.", 9));
    Check(tracker.TakeReady().empty(), "ignore unrelated and uncorrelated app messages");
    tracker.Feed(Event("delta.message_start", "reply-z", "reply-a", nullptr, 10));
    tracker.Feed(Event("delta.text_append", "reply-z", nullptr, "Second reply.", 11));
    tracker.Feed(Event("delta.message_done", "reply-z", nullptr, nullptr, 12));
    Check(tracker.TakeReady() == std::vector<std::string>{"Second reply."}, "accept descendants of this turn");
    tracker.Feed(nlohmann::json{{"type", "event"}, {"seq", 13}, {"event", "message.assistant"},
        {"payload", {{"message_id", "reply-full"}, {"reply_to_message_id", nullptr},
                     {"parent_message_id", "note-1"}, {"display_text_ready", nullptr},
                     {"content", "Full reply."}}}}.dump() + "\n");
    Check(tracker.TakeReady() == std::vector<std::string>{"Full reply."}, "nullable event fields and parent fallback");
    muse::ReplyTracker isolated("voice-session");
    Check(isolated.SetAcknowledgement({{"result", {{"message_id", "note-voice"}, {"session_id", "voice-session"}}}}),
          "server must acknowledge the requested voice session");
    isolated.Feed(nlohmann::json{{"type", "event"}, {"seq", 1}, {"event", "delta.message_done"},
        {"payload", {{"message_id", "own-parentless"}, {"session_id", "voice-session"},
                     {"reply_to_message_id", nullptr}, {"transcript", "Voice reply."}}}}.dump() + "\n");
    Check(isolated.TakeReady() == std::vector<std::string>{"Voice reply."}, "parentless transcript in isolated session");
    isolated.Feed(nlohmann::json{{"type", "event"}, {"seq", 2}, {"event", "delta.message_done"},
        {"payload", {{"message_id", "other-room"}, {"session_id", "app-chat"}, {"transcript", "Private reply."}}}}.dump() + "\n");
    Check(isolated.TakeReady().empty(), "never speak another app session");
    bool session_rejected = false;
    try {
        muse::ReplyTracker wrong("requested");
        wrong.SetAcknowledgement({{"message_id", "note"}, {"session_id", "other"}});
    } catch (const std::exception&) { session_rejected = true; }
    Check(session_rejected, "do not silently fall back to the main chat");
    muse::ReplyTracker no_id;
    Check(!no_id.SetAcknowledgement({{"ok", true}}), "missing correlation must fail closed");
    bool rejected = false;
    try { no_id.Feed(std::string(65537, 'x')); } catch (const std::exception&) { rejected = true; }
    Check(rejected, "bound subscription memory");
    muse::ReplyTracker main_chat("", true);
    Check(main_chat.SetAcknowledgement({{"result", {{"message_id", "note-main"}}}}),
          "main-chat upload acknowledgement");
    main_chat.Feed(nlohmann::json{{"type", "event"}, {"seq", 1}, {"event", "delta.message_start"},
        {"payload", {{"message_id", "m1"}, {"reply_to_message_id", nullptr}}}}.dump() + "\n");
    main_chat.Feed(nlohmann::json{{"type", "event"}, {"seq", 2}, {"event", "delta.text_append"},
        {"payload", {{"message_id", "m1"}, {"text", "Hi there."}}}}.dump() + "\n");
    main_chat.Feed(nlohmann::json{{"type", "event"}, {"seq", 3}, {"event", "delta.message_done"},
        {"payload", {{"message_id", "m1"}}}}.dump() + "\n");
    Check(main_chat.TakeReady() == std::vector<std::string>{"Hi there."},
          "main chat speaks parentless assistant reply");

    std::cout << "PASS Muse reply correlation tests\n";
}
