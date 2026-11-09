#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "app/Defaults.h"

namespace lva::app {

struct CliOptions {
    std::string device_name = "3RSPK";
    std::string host = "0.0.0.0";
    std::string audio_device;
    std::uint16_t port = 6053;
    std::filesystem::path preferences_file = "/data/conf/sound.json";
    std::string wakeword_type = "micro";  // "micro" or "open"
    std::string wakeword_models = "okay_nabu";

    std::string capture_backend = "alsa";
    std::string capture_alsa_device = "hw:0,4";
    int capture_mic_channel = 0;
    std::string capture_ref_channels = "2,3";

    double continue_conversation_delay = 0.5;  // seconds

    std::string voice_backend = "ha";
    std::string stt_model = kSttModelPath;
    std::string stt_model_url = kSttModelUrl;
    int stt_threads = 4;
    int tts_threads = 2;
    std::string tts_data_dir = "/usr/share/thirdreality/tts/espeak-ng-data";
    std::string conversation_agent;
    std::string stt_dump_wav;

    bool debug = false;
};

bool ParseCli(int argc, char** argv, CliOptions& out);

}  // namespace lva::app
