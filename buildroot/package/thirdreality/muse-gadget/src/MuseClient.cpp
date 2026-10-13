#include "MuseClient.h"

#include "OpenSslCryptoBackend.h"
#include "ReplyTracker.h"
#include "WavWriter.h"

#include <xplat/noise/core/ClientSession.h>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netdb.h>
#include <poll.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <sstream>

#include <nlohmann/json.hpp>

namespace muse {
using namespace musegadgets::noise::core;
namespace {
constexpr size_t kScratch = 64 * 1024;
std::string RandomId();
constexpr char kNoteHead[] =
    "{\"message\":\"\",\"output_modality\":\"text\",\"items\":[{\"type\":\"file\","
    "\"mime_type\":\"audio/wav\",\"filename\":\"voice_note.wav\",\"data_base64\":\"";
constexpr char kNoteTail[] = "\"}]}";

size_t CurlWrite(char* ptr, size_t size, size_t count, void* context) {
    auto* out = static_cast<std::string*>(context);
    out->append(ptr, size * count);
    return size * count;
}

bool ResolveVm(const std::string& token, std::string& vm_id,
               std::string& vm_token, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) { error = "cannot initialize account request"; return false; }
    std::string response;
    std::string auth = "Authorization: Bearer " + token;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "X-API-Version: 1.0.0");
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.muse.ai/fetch_vms");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    // libcurl's built-in default CA path does not match this image; point it
    // at the system bundle so TLS verification succeeds.
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || status != 200) {
        error = "Muse account request failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    try {
        const auto root = nlohmann::json::parse(response);
        const auto& list = root.at("vm_list");
        const nlohmann::json* selected = nullptr;
        for (const auto& item : list) {
            if (!item.contains("vm_auth_token")) continue;
            if (!selected || item.value("default", false)) selected = &item;
        }
        if (!selected) { error = "Muse account has no VM"; return false; }
        vm_id = selected->value("vm_id", "");
        vm_token = selected->value("vm_auth_token", "");
        if (vm_id.empty()) {
            const std::string url = selected->value("vm_ws_url", selected->value("vm_url", ""));
            const auto scheme = url.find("://");
            const auto dot = scheme == std::string::npos ? std::string::npos : url.find('.', scheme + 3);
            if (dot != std::string::npos) vm_id = url.substr(scheme + 3, dot - scheme - 3);
        }
        if (vm_id.empty() || vm_token.empty()) { error = "Muse VM credentials are incomplete"; return false; }
        return true;
    } catch (const std::exception&) {
        error = "Muse account returned invalid JSON";
        return false;
    }
}

std::string QueryEscape(const std::string& input) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : input) {
        if (std::isalnum(c) || std::strchr("-_.!~*'()", c)) out.push_back(c);
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
    }
    return out;
}

class Connection {
 public:
    Connection() : session_(crypto_), ws_(ClientSession::kMaxOutboundWebSocketPayloadSize),
                   rx_(kScratch), transport_(kScratch), service_(kScratch), envelope_(kScratch) {}
    ~Connection() {
        if (ssl_) SSL_free(ssl_);
        if (ctx_) SSL_CTX_free(ctx_);
        if (fd_ >= 0) close(fd_);
    }

    bool Connect(const std::string& host, const std::string& vm_id,
                 const std::string& token, std::string& error) {
        addrinfo hints{}, *results = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        if (getaddrinfo(host.c_str(), "443", &hints, &results) != 0) {
            error = "cannot resolve Muse host"; return false;
        }
        for (addrinfo* it = results; it; it = it->ai_next) {
            fd_ = socket(it->ai_family, it->ai_socktype | SOCK_CLOEXEC, it->ai_protocol);
            if (fd_ >= 0 && connect(fd_, it->ai_addr, it->ai_addrlen) == 0) break;
            if (fd_ >= 0) close(fd_);
            fd_ = -1;
        }
        freeaddrinfo(results);
        if (fd_ < 0) { error = "cannot connect to Muse host"; return false; }
        // The official Linux SDK allows 20 seconds for upgrade/Noise setup.
        timeval timeout{20, 0};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) { error = "cannot initialize TLS"; return false; }
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_load_verify_locations(ctx_, "/etc/ssl/certs/ca-certificates.crt", nullptr) != 1 &&
            SSL_CTX_set_default_verify_paths(ctx_) != 1) {
            error = "cannot load CA certificates"; return false;
        }
        ssl_ = SSL_new(ctx_);
        SSL_set_tlsext_host_name(ssl_, host.c_str());
        SSL_set1_host(ssl_, host.c_str());
        SSL_set_fd(ssl_, fd_);
        if (SSL_connect(ssl_) != 1) { error = "Muse TLS handshake failed"; return false; }
        std::ostringstream request;
        request << "GET /v1/noise?vm_id=" << QueryEscape(vm_id) << " HTTP/1.1\r\n"
                << "Host: " << host << "\r\nAuthorization: Bearer " << token
                << "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                << "User-Agent: musegadget/0.1.0 (Linux aarch64) Python/3.12.10\r\n"
                << "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
        if (!WriteAll(reinterpret_cast<const uint8_t*>(request.str().data()), request.str().size())) {
            error = "Muse WebSocket upgrade write failed"; return false;
        }
        std::string header;
        std::array<char, 512> buf{};
        while (header.find("\r\n\r\n") == std::string::npos && header.size() < 4096) {
            const int n = SSL_read(ssl_, buf.data(), buf.size());
            if (n <= 0) break;
            header.append(buf.data(), n);
        }
        const size_t hdr_end = header.find("\r\n\r\n");
        if (hdr_end != std::string::npos) {
            const size_t body_at = hdr_end + 4;
            if (body_at < header.size())
                leftover_.assign(header.begin() + body_at, header.end());
        }
        if (header.rfind("HTTP/1.1 101", 0) != 0 && header.rfind("HTTP/1.0 101", 0) != 0) {
            error = "Muse WebSocket upgrade rejected (HTTP " +
                    (header.size() >= 12 ? header.substr(9, 3) : "unknown") + ")";
            return false;
        }
        auto m1 = session_.WriteHandshakeMessage1(ByteSpan(ws_.data(), ws_.size()));
        if (!m1.ok() || !SendFrame(ws_.data(), m1.size())) { error = "Noise message 1 failed"; return false; }
        std::cerr << "[muse] link: handshake msg1 " << m1.size() << " bytes\n";
        const auto handshake_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        ssize_t n = -1;
        do {
            n = ReceiveFrame(rx_);
            if (n != -2 && n != -3) break;
        } while (std::chrono::steady_clock::now() < handshake_deadline);
        if (n <= 0) {
            error = "Noise message 2 missing (WebSocket receive " + std::to_string(n) + ")";
            return false;
        }
        std::cerr << "[muse] link: handshake msg2 " << n << " bytes\n";
        size_t extra = 0;
        if (!session_.ReadHandshakeMessage2(ConstByteSpan(rx_.data(), n), ByteSpan(service_.data(), service_.size()), extra).ok()) {
            error = "Noise message 2 invalid"; return false;
        }
        std::cerr << "[muse] link: msg2 extra payload " << extra << " bytes\n";
        auto m3 = session_.WriteHandshakeMessage3(ConstByteSpan(), ByteSpan(ws_.data(), ws_.size()));
        if (!m3.ok() || !SendFrame(ws_.data(), m3.size()) || !session_.isEstablished()) {
            error = "Noise message 3 failed"; return false;
        }
        std::cerr << "[muse] link: handshake msg3 " << m3.size() << " bytes, established=" << session_.isEstablished() << "\n";
        timeout.tv_sec = 5;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        return true;
    }

    bool Open(int64_t id, const char* path, bool end_body, const char* accept = nullptr) {
        const std::string request_id = RandomId();
        std::array<HeaderView, 4> headers{{
            {StringView("x-request-id"), StringView(request_id.data(), request_id.size())},
            {StringView("x-app-id"), StringView("hatch-web")},
            {StringView("Content-Type"), StringView("application/json")},
            {StringView("accept"), StringView(accept ? accept : "application/json")},
        }};
        ApplicationRequestView request{StringView("POST"), StringView(path),
            Span<const HeaderView>(headers.data(), accept ? 4 : 3), ConstByteSpan(), end_body};
        auto result = session_.StartOutboundApplicationRequest(ServiceType::Daemon, id, request,
                                                                 ByteSpan(service_.data(), service_.size()), ByteSpan(envelope_.data(), envelope_.size()));
        return result.ok() && Flush();
    }
    bool Body(int64_t id, const uint8_t* data, size_t size, bool end) {
        BodyChunkView chunk{ConstByteSpan(data, size), end};
        auto result = session_.StartOutboundBodyChunk(ServiceType::Daemon, id, chunk,
                                                       ByteSpan(service_.data(), service_.size()), ByteSpan(envelope_.data(), envelope_.size()));
        return result.ok() && Flush();
    }
    bool WaitForSuccess(int64_t id, std::string& error) {
        std::array<HeaderView, 16> headers{};
        bool accepted = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            const ssize_t n = ReceiveFrame(rx_);
            if (n == -2 || n == -3) continue;  // keepalive or idle read timeout
            if (n <= 0) { error = "Muse closed the voice-note stream"; return false; }
            auto result = session_.ProcessInboundWebSocketPayload(
                ConstByteSpan(rx_.data(), n), ByteSpan(transport_.data(), transport_.size()), ByteSpan(service_.data(), service_.size()),
                Span<HeaderView>(headers.data(), headers.size()));
            if (!result.ok()) { error = "invalid Noise response"; return false; }
            if (result.frame_status != InboundFrameStatus::Complete || result.frame.stream_id != id) continue;
            bool ended = false;
            if (result.frame.kind == ServiceFrameKind::Response) {
                if (result.frame.response.status != 200) {
                    error = "Muse rejected voice note (HTTP " +
                            std::to_string(result.frame.response.status) + ")";
                    return false;
                }
                accepted = true;
                ended = result.frame.response.end_body;
            } else if (result.frame.kind == ServiceFrameKind::BodyChunk) {
                ended = result.frame.body_chunk.end_body;
            } else if (result.frame.kind == ServiceFrameKind::Reset) {
                error = "Muse reset the voice-note stream"; return false;
            }
            if (ended) {
                if (!accepted) { error = "voice-note stream ended without HTTP success"; return false; }
                std::cerr << "[muse] voice-note acknowledged (HTTP 200)\n";
                return true;
            }
        }
        error = "Muse voice-note acknowledgement timed out after 30s";
        return false;
    }

    bool WaitForSubscription(int64_t id, std::atomic<bool>* cancel, std::string& error) {
        cancel_ = cancel;
        std::array<HeaderView, 16> headers{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while ((!cancel || !cancel->load()) && std::chrono::steady_clock::now() < deadline) {
            const ssize_t n = ReceiveFrame(rx_);
            if (n == -2 || n == -3) continue;
            if (n <= 0) { error = "Muse subscription connection closed"; return false; }
            auto result = session_.ProcessInboundWebSocketPayload(
                ConstByteSpan(rx_.data(), n), ByteSpan(transport_.data(), transport_.size()),
                ByteSpan(service_.data(), service_.size()), Span<HeaderView>(headers));
            if (!result.ok()) { error = "invalid subscription response"; return false; }
            if (result.frame_status != InboundFrameStatus::Complete || result.frame.stream_id != id) continue;
            if (result.frame.kind == ServiceFrameKind::Response) {
                if (result.frame.response.status != 200 || result.frame.response.end_body) {
                    error = "Muse subscription rejected or ended (HTTP " + std::to_string(result.frame.response.status) + ")";
                    return false;
                }
                // Ignore initial snapshots: no user message has been posted yet.
                std::cerr << "[muse] reply subscription ready (HTTP 200)\n";
                return true;
            }
            if (result.frame.kind == ServiceFrameKind::Reset) { error = "Muse reset the subscription"; return false; }
        }
        error = cancel && cancel->load() ? "reply cancelled" : "Muse subscription acknowledgement timed out";
        return false;
    }

    bool WaitForReply(int64_t note_id, int64_t subscription_id,
                      const MuseClient::ReplyCallback& reply, std::atomic<bool>& cancel,
                      const std::function<void()>& waiting, const std::string& voice_session,
                      bool main_chat, std::string& error) {
        try {
            cancel_ = &cancel;
            ReplyTracker tracker(voice_session, main_chat);
            std::array<HeaderView, 16> headers{};
            std::string ack_body;
            bool accepted = false, acknowledged = false, spoken = false;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            auto last_activity = std::chrono::steady_clock::now();
            while (!cancel.load() && std::chrono::steady_clock::now() < deadline) {
                const ssize_t n = ReceiveFrame(rx_);
                if (n == -2 || n == -3) {
                    if (spoken && !tracker.agent_busy() &&
                        std::chrono::steady_clock::now() - last_activity >= std::chrono::seconds(5)) return true;
                    continue;
                }
                if (n <= 0) { error = "Muse reply connection closed"; return false; }
                auto result = session_.ProcessInboundWebSocketPayload(
                    ConstByteSpan(rx_.data(), n), ByteSpan(transport_.data(), transport_.size()),
                    ByteSpan(service_.data(), service_.size()), Span<HeaderView>(headers));
                if (!result.ok()) { error = "invalid Noise reply payload"; return false; }
                if (result.frame_status != InboundFrameStatus::Complete) continue;
                const auto& frame = result.frame;
                ConstByteSpan body;
                bool ended = false;
                if (frame.kind == ServiceFrameKind::Response) {
                    if (frame.response.status != 200) {
                        error = "Muse reply request refused (HTTP " + std::to_string(frame.response.status) + ")";
                        return false;
                    }
                    if (frame.stream_id == note_id) accepted = true;
                    if (frame.stream_id == subscription_id)
                        std::cerr << "[muse] reply subscription accepted (HTTP 200)\n";
                    body = frame.response.body;
                    ended = frame.response.end_body;
                } else if (frame.kind == ServiceFrameKind::BodyChunk) {
                    body = frame.body_chunk.data;
                    ended = frame.body_chunk.end_body;
                } else if (frame.kind == ServiceFrameKind::Reset) {
                    error = "Muse reset the reply stream"; return false;
                } else continue;
                if (frame.stream_id == note_id && !acknowledged) {
                    if (body.size()) ack_body.append(reinterpret_cast<const char*>(body.data()), body.size());
                    if (ack_body.size() > 65536) throw std::runtime_error("Muse acknowledgement too large");
                    if (ended) {
                        if (!accepted) throw std::runtime_error("voice note ended without HTTP success");
                        std::cerr << "[muse] voice-note acknowledged (HTTP 200)\n";
                        if (!tracker.SetAcknowledgement(nlohmann::json::parse(ack_body)))
                            throw std::runtime_error("voice note accepted but reply correlation ID missing");
                        acknowledged = true;
                        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
                        if (waiting) waiting();
                        std::cerr << "[muse] waiting for this turn's Muse reply\n";
                    }
                } else if (frame.stream_id == subscription_id) {
                    if (body.size() && tracker.Feed(std::string(reinterpret_cast<const char*>(body.data()), body.size())))
                        last_activity = std::chrono::steady_clock::now();
                    if (ended) throw std::runtime_error("Muse reply subscription ended");
                }
                if (acknowledged) {
                    for (const auto& text : tracker.TakeReady()) {
                        if (cancel.load()) break;
                        std::cerr << "[muse] matched Muse reply (" << text.size() << " bytes)\n";
                        if (!reply(text, cancel, error)) return false;
                        spoken = true;
                        last_activity = std::chrono::steady_clock::now();
                        deadline = last_activity + std::chrono::seconds(120);
                    }
                }
            }
            error = cancel.load() ? "reply cancelled" : "Muse reply timed out";
            return false;
        } catch (const nlohmann::json::exception&) { error = "invalid Muse reply JSON"; return false; }
          catch (const std::exception& e) { error = e.what(); return false; }
    }

    // Open a stream with a chosen HTTP verb/path and app-id, leaving the body
    // open so further BodyChunks can be streamed (used by /link-control).
    bool OpenStream(int64_t id, const char* verb, const char* path,
                    const char* app_id, bool end_body) {
        (void)app_id;
        // Match the upstream SDK: no extra headers on the control stream.
        ApplicationRequestView request{StringView(verb), StringView(path),
            Span<const HeaderView>(nullptr, 0), ConstByteSpan(), end_body};
        auto result = session_.StartOutboundApplicationRequest(ServiceType::Daemon, id, request,
                                                               ByteSpan(service_.data(), service_.size()),
                                                               ByteSpan(envelope_.data(), envelope_.size()));
        return result.ok() && Flush();
    }

    // Send one length-prefixed JSON control message as a (non-final) body chunk.
    bool SendControlJson(int64_t id, const std::string& json) {
        std::vector<uint8_t> framed(4 + json.size());
        const uint32_t len = static_cast<uint32_t>(json.size());
        framed[0] = len & 0xff; framed[1] = (len >> 8) & 0xff;
        framed[2] = (len >> 16) & 0xff; framed[3] = (len >> 24) & 0xff;
        std::memcpy(framed.data() + 4, json.data(), json.size());
        return Body(id, framed.data(), framed.size(), false);
    }

    // Block for up to one receive timeout for the next inbound control payload
    // on the given stream, appending decoded bytes to `buffer`. Returns:
    //   1  = got data (buffer grew)
    //   0  = stream/connection ended
    //  -1  = error
    //  -2  = ping/keepalive, retry
    int PumpControl(int64_t id, std::vector<uint8_t>& buffer, std::string& error) {
        std::array<HeaderView, 16> headers{};
        const ssize_t n = ReceiveFrame(rx_);
        if (n == -2 || n == -3) return -2;
        if (n == 0) return 0;
        if (n < 0) { error = "Muse control connection read failed"; return -1; }
        auto result = session_.ProcessInboundWebSocketPayload(
            ConstByteSpan(rx_.data(), n), ByteSpan(transport_.data(), transport_.size()),
            ByteSpan(service_.data(), service_.size()),
            Span<HeaderView>(headers.data(), headers.size()));
        if (!result.ok()) { error = "invalid Noise control payload"; return -1; }
        if (result.frame_status != InboundFrameStatus::Complete) return -2;
        if (result.frame.stream_id != id) return -2;
        if (result.frame.kind == ServiceFrameKind::Response) {
            if (result.frame.response.status >= 400) {
                error = "/link-control refused (HTTP " +
                        std::to_string(result.frame.response.status) + ")";
                return -1;
            }
            const auto& b = result.frame.response.body;
            if (b.size()) buffer.insert(buffer.end(), b.data(), b.data() + b.size());
            return result.frame.response.end_body ? 0 : 1;
        } else if (result.frame.kind == ServiceFrameKind::BodyChunk) {
            const auto& b = result.frame.body_chunk.data;
            if (b.size()) buffer.insert(buffer.end(), b.data(), b.data() + b.size());
            return result.frame.body_chunk.end_body ? 0 : 1;
        } else if (result.frame.kind == ServiceFrameKind::Reset) {
            error = "Muse reset the control stream"; return 0;
        }
        return -2;
    }

 private:
    bool WriteAll(const uint8_t* data, size_t size) {
        while (size) { const int n = SSL_write(ssl_, data, size); if (n <= 0) return false; data += n; size -= n; }
        return true;
    }
    bool SendFrame(const uint8_t* data, size_t size, uint8_t opcode = 2) {
        std::array<uint8_t, 14> h{}; size_t hn = 0;
        h[0] = 0x80 | opcode;
        if (size < 126) { h[1] = 0x80 | size; hn = 2; }
        else { h[1] = 0x80 | 126; h[2] = size >> 8; h[3] = size; hn = 4; }
        std::array<uint8_t, 4> mask{}; if (RAND_bytes(mask.data(), mask.size()) != 1) return false;
        std::memcpy(h.data() + hn, mask.data(), mask.size()); hn += mask.size();
        if (!WriteAll(h.data(), hn)) return false;
        std::vector<uint8_t> payload(size);
        for (size_t i = 0; i < size; ++i) payload[i] = data[i] ^ mask[i & 3];
        return WriteAll(payload.data(), payload.size());
    }
    bool SendPong(const uint8_t* data, size_t size) {
        // RFC 6455 requires a pong to echo the ping payload, also during PTT.
        return size <= 125 && SendFrame(data, size, 10);
    }
    // Read exactly `size` bytes, draining any leftover bytes captured during
    // the HTTP upgrade before falling back to the TLS socket.
    bool ReadBytes(uint8_t* dst, size_t size) {
        read_partial_ = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (size) {
            if (cancel_ && cancel_->load()) return false;
            if (!leftover_.empty()) {
                const size_t take = std::min(size, leftover_.size());
                std::memcpy(dst, leftover_.data(), take);
                leftover_.erase(leftover_.begin(), leftover_.begin() + take);
                dst += take; size -= take; read_partial_ = true; continue;
            }
            if (cancel_ && !SSL_pending(ssl_)) {
                pollfd input{fd_, POLLIN, 0};
                const int ready = poll(&input, 1, 100);
                if (ready < 0) { if (errno == EINTR) continue; return false; }
                if (!ready) {
                    if (!frame_in_progress_ && !read_partial_) { read_timed_out_ = true; return false; }
                    if (std::chrono::steady_clock::now() >= deadline) return false;
                    continue;
                }
            }
            errno = 0;
            const int n = SSL_read(ssl_, dst, size);
            if (n <= 0) {
                const int ssl_error = SSL_get_error(ssl_, n);
                read_timed_out_ = ssl_error == SSL_ERROR_WANT_READ ||
                    (ssl_error == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK));
                return false;
            }
            dst += n; size -= static_cast<size_t>(n); read_partial_ = true;
        }
        return true;
    }
    ssize_t ReceiveFrame(std::vector<uint8_t>& out) {
        uint8_t h[2];
        read_timed_out_ = false;
        if (!ReadBytes(h, 2)) return read_timed_out_ && !read_partial_ ? -3 : -1;
        frame_in_progress_ = true;
        struct FrameGuard { bool& flag; ~FrameGuard() { flag = false; } } guard{frame_in_progress_};
        uint64_t size = h[1] & 0x7f;
        if (size == 126) { uint8_t e[2]; if (!ReadBytes(e, 2)) return -1; size = e[0] << 8 | e[1]; }
        else if (size == 127) { uint8_t e[8]; if (!ReadBytes(e, 8)) return -1; size = 0; for (auto b : e) size = size << 8 | b; }
        if (size > out.size() || (h[1] & 0x80)) return -1;
        if (!ReadBytes(out.data(), size)) return -1;
        const int opcode = h[0] & 0xf;
        if (opcode == 8) return 0;
        if (opcode == 9) return SendPong(out.data(), size) ? -2 : -1;
        if (opcode == 10) return -2;
        if (opcode != 2) return -1;
        return static_cast<ssize_t>(size);
    }
    bool Flush() {
        int frames = 0;
        while (session_.HasOutboundWebSocketPayload()) {
            auto result = session_.WriteNextOutboundWebSocketPayload(ByteSpan(ws_.data(), ws_.size()));
            if (!result.ok() || !SendFrame(ws_.data(), result.size())) return false;
            ++frames;
        }
        (void)frames;
        return true;
    }

    int fd_{-1}; SSL_CTX* ctx_{nullptr}; SSL* ssl_{nullptr};
    OpenSslCryptoBackend crypto_; ClientSession session_;
    std::vector<uint8_t> ws_, rx_, transport_, service_, envelope_;
    std::vector<uint8_t> leftover_;
    bool read_timed_out_{false}, read_partial_{false}, frame_in_progress_{false};
    std::atomic<bool>* cancel_{nullptr};
};

std::string Base64(const uint8_t* data, size_t size) {
    std::string out(4 * ((size + 2) / 3), '\0');
    out.resize(EVP_EncodeBlock(reinterpret_cast<uint8_t*>(out.data()), data, size));
    return out;
}
}  // namespace

MuseClient::MuseClient(std::string noise_host) : noise_host_(std::move(noise_host)) {}

std::string MuseClient::NewVoiceSessionId() { return RandomId(); }

bool MuseClient::VerifyAccount(const std::string& device_token, std::string& error) {
    std::string vm_id, vm_token;
    return ResolveVm(device_token, vm_id, vm_token, error);
}

bool MuseClient::RefreshDeviceToken(const std::string& refresh_token,
                                    const std::string& device_id,
                                    const std::string& sdk_token,
                                    const std::string& api_url_v2,
                                    std::string& new_access_token,
                                    std::string& new_refresh_token,
                                    std::string& error) {
    // Only the trailing segment of the refresh token is presented, prefixed
    // once with hatch_refresh: (doubling it makes the server reject it).
    std::string raw_refresh = refresh_token;
    const auto colon = raw_refresh.rfind(':');
    if (colon != std::string::npos) raw_refresh = raw_refresh.substr(colon + 1);

    std::string root = (api_url_v2.rfind("https://", 0) == 0) ? api_url_v2 : "https://api.muse.ai";
    while (!root.empty() && root.back() == '/') root.pop_back();
    const std::string url = root + "/device_token/refresh";

    nlohmann::json body = nlohmann::json::object();
    body["device_id"] = device_id;
    if (!sdk_token.empty()) body["sdk_token"] = sdk_token;
    const std::string body_str = body.dump();

    CURL* curl = curl_easy_init();
    if (!curl) { error = "cannot initialize token refresh"; return false; }
    std::string response;
    const std::string auth = "Authorization: Bearer hatch_refresh:" + raw_refresh;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_str.size());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || status != 200) {
        error = "token refresh failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    try {
        auto root_json = nlohmann::json::parse(response);
        if (root_json.contains("payload") && root_json["payload"].is_object())
            root_json = root_json["payload"];
        new_access_token = root_json.value("access_token", "");
        new_refresh_token = root_json.value("refresh_token", "");
        if (new_access_token.empty() || new_refresh_token.empty()) {
            error = "token refresh response missing tokens"; return false;
        }
        return true;
    } catch (const std::exception&) {
        error = "token refresh returned invalid JSON"; return false;
    }
}

bool MuseClient::SendVoiceNote(const std::string& device_token,
                               const std::vector<int16_t>& pcm,
                               std::string& error,
                               const ReplyCallback& reply,
                               std::atomic<bool>* cancel,
                               const std::function<void()>& waiting,
                               const std::string& voice_session,
                               bool main_chat) {
    if (pcm.empty() || device_token.empty()) {
        error = "voice note requires audio and paired credentials";
        return false;
    }
    std::cerr << "[muse] PTT uploading " << pcm.size() << " frames ("
              << pcm.size() / 16000.0 << "s)\n";
    std::string vm_id, vm_token;
    if (!ResolveVm(device_token, vm_id, vm_token, error)) return false;
    std::unique_ptr<Connection> connection;
    for (int attempt = 0; attempt < 2; ++attempt) {
        connection = std::make_unique<Connection>();
        if (connection->Connect(noise_host_, vm_id, vm_token, error)) break;
        connection.reset();
        if (attempt == 1) return false;
        // Retry only before opening /chat/stream, never after posting a note.
        std::cerr << "[muse] PTT connection setup failed: " << error << "; retrying once\n";
        ::sleep(1);
        if (!ResolveVm(device_token, vm_id, vm_token, error)) return false;
    }
    // Match the official client: subscription first (1), then the note (2).
    constexpr int64_t subscription_id = 1;
    const int64_t stream_id = reply ? 2 : 1;
    if (reply && (!connection->Open(subscription_id, "/chat/subscribe", false, "application/x-ndjson") ||
                  !connection->Body(subscription_id, reinterpret_cast<const uint8_t*>("{}"), 2, true) ||
                  !connection->WaitForSubscription(subscription_id, cancel, error))) {
        if (error.empty()) error = "cannot open Muse reply subscription";
        return false;
    }
    const std::string note_head = voice_session.empty() ? std::string(kNoteHead) :
        "{\"session_id\":" + nlohmann::json(voice_session).dump() + "," + std::string(kNoteHead + 1);
    if (!connection->Open(stream_id, "/chat/stream", false) ||
        !connection->Body(stream_id, reinterpret_cast<const uint8_t*>(note_head.data()), note_head.size(), false)) {
        error = "cannot open Muse voice-note stream"; return false;
    }
    auto wav = StreamingWavHeader(16000);
    wav.insert(wav.end(), reinterpret_cast<const uint8_t*>(pcm.data()),
               reinterpret_cast<const uint8_t*>(pcm.data() + pcm.size()));
    for (size_t at = 0; at < wav.size();) {
        const size_t take = std::min<size_t>(6144, wav.size() - at);
        const std::string encoded = Base64(wav.data() + at, take);
        if (!connection->Body(stream_id, reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size(), false)) {
            error = "cannot upload Muse voice note"; return false;
        }
        at += take;
    }
    if (!connection->Body(stream_id, reinterpret_cast<const uint8_t*>(kNoteTail), sizeof(kNoteTail) - 1, true)) {
        error = "cannot finish Muse voice note"; return false;
    }
    if (reply) {
        std::atomic<bool> local_cancel{false};
        return connection->WaitForReply(stream_id, subscription_id, reply,
                                       cancel ? *cancel : local_cancel, waiting, voice_session, main_chat, error);
    }
    return connection->WaitForSuccess(stream_id, error);
}

namespace {
// Minimal RFC4122-ish random id; good enough for request correlation.
std::string RandomId() {
    uint8_t b[16];
    RAND_bytes(b, sizeof(b));
    // RFC 4122 version 4 (random) UUID: set version and variant bits so the
    // id parses as a valid UUID, matching Python's uuid.uuid4().
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 16; ++i) {
        out += hex[b[i] >> 4];
        out += hex[b[i] & 0xf];
        if (i == 3 || i == 5 || i == 7 || i == 9) out += '-';
    }
    return out;
}

// Pull complete length-prefixed (4-byte LE) JSON messages out of a buffer.
std::vector<nlohmann::json> DrainMessages(std::vector<uint8_t>& buf) {
    std::vector<nlohmann::json> out;
    size_t off = 0;
    while (buf.size() - off >= 4) {
        const uint32_t len = static_cast<uint32_t>(buf[off]) |
                             (static_cast<uint32_t>(buf[off + 1]) << 8) |
                             (static_cast<uint32_t>(buf[off + 2]) << 16) |
                             (static_cast<uint32_t>(buf[off + 3]) << 24);
        if (buf.size() - off - 4 < len) break;
        if (len > 0) {
            try {
                out.push_back(nlohmann::json::parse(buf.begin() + off + 4,
                                                    buf.begin() + off + 4 + len));
            } catch (const std::exception&) { /* skip malformed */ }
        }
        off += 4 + len;
    }
    buf.erase(buf.begin(), buf.begin() + off);
    return out;
}
}  // namespace

void MuseClient::RunLinkService(const std::string& device_token,
                                const std::string& node_id,
                                const std::string& display_name,
                                std::atomic<bool>& stop) {
    int backoff = 2;
    while (!stop.load()) {
        std::string vm_id, vm_token, error;
        if (!ResolveVm(device_token, vm_id, vm_token, error)) {
            std::cerr << "[muse] link: VM lookup failed: " << error << '\n';
            for (int i = 0; i < backoff && !stop.load(); ++i) ::sleep(1);
            backoff = std::min(backoff * 2, 60);
            continue;
        }
        Connection connection;
        if (!connection.Connect(noise_host_, vm_id, vm_token, error)) {
            std::cerr << "[muse] link: connect failed: " << error << '\n';
            for (int i = 0; i < backoff && !stop.load(); ++i) ::sleep(1);
            backoff = std::min(backoff * 2, 60);
            continue;
        }

        constexpr int64_t control_id = 1;
        if (!connection.OpenStream(control_id, "POST", "/link-control", "musegadget", false)) {
            std::cerr << "[muse] link: cannot open control stream\n";
            for (int i = 0; i < backoff && !stop.load(); ++i) ::sleep(1);
            backoff = std::min(backoff * 2, 60);
            continue;
        }

        const std::string register_id = RandomId();
        nlohmann::ordered_json params = nlohmann::ordered_json::object();
        params["node_id"] = node_id;
        params["display_name"] = display_name;
        params["platform"] = "linux";
        params["version"] = "0.1.0";
        params["device_family"] = "homehub";
        params["model_id"] = "linux";
        params["is_wakeup_supported"] = false;
        // Exact COMMAND_SPECS from the official musegadget client so the VM
        // accepts the registration (it may schema-validate commands_v2).
        static const char* kCommandSpecs = R"JSON({"system.run":{"description":"Run a shell command on this Linux device with bash and return its stdout, stderr and exit code. Output over 96 KiB per stream is truncated.","required":{"command":{"type":"string","description":"Shell command line to run."}},"optional":{"cwd":{"type":"string","description":"Working directory. Default: the account's home."},"timeout_ms":{"type":"integer","description":"Kill the command after this long. Default 120000, max 600000."}},"timeout_ms":605000},"file.read":{"description":"Read up to 64 KiB of a file, base64-encoded. Call again with next_offset until eof.","required":{"path":{"type":"string","description":"Absolute path."}},"optional":{"offset":{"type":"integer","description":"Byte offset. Default 0."},"limit":{"type":"integer","description":"Max bytes, up to 65536."}}},"file.write":{"description":"Write a file in chunks of up to 64 KiB (base64). Start at offset 0, send chunks in order, and set final=true on the last one; the file is replaced atomically, and only if sha256 (when given) matches.","required":{"path":{"type":"string","description":"Absolute destination path."},"data_b64":{"type":"string","description":"This chunk's bytes, base64-encoded."}},"optional":{"offset":{"type":"integer","description":"Byte offset of this chunk. Default 0."},"final":{"type":"boolean","description":"True on the last chunk. Default false."},"sha256":{"type":"string","description":"Hex SHA-256 of the whole file, checked on the final chunk."},"create_parents":{"type":"boolean","description":"Create missing parent directories. Default false."},"overwrite":{"type":"boolean","description":"Replace an existing file. Default true."}}},"device.health":{"description":"Device status: uptime, load, memory, disk, temperature, software version.","required":{},"optional":{}}})JSON";
        params["commands_v2"] = nlohmann::ordered_json::parse(kCommandSpecs);
        nlohmann::ordered_json register_req = nlohmann::ordered_json::object();
        register_req["type"] = "req";
        register_req["id"] = register_id;
        register_req["method"] = "link.register";
        register_req["params"] = params;
        const std::string rdump = register_req.dump();
        if (!connection.SendControlJson(control_id, rdump)) {
            std::cerr << "[muse] link: register send failed\n";
            continue;
        }
        std::cerr << "[muse] link: sent link.register (" << rdump.size()
                  << " bytes) as " << node_id << '\n';

        std::vector<uint8_t> buffer;
        bool alive = true;
        bool healthy = false;
        bool registered = false;
        const time_t register_sent_at = time(nullptr);
        while (alive && !stop.load()) {
            std::string perr;
            const int rc = connection.PumpControl(control_id, buffer, perr);
            if (rc == -1) { std::cerr << "[muse] link: " << perr << '\n'; break; }
            if (rc == 0) { std::cerr << "[muse] link: control stream closed\n"; break; }
            // If the VM accepts the socket but never answers link.register, the
            // leased VM is not serving yet. Reconnect so it gets another try
            // (fetch_vms signals this with retry_after_ms/max_retry_count).
            if (!registered && time(nullptr) - register_sent_at > 12) {
                std::cerr << "[muse] link: no register ack in 12s; reconnecting\n";
                break;
            }
            if (rc == -2) continue;  // ping / timeout / partial
            auto drained = DrainMessages(buffer);
            if (!drained.empty())
                std::cerr << "[muse] link: rx " << drained.size()
                          << " msg(s), buf=" << buffer.size() << '\n';
            for (auto& msg : drained) {
                std::cerr << "[muse] link: msg=" << msg.dump().substr(0, 200) << '\n';
                // register reply (no method field, id matches)
                if (msg.value("id", "") == register_id && !msg.contains("method")) {
                    if (msg.contains("error")) {
                        std::cerr << "[muse] link: register rejected\n";
                        alive = false;
                    } else {
                        std::cerr << "[muse] link: registered with the Muse\n";
                        healthy = true;
                        registered = true;
                        backoff = 2;
                    }
                    continue;
                }
                const std::string event = msg.value("event", "");
                if (event == "link.unpaired" || event == "node.unpaired") {
                    std::cerr << "[muse] link: device was removed from the Muse\n";
                    alive = false;
                    continue;
                }
                if (msg.value("method", "") == "link.invoke") {
                    // We expose no executable commands yet; acknowledge so the
                    // Muse does not treat the request as a timeout.
                    nlohmann::json err = nlohmann::json::object();
                    err["code"] = "unsupported";
                    err["message"] = "no commands registered";
                    nlohmann::json result = nlohmann::json::object();
                    result["method"] = "link.result";
                    result["id"] = msg.value("id", "");
                    result["ok"] = false;
                    result["error"] = err;
                    connection.SendControlJson(control_id, result.dump());
                }
            }
        }
        (void)healthy;
        if (stop.load()) break;
        // Rapid retries while the VM is still spinning up; exponential backoff
        // only after we have successfully registered at least once.
        const int delay = registered ? backoff : 2;
        for (int i = 0; i < delay && !stop.load(); ++i) ::sleep(1);
        if (registered) backoff = std::min(backoff * 2, 60);
    }
    std::cerr << "[muse] link: service stopped\n";
}
}  // namespace muse
