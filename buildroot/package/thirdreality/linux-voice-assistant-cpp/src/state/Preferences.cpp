#include "state/Preferences.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "util/Log.h"

namespace lva::state {

namespace {

constexpr const char* kTag = "prefs";
constexpr const char* kSoundFileName = "sound.json";
constexpr const char* kSoundLockPath = "/tmp/sound_config.lock";

template <typename... Args>
void Warn(const std::filesystem::path& path, const char* format, Args... args) {
    if (Preferences::ShouldLogWarning(path)) LVA_LOGW(kTag, format, args...);
}

enum class ReadStatus { kOk, kMissing, kError };

ReadStatus ReadText(const std::filesystem::path& path, std::string& text) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? ReadStatus::kMissing : ReadStatus::kError;
    struct CloseFd {
        int fd;
        ~CloseFd() {
            const int saved = errno;
            ::close(fd);
            errno = saved;
        }
    } close_fd{fd};
    text.clear();
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) text.append(buffer, static_cast<std::size_t>(n));
        else if (n == 0) return ReadStatus::kOk;  // Empty is valid I/O, but invalid JSON.
        else if (errno != EINTR) return ReadStatus::kError;
    }
}

bool SyncDirectory(const std::filesystem::path& path) {
    const auto dir = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        Warn(path, "cannot open directory %s: %s", dir.c_str(), std::strerror(errno));
        return false;
    }
    const bool synced = ::fsync(fd) == 0;
    const int sync_error = errno;
    const int close_result = ::close(fd);
    if (!synced || close_result != 0) {
        Warn(path, "directory sync/close failed for %s: %s", dir.c_str(),
             std::strerror(!synced ? sync_error : errno));
        return false;
    }
    return true;
}

// Called only after re-reading under the settings lock. Rename once rather
// than copying on every retry: if the subsequent write fails, the next retry
// sees a missing file and recreates it without accumulating more backups.
bool ArchiveCorrupt(const std::filesystem::path& path) {
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::error_code ec;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto backup = std::filesystem::path(path.string() + ".corrupt-" +
            std::to_string(stamp) + "-" + std::to_string(::getpid()) + "-" +
            std::to_string(attempt));
        const bool exists = std::filesystem::exists(backup, ec);
        if (ec) break;
        if (exists) continue;
        std::filesystem::rename(path, backup, ec);
        if (ec) break;
        LVA_LOGI(kTag, "archived corrupt preferences: %s", backup.c_str());
        return SyncDirectory(path);
    }
    Warn(path, "cannot archive corrupt preferences %s: %s", path.c_str(),
         ec ? ec.message().c_str() : "backup name collision");
    return false;
}

class ScopedSoundConfigLock {
   public:
    explicit ScopedSoundConfigLock(bool required, bool nonblocking = false) {
        if (!required) {
            locked_ = true;
            return;
        }
        fd_ = ::open(kSoundLockPath,
                     O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ >= 0 && ::flock(fd_, LOCK_EX | (nonblocking ? LOCK_NB : 0)) == 0) {
            locked_ = true;
        }
    }

    ~ScopedSoundConfigLock() {
        if (fd_ < 0) return;
        if (locked_) ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }

    bool locked() const noexcept { return locked_; }

   private:
    int  fd_ = -1;
    bool locked_ = false;
};

double Clamp01(double v) noexcept {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

int CoerceInt(const nlohmann::json& v, int fallback) noexcept {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number_float())   return static_cast<int>(v.get<double>());
    if (v.is_boolean())        return v.get<bool>() ? 1 : 0;
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

bool DetectSoundFormat(const nlohmann::json& obj,
                       const std::filesystem::path& path) noexcept {
    if (obj.contains("mic_gain") || obj.contains("mic_mute")) return true;
    if (path.filename() == kSoundFileName) return true;
    return false;
}

void PopulateFromJson(Preferences& out,
                      const nlohmann::json& obj,
                      const std::filesystem::path& storage_path) {
    if (!obj.is_object()) return;

    out = Preferences::ForPath(storage_path);

    const bool sound_format = DetectSoundFormat(obj, storage_path);
    if (sound_format && out.format() != Preferences::Format::kSound) {
        out = Preferences::ForPath(
            storage_path.parent_path() / kSoundFileName);
    }

    if (auto it = obj.find("active_wake_words"); it != obj.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (item.is_null()) {
                out.active_wake_words.emplace_back(std::nullopt);
            } else if (item.is_string()) {
                out.active_wake_words.emplace_back(item.get<std::string>());
            } else {
                out.active_wake_words.emplace_back(std::nullopt);
            }
        }
    }

    if (auto it = obj.find("volume"); it != obj.end() && !it->is_null()
                                       && it->is_number()) {
        const double raw = it->get<double>();
        out.volume = (out.format() == Preferences::Format::kSound)
                         ? Clamp01(raw / 100.0)
                         : Clamp01(raw);
    }

    out.thinking_sound        = CoerceInt(obj.value("thinking_sound", 0), 0) ? 1 : 0;
    out.mic_gain              = CoerceInt(obj.value("mic_gain", 30), 30);
    out.mic_mute              = CoerceInt(obj.value("mic_mute", 1), 1) == 0 ? 0 : 1;
    auto load_mic_setting = [&](const char* key, int fallback) {
        const auto it = obj.find(key);
        return it == obj.end() ? fallback : CoerceInt(*it, fallback);
    };
    out.mic_auto_gain = load_mic_setting("mic_auto_gain", out.mic_auto_gain);
    out.mic_noise_suppression =
        load_mic_setting("mic_noise_suppression", out.mic_noise_suppression);
    out.mic_volume = std::clamp(
        load_mic_setting("mic_volume", out.mic_volume), 1, 4000);

    auto load_optional_double = [&](const char* key) -> std::optional<double> {
        if (auto it = obj.find(key);
            it != obj.end() && !it->is_null() && it->is_number()) {
            return it->get<double>();
        }
        return std::nullopt;
    };
    out.wake_word_1_sensitivity = load_optional_double("wake_word_1_sensitivity");
    out.wake_word_2_sensitivity = load_optional_double("wake_word_2_sensitivity");
    out.stop_word_sensitivity   = load_optional_double("stop_word_sensitivity");
    out.continue_conversation_delay =
        load_optional_double("continue_conversation_delay");

    if (auto it = obj.find("voice_backend");
        it != obj.end() && it->is_string()) {
        const auto v = it->get<std::string>();
        if (v == "ha" || v == "local") out.voice_backend = v;
    }
    if (auto it = obj.find("tts_rate");
        it != obj.end() && it->is_number_integer()) {
        out.tts_rate = it->get<int>();
    }
    out.sendspin_enabled =
        CoerceInt(obj.value("sendspin_enabled", 1), 1) != 0;
}

void ApplyToJson(const Preferences& prefs, nlohmann::json& obj) {
    if (!obj.is_object()) {
        obj = nlohmann::json::object();
    }

    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& slot : prefs.active_wake_words) {
            if (slot.has_value()) arr.push_back(*slot);
            else                  arr.push_back(nullptr);
        }
        obj["active_wake_words"] = std::move(arr);
    }

    obj["thinking_sound"] = prefs.thinking_sound ? 1 : 0;

    if (prefs.format() == Preferences::Format::kSound) {
        const double v = prefs.volume.has_value() ? *prefs.volume : 1.0;
        obj["volume"] = static_cast<int>(std::lround(Clamp01(v) * 100.0));
        obj["mic_gain"] = prefs.mic_gain;
        obj["mic_mute"] = prefs.is_mic_muted() ? 0 : 1;
    } else {
        if (prefs.volume.has_value()) {
            obj["volume"] = Clamp01(*prefs.volume);
        } else {
            obj["volume"] = nullptr;
        }
    }

    obj["mic_auto_gain"]         = prefs.mic_auto_gain;
    obj["mic_noise_suppression"] = prefs.mic_noise_suppression;
    obj["mic_volume"]            = prefs.mic_volume;

    auto store_optional_double = [&](const char* key,
                                     const std::optional<double>& v) {
        if (v.has_value()) {
            obj[key] = *v;
        } else {
            obj.erase(key);
        }
    };
    store_optional_double("wake_word_1_sensitivity", prefs.wake_word_1_sensitivity);
    store_optional_double("wake_word_2_sensitivity", prefs.wake_word_2_sensitivity);
    store_optional_double("stop_word_sensitivity",   prefs.stop_word_sensitivity);
    store_optional_double("continue_conversation_delay",
                          prefs.continue_conversation_delay);

    if (prefs.voice_backend.has_value()) {
        obj["voice_backend"] = *prefs.voice_backend;
    } else {
        obj.erase("voice_backend");
    }
    if (prefs.tts_rate.has_value()) {
        obj["tts_rate"] = *prefs.tts_rate;
    } else {
        obj.erase("tts_rate");
    }
    obj["sendspin_enabled"] = prefs.sendspin_enabled ? 1 : 0;
}

}  // namespace


Preferences Preferences::ForPath(const std::filesystem::path& storage_path) {
    Preferences p;
    p.format_ = (storage_path.filename() == kSoundFileName)
                    ? Format::kSound
                    : Format::kPreferences;
    return p;
}

bool Preferences::ShouldLogWarning(const std::filesystem::path& storage_path) {
    static std::mutex mutex;
    static std::map<std::filesystem::path, std::chrono::steady_clock::time_point> next;
    std::lock_guard lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    auto it = next.find(storage_path);
    if (it != next.end() && now < it->second) return false;
    next[storage_path] = now + std::chrono::seconds(30);
    return true;
}

Preferences Preferences::LoadFromFile(const std::filesystem::path& storage_path,
                                       bool* loaded, bool* corrupt) {
    if (loaded) *loaded = false;
    if (corrupt) *corrupt = false;
    std::string text;
    const auto status = ReadText(storage_path, text);
    if (status == ReadStatus::kMissing) {
        LVA_LOGI(kTag, "no preferences file at %s — using defaults",
                 storage_path.c_str());
        return ForPath(storage_path);
    }

    if (status == ReadStatus::kError) {
        Warn(storage_path, "failed to read %s: %s",
                 storage_path.c_str(), std::strerror(errno));
        return ForPath(storage_path);
    }

    nlohmann::json obj;
    try {
        obj = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        if (corrupt) *corrupt = true;
        Warn(storage_path, "failed to parse %s as JSON: %s",
                 storage_path.c_str(), e.what());
        return ForPath(storage_path);
    }

    if (!obj.is_object()) {
        if (corrupt) *corrupt = true;
        Warn(storage_path, "preferences file %s is not a JSON object",
                 storage_path.c_str());
        return ForPath(storage_path);
    }

    Preferences p;
    PopulateFromJson(p, obj, storage_path);
    p.mic_defaults_applied_ = false;
    for (const auto* key : {"mic_auto_gain", "mic_noise_suppression", "mic_volume"}) {
        const auto it = obj.find(key);
        if (it == obj.end() || it->is_null()) {
            p.mic_defaults_applied_ = true;
            break;
        }
    }
    p.raw_data_text_ = std::move(text);
    if (loaded) *loaded = true;
    // Status-query callers include the periodic watcher; successful polling
    // reads should not generate INFO messages on every deferred retry.
    const auto level = loaded ? lva::log::Level::kDebug : lva::log::Level::kInfo;
    LVA_LOG(level, kTag, "loaded preferences from %s (format=%s)",
             storage_path.c_str(),
             p.format_ == Format::kSound ? "sound" : "preferences");
    return p;
}

bool Preferences::SaveToFile(const std::filesystem::path& storage_path) const {
    return Save(storage_path, nullptr, true);
}

bool Preferences::SaveFields(const std::filesystem::path& storage_path,
                             const std::set<std::string>& keys, bool recover_corrupt) const {
    return (keys.empty() && !recover_corrupt) || Save(storage_path, &keys, recover_corrupt);
}

std::set<std::string> Preferences::AllFields() {
    return {"active_wake_words", "volume", "thinking_sound", "mic_gain", "mic_mute",
            "mic_auto_gain", "mic_noise_suppression", "mic_volume",
            "wake_word_1_sensitivity", "wake_word_2_sensitivity", "stop_word_sensitivity",
            "continue_conversation_delay", "voice_backend", "tts_rate", "sendspin_enabled"};
}

std::set<std::string> Preferences::ChangedFields(const Preferences& previous) const {
    std::set<std::string> keys;
    const auto changed = [&keys](const char* key, const auto& value, const auto& old) {
        if (value != old) keys.emplace(key);
    };
    changed("active_wake_words", active_wake_words, previous.active_wake_words);
    changed("volume", volume, previous.volume);
    changed("thinking_sound", thinking_sound, previous.thinking_sound);
    changed("mic_gain", mic_gain, previous.mic_gain);
    changed("mic_mute", mic_mute, previous.mic_mute);
    changed("mic_auto_gain", mic_auto_gain, previous.mic_auto_gain);
    changed("mic_noise_suppression", mic_noise_suppression, previous.mic_noise_suppression);
    changed("mic_volume", mic_volume, previous.mic_volume);
    changed("wake_word_1_sensitivity", wake_word_1_sensitivity, previous.wake_word_1_sensitivity);
    changed("wake_word_2_sensitivity", wake_word_2_sensitivity, previous.wake_word_2_sensitivity);
    changed("stop_word_sensitivity", stop_word_sensitivity, previous.stop_word_sensitivity);
    changed("continue_conversation_delay", continue_conversation_delay,
            previous.continue_conversation_delay);
    changed("voice_backend", voice_backend, previous.voice_backend);
    changed("tts_rate", tts_rate, previous.tts_rate);
    changed("sendspin_enabled", sendspin_enabled, previous.sendspin_enabled);
    return keys;
}

bool Preferences::Save(const std::filesystem::path& storage_path,
                       const std::set<std::string>* keys, bool recover_corrupt) const {
    std::error_code ec;
    if (storage_path.has_parent_path()) {
        std::filesystem::create_directories(storage_path.parent_path(), ec);
        if (ec) {
            Warn(storage_path, "failed to create parent of %s: %s",
                     storage_path.c_str(), ec.message().c_str());
            return false;
        }
    }

    ScopedSoundConfigLock lock(format_ == Format::kSound, keys != nullptr);
    if (!lock.locked()) {
        Warn(storage_path, "failed to lock %s", kSoundLockPath);
        return false;
    }

    std::string base_text = raw_data_text_;
    bool write_all = keys == nullptr;
    const auto status = ReadText(storage_path, base_text);
    if (status == ReadStatus::kError) {
        Warn(storage_path, "cannot read %s before merging settings: %s",
             storage_path.c_str(), std::strerror(errno));
        return false;  // Never archive an I/O failure as corrupt contents.
    }
    if (status == ReadStatus::kMissing) {
        // Recreate a missing file from the complete current snapshot.
        base_text = raw_data_text_;
        write_all = true;
    }

    auto obj = base_text.empty() ? nlohmann::json::object()
                                : nlohmann::json::parse(base_text, nullptr, false);
    const bool corrupt = (status == ReadStatus::kOk && base_text.empty()) || !obj.is_object();
    if (corrupt) {
        if (!recover_corrupt) {
            Warn(storage_path, "invalid JSON in %s; awaiting recovery", storage_path.c_str());
            return false;
        }
        if (!ArchiveCorrupt(storage_path)) return false;
        // Retain unknown fields from the last successfully loaded JSON where
        // possible; known fields below always come from the latest snapshot.
        obj = raw_data_text_.empty() ? nlohmann::json::object()
                                    : nlohmann::json::parse(raw_data_text_, nullptr, false);
        if (!obj.is_object()) obj = nlohmann::json::object();
        write_all = true;
    }
    // A recovery-only request must not overwrite a file an external writer
    // has already repaired. Normal pending edits still merge selected fields.
    if (keys && keys->empty() && !write_all) {
        // Also finish durability after a recovery that renamed the new file
        // but failed its directory fsync. Do not rewrite valid external data.
        const int fd = ::open(storage_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const bool synced = ::fsync(fd) == 0;
        const int sync_error = errno;
        const int close_result = ::close(fd);
        if (!synced || close_result != 0) {
            Warn(storage_path, "recovery sync failed for %s: %s", storage_path.c_str(),
                 std::strerror(!synced ? sync_error : errno));
            return false;
        }
        return SyncDirectory(storage_path);
    }

    if (!write_all) {
        auto updated = obj;
        ApplyToJson(*this, updated);
        for (const auto& key : *keys) {
            if (updated.contains(key)) obj[key] = updated[key];
            else obj.erase(key);
        }
    } else {
        ApplyToJson(*this, obj);
    }
    obj.erase("tts_voice");  // Legacy-key cleanup is not a changed setting.

    const auto tmp_path = storage_path.string() + ".tmp";
    const std::string payload = obj.dump(4) + '\n';

    // fsync file and directory so a power cut right after saving keeps it.
    const int fd = ::open(tmp_path.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        Warn(storage_path, "failed to open %s for write: %s",
                 tmp_path.c_str(), std::strerror(errno));
        return false;
    }
    {
        std::size_t off = 0;
        bool write_ok = true;
        while (off < payload.size()) {
            const ssize_t n = ::write(fd, payload.data() + off,
                                      payload.size() - off);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                Warn(storage_path, "write to %s failed: %s",
                         tmp_path.c_str(), n == 0 ? "zero-byte write" : std::strerror(errno));
                write_ok = false;
                break;
            }
            off += static_cast<std::size_t>(n);
        }
        if (write_ok && ::fsync(fd) != 0) {
            Warn(storage_path, "fsync %s failed: %s",
                     tmp_path.c_str(), std::strerror(errno));
            write_ok = false;
        }
        if (::close(fd) != 0 && write_ok) {
            Warn(storage_path, "close %s failed: %s",
                     tmp_path.c_str(), std::strerror(errno));
            write_ok = false;
        }
        if (!write_ok) {
            std::filesystem::remove(tmp_path, ec);
            return false;
        }
    }

    std::filesystem::rename(tmp_path, storage_path, ec);
    if (ec) {
        Warn(storage_path, "rename %s -> %s failed: %s",
                 tmp_path.c_str(), storage_path.c_str(),
                 ec.message().c_str());
        std::filesystem::remove(tmp_path, ec);
        return false;
    }

    if (!SyncDirectory(storage_path)) return false;
    if (write_all) LVA_LOGI(kTag, "saved complete preferences snapshot to %s", storage_path.c_str());
    return true;
}

}  // namespace lva::state
