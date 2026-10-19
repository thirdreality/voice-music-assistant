// Separate TU: nlohmann/json clashes with the `log` extension in api.pb.h.

#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace lva::audio {

struct WakeWordInfo {
    std::string id;
    std::string wake_word;
    std::vector<std::string> trained_languages;
    // "micro", "open", or empty (treat as "micro").
    std::string type;
};

std::vector<WakeWordInfo> ScanAvailableWakeWords(
    const std::filesystem::path& dir);

std::string ReadWakeWordType(const std::filesystem::path& dir,
                             const std::string& id);

}  // namespace lva::audio
