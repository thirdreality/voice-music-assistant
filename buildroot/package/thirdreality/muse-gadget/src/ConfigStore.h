#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace muse {
class ConfigStore {
 public:
    explicit ConfigStore(std::filesystem::path directory);
    nlohmann::json Load() const;
    void Save(const nlohmann::json& value) const;
    const std::filesystem::path& directory() const { return directory_; }
 private:
    std::filesystem::path directory_;
};
}  // namespace muse
