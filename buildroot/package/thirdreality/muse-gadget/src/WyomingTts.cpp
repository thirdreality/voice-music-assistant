#include "WyomingTts.h"
#include <nlohmann/json.hpp>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <pulse/error.h>
#include <pulse/simple.h>

namespace muse {
namespace {
class Socket {
 public:
    ~Socket() { if (fd >= 0) close(fd); }
    int fd{-1};
};
class Reader {
 public:
    Reader(int fd, std::atomic<bool>& cancel) : fd_(fd), cancel_(cancel),
        deadline_(std::chrono::steady_clock::now() + std::chrono::seconds(60)) {}
    std::string Read(size_t size) {
        std::string out(size, '\0');
        for (size_t at = 0; at < size;) {
            if (cancel_.load()) throw std::runtime_error("reply cancelled");
            if (std::chrono::steady_clock::now() >= deadline_) throw std::runtime_error("Piper response timed out");
            pollfd p{fd_, POLLIN, 0};
            const int ready = poll(&p, 1, 100);
            if (ready < 0) { if (errno == EINTR) continue; throw std::runtime_error("Piper socket poll failed"); }
            if (!ready) continue;
            const ssize_t n = recv(fd_, out.data() + at, size - at, 0);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n <= 0) throw std::runtime_error("Piper stream closed before audio-stop");
            at += static_cast<size_t>(n);
        }
        return out;
    }
    std::string Line() {
        std::string out;
        while (out.size() < 8192) {
            const auto byte = Read(1);
            if (byte == "\n") return out;
            out += byte;
        }
        throw std::runtime_error("Piper event header too large");
    }
 private:
    int fd_;
    std::atomic<bool>& cancel_;
    std::chrono::steady_clock::time_point deadline_;
};
}
WyomingTts::WyomingTts(std::string host, uint16_t port, std::string voice)
    : host_(std::move(host)), voice_(std::move(voice)), port_(port) {}
bool WyomingTts::Synthesize(const std::string& text, std::atomic<bool>& cancel,
                            SpeechAudio& audio, std::string& error) {
    audio = {};
    try {
        if (cancel.load()) throw std::runtime_error("reply cancelled");
        addrinfo hints{}, *addresses = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        const auto port = std::to_string(port_);
        if (getaddrinfo(host_.c_str(), port.c_str(), &hints, &addresses))
            throw std::runtime_error("cannot resolve Piper host");
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owned(addresses, freeaddrinfo);
        Socket socket;
        for (auto* address = addresses; address; address = address->ai_next) {
            socket.fd = ::socket(address->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, address->ai_protocol);
            if (socket.fd < 0) continue;
            int rc = connect(socket.fd, address->ai_addr, address->ai_addrlen);
            if (rc < 0 && errno == EINPROGRESS) {
                for (int i = 0; i < 50 && !cancel.load(); ++i) {
                    pollfd p{socket.fd, POLLOUT, 0};
                    if (poll(&p, 1, 100) > 0) {
                        int status = 0; socklen_t length = sizeof(status);
                        rc = getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &status, &length);
                        if (status) rc = -1;
                        break;
                    }
                }
            }
            if (rc == 0) break;
            close(socket.fd); socket.fd = -1;
        }
        if (socket.fd < 0) throw std::runtime_error("cannot connect to Piper");
        nlohmann::json data{{"text", text}};
        if (!voice_.empty()) data["voice"] = {{"name", voice_}};
        const auto body = data.dump();
        const auto request = nlohmann::json{{"type", "synthesize"}, {"data_length", body.size()}}.dump() + "\n" + body;
        for (size_t at = 0; at < request.size();) {
            if (cancel.load()) throw std::runtime_error("reply cancelled");
            const ssize_t n = send(socket.fd, request.data() + at, request.size() - at, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("Piper request write failed");
            at += static_cast<size_t>(n);
        }
        Reader reader(socket.fd, cancel);
        bool started = false;
        for (;;) {
            const auto header = nlohmann::json::parse(reader.Line());
            const size_t data_size = header.value("data_length", size_t{0});
            const size_t payload_size = header.value("payload_length", size_t{0});
            if (data_size > 65536 || payload_size > 1024 * 1024)
                throw std::runtime_error("Piper event exceeds size limit");
            auto metadata = header.value("data", nlohmann::json::object());
            if (data_size) metadata.update(nlohmann::json::parse(reader.Read(data_size)));
            const auto payload = reader.Read(payload_size);
            const auto type = header.value("type", "");
            if (type == "error") throw std::runtime_error("Piper synthesis rejected");
            if (type == "audio-start") {
                if (started) throw std::runtime_error("duplicate Piper audio-start");
                const int channels = metadata.value("channels", 0);
                audio.rate = metadata.value("rate", uint32_t{0});
                if (metadata.value("width", 0) != 2 || (channels != 1 && channels != 2) ||
                    audio.rate < 8000 || audio.rate > 48000)
                    throw std::runtime_error("unsupported Piper PCM format");
                audio.channels = static_cast<uint8_t>(channels);
                started = true;
            } else if (type == "audio-chunk") {
                if (!started || payload_size % (2 * audio.channels))
                    throw std::runtime_error("invalid Piper PCM chunk");
                if (metadata.value("rate", audio.rate) != audio.rate ||
                    metadata.value("width", 2) != 2 || metadata.value("channels", int(audio.channels)) != audio.channels)
                    throw std::runtime_error("Piper PCM format changed mid-stream");
                if (audio.pcm.size() + payload_size > 16 * 1024 * 1024)
                    throw std::runtime_error("Piper audio exceeds size limit");
                audio.pcm.insert(audio.pcm.end(), payload.begin(), payload.end());
            } else if (type == "audio-stop") {
                if (!started || audio.pcm.empty()) throw std::runtime_error("Piper returned no audio");
                std::cerr << "[muse] Piper synthesis complete: " << audio.pcm.size()
                          << " bytes, " << audio.rate << " Hz, " << int(audio.channels) << " channels\n";
                return true;
            }
        }
    } catch (const nlohmann::json::exception&) { error = "invalid Piper event JSON"; return false; }
      catch (const std::exception& e) { error = e.what(); return false; }
}
bool PlaySpeech(const SpeechAudio& audio, std::atomic<bool>& cancel, std::string& error) {
    if (cancel.load()) { error = "reply cancelled"; return false; }
    if (audio.pcm.empty() || !audio.rate || !audio.channels) { error = "empty reply audio"; return false; }
    pa_sample_spec spec{PA_SAMPLE_S16LE, audio.rate, audio.channels};
    pa_buffer_attr attr{UINT32_MAX, pa_usec_to_bytes(200000, &spec), UINT32_MAX, UINT32_MAX, UINT32_MAX};
    int rc = 0;
    pa_simple* stream = pa_simple_new(nullptr, "muse-gadget", PA_STREAM_PLAYBACK, nullptr,
                                      "Muse reply", &spec, nullptr, &attr, &rc);
    if (!stream) { error = std::string("PulseAudio open failed: ") + pa_strerror(rc); return false; }
    std::unique_ptr<pa_simple, decltype(&pa_simple_free)> owned(stream, pa_simple_free);
    std::cerr << "[muse] reply playback started (PulseAudio)\n";
    for (size_t at = 0; at < audio.pcm.size();) {
        if (cancel.load()) { pa_simple_flush(stream, &rc); error = "reply cancelled"; return false; }
        const size_t take = std::min<size_t>(audio.rate / 50 * audio.channels * 2, audio.pcm.size() - at);
        if (pa_simple_write(stream, audio.pcm.data() + at, take, &rc) < 0) {
            error = std::string("PulseAudio write failed: ") + pa_strerror(rc); return false;
        }
        at += take;
    }
    if (cancel.load()) { pa_simple_flush(stream, &rc); error = "reply cancelled"; return false; }
    if (pa_simple_drain(stream, &rc) < 0) { error = std::string("PulseAudio drain failed: ") + pa_strerror(rc); return false; }
    std::cerr << "[muse] reply playback completed\n";
    return true;
}
}
