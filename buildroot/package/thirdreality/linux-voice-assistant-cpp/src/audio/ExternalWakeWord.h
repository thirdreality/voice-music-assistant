// Downloads custom wake words offered by HA. The rootfs is an overlay on
// /data, so they go next to the bundled models and survive reboot.

#pragma once

#include <cstdint>
#include <atomic>
#include "tr/NetworkPolicy.h"
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

// Reuses a nonempty cached .tflite if it matches all supplied size/hash
// metadata. Zero size and empty hash mean that field was not provided.
// Returns the JSON config path, or empty on failure.
std::filesystem::path DownloadExternalWakeWord(
    const ExternalWakeWordInfo& info,
    const std::filesystem::path& dest_dir,
    const lva::tr::NetworkPolicy& policy, const std::atomic<bool>* cancel);

}  // namespace lva::audio
