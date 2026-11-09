#include "app/CliOptions.h"

#include <getopt.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace lva::app {
namespace {
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
        "  --wakeword-type <type>   micro|open (default: micro); selects model directory\n"
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

}  // namespace

bool ParseCli(int argc, char** argv, CliOptions& out) {
    static const struct option long_options[] = {
        {"name", required_argument, nullptr, 'n'},
        {"port", required_argument, nullptr, 'p'},
        {"host", required_argument, nullptr, 'H'},
        {"preferences-file", required_argument, nullptr, 'f'},
        {"audio-device", required_argument, nullptr, 'a'},
        {"wakeword-type", required_argument, nullptr, 'w'},
        {"wakeword-models", required_argument, nullptr, 'm'},
        {"capture-backend", required_argument, nullptr, 'B'},
        {"capture-alsa-device", required_argument, nullptr, 'A'},
        {"capture-mic-channel", required_argument, nullptr, 'M'},
        {"capture-ref-channels", required_argument, nullptr, 'R'},
        {"continue-conversation-delay", required_argument, nullptr, 'C'},
        {"voice-backend", required_argument, nullptr, 'V'},
        {"stt-model", required_argument, nullptr, 'S'},
        {"stt-model-url", required_argument, nullptr, 'U'},
        {"stt-threads", required_argument, nullptr, 'J'},
        {"tts-threads", required_argument, nullptr, 'K'},
        {"tts-data-dir", required_argument, nullptr, 'T'},
        {"conversation-agent", required_argument, nullptr, 'G'},
        {"stt-dump-wav", required_argument, nullptr, 'D'},
        {"debug", no_argument, nullptr, 'd'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int c;
    while ((c = ::getopt_long(argc, argv, "", long_options, nullptr)) != -1) {
        switch (c) {
            case 'n':
                out.device_name = optarg;
                break;
            case 'H':
                out.host = optarg;
                break;
            case 'd':
                out.debug = true;
                break;
            case 'f':
                out.preferences_file = optarg;
                break;
            case 'a':
                out.audio_device = optarg;
                break;
            case 'w':
                out.wakeword_type = optarg;
                break;
            case 'm':
                out.wakeword_models = optarg;
                break;
            case 'B':
                out.capture_backend = optarg;
                break;
            case 'A':
                out.capture_alsa_device = optarg;
                break;
            case 'M':
                out.capture_mic_channel = std::atoi(optarg);
                break;
            case 'R':
                out.capture_ref_channels = optarg;
                break;
            case 'V':
                out.voice_backend = optarg;
                if (out.voice_backend != "local" && out.voice_backend != "ha") {
                    std::fprintf(stderr, "Invalid --voice-backend: %s\n", optarg);
                    return false;
                }
                break;
            case 'S':
                out.stt_model = optarg;
                break;
            case 'U':
                out.stt_model_url = optarg;
                break;
            case 'J': {
                char* end = nullptr;
                const long v = std::strtol(optarg, &end, 10);
                if (end == optarg || *end != '\0' || v < 1 || v > 4) {
                    std::fprintf(stderr, "Invalid --stt-threads: %s\n", optarg);
                    return false;
                }
                out.stt_threads = static_cast<int>(v);
                break;
            }
            case 'K': {
                char* end = nullptr;
                const long v = std::strtol(optarg, &end, 10);
                if (end == optarg || *end != '\0' || v < 1 || v > 4) {
                    std::fprintf(stderr, "Invalid --tts-threads: %s\n", optarg);
                    return false;
                }
                out.tts_threads = static_cast<int>(v);
                break;
            }
            case 'T':
                out.tts_data_dir = optarg;
                break;
            case 'G':
                out.conversation_agent = optarg;
                break;
            case 'D':
                out.stt_dump_wav = optarg;
                break;
            case 'C': {
                char* end = nullptr;
                const double v = std::strtod(optarg, &end);
                if (end == optarg || v < 0.0) {
                    std::fprintf(stderr, "Invalid --continue-conversation-delay: %s\n", optarg);
                    return false;
                }
                out.continue_conversation_delay = v;
                break;
            }
            case 'p': {
                char* end = nullptr;
                errno = 0;
                const long v = std::strtol(optarg, &end, 10);
                if (errno == ERANGE || end == optarg || *end != '\0' || v <= 0 || v > 65535) {
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

}  // namespace lva::app
