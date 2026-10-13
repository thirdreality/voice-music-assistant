#include "AudioCapture.h"
#include "ConfigStore.h"
#include "PairingSession.h"
#include "MuseClient.h"
#include "WavWriter.h"
#include "WyomingTts.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>
#include <iostream>
#include <sstream>

namespace {
constexpr char kSocketPath[] = "/tmp/muse-gadget-control.sock";
std::atomic<bool> running{true};
void OnSignal(int) { running = false; }

std::string ReadTrimmed(const std::string& path) {
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    return value;
}

std::string MacAddress() {
    std::string mac = ReadTrimmed("/sys/class/net/wlan0/address");
    return mac.empty() ? "00:00:00:00:00:00" : mac;
}

std::string Tail(const std::string& mac) {
    std::string out;
    for (char c : mac) if (c != ':') out += static_cast<char>(std::tolower(c));
    return out.size() >= 6 ? out.substr(out.size() - 6) : "000000";
}

bool MicrophoneMuted() {
    // Match the existing speaker contract: 0 = muted, 1 = unmuted.
    if (ReadTrimmed("/sys/class/gpio/gpio438/value") == "0") return true;
    std::ifstream input("/data/conf/sound.json");
    if (!input.is_open()) return false;
    try {
        nlohmann::json sound;
        input >> sound;
        return sound.value("mic_mute", 1) == 0;
    } catch (const std::exception&) {
        return true;  // Do not record when the privacy state is unreadable.
    }
}

void SendControl(const char* path, const char* command) {
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sockaddr_un target{};
    target.sun_family = AF_UNIX;
    std::strncpy(target.sun_path, path, sizeof(target.sun_path) - 1);
    sendto(fd, command, std::strlen(command), MSG_NOSIGNAL,
           reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    close(fd);
}

void Reply(int fd, const sockaddr_un& peer, socklen_t peer_len,
           const nlohmann::json& value) {
    if (peer_len <= sizeof(sa_family_t)) return;
    const std::string data = value.dump();
    sendto(fd, data.data(), data.size(), MSG_NOSIGNAL,
           reinterpret_cast<const sockaddr*>(&peer), peer_len);
}

std::string ActiveWifiSsid() {
    // Ask wpa_cli for the SSID of the active association.
    FILE* pipe = popen("wpa_cli -i wlan0 status 2>/dev/null", "r");
    if (!pipe) return "";
    std::string line, ssid;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        line = buf;
        const std::string key = "ssid=";
        if (line.rfind(key, 0) == 0) {
            ssid = line.substr(key.size());
            while (!ssid.empty() && (ssid.back() == '\n' || ssid.back() == '\r'))
                ssid.pop_back();
            break;
        }
    }
    pclose(pipe);
    return ssid;
}
}  // namespace

int main(int argc, char** argv) {
    std::string state_dir = "/data/conf/muse";
    std::string token_file = state_dir + "/sdk-token";
    std::string audio_device = "hw:0,4";
    std::string log_file;
    std::string tts_host, tts_voice;
    std::string chat_mode = "main";
    int tts_port = 10200;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--state-dir" && i + 1 < argc) state_dir = argv[++i];
        else if (arg == "--sdk-token-file" && i + 1 < argc) token_file = argv[++i];
        else if (arg == "--audio-device" && i + 1 < argc) audio_device = argv[++i];
        else if (arg == "--log-file" && i + 1 < argc) log_file = argv[++i];
        else if (arg == "--tts-host" && i + 1 < argc) tts_host = argv[++i];
        else if (arg == "--tts-voice" && i + 1 < argc) tts_voice = argv[++i];
        else if (arg == "--chat-mode" && i + 1 < argc) chat_mode = argv[++i];
        else if (arg == "--tts-port" && i + 1 < argc) {
            try {
                size_t used = 0;
                const std::string value = argv[++i];
                tts_port = std::stoi(value, &used);
                if (used != value.size() || tts_port < 1 || tts_port > 65535) throw std::invalid_argument("port");
            } catch (const std::exception&) { std::cerr << "[muse] invalid TTS port\n"; return 2; }
        }
        else if (arg == "--help") {
            std::cout << "muse-gadget " MUSE_GADGET_VERSION "\n"
                      << "  --state-dir PATH\n  --sdk-token-file PATH\n  --audio-device ALSA_DEVICE\n  --log-file PATH\n"
                      << "  --tts-host HOST\n  --tts-port PORT\n  --tts-voice NAME\n"
                      << "  --chat-mode main|session\n";
            return 0;
        }
    }

    if (!log_file.empty()) {
        const int log_fd = open(log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (log_fd < 0) { std::perror("open log file"); return 1; }
        if (dup2(log_fd, STDERR_FILENO) < 0 || dup2(log_fd, STDOUT_FILENO) < 0) {
            std::perror("redirect log file"); close(log_fd); return 1;
        }
        close(log_fd);
    }
    std::cerr << "[muse] starting version " MUSE_GADGET_VERSION " pid=" << getpid() << '\n';
    const std::string token = ReadTrimmed(token_file);
    if (token.rfind("mgst_", 0) != 0) {
        std::cerr << "[muse] SDK token is missing or invalid\n";
        return 2;
    }
    const std::string mac = MacAddress();
    const std::string tail = Tail(mac);
    muse::ConfigStore store(state_dir);
    muse::PairingSession pairing("homelink-" + tail, "hatch-link:" + mac,
                                 mac, MUSE_GADGET_VERSION, token);
    muse::AudioCapture capture(audio_device);
    muse::MuseClient client;
    std::vector<int16_t> recording;
    std::thread upload_thread;
    std::atomic<bool> uploading{false}, replying{false}, turn_cancel{false};
    bool ptt_recording = false;
    std::string voice_session;

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    // A dropped TLS socket during upload would otherwise raise SIGPIPE and
    // terminate the whole daemon; ignore it and handle write errors inline.
    std::signal(SIGPIPE, SIG_IGN);
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { std::perror("socket"); return 1; }
    unlink(kSocketPath);
    sockaddr_un local{};
    local.sun_family = AF_UNIX;
    std::strncpy(local.sun_path, kSocketPath, sizeof(local.sun_path) - 1);
    if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        std::perror("bind"); close(fd); return 1;
    }
    if (chmod(kSocketPath, 0600) < 0) {
        std::perror("chmod control socket"); close(fd); unlink(kSocketPath); return 1;
    }
    std::cerr << "[muse] control socket ready; BLE GATT bridge may connect\n";

    // Start the persistent link both after provisioning and on normal startup
    // so the device shows up and stays online in the Muse app.
    std::atomic<bool> link_stop{false};
    std::thread link_thread;
    auto start_link = [&]() {
        link_stop.store(true);
        if (link_thread.joinable()) link_thread.join();
        link_stop.store(false);
        auto state = store.Load();
        if (state.value("paired", false)) {
            if (!tts_host.empty() && chat_mode == "session") {
                voice_session = state.value("voice_session_id", "");
                if (voice_session.empty()) {
                    voice_session = muse::MuseClient::NewVoiceSessionId();
                    state["voice_session_id"] = voice_session;
                    store.Save(state);
                }
            }
            const std::string noise_host = state.value("noise_host", "hatch.metaaivm.com");
            const std::string api_url_v2 = state.value("api_url_v2", "");
            const std::string refresh_token = state.value("refresh_token", "");
            const std::string link_node = "homelink-" + tail;
            char hostbuf[256] = {0};
            std::string link_name = (gethostname(hostbuf, sizeof(hostbuf) - 1) == 0 && hostbuf[0])
                                        ? std::string(hostbuf) : ("3RSPK-" + tail);

            // Report the SDK token (via /device_token/refresh) exactly once per
            // pairing. Each refresh rotates and invalidates the previous
            // refresh_token, so repeating it on every restart would break the
            // credentials. A marker file records that the report succeeded.
            const std::string report_marker = state_dir + "/sdk-reported";
            const bool already_reported = (access(report_marker.c_str(), F_OK) == 0);
            if (!already_reported && !refresh_token.empty()) {
                muse::MuseClient refresher(noise_host.empty() ? "hatch.metaaivm.com" : noise_host);
                std::string new_access, new_refresh, rerr;
                if (refresher.RefreshDeviceToken(refresh_token, link_node, token, api_url_v2,
                                                 new_access, new_refresh, rerr)) {
                    state["access_token"] = new_access;
                    state["refresh_token"] = new_refresh;
                    try { store.Save(state); } catch (const std::exception& e) {
                        std::cerr << "[muse] link: cannot persist rotated tokens: " << e.what() << '\n';
                    }
                    std::ofstream(report_marker) << "1\n";
                    std::cerr << "[muse] link: device token rotated and SDK token reported (once)\n";
                } else {
                    std::cerr << "[muse] link: SDK token report failed: " << rerr
                              << " (continuing with existing token)\n";
                }
            } else if (already_reported) {
                std::cerr << "[muse] link: SDK token already reported; using saved token\n";
            }

            const std::string access_token = state.value("access_token", "");
            if (!access_token.empty()) {
                link_thread = std::thread([access_token, noise_host, link_node, link_name, &link_stop]() {
                    muse::MuseClient link(noise_host.empty() ? "hatch.metaaivm.com" : noise_host);
                    link.RunLinkService(access_token, link_node, link_name, link_stop);
                });
            }
        }
    };
    start_link();

    while (running) {
        pollfd control{fd, POLLIN, 0};
        const int ready = poll(&control, 1, 500);
        if (ready < 0) { if (errno == EINTR) continue; std::perror("poll"); break; }
        if (ptt_recording && MicrophoneMuted()) {
            capture.Stop();
            recording.clear();
            ptt_recording = false;
            SendControl("/tmp/sendspin-control.sock", "UNDUCK");
            std::cerr << "[muse] PTT cancelled: microphone muted\n";
        }
        if (!running || ready == 0) continue;
        char buffer[8192];
        sockaddr_un peer{};
        socklen_t peer_len = sizeof(peer);
        const ssize_t n = recvfrom(fd, buffer, sizeof(buffer), 0,
                                   reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (n < 0) { if (errno == EINTR) continue; std::perror("recvfrom"); break; }
        try {
            const std::string message(buffer, static_cast<size_t>(n));
            if (message == "DEVICE_INFO") {
                Reply(fd, peer, peer_len, pairing.DeviceInfo());
            } else if (message == "BUTTON_DOWN") {
                if (uploading.load() && replying.load()) {
                    turn_cancel.store(true);
                    if (upload_thread.joinable()) upload_thread.join();
                    std::cerr << "[muse] previous reply interrupted by Home\n";
                }
                if (pairing.state() == muse::PairingSession::State::ConfirmRequired) {
                    Reply(fd, peer, peer_len, pairing.Confirm());
                } else if (uploading.load()) {
                    Reply(fd, peer, peer_len, {{"ptt", "busy"}});
                    std::cerr << "[muse] PTT press ignored: upload in progress\n";
                } else if (!ptt_recording) {
                    if (MicrophoneMuted()) {
                        Reply(fd, peer, peer_len, {{"ptt", "muted"}});
                        std::cerr << "[muse] PTT press ignored: microphone muted\n";
                        continue;
                    }
                    const auto state = store.Load();
                    if (!state.value("paired", false) || state.value("access_token", "").empty()) {
                        Reply(fd, peer, peer_len, {{"ptt", "not_paired"}});
                        continue;
                    }
                    if (upload_thread.joinable()) upload_thread.join();
                    SendControl("/tmp/sendspin-control.sock", "DUCK");
                    recording.clear();
                    recording.reserve(16000 * 30);
                    ptt_recording = capture.Start([&recording](const int16_t* pcm, size_t frames) {
                        if (recording.size() + frames > 16000 * 30) return false;
                        recording.insert(recording.end(), pcm, pcm + frames);
                        return true;
                    });
                    if (!ptt_recording) {
                        std::cerr << "[muse] PTT capture failed to start (device busy?)\n";
                        SendControl("/tmp/sendspin-control.sock", "UNDUCK");
                    } else {
                        std::cerr << "[muse] PTT recording started\n";
                    }
                    Reply(fd, peer, peer_len, {{"ptt", ptt_recording ? "recording" : "audio_error"}});
                } else {
                    Reply(fd, peer, peer_len, {{"ptt", "recording"}});
                }
            } else if (message == "BUTTON_UP") {
                if (!ptt_recording) {
                    Reply(fd, peer, peer_len, {{"ptt", uploading.load() ? "busy" : "idle"}});
                    continue;
                }
                ptt_recording = false;
                capture.Stop();
                auto pcm = std::move(recording);
                std::cerr << "[muse] PTT captured " << pcm.size() << " frames\n";
                if (pcm.size() >= 4800) {
                    const auto state = store.Load();
                    const std::string access_token = state.value("access_token", "");
                    const std::string noise_host = state.value("noise_host", "hatch.metaaivm.com");
                    turn_cancel.store(false);
                    replying.store(false);
                    uploading.store(true);
                    Reply(fd, peer, peer_len, {{"ptt", "uploading"}, {"frames", pcm.size()}});
                    const bool main_chat = chat_mode != "session";
                    upload_thread = std::thread([pcm = std::move(pcm), access_token, noise_host, voice_session,
                                                 tts_host, tts_port, tts_voice, main_chat, &uploading, &replying, &turn_cancel]() {
                        muse::MuseClient uploader(noise_host.empty() ? "hatch.metaaivm.com" : noise_host);
                        muse::MuseClient::ReplyCallback reply;
                        if (!tts_host.empty()) {
                            reply = [tts_host, tts_port, tts_voice](const std::string& text,
                                      std::atomic<bool>& cancel, std::string& error) {
                                muse::SpeechAudio audio;
                                muse::WyomingTts tts(tts_host, static_cast<uint16_t>(tts_port), tts_voice);
                                return tts.Synthesize(text, cancel, audio, error) && muse::PlaySpeech(audio, cancel, error);
                            };
                        }
                        std::string error;
                        const bool ok = uploader.SendVoiceNote(access_token, pcm, error, reply, &turn_cancel,
                                                              [&replying]() { replying.store(true); }, voice_session, main_chat);
                        if (turn_cancel.load())
                            std::cerr << "[muse] reply cancelled\n";
                        else if (!ok)
                            std::cerr << "[muse] PTT/reply failed: " << error << '\n';
                        else
                            std::cerr << (reply ? "[muse] spoken reply turn completed\n" : "[muse] PTT delivered\n");
                        SendControl("/tmp/sendspin-control.sock", "UNDUCK");
                        replying.store(false);
                        uploading.store(false);
                    });
                } else {
                    Reply(fd, peer, peer_len, {{"ptt", "too_short"}, {"frames", pcm.size()}});
                    std::cerr << "[muse] PTT skipped: recording shorter than 300 ms\n";
                    SendControl("/tmp/sendspin-control.sock", "UNDUCK");
                }
            } else {
                const auto command = nlohmann::json::parse(message);
                auto response = pairing.Handle(command);
                const std::string inner = response.value("action", "");
                if (inner == "wifi_scan") {
                    // Device is already online: offer the current connection as
                    // an open network so the App skips the password field.
                    std::string ssid = ActiveWifiSsid();
                    if (ssid.empty()) ssid = "Use current connection";
                    nlohmann::json result{
                        {"type", "wifi_scan_result"},
                        {"networks", nlohmann::json::array({
                            {{"ssid", ssid}, {"rssi", -40}, {"secure", false}}
                        })}
                    };
                    Reply(fd, peer, peer_len, pairing.Seal(result));
                } else if (inner == "provision_v2") {
                    const std::string access_token = response.value("access_token", "");
                    const std::string refresh_token = response.value("refresh_token", "");
                    // Device is already online: tell the App we are connecting.
                    Reply(fd, peer, peer_len,
                          pairing.Seal({{"type", "status"}, {"status", "wifi_connecting"}}));
                    Reply(fd, peer, peer_len,
                          pairing.Seal({{"type", "status"}, {"status", "wifi_connected"}}));
                    // Validate the device token and bring the device online in
                    // the Muse account (GET /fetch_vms) before saving.
                    muse::MuseClient verifier(response.value("noise_host", "hatch.metaaivm.com"));
                    std::string verify_error;
                    if (!verifier.VerifyAccount(access_token, verify_error)) {
                        std::cerr << "[muse] provision auth failed: " << verify_error << '\n';
                        Reply(fd, peer, peer_len,
                              pairing.Seal({{"type", "status"}, {"status", "auth_failed"}}));
                    } else {
                        nlohmann::json saved{
                            {"access_token", access_token},
                            {"refresh_token", refresh_token},
                            {"username", response.value("username", "")},
                            {"api_url", response.value("api_url", "")},
                            {"api_url_v2", response.value("api_url_v2", "")},
                            {"noise_host", response.value("noise_host", "")},
                            {"paired", true}
                        };
                        store.Save(saved);
                        // Fresh pairing: allow the SDK token to be reported again.
                        ::unlink((state_dir + "/sdk-reported").c_str());
                        std::cerr << "[muse] provision complete; device online\n";
                        Reply(fd, peer, peer_len,
                              pairing.Seal({{"type", "status"}, {"status", "auth_ok"}}));
                        start_link();
                    }
                } else {
                    Reply(fd, peer, peer_len, response);
                }
            }
        } catch (const std::exception& e) {
            Reply(fd, peer, peer_len, {{"status", "error_invalid_command"}});
            std::cerr << "[muse] command rejected: " << e.what() << '\n';
        }
    }
    capture.Stop();
    turn_cancel.store(true);
    SendControl("/tmp/sendspin-control.sock", "UNDUCK");
    if (upload_thread.joinable()) upload_thread.join();
    link_stop.store(true);
    if (link_thread.joinable()) link_thread.join();
    unlink(kSocketPath);
    close(fd);
    return 0;
}
