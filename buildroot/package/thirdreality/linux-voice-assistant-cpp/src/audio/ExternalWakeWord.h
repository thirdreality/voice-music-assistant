// Downloads custom wake words offered by HA. The rootfs is an overlay on
// /data, so they go next to the bundled models and survive reboot.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lva::audio {

struct ExternalWakeWordInfo {
    std::string id;
    std::string wake_word;
    std::vector<std::string> trained_languages;
    std::string model_type;       // "micro" (only type we support)
    std::uint32_t model_size = 0; // expected .tflite size in bytes
    std::string model_hash;       // expected .tflite SHA-256 (hex, lower)
    std::string url;              // URL of the JSON config file
};

// Skips the download if the cached .tflite matches size and hash.
// Returns the JSON config path, or empty on failure.
std::filesystem::path DownloadExternalWakeWord(
    const ExternalWakeWordInfo& info,
    const std::filesystem::path& dest_dir);

}  // namespace lva::audio
