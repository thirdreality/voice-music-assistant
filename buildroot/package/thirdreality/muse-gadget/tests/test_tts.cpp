#include "WyomingTts.h"
#include <nlohmann/json.hpp>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL " << message << '\n'; std::exit(1); }
}
std::string Read(int fd, size_t size) {
    std::string result(size, '\0');
    for (size_t at = 0; at < size;) {
        const auto n = recv(fd, result.data() + at, size - at, 0);
        Check(n > 0, "read fake Piper request");
        at += n;
    }
    return result;
}
void SendEvent(int fd, const char* type, const nlohmann::json& data, const std::string& payload = "") {
    const auto metadata = data.dump();
    const auto header = nlohmann::json{{"type", type}, {"data_length", metadata.size()}, {"payload_length", payload.size()}}.dump() + "\n";
    const auto wire = header + metadata + payload;
    // Deliberately fragment both event headers and binary payloads.
    for (size_t at = 0; at < wire.size();) {
        const auto n = send(fd, wire.data() + at, std::min<size_t>(3, wire.size() - at), MSG_NOSIGNAL);
        Check(n > 0, "send fake Piper event"); at += n;
    }
}
}
int main() {
    for (int mode = 0; mode < 3; ++mode) {
        const int server = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind fake Piper");
        socklen_t length = sizeof(address);
        Check(getsockname(server, reinterpret_cast<sockaddr*>(&address), &length) == 0, "resolve fake server port");
        Check(listen(server, 1) == 0, "listen for synthesis");
        std::thread peer([server, mode]() {
            const int client = accept(server, nullptr, nullptr);
            Check(client >= 0, "accept synthesis client");
            std::string line;
            for (;;) { const auto byte = Read(client, 1); if (byte == "\n") break; line += byte; }
            const auto header = nlohmann::json::parse(line);
            const auto body = nlohmann::json::parse(Read(client, header.at("data_length").get<size_t>()));
            Check(header.at("type") == "synthesize", "Wyoming synthesis request type");
            Check(body.at("text") == "Voice test.", "synthesis request body");
            Check(body.at("voice").at("name") == "test-voice", "explicit voice selection");
            if (mode == 2) {
                SendEvent(client, "error", {{"code", "test"}});
            } else {
                SendEvent(client, "audio-start", {{"rate", 22050}, {"width", 2}, {"channels", 1}});
                SendEvent(client, "audio-chunk", {{"rate", 22050}, {"width", 2}, {"channels", 1}}, std::string("\1\0\2\0", 4));
                if (mode == 0) SendEvent(client, "audio-stop", nlohmann::json::object());
            }
            close(client); close(server);
        });
        muse::WyomingTts tts("127.0.0.1", ntohs(address.sin_port), "test-voice");
        muse::SpeechAudio audio;
        std::atomic<bool> cancel{false};
        std::string error;
        const bool ok = tts.Synthesize("Voice test.", cancel, audio, error);
        peer.join();
        if (mode == 0) {
            Check(ok && audio.rate == 22050 && audio.channels == 1 && audio.pcm.size() == 4,
                  "fragmented Piper PCM synthesis");
        } else Check(!ok && !error.empty(), "reject truncated stream and synthesis error");
    }
    std::atomic<bool> cancel{true};
    muse::SpeechAudio empty;
    std::string error;
    Check(!muse::PlaySpeech(empty, cancel, error), "cancel before opening audio server");
    std::cout << "PASS Wyoming Piper protocol tests\n";
}
