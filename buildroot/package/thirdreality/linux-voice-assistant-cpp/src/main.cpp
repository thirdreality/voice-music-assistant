
#include <getopt.h>
#include <malloc.h>
#include <fcntl.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "audio/AudioCapture.h"
#include "audio/SanoTts.h"
#include "audio/LibMpvPlayer.h"
#include "audio/MicroWakeWord.h"
#include "audio/OpenWakeWord.h"
#include "audio/OpenWakeWordFeatures.h"
#include "audio/PcmRingBuffer.h"
#include "audio/WhisperStt.h"
#include "audio/WakeWordEngine.h"
#include "audio/WebRtcProcessor.h"
#include "entities/EventEntity.h"
#include "entities/MediaPlayerEntity.h"
#include "entities/MuteSwitchEntity.h"
#include "entities/NumberEntity.h"
#include "entities/SelectEntity.h"
#include "entities/SendspinSwitchEntity.h"
#include "entities/ThinkingSoundEntity.h"
#include "entities/UpdateEntity.h"
#include "protocol/ApiServer.h"
#include "protocol/MdnsPublisher.h"
#include "satellite/LocalStt.h"
#include "satellite/Satellite.h"
#include "sendspin/SendspinHub.h"
#include "state/Preferences.h"
#include "state/ServerState.h"
#include "tr/ControlListener.h"
#include "tr/MicMuteGpio.h"
#include "tr/ModelDownloader.h"
#include "tr/SoundConfWatcher.h"
#include "tr/Supervisor.h"
#include "tr/SupervisorHttpServer.h"
#include "tr/SysInfo.h"
#include "tr/SystemVolume.h"
#include "util/Log.h"
#ifdef LVA_DIAG_LOG
#include "util/MemProbe.h"
#endif

namespace {

constexpr const char* kTag = "main";
constexpr const char* kVersion = "0.0.1";

constexpr double kDefaultMicroWakeSensitivity = 0.85;
constexpr double kDefaultOpenWakeSensitivity  = 0.5;
constexpr double kDefaultStopSensitivity      = 0.5;

// The on-device STT model is downloaded to /data instead of shipping in the
// rootfs. It is an asset of the v1.2.4 GitHub release; later firmware keeps
// this URL until the model changes. Never delete or replace that asset.
constexpr const char* kSttModelUrl =
    "https://github.com/thirdreality/voice-music-assistant/releases/download/"
    "v1.2.4/ggml-tiny.en-q5_1.bin";
constexpr std::uintmax_t kSttModelSize = 32168608;
constexpr const char* kSttModelSha256 =
    "6f98f0dacb0ed1ab5c1d7eed51d132ca139b5aca8c843a06926362d49e8cb0d0";
constexpr const char* kSttModelPath =
    "/data/models/stt/ggml-tiny.en-q5_1.bin";

constexpr const char* kVoiceBackendHa    = "Home Assistant";
constexpr const char* kVoiceBackendLocal = "3R Speaker";

constexpr int kDefaultTtsRate = 165;  // words per minute = speed 1.0

constexpr const char* kTtsVoiceDir = "/usr/share/thirdreality/tts/amy";

constexpr const char* kVoiceNotifyId    = "lva_voice_model";
constexpr const char* kVoiceNotifyTitle = "Local speech";

// Speech Rate (wpm) -> TTS speed factor.
float TtsSpeed(int rate_wpm) {
    return static_cast<float>(rate_wpm) / static_cast<float>(kDefaultTtsRate);
}


std::atomic<int> g_shutdown_signal{0};
lva::proto::ApiServer* g_server = nullptr;
lva::audio::WakeWordEngine* g_wakeword_engine = nullptr;

extern "C" void OnSignal(int signo) {
    g_shutdown_signal.store(signo, std::memory_order_relaxed);
    if (g_wakeword_engine) {
        g_wakeword_engine->RequestStop();
    }
    if (g_server) {
        g_server->Stop();
    }
}

extern "C" void OnSigchld(int /*signo*/) {
    int saved_errno = errno;
    while (::waitpid(-1, nullptr, WNOHANG) > 0) {
    }
    errno = saved_errno;
}

void InstallSignalHandlers() {
    struct sigaction sa{};
    sa.sa_handler = &OnSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    struct sigaction sc{};
    sc.sa_handler = &OnSigchld;
    sigemptyset(&sc.sa_mask);
    sc.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    ::sigaction(SIGCHLD, &sc, nullptr);
}

void PrintUsage(const char* argv0) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --name <name>            Device name advertised to HA\n"
        "  --port <port>            TCP port to listen on (default: 6053)\n"
        "  --host <addr>            Bind address (default: 0.0.0.0)\n"
        "  --preferences-file <p>   JSON file for persisted prefs\n"
        "                           (default: /data/conf/sound.json)\n"
        "  --wakeword-dir <dir>     Wake-word model directory\n"
        "  --wakeword-models <ids>  Comma-separated model IDs (default: okay_nabu)\n"
        "  --audio-device <dev>     PulseAudio device name\n"
        "  --capture-backend <b>    Capture backend: alsa|pulse (default: alsa)\n"
        "                           alsa enables hardware-loopback AEC.\n"
        "  --capture-alsa-device    ALSA capture device for the alsa backend\n"
        "                           (default: hw:0,4)\n"
        "  --capture-mic-channel    0-based mic channel within the alsa\n"
        "                           stream (default: 0)\n"
        "  --capture-ref-channels   Comma-separated 0-based ref channels;\n"
        "                           empty/none disables AEC. (default: 2,3)\n"
        "  --continue-conversation-delay <s>\n"
        "                           Seconds to wait after TTS finishes before\n"
        "                           opening the mic for a continued\n"
        "                           conversation (default: 0.5)\n"
        "  --voice-backend <b>      ha|local. ha = stream audio to the HA\n"
        "                           Assist pipeline (STT/TTS in HA); local =\n"
        "                           on-device STT + TTS, HA only resolves\n"
        "                           the intent. Default for the HA \"Voice\n"
        "                           Processing\" select, which overrides it.\n"
        "                           (default: ha; local uses ha until the\n"
        "                           STT model is downloaded)\n"
        "  --stt-model <path>       whisper.cpp ggml model for local STT\n"
        "                           (default: downloaded to /data/models/stt/\n"
        "                           ggml-tiny.en-q5_1.bin; a custom path is\n"
        "                           used as is and never downloaded)\n"
        "  --stt-model-url <url>    Download URL for the default STT model\n"
        "                           (testing a mirror; size and sha256 must\n"
        "                           still match)\n"
        "  --stt-threads <n>        Local STT decode threads, 1-4 (default: 4)\n"
        "  --tts-threads <n>        Local TTS decode threads, 1-4 (default: 2)\n"
        "  --tts-data-dir <dir>     espeak-ng-data directory\n"
        "                           (default: /usr/share/thirdreality/tts/espeak-ng-data)\n"
        "  --conversation-agent <id>\n"
        "                           HA conversation agent for the local\n"
        "                           backend (default: HA's default agent)\n"
        "  --stt-dump-wav <path>    Write each local-STT utterance to a WAV\n"
        "                           file (mic diagnostics)\n"
        "  --debug                  Enable debug logging and write it to\n"
        "                           /usr/share/thirdreality/log/lva.log\n"
        "  --help                   Show this help and exit\n",
        argv0);
}

struct CliOptions {
    std::string device_name = "3RSPK";
    std::string host = "0.0.0.0";
    std::string audio_device;
    std::uint16_t port = 6053;
    std::filesystem::path preferences_file = "/data/conf/sound.json";
    std::string wakeword_type = "micro";  // "micro" or "open"
    std::string wakeword_models = "okay_nabu";

    std::string capture_backend     = "alsa";
    std::string capture_alsa_device = "hw:0,4";
    int         capture_mic_channel = 0;
    std::string capture_ref_channels = "2,3";

    double      continue_conversation_delay = 0.5;  // seconds

    std::string voice_backend      = "ha";
    std::string stt_model          = kSttModelPath;
    std::string stt_model_url      = kSttModelUrl;
    int         stt_threads        = 4;
    int         tts_threads        = 2;
    std::string tts_data_dir       = "/usr/share/thirdreality/tts/espeak-ng-data";
    std::string conversation_agent;
    std::string stt_dump_wav;

    bool debug = false;
};

bool ParseCli(int argc, char** argv, CliOptions& out) {
    static const struct option long_options[] = {
        {"name",                  required_argument, nullptr, 'n'},
        {"port",                  required_argument, nullptr, 'p'},
        {"host",                  required_argument, nullptr, 'H'},
        {"preferences-file",      required_argument, nullptr, 'f'},
        {"audio-device",          required_argument, nullptr, 'a'},
        {"wakeword-type",         required_argument, nullptr, 'w'},
        {"wakeword-models",       required_argument, nullptr, 'm'},
        {"capture-backend",       required_argument, nullptr, 'B'},
        {"capture-alsa-device",   required_argument, nullptr, 'A'},
        {"capture-mic-channel",   required_argument, nullptr, 'M'},
        {"capture-ref-channels",  required_argument, nullptr, 'R'},
        {"continue-conversation-delay", required_argument, nullptr, 'C'},
        {"voice-backend",         required_argument, nullptr, 'V'},
        {"stt-model",             required_argument, nullptr, 'S'},
        {"stt-model-url",         required_argument, nullptr, 'U'},
        {"stt-threads",           required_argument, nullptr, 'J'},
        {"tts-threads",           required_argument, nullptr, 'K'},
        {"tts-data-dir",          required_argument, nullptr, 'T'},
        {"conversation-agent",    required_argument, nullptr, 'G'},
        {"stt-dump-wav",          required_argument, nullptr, 'D'},
        {"debug",                 no_argument,       nullptr, 'd'},
        {"help",                  no_argument,       nullptr, 'h'},
        {nullptr,                 0,                 nullptr, 0  },
    };

    int c;
    while ((c = ::getopt_long(argc, argv, "", long_options, nullptr)) != -1) {
        switch (c) {
            case 'n': out.device_name = optarg; break;
            case 'H': out.host = optarg; break;
            case 'd': out.debug = true; break;
            case 'f': out.preferences_file = optarg; break;
            case 'a': out.audio_device = optarg; break;
            case 'w': out.wakeword_type = optarg; break;
            case 'm': out.wakeword_models = optarg; break;
            case 'B': out.capture_backend = optarg; break;
            case 'A': out.capture_alsa_device = optarg; break;
            case 'M': out.capture_mic_channel = std::atoi(optarg); break;
            case 'R': out.capture_ref_channels = optarg; break;
            case 'V':
                out.voice_backend = optarg;
                if (out.voice_backend != "local" && out.voice_backend != "ha") {
                    std::fprintf(stderr, "Invalid --voice-backend: %s\n",
                                 optarg);
                    return false;
                }
                break;
            case 'S': out.stt_model = optarg; break;
            case 'U': out.stt_model_url = optarg; break;
            case 'J': {
                char* end = nullptr;
                const long v = std::strtol(optarg, &end, 10);
                if (end == optarg || *end != '\0' || v < 1 || v > 4) {
                    std::fprintf(stderr, "Invalid --stt-threads: %s\n",
                                 optarg);
                    return false;
                }
                out.stt_threads = static_cast<int>(v);
                break;
            }
            case 'K': {
                char* end = nullptr;
                const long v = std::strtol(optarg, &end, 10);
                if (end == optarg || *end != '\0' || v < 1 || v > 4) {
                    std::fprintf(stderr, "Invalid --tts-threads: %s\n",
                                 optarg);
                    return false;
                }
                out.tts_threads = static_cast<int>(v);
                break;
            }
            case 'T': out.tts_data_dir = optarg; break;
            case 'G': out.conversation_agent = optarg; break;
            case 'D': out.stt_dump_wav = optarg; break;
            case 'C': {
                char* end = nullptr;
                const double v = std::strtod(optarg, &end);
                if (end == optarg || v < 0.0) {
                    std::fprintf(stderr,
                                 "Invalid --continue-conversation-delay: %s\n",
                                 optarg);
                    return false;
                }
                out.continue_conversation_delay = v;
                break;
            }
            case 'p': {
                const long v = std::strtol(optarg, nullptr, 10);
                if (v <= 0 || v > 65535) {
                    std::fprintf(stderr, "Invalid --port: %s\n", optarg);
                    return false;
                }
                out.port = static_cast<std::uint16_t>(v);
                break;
            }
            case 'h':
                PrintUsage(argv[0]);
                std::exit(0);
            case '?':
            default:
                PrintUsage(argv[0]);
                return false;
        }
    }
    return true;
}

std::optional<int> ParseControlInteger(const std::string& command,
                                       const char* prefix) {
    const std::size_t prefix_len = std::strlen(prefix);
    if (command.compare(0, prefix_len, prefix) != 0) return std::nullopt;

    const char* start = command.c_str() + prefix_len;
    if (*start == '\0') return std::nullopt;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(start, &end, 10);
    if (end == start || *end != '\0' || errno == ERANGE ||
        value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::string ReadMacFromDeviceJson(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};
    std::ifstream f(path);
    if (!f) return {};
    nlohmann::json j;
    try {
        f >> j;
    } catch (...) {
        return {};
    }
    if (!j.is_object()) return {};
    if (auto it = j.find("device"); it != j.end() && it->is_object()) {
        if (auto mit = it->find("macAddress");
            mit != it->end() && mit->is_string()) {
            return mit->get<std::string>();
        }
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    // Per-thread arenas keep freed memory after each voice turn. Must run
    // before any thread is created.
    ::mallopt(M_ARENA_MAX, 1);
    ::mallopt(M_MMAP_THRESHOLD, 128 * 1024);
    ::mallopt(M_TRIM_THRESHOLD, 256 * 1024);

    CliOptions cli;
    if (!ParseCli(argc, argv, cli)) {
        return 2;
    }

    lva::log::SetLevel(cli.debug ? lva::log::Level::kDebug
                                 : lva::log::Level::kInfo);

    if (cli.debug) {
        lva::log::SetLogFile("/usr/share/thirdreality/log/lva.log", 5u << 20);
    }

#ifdef LVA_DIAG_LOG
    lva::memprobe::Start();
#endif

    LVA_LOGI(kTag,
             "linux-voice-assistant-cpp %s starting "
             "(name=%s host=%s port=%u prefs=%s)",
             kVersion, cli.device_name.c_str(), cli.host.c_str(),
             static_cast<unsigned>(cli.port),
             cli.preferences_file.c_str());

    lva::state::ServerState state;
    state.name             = cli.device_name;
    state.friendly_name    = cli.device_name;
    {
        const lva::tr::DeviceInfo dev = lva::tr::ReadDeviceInfo();
        if (!dev.firmware_version.empty()) {
            state.version = dev.firmware_version;
        } else {
            LVA_LOGW(kTag, "device.firmwareVersion missing; "
                           "falling back to package version %s",
                     kVersion);
            state.version = kVersion;
        }
        if (!dev.mac_address.empty()) {
            state.mac_address = dev.mac_address;
        }
    }
    state.esphome_version  = "2025.9.0";
    if (state.mac_address.empty()) {
        state.mac_address = ReadMacFromDeviceJson("/data/conf/device.json");
    }
    std::transform(state.mac_address.begin(), state.mac_address.end(),
                   state.mac_address.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    state.preferences_path = cli.preferences_file;
    const bool use_openwakeword = (cli.wakeword_type == "open");
    const std::filesystem::path wakeword_dir = use_openwakeword
        ? "/usr/share/thirdreality/wakewords/openwakeword"
        : "/usr/share/thirdreality/wakewords/microwakeword";
    state.wakeword_dir = wakeword_dir;
    state.preferences      = lva::state::Preferences::LoadFromFile(
        cli.preferences_file);

    if (state.preferences.volume.has_value()) {
        state.volume.store(*state.preferences.volume,
                           std::memory_order_relaxed);
        lva::tr::SetSystemVolumeSilent(
            static_cast<int>(*state.preferences.volume * 100.0 + 0.5));
    }
    state.muted.store(state.preferences.is_mic_muted(),
                      std::memory_order_relaxed);
    state.mic_volume_live.store(state.preferences.mic_volume,
                                std::memory_order_relaxed);
    {
        const double delay_s =
            state.preferences.continue_conversation_delay.value_or(
                cli.continue_conversation_delay);
        state.continue_conversation_delay_ns =
            static_cast<std::int64_t>(delay_s * 1e9);
    }

    auto mic_mute_gpio = std::make_unique<lva::tr::MicMuteGpio>(state);
    state.mic_mute_gpio = mic_mute_gpio.get();
    if (mic_mute_gpio->Available()) {
        mic_mute_gpio->ReadAndApplyOnce();
    }

    lva::audio::LibMpvPlayer::Options mpv_opts;
    mpv_opts.audio_device = cli.audio_device;
    mpv_opts.cache_secs   = 10;
    mpv_opts.short_sound_safe = false;
    auto music_player = std::make_unique<lva::audio::LibMpvPlayer>(mpv_opts);

    lva::audio::LibMpvPlayer::Options tts_opts;
    tts_opts.audio_device     = cli.audio_device;
    tts_opts.cache_secs       = 1;
    tts_opts.short_sound_safe = true;
    auto announce_player = std::make_unique<lva::audio::LibMpvPlayer>(tts_opts);

    auto wake_ring = std::make_unique<lva::audio::PcmRingBuffer>(
        16'000 * 2);

    // Shadow detector input: WebRTC linear-AEC output.
    auto linear_wake_ring = std::make_unique<lva::audio::PcmRingBuffer>(
        16'000 * 2);

    auto satellite_ring = std::make_unique<lva::audio::PcmRingBuffer>(
        16'000 * 2);

    // The backend is fixed for the life of the process; switching it from
    // HA restarts LVA. The STT tap must be registered before capture starts.
    const std::string wanted_backend =
        state.preferences.voice_backend.value_or(cli.voice_backend);
    const bool stt_model_is_default = (cli.stt_model == kSttModelPath);
    std::unique_ptr<lva::tr::ModelDownloader> stt_model_downloader;
    if (stt_model_is_default) {
        stt_model_downloader = std::make_unique<lva::tr::ModelDownloader>(
            lva::tr::ModelSpec{
                .url    = cli.stt_model_url,
                .path   = kSttModelPath,
                .size   = kSttModelSize,
                .sha256 = kSttModelSha256,
            });
    }
    // Fetch the model on every start until it is installed, so local speech
    // is ready when the user picks it.
    if (stt_model_downloader) stt_model_downloader->Start();
    const auto stt_model_ready = [&]() {
        return stt_model_downloader ? stt_model_downloader->Installed()
                                    : std::filesystem::exists(cli.stt_model);
    };
    bool use_local_voice = (wanted_backend == "local");
    if (use_local_voice && !stt_model_ready()) {
        LVA_LOGW(kTag, "local STT model %s not found; using ha voice "
                       "backend%s", cli.stt_model.c_str(),
                 stt_model_downloader ? " until it is downloaded" : "");
        use_local_voice = false;
    }
    auto local_stt_ring = use_local_voice
        ? std::make_unique<lva::audio::PcmRingBuffer>(16'000 * 4)
        : nullptr;

    lva::audio::AudioCapture::Options cap_opts;
    const bool use_alsa_backend = (cli.capture_backend == "alsa");
    if (use_alsa_backend) {
        cap_opts.backend            = lva::audio::AudioCapture::Backend::kAlsa;
        cap_opts.alsa_device        = cli.capture_alsa_device;
        cap_opts.alsa_channels      = 4;
        cap_opts.mic_channel        =
            static_cast<unsigned>(std::max(0, cli.capture_mic_channel));
        cap_opts.frames_per_read    = 160;   // one WebRTC frame
        // "a,b" / "a" / "" / "none"
        std::array<int, 2> refs = {-1, -1};
        if (!cli.capture_ref_channels.empty() &&
            cli.capture_ref_channels != "none") {
            std::size_t comma = cli.capture_ref_channels.find(',');
            try {
                refs[0] = std::stoi(cli.capture_ref_channels.substr(0, comma));
                if (comma != std::string::npos) {
                    refs[1] = std::stoi(cli.capture_ref_channels.substr(comma + 1));
                }
            } catch (...) {
                LVA_LOGW(kTag,
                         "could not parse --capture-ref-channels '%s'; "
                         "AEC disabled",
                         cli.capture_ref_channels.c_str());
                refs = {-1, -1};
            }
        }
        cap_opts.ref_channels = refs;
    } else if (cli.capture_backend == "pulse") {
        cap_opts.backend            = lva::audio::AudioCapture::Backend::kPulse;
        cap_opts.frames_per_read    = 1600;
        cap_opts.buffer_latency_us  = 100'000;
    } else {
        LVA_LOGE(kTag, "unknown --capture-backend '%s'; expected alsa|pulse",
                 cli.capture_backend.c_str());
        return 2;
    }

    auto audio_capture = std::make_unique<lva::audio::AudioCapture>(
        cap_opts, *satellite_ring);
    audio_capture->AddWakeTap(*wake_ring);
    audio_capture->AddLinearWakeTap(*linear_wake_ring);
    if (local_stt_ring) audio_capture->AddTap(*local_stt_ring);

    // Set when a first-boot default is filled in, so sound.json is saved
    // once with every setting.
    bool prefs_dirty = false;
    if (state.preferences.mic_auto_gain == 0) {
        state.preferences.mic_auto_gain = 10;
        prefs_dirty = true;
    }
    if (state.preferences.mic_noise_suppression == 0) {
        state.preferences.mic_noise_suppression = 2;  // Medium NS
        prefs_dirty = true;
    }
    if (state.preferences.mic_volume == 100) {
        state.preferences.mic_volume = 1600;
        state.mic_volume_live.store(1600, std::memory_order_relaxed);
        prefs_dirty = true;
    }
    if (prefs_dirty) {
        LVA_LOGI(kTag,
                 "applied first-boot mic defaults: agc=%d, ns=%d, vol=%d",
                 state.preferences.mic_auto_gain,
                 state.preferences.mic_noise_suppression,
                 state.preferences.mic_volume);
    }

    const bool aec_enabled = use_alsa_backend &&
                             (cap_opts.ref_channels[0] >= 0);
    LVA_LOGI(kTag, "capture backend=%s aec=%s",
             use_alsa_backend ? "alsa" : "pulse",
             aec_enabled ? "enabled" : "disabled");

    auto webrtc_processor = std::make_unique<lva::audio::WebRtcProcessor>(
        state.preferences.mic_auto_gain,
        state.preferences.mic_noise_suppression,
        aec_enabled);
    audio_capture->SetProcessor(webrtc_processor.get());
    audio_capture->SetMicVolumeSource(&state.mic_volume_live);
    state.audio_capture = audio_capture.get();
    state.webrtc_processor = webrtc_processor.get();
    state.announce_player  = announce_player.get();
    state.music_player     = music_player.get();

    std::uint32_t next_key = 0;

    {
        auto media = std::make_unique<lva::entities::MediaPlayerEntity>(
            next_key++, state, music_player.get());
        state.media_player_entity = media.get();
        state.entities.push_back(std::move(media));
    }

    // In-process Sendspin player: a second media source behind the HA
    // media player (ESPHome sendspin hub + media_source).
    auto sendspin_hub = std::make_unique<lva::sendspin::SendspinHub>(
        lva::sendspin::SendspinHub::Config{
            .name             = state.friendly_name,
            .product_name     = "Voice & Music Assistant",
            .manufacturer     = "ThirdReality",
            .software_version = state.version,
            .mac_address      = state.mac_address,
            .initial_volume   = static_cast<int>(
                state.volume.load(std::memory_order_relaxed) * 100.0 + 0.5),
        },
        lva::sendspin::SendspinHub::Callbacks{
            // state.sendspin is cleared before Stop(), so callbacks fired
            // while stopping (e.g. at shutdown) are dropped here.
            .on_active_changed = [&state](bool active) {
                if (state.sendspin == nullptr) return;
                state.media_player_entity->OnSendspinActiveChanged(active);
            },
            .on_volume_request = [&state](int percent) {
                if (state.sendspin == nullptr) return;
                state.PersistVolume(static_cast<double>(percent) / 100.0);
            },
            .on_mute_request = [&state](bool muted) {
                if (state.sendspin == nullptr) return;
                state.media_player_entity->OnSendspinMuteRequest(muted);
            },
        });
    // _sendspin._tcp is published only while the client runs, like
    // ESPHome's enabled switch, so Music Assistant does not keep finding a
    // speaker it cannot connect to.
    lva::proto::MdnsPublisher sendspin_mdns;
    // Returns whether the client runs afterwards. The switch shows the
    // stored preference; a failed start is retried from the main loop.
    const auto set_sendspin_running = [&state, &sendspin_hub,
                                       &sendspin_mdns](bool run) -> bool {
        if (run) {
            if (!sendspin_hub->Start()) return false;
            state.sendspin = sendspin_hub.get();
            state.media_player_entity->SetSendspin(sendspin_hub.get());
            lva::proto::MdnsPublisher::Options opts;
            opts.name = state.name;
            opts.type = "_sendspin._tcp";
            opts.port = 8928;  // SendspinClientConfig::DEFAULT_SERVER_PORT
            opts.txt  = {"path=/sendspin"};
            if (!sendspin_mdns.Start(opts)) {
                LVA_LOGW(kTag, "_sendspin._tcp mDNS registration failed");
            }
            return true;
        }
        sendspin_mdns.Stop();
        state.sendspin = nullptr;
        state.media_player_entity->SetSendspin(nullptr);
        sendspin_hub->Stop();
        state.media_player_entity->OnSendspinActiveChanged(false);
        return false;
    };

    {
        auto mute = std::make_unique<lva::entities::MuteSwitchEntity>(
            next_key++, state);
        state.mute_switch_entity = mute.get();
        state.entities.push_back(std::move(mute));
    }
    state.entities.push_back(std::make_unique<lva::entities::ThinkingSoundEntity>(
        next_key++, state));


    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++,
        lva::entities::NumberEntity::Config{
            .object_id     = "wake_word_1_sensitivity",
            .display_name  = "Wake Word 1 Sensitivity",
            .icon          = "",
            .min_value     = 0.0,
            .max_value     = 1.0,
            .step          = 0.001,
            .mode_enum     = 1,  // BOX
            .getter        = [&state, use_openwakeword] {
                const double fallback = use_openwakeword
                    ? kDefaultOpenWakeSensitivity
                    : kDefaultMicroWakeSensitivity;
                return state.preferences.wake_word_1_sensitivity.value_or(
                    fallback);
            },
            .setter        = [&state](double v) {
                state.PersistWakeWordSensitivity(1, v);
            },
        }));

    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++,
        lva::entities::NumberEntity::Config{
            .object_id     = "wake_word_2_sensitivity",
            .display_name  = "Wake Word 2 Sensitivity",
            .icon          = "",
            .min_value     = 0.0,
            .max_value     = 1.0,
            .step          = 0.001,
            .mode_enum     = 1,
            .getter        = [&state, use_openwakeword] {
                const double fallback = use_openwakeword
                    ? kDefaultOpenWakeSensitivity
                    : kDefaultMicroWakeSensitivity;
                return state.preferences.wake_word_2_sensitivity.value_or(
                    fallback);
            },
            .setter        = [&state](double v) {
                state.PersistWakeWordSensitivity(2, v);
            },
        }));

    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++,
        lva::entities::NumberEntity::Config{
            .object_id     = "stop_word_sensitivity",
            .display_name  = "Stop Word Sensitivity",
            .icon          = "mdi:hand-back-left",
            .min_value     = 0.0,
            .max_value     = 1.0,
            .step          = 0.001,
            .mode_enum     = 1,
            .getter        = [&state] {
                return state.preferences.stop_word_sensitivity.value_or(
                    kDefaultStopSensitivity);
            },
            .setter        = [&state](double v) {
                state.PersistStopWordSensitivity(v);
            },
        }));

    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++,
        lva::entities::NumberEntity::Config{
            .object_id     = "continue_conversation_delay",
            .display_name  = "Continue Conversation Delay",
            .icon          = "mdi:timer-sand",
            .min_value     = 0.0,
            .max_value     = 10.0,
            .step          = 0.1,
            .mode_enum     = 1,  // BOX
            .getter        = [&state, default_ccd = cli.continue_conversation_delay] {
                return state.preferences.continue_conversation_delay.value_or(
                    default_ccd);
            },
            .setter        = [&state](double v) {
                state.PersistContinueConversationDelay(v);
            },
        }));

    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++,
        lva::entities::NumberEntity::Config{
            .object_id     = "mic_gain",
            .display_name  = "Mic Auto Gain",
            .icon          = "mdi:microphone-plus",
            .min_value     = 0.0,
            .max_value     = 31.0,
            .step          = 1.0,
            .mode_enum     = 0,  // AUTO
            .getter        = [&state] {
                return static_cast<double>(state.preferences.mic_auto_gain);
            },
            .setter        = [&state](double v) {
                state.PersistMicAutoGain(static_cast<int>(v));
            },
        }));

    {
        static const std::vector<std::string> kNoiseOptions = {
            "Off", "Low", "Medium", "High", "Max",
        };
        state.entities.push_back(std::make_unique<lva::entities::SelectEntity>(
            next_key++,
            lva::entities::SelectEntity::Config{
                .object_id     = "mic_noise",
                .display_name  = "Mic Noise Suppression",
                .icon          = "mdi:waveform",
                .options       = kNoiseOptions,
                .getter        = [&state]() -> std::string {
                    const int idx = std::clamp(
                        state.preferences.mic_noise_suppression, 0, 4);
                    return kNoiseOptions[static_cast<std::size_t>(idx)];
                },
                .setter        = [&state](const std::string& label) {
                    for (std::size_t i = 0; i < kNoiseOptions.size(); ++i) {
                        if (kNoiseOptions[i] == label) {
                            state.PersistMicNoiseSuppression(
                                static_cast<int>(i));
                            return;
                        }
                    }
                    LVA_LOGW(kTag, "mic_noise: unknown label '%s'",
                             label.c_str());
                },
            }));
    }

    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++,
        lva::entities::NumberEntity::Config{
            .object_id     = "mic_volume",
            .display_name  = "Mic Volume",
            .icon          = "mdi:microphone-settings",
            .min_value     = 1.0,
            .max_value     = 4000.0,
            .step          = 1.0,
            .mode_enum     = 0,  // AUTO
            .getter        = [&state] {
                return static_cast<double>(state.preferences.mic_volume);
            },
            .setter        = [&state](double v) {
                state.PersistMicVolume(static_cast<int>(v));
            },
        }));

    // Created later if the local backend is in use; the rate entity below
    // applies changes to it.
    std::unique_ptr<lva::audio::SanoTts> local_tts;

    // Set when HA picks another voice backend; main loop restarts LVA.
    bool restart_for_backend = false;
    const auto DownloadNotice = [&]() -> std::string {
        const std::string name = state.name;
        if (!stt_model_downloader) {
            return name + ": the speech model is missing; using Home "
                          "Assistant for voice.";
        }
        const auto st = stt_model_downloader->GetStatus();
        return name + ": the speech model is still downloading (" +
               std::to_string(st.percent) + "%). The speaker keeps using "
               "Home Assistant for voice and switches to local speech "
               "automatically when the download finishes.";
    };
    const auto backend_label = [&state, &cli]() -> std::string {
        return state.preferences.voice_backend.value_or(cli.voice_backend) ==
                       "local"
                   ? kVoiceBackendLocal
                   : kVoiceBackendHa;
    };
    auto backend_select = std::make_unique<lva::entities::SelectEntity>(
        next_key++,
        lva::entities::SelectEntity::Config{
            .object_id     = "speech_processing",
            .display_name  = "Speech Processing",
            .icon          = "mdi:account-voice",
            .options       = {kVoiceBackendHa, kVoiceBackendLocal},
            .getter        = backend_label,
            .setter        = [&](const std::string& label) {
                std::string backend;
                if (label == kVoiceBackendHa)         backend = "ha";
                else if (label == kVoiceBackendLocal) backend = "local";
                else {
                    LVA_LOGW(kTag, "speech_processing: unknown label '%s'",
                             label.c_str());
                    return;
                }
                state.PersistVoiceBackend(backend);
                const bool want_local = (backend == "local");
                if (want_local && !stt_model_ready()) {
                    // Keep the HA backend until the model is in place. A
                    // failed download is retried; if it fails again the
                    // main loop sets the select back to Home Assistant.
                    if (stt_model_downloader) stt_model_downloader->Start();
                    state.NotifyHa(kVoiceNotifyId, kVoiceNotifyTitle,
                             DownloadNotice());
                    return;
                }
                // Picking the running backend again cancels a pending
                // switch, e.g. back to HA right after the model finished.
                restart_for_backend = (want_local != use_local_voice);
                if (restart_for_backend) {
                    LVA_LOGI(kTag, "voice backend -> %s; restart pending",
                             backend.c_str());
                }
            },
        });
    lva::entities::SelectEntity* backend_select_ptr = backend_select.get();
    state.entities.push_back(std::move(backend_select));

    state.entities.push_back(std::make_unique<lva::entities::NumberEntity>(
        next_key++,
        lva::entities::NumberEntity::Config{
            .object_id     = "speech_rate",
            .display_name  = "Speech Rate (Local TTS)",
            .icon          = "mdi:speedometer",
            .min_value     = 100.0,
            .max_value     = 250.0,
            .step          = 5.0,
            .mode_enum     = 2,  // SLIDER
            .getter        = [&state] {
                return static_cast<double>(
                    state.preferences.tts_rate.value_or(kDefaultTtsRate));
            },
            .setter        = [&](double v) {
                const int rate = static_cast<int>(v);
                state.PersistTtsRate(rate);
                if (local_tts) local_tts->SetSpeed(TtsSpeed(rate));
            },
        }));

    state.entities.push_back(
        std::make_unique<lva::entities::SendspinSwitchEntity>(
            next_key++,
            [&state] { return state.preferences.sendspin_enabled; },
            [&state, &sendspin_hub, set_sendspin_running](bool on) {
                state.PersistSendspinEnabled(on);
                if (on == sendspin_hub->IsStarted()) return;
                if (on && !set_sendspin_running(true)) {
                    LVA_LOGW(kTag, "Sendspin start failed; retrying");
                } else if (!on) {
                    set_sendspin_running(false);
                }
            }));

    const std::uint32_t home_button_entity_key = next_key++;
    lva::entities::EventEntity* home_button_entity = nullptr;
    {
        auto ev = std::make_unique<lva::entities::EventEntity>(
            home_button_entity_key,
            lva::entities::EventEntity::Config{
                .object_id    = "thirdreality_home_button",
                .display_name = "Home Button",
                .icon         = "mdi:gesture-tap-button",
                .event_types  = {"single_press", "double_press",
                                 "triple_press"},
            });
        home_button_entity = ev.get();
        state.entities.push_back(std::move(ev));
    }

    lva::entities::UpdateEntity* update_entity_ptr = nullptr;
    {
        auto upd = std::make_unique<lva::entities::UpdateEntity>(
            next_key++, state);
        update_entity_ptr = upd.get();
        state.entities.push_back(std::move(upd));
    }

    LVA_LOGI(kTag, "registered %zu entities", state.entities.size());

    if (update_entity_ptr != nullptr) {
        state.on_client_authenticated = [update_entity_ptr] {
            update_entity_ptr->TriggerCheck();
        };
    }

    lva::proto::ApiServer::Options opts;
    opts.bind_address = cli.host;
    opts.port         = cli.port;

    lva::proto::ApiServer server(state, opts);
    g_server = &server;

    InstallSignalHandlers();

    if (!server.Start()) {
        LVA_LOGE(kTag, "ApiServer::Start failed");
        g_server = nullptr;
        return 1;
    }

    lva::proto::MdnsPublisher mdns;
    {
        lva::proto::MdnsPublisher::Options mdns_opts;
        mdns_opts.name    = state.name;
        mdns_opts.port    = cli.port;
        mdns_opts.mac     = state.mac_address;
        mdns_opts.version = state.esphome_version;
        if (!mdns.Start(mdns_opts)) {
            LVA_LOGW(kTag, "mDNS registration failed; device won't "
                           "be auto-discovered by HA");
        }
    }

    if (music_player->WakeupFd() >= 0) {
        server.AddAuxFd(music_player->WakeupFd(),
                        [&music_player] {
                            music_player->DrainEvents();
                        });
    }
    if (announce_player->WakeupFd() >= 0) {
        server.AddAuxFd(announce_player->WakeupFd(),
                        [&announce_player] {
                            announce_player->DrainEvents();
                        });
    }

    if (!audio_capture->Start()) {
        LVA_LOGW(kTag, "audio capture failed to start; mic-using "
                       "features (wake word) will be unavailable");
    }

    auto wakeword_engine =
        std::make_unique<lva::audio::WakeWordEngine>(*wake_ring);

    {
        std::vector<std::string> ids;
        for (const auto& slot : state.preferences.active_wake_words) {
            if (slot.has_value() && !slot->empty()) ids.push_back(*slot);
        }
        if (ids.empty()) {
            const std::string& list = cli.wakeword_models;
            std::size_t start = 0;
            while (start < list.size()) {
                std::size_t comma = list.find(',', start);
                if (comma == std::string::npos) comma = list.size();
                std::string id(list, start, comma - start);
                while (!id.empty() && std::isspace(static_cast<unsigned char>(id.front()))) id.erase(0,1);
                while (!id.empty() && std::isspace(static_cast<unsigned char>(id.back())))  id.pop_back();
                if (!id.empty()) ids.push_back(std::move(id));
                start = comma + 1;
            }
            LVA_LOGI(kTag, "no persisted wake words; using CLI default '%s'",
                     cli.wakeword_models.c_str());
        } else {
            LVA_LOGI(kTag, "loading %zu persisted wake word(s)", ids.size());
        }

        for (const auto& id : ids) {
            const auto json_path = wakeword_dir / (id + ".json");
            if (use_openwakeword) {
                auto model = lva::audio::OpenWakeWord::FromConfig(json_path);
                if (model && model->Ok()) {
                    wakeword_engine->AddOpenModel(std::move(model));
                } else {
                    LVA_LOGW(kTag, "failed to load OWW '%s'", id.c_str());
                }
            } else {
                auto model = lva::audio::MicroWakeWord::FromConfig(json_path);
                if (model && model->Ok()) {
                    model->SetSourceTag("post_apm");
                    wakeword_engine->AddModel(std::move(model));
                } else {
                    LVA_LOGW(kTag, "failed to load wake word '%s'", id.c_str());
                }
            }
        }
    }

    if (use_openwakeword) {
        auto oww_feat = std::make_unique<lva::audio::OpenWakeWordFeatures>();
        if (oww_feat->Load(wakeword_dir)) {
            wakeword_engine->SetOpenFeatures(std::move(oww_feat));
        } else {
            LVA_LOGE(kTag, "OpenWakeWord features failed to load");
        }
    }

    if (!use_openwakeword) {
        // The stop model only exists for microWakeWord.
        const std::filesystem::path micro_dir =
            "/usr/share/thirdreality/wakewords/microwakeword";
        const auto stop_json = micro_dir / "stop.json";
        auto stop_model = lva::audio::MicroWakeWord::FromConfig(stop_json);
        if (stop_model && stop_model->Ok()) {
            wakeword_engine->AddModel(std::move(stop_model));
        } else {
            LVA_LOGW(kTag, "stop word model not loaded");
        }
    }

    // Models load with their built-in cutoff; apply the saved ones. Slot
    // order must match PersistWakeWordSensitivity(): micro, then open.
    {
        std::size_t wake_idx = 0;
        const auto micro_models = wakeword_engine->SnapshotModels();
        for (const auto& m : micro_models) {
            if (m->config().id == "stop") {
                if (state.preferences.stop_word_sensitivity.has_value()) {
                    m->SetProbabilityCutoff(static_cast<float>(
                        *state.preferences.stop_word_sensitivity));
                }
                continue;
            }
            const auto& slot = (wake_idx == 0)
                ? state.preferences.wake_word_1_sensitivity
                : state.preferences.wake_word_2_sensitivity;
            if (wake_idx < 2 && slot.has_value()) {
                m->SetProbabilityCutoff(static_cast<float>(*slot));
            }
            ++wake_idx;
        }
        const auto open_models = wakeword_engine->SnapshotOpenModels();
        for (const auto& m : open_models) {
            const auto& slot = (wake_idx == 0)
                ? state.preferences.wake_word_1_sensitivity
                : state.preferences.wake_word_2_sensitivity;
            if (wake_idx < 2 && slot.has_value()) {
                m->SetProbabilityCutoff(static_cast<float>(*slot));
            }
            ++wake_idx;
        }
    }

    // First-boot defaults, only for slots with a loaded model.
    {
        const double wake_default = use_openwakeword
            ? kDefaultOpenWakeSensitivity
            : kDefaultMicroWakeSensitivity;
        std::size_t loaded_wake_words =
            wakeword_engine->SnapshotOpenModels().size();
        for (const auto& m : wakeword_engine->SnapshotModels()) {
            if (m->config().id != "stop") ++loaded_wake_words;
        }

        if (loaded_wake_words >= 1 &&
            !state.preferences.wake_word_1_sensitivity.has_value()) {
            state.preferences.wake_word_1_sensitivity = wake_default;
            prefs_dirty = true;
        }
        if (loaded_wake_words >= 2 &&
            !state.preferences.wake_word_2_sensitivity.has_value()) {
            state.preferences.wake_word_2_sensitivity = wake_default;
            prefs_dirty = true;
        }
        if (!use_openwakeword &&
            !state.preferences.stop_word_sensitivity.has_value()) {
            state.preferences.stop_word_sensitivity = kDefaultStopSensitivity;
            prefs_dirty = true;
        }
        if (!state.preferences.continue_conversation_delay.has_value()) {
            state.preferences.continue_conversation_delay =
                cli.continue_conversation_delay;
            prefs_dirty = true;
        }
    }

    if (prefs_dirty) {
        if (state.SavePreferences()) {
            LVA_LOGI(kTag, "persisted complete config to %s",
                     state.preferences_path.c_str());
        } else {
            LVA_LOGW(kTag, "failed to persist first-boot config to %s",
                     state.preferences_path.c_str());
        }
    }

    // Only one of the two wake threads may hand a wake to Satellite at a
    // time; they would otherwise race on the pending-wake payload.
    std::atomic<std::int64_t> last_wake_dispatch_ns{0};

    auto satellite = std::make_unique<lva::satellite::Satellite>(
        state, *satellite_ring, *wakeword_engine, announce_player.get());
    state.satellite = satellite.get();

    std::unique_ptr<lva::satellite::LocalStt> local_stt;
    if (use_local_voice) {
        lva::audio::WhisperStt::Config wcfg;
        wcfg.model_path  = cli.stt_model;
        wcfg.num_threads = cli.stt_threads;

        lva::satellite::LocalStt::Options stt_opts;
        stt_opts.dump_wav_path = cli.stt_dump_wav;
        local_stt = std::make_unique<lva::satellite::LocalStt>(
            std::make_unique<lva::audio::WhisperStt>(wcfg),
            *local_stt_ring, stt_opts);
        local_stt->Start();

        lva::audio::SanoTts::Config tts_cfg;
        tts_cfg.espeak_data_dir = cli.tts_data_dir;
        tts_cfg.voice_dir       = kTtsVoiceDir;
        tts_cfg.speed           = TtsSpeed(
            state.preferences.tts_rate.value_or(kDefaultTtsRate));
        tts_cfg.threads         = cli.tts_threads;
        local_tts = std::make_unique<lva::audio::SanoTts>(tts_cfg);
        if (!local_tts->Start()) {
            LVA_LOGW(kTag, "local TTS unavailable; replies will be silent");
            local_tts.reset();
        }

        lva::satellite::LocalVoiceConfig voice_cfg;
        voice_cfg.stt                = local_stt.get();
        voice_cfg.tts                = local_tts.get();
        voice_cfg.conversation_agent = cli.conversation_agent;
        satellite->EnableLocalVoice(std::move(voice_cfg));
    }
    LVA_LOGI(kTag, "voice backend=%s", use_local_voice ? "local" : "ha");

    // Independent shadow engine; deliberately not in state.wakeword_engine,
    // HA only manages the primary one.
    auto linear_wakeword_engine =
        std::make_unique<lva::audio::WakeWordEngine>(*linear_wake_ring);

    if (!use_openwakeword) {
        for (const auto& primary_model :
             wakeword_engine->SnapshotModels()) {
            if (!primary_model) continue;

            const auto json_path =
                wakeword_dir / (primary_model->config().id + ".json");

            auto shadow_model =
                lva::audio::MicroWakeWord::FromConfig(json_path);

            if (shadow_model && shadow_model->Ok()) {
                // Linear-AEC output only exists during playback, where real
                // wakes score far below the primary cutoff.
                constexpr float kShadowProbabilityCutoff = 0.40f;
                shadow_model->SetProbabilityCutoff(kShadowProbabilityCutoff);
                shadow_model->SetSourceTag("linear_aec");

                LVA_LOGI(
                    kTag,
                    "shadow model loaded: id=%s cutoff=%.3f "
                    "(primary cutoff=%.3f)",
                    primary_model->config().id.c_str(),
                    shadow_model->ProbabilityCutoff(),
                    primary_model->ProbabilityCutoff());

                linear_wakeword_engine->AddModel(
                    std::move(shadow_model));
            } else {
                LVA_LOGW(
                    kTag,
                    "shadow model failed to load: %s",
                    primary_model->config().id.c_str());
            }
        }
    }

    state.on_client_disconnected = [&satellite] {
        satellite->OnDisconnected();
    };

    auto sound_watcher = std::make_unique<lva::tr::SoundConfWatcher>(
        state, cli.preferences_file);

    auto supervisor = std::make_unique<lva::tr::Supervisor>();
    state.supervisor = supervisor.get();
    auto supervisor_http =
        std::make_unique<lva::tr::SupervisorHttpServer>(*supervisor);
    if (!supervisor_http->Start()) {
        LVA_LOGW(kTag, "supervisor HTTP failed to start; OTA over "
                       "the legacy supervisor API will not be "
                       "available (HA UI Update entity still works)");
    }

    wakeword_engine->SetDetectionCallback(
        [&satellite, &last_wake_dispatch_ns](
            const std::string& model_id, float prob) {
            if (model_id == "stop") {
                LVA_LOGD("wake",
                         "stop word detected prob=%.3f", prob);
                satellite->OnStopDetected();
                return;
            }

            const auto now_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()).count();

            constexpr std::int64_t kWakeDedupNs =
                1'000'000'000LL;

            auto previous =
                last_wake_dispatch_ns.load(
                    std::memory_order_relaxed);

            while (true) {
                if (previous != 0 &&
                    now_ns - previous < kWakeDedupNs) {
                    LVA_LOGD(
                        "wake",
                        "duplicate wake suppressed source=post_apm "
                        "id=%s prob=%.3f",
                        model_id.c_str(), prob);
                    return;
                }

                if (last_wake_dispatch_ns
                        .compare_exchange_weak(
                            previous,
                            now_ns,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
                    break;
                }
            }

            LVA_LOGD(
                "wake",
                "wake candidate source=post_apm "
                "id=%s prob=%.3f",
                model_id.c_str(), prob);

            satellite->OnWakeDetected(model_id, prob);
        });

    linear_wakeword_engine->SetDetectionCallback(
        [&satellite,
         &last_wake_dispatch_ns,
         &state](
            const std::string& model_id, float prob) {
            const auto now_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()).count();

            if (model_id == "stop") {
                const auto guard_until_ns =
                    state.webrtc_processor != nullptr
                        ? state.webrtc_processor->LinearGuardUntilNs()
                        : 0;

                if (guard_until_ns != 0 &&
                    now_ns < guard_until_ns) {
                    const auto remaining_ms =
                        (guard_until_ns - now_ns) / 1'000'000LL;

                    LVA_LOGD(
                        "wake",
                        "linear stop guard suppressed "
                        "prob=%.3f remaining_ms=%lld",
                        prob,
                        static_cast<long long>(remaining_ms));
                    return;
                }

                LVA_LOGD(
                    "wake",
                    "stop word detected source=linear_aec prob=%.3f",
                    prob);
                satellite->OnStopDetected();
                return;
            }

            const auto guard_until_ns =
                state.webrtc_processor != nullptr
                    ? state.webrtc_processor->LinearGuardUntilNs()
                    : 0;

            if (guard_until_ns != 0 &&
                now_ns < guard_until_ns) {
                const auto remaining_ms =
                    (guard_until_ns - now_ns) / 1'000'000LL;

                LVA_LOGD(
                    "wake",
                    "linear resume guard suppressed "
                    "id=%s prob=%.3f remaining_ms=%lld",
                    model_id.c_str(),
                    prob,
                    static_cast<long long>(remaining_ms));

                return;
            }

            constexpr std::int64_t kWakeDedupNs =
                1'000'000'000LL;

            auto previous =
                last_wake_dispatch_ns.load(
                    std::memory_order_relaxed);

            while (true) {
                if (previous != 0 &&
                    now_ns - previous < kWakeDedupNs) {
                    LVA_LOGD(
                        "wake",
                        "duplicate wake suppressed source=linear_aec "
                        "id=%s prob=%.3f",
                        model_id.c_str(), prob);
                    return;
                }

                if (last_wake_dispatch_ns
                        .compare_exchange_weak(
                            previous,
                            now_ns,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
                    break;
                }
            }

            LVA_LOGD(
                "wake",
                "wake candidate source=linear_aec "
                "id=%s prob=%.3f",
                model_id.c_str(), prob);

            satellite->OnWakeDetected(model_id, prob);
        });

    state.wakeword_engine = wakeword_engine.get();
    g_wakeword_engine = wakeword_engine.get();

    if (!wakeword_engine->Start()) {
        LVA_LOGW(kTag, "wake-word engine failed to start; voice "
                       "pipeline disabled");
    }

    if (!use_openwakeword &&
        !linear_wakeword_engine->SnapshotModels().empty()) {
        if (!linear_wakeword_engine->Start()) {
            LVA_LOGW(
                kTag,
                "linear-AEC shadow wake engine failed to start");
        } else {
            LVA_LOGI(
                kTag,
                "dual-path wake active "
                "(primary=post_apm secondary=linear_aec)");

            LVA_LOGI(
                kTag,
                "wake guards active: linear_resume=1500 ms stop_after_wake=1000 ms");
        }
    }

    const int satellite_tick_fd = ::timerfd_create(CLOCK_MONOTONIC,
                                                   TFD_CLOEXEC | TFD_NONBLOCK);
    if (satellite_tick_fd >= 0) {
        itimerspec ts{};
        ts.it_value.tv_sec     = 0;
        ts.it_value.tv_nsec    = 50'000'000;  // first fire 50 ms
        ts.it_interval.tv_sec  = 0;
        ts.it_interval.tv_nsec = 50'000'000;  // every 50 ms
        if (::timerfd_settime(satellite_tick_fd, 0, &ts, nullptr) == 0) {
            server.AddAuxFd(satellite_tick_fd,
                            [&satellite, &mic_mute_gpio, &sound_watcher,
                             update_entity_ptr, satellite_tick_fd,
                             &stt_model_downloader, &restart_for_backend,
                             &use_local_voice, &state, &cli, &server,
                             backend_select_ptr]() {
                std::uint64_t expirations = 0;
                while (::read(satellite_tick_fd,
                              &expirations, sizeof(expirations)) > 0) {
                }
                satellite->OnLoopTick();
                mic_mute_gpio->Poll();
                sound_watcher->Poll();
                if (update_entity_ptr) update_entity_ptr->OnPeriodicTick();

                const bool want_local =
                    state.preferences.voice_backend.value_or(
                        cli.voice_backend) == "local";
                if (stt_model_downloader && want_local && !use_local_voice &&
                    !restart_for_backend) {
                    using Phase = lva::tr::ModelDownloader::Phase;
                    const auto st = stt_model_downloader->GetStatus();
                    if (st.phase == Phase::kReady) {
                        state.NotifyHa(kVoiceNotifyId, kVoiceNotifyTitle,
                                       state.name + ": the speech model is "
                                       "ready. Switching to local speech.");
                        restart_for_backend = true;
                    } else if (st.phase == Phase::kFailed) {
                        // Local speech cannot work; show HA as the backend
                        // again instead of a choice that is not in effect.
                        LVA_LOGW(kTag, "STT model download failed (%s); "
                                       "voice backend back to ha",
                                 st.error.c_str());
                        state.PersistVoiceBackend("ha");
                        if (state.broadcast) {
                            backend_select_ptr->OnSubscribeStates(
                                state.broadcast);
                        }
                        state.NotifyHa(kVoiceNotifyId, kVoiceNotifyTitle,
                                       state.name + ": the speech model "
                                       "download failed (" + st.error +
                                       "). Speech Processing is set back to "
                                       "Home Assistant; pick 3R Speaker to "
                                       "try again.");
                    }
                }
                if (restart_for_backend && satellite->IsIdle()) {
                    // The init script's monitor starts LVA again.
                    LVA_LOGI(kTag, "restarting to apply the voice backend");
                    restart_for_backend = false;
                    server.Stop();
                }
            });
        } else {
            LVA_LOGW(kTag, "timerfd_settime failed; satellite ticks disabled");
            ::close(satellite_tick_fd);
        }
    } else {
        LVA_LOGW(kTag, "timerfd_create failed; satellite ticks disabled");
    }

    bool tap_playback_pending = false;
    bool tap_ha_was_playing = false;

    // tr-buttond owns the input device and forwards gestures here.
    auto control_listener = std::make_unique<lva::tr::ControlListener>(
        [&state, home_button_entity, &satellite, &sendspin_hub,
         &tap_playback_pending, &tap_ha_was_playing](const std::string& cmd) {
            if (cmd == "TAP_DOWN") {
                if (satellite) satellite->OnTapPressed();
                tap_ha_was_playing =
                    state.media_player_entity != nullptr &&
                    state.media_player_entity->IsPlaying();
                tap_playback_pending = true;
                return true;
            }
            if (cmd == "TAP") {
                const bool tap_down_seen = tap_playback_pending;
                tap_playback_pending = false;
                const bool ha_was_playing = tap_down_seen
                    ? tap_ha_was_playing
                    : state.media_player_entity != nullptr &&
                          state.media_player_entity->IsPlaying();

                if (satellite && satellite->AbortSpeechIfSpeaking()) {
                    return true;
                }
                if (ha_was_playing) {
                    state.media_player_entity->PausePlayback();
                } else {
                    state.media_player_entity->ResumePlayback();
                }
                return true;
            }
            if (cmd == "LED_TOGGLE") {
                sendspin_hub->ToggleLed();
                return true;
            }
            if (cmd == "HOME_SINGLE" || cmd == "HOME_DOUBLE" ||
                cmd == "HOME_TRIPLE") {
                if (home_button_entity != nullptr && state.broadcast) {
                    const char* event_type =
                        (cmd == "HOME_SINGLE") ? "single_press"
                        : (cmd == "HOME_DOUBLE") ? "double_press"
                                                 : "triple_press";
                    home_button_entity->Trigger(event_type, state.broadcast);
                }
                return true;
            }
            if (const auto value =
                    ParseControlInteger(cmd, "CONFIG_SET_VOLUME:")) {
                if (*value < 0 || *value > 100) return false;
                state.PersistVolume(static_cast<double>(*value) / 100.0);
                return true;
            }
            if (const auto value =
                    ParseControlInteger(cmd, "CONFIG_SET_MIC_MUTE:")) {
                if (*value != 0 && *value != 1) return false;
                const bool muted = (*value == 0);
                state.PersistMuted(muted);
                state.PlayMuteToggleSound(muted);
                return true;
            }
            return false;
        });
    {
        const int ctl_fd = control_listener->Start();
        if (ctl_fd >= 0) {
            server.AddAuxFd(ctl_fd, [&control_listener] {
                control_listener->OnReadable();
            });
        } else {
            LVA_LOGW(kTag, "button control listener disabled");
        }
    }

    // sendspin-cpp delivers its callbacks from loop() on this thread.
    if (state.preferences.sendspin_enabled && !set_sendspin_running(true)) {
        LVA_LOGW(kTag, "Sendspin start failed; retrying");
    }
    const int sendspin_tick_fd = ::timerfd_create(CLOCK_MONOTONIC,
                                                  TFD_CLOEXEC | TFD_NONBLOCK);
    if (sendspin_tick_fd >= 0) {
        itimerspec ts{};
        ts.it_value.tv_nsec    = 33'000'000;
        ts.it_interval.tv_nsec = 33'000'000;
        if (::timerfd_settime(sendspin_tick_fd, 0, &ts, nullptr) == 0) {
            server.AddAuxFd(sendspin_tick_fd,
                            [sendspin_tick_fd, &sendspin_hub, &state,
                             set_sendspin_running] {
                std::uint64_t expirations = 0;
                while (::read(sendspin_tick_fd, &expirations,
                              sizeof(expirations)) > 0) {
                }
                // Enabled but not running (e.g. PulseAudio was not up yet):
                // retry every 5 s.
                static auto next_retry = std::chrono::steady_clock::now();
                if (state.preferences.sendspin_enabled &&
                    !sendspin_hub->IsStarted() &&
                    std::chrono::steady_clock::now() >= next_retry) {
                    next_retry = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(5);
                    if (set_sendspin_running(true)) {
                        LVA_LOGI(kTag, "Sendspin started on retry");
                    }
                }
                sendspin_hub->Loop();
            });
        } else {
            LVA_LOGE(kTag, "sendspin timerfd_settime failed");
        }
    } else {
        LVA_LOGE(kTag, "sendspin timerfd_create failed");
    }

    const int rc = server.Run();
    g_server = nullptr;

    // client/goodbye to the Sendspin server before the process exits.
    sendspin_mdns.Stop();
    state.sendspin = nullptr;
    state.media_player_entity->SetSendspin(nullptr);
    sendspin_hub->Stop();
    g_wakeword_engine = nullptr;

    if (supervisor_http) supervisor_http->Stop();
    mdns.Stop();
    wakeword_engine->Stop();
    linear_wakeword_engine->Stop();
    if (local_stt) local_stt->Stop();
    if (stt_model_downloader) stt_model_downloader->Cancel();
    audio_capture->Stop();

    const int signo = g_shutdown_signal.load(std::memory_order_relaxed);
    if (signo != 0) {
        LVA_LOGI(kTag, "exiting after signal %d", signo);
    }
    ::_exit(rc);
}
