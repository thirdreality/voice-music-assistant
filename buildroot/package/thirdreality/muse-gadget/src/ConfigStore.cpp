#include "ConfigStore.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <stdexcept>

namespace muse {
ConfigStore::ConfigStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

nlohmann::json ConfigStore::Load() const {
    std::ifstream input(directory_ / "pairing.json");
    if (!input) return nlohmann::json::object();
    nlohmann::json value;
    input >> value;
    return value;
}

void ConfigStore::Save(const nlohmann::json& value) const {
    std::filesystem::create_directories(directory_);
    std::filesystem::permissions(directory_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    const auto temporary = directory_ / "pairing.json.tmp";
    const auto target = directory_ / "pairing.json";
    const std::string contents = value.dump(2) + "\n";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("cannot open pairing state");
    size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t n = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (n <= 0) { ::close(fd); throw std::runtime_error("cannot write pairing state"); }
        offset += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0 || ::rename(temporary.c_str(), target.c_str()) != 0)
        throw std::runtime_error("cannot commit pairing state");
}
}  // namespace muse
