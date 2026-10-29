#include "sendspin/SendspinHub.h"

#include <sendspin/client.h>
#include <sendspin/color_role.h>
#include <sendspin/controller_role.h>
#include <sendspin/metadata_role.h>
#include <sendspin/player_role.h>

#include <pulse/error.h>
#include <pulse/pulseaudio.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "util/Diag.h"
#include "util/Log.h"
#ifdef LVA_DIAG_LOG
#include "util/MemProbe.h"
#endif

extern char** environ;

namespace lva::sendspin {

namespace {

using namespace ::sendspin;

constexpr const char* kTag = "sendspin";

// Unpaired servers may play (unchanged from the standalone client).
constexpr bool kUnpairedAccess = true;

// A stream still open this long after client/leave means the server kept
// streaming to us; sendspin-cpp may report a real stream end several
// seconds late, so do not resume before.
constexpr std::int64_t kLeaveGraceUs = 15'000'000;

constexpr const char* kSendspinConf     = "/data/conf/sendspin.json";
constexpr const char* kSendspinStateDir = "/data/conf/sendspin-state";
constexpr const char* kSoundLockPath    = "/tmp/sound_config.lock";

std::int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Flat JSON helpers for /data/conf/sendspin.json (kept byte-compatible with
// the standalone client).
// ---------------------------------------------------------------------------

int ReadJsonInt(const char* path, const char* field) {
    std::ifstream f(path);
    if (!f) return -1;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    const std::string key = std::string("\"") + field + "\"";
    auto pos = content.find(key);
    if (pos == std::string::npos) return -1;
    pos = content.find(':', pos);
    if (pos == std::string::npos) return -1;
    const char* start = content.c_str() + pos + 1;
    while (*start == ' ' || *start == '\t' || *start == '\r' ||
           *start == '\n') {
        ++start;
    }
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(start, &end, 10);
    if (end == start || errno == ERANGE || value < INT_MIN ||
        value > INT_MAX) {
        return -1;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }
    if (*end != ',' && *end != '}') return -1;
    return static_cast<int>(value);
}

class ScopedFileLock {
   public:
    ScopedFileLock() {
        fd_ = ::open(kSoundLockPath, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ >= 0 && ::flock(fd_, LOCK_EX) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~ScopedFileLock() {
        if (fd_ < 0) return;
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
    bool locked() const { return fd_ >= 0; }

   private:
    int fd_ = -1;
};

bool WriteJsonInt(const char* path, const char* field, int value) {
    ScopedFileLock lock;
    if (!lock.locked()) return false;
    std::ifstream in(path);
    if (!in) return false;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    in.close();

    const std::string key = std::string("\"") + field + "\"";
    auto pos = content.find(key);
    if (pos == std::string::npos) return false;
    pos = content.find(':', pos);
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < content.size() &&
           (content[pos] == ' ' || content[pos] == '\t')) {
        pos++;
    }
    auto end = pos;
    if (end < content.size() && content[end] == '-') end++;
    while (end < content.size() && content[end] >= '0' &&
           content[end] <= '9') {
        end++;
    }
    if (end == pos) return false;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", value);
    content.replace(pos, end - pos, buf);

    const std::string tmp = std::string(path) + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << content;
    out.close();
    if (!out) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Persistence: identity, trust records, output delay. Same files as the
// standalone client, so the client_id and pairing survive the move.
// ---------------------------------------------------------------------------

class FilePersistenceProvider : public SendspinPersistenceProvider {
   public:
    FilePersistenceProvider() {
        std::ifstream test(kSendspinConf);
        if (!test) {
            std::ofstream out(kSendspinConf, std::ios::trunc);
            out << "{\n  \"led_disabled\": 1\n}\n";
        } else {
            std::string content((std::istreambuf_iterator<char>(test)),
                                std::istreambuf_iterator<char>());
            test.close();
            if (content.find("\"led_disabled\"") == std::string::npos) {
                const auto open = content.find('{');
                const auto close = content.rfind('}');
                if (open != std::string::npos &&
                    close != std::string::npos && open < close) {
                    const bool has_fields = std::any_of(
                        content.begin() + static_cast<std::ptrdiff_t>(open + 1),
                        content.begin() + static_cast<std::ptrdiff_t>(close),
                        [](unsigned char c) { return !std::isspace(c); });
                    content.insert(close, has_fields
                                              ? ",\n  \"led_disabled\": 1\n"
                                              : "\n  \"led_disabled\": 1\n");
                    std::ofstream out(kSendspinConf, std::ios::trunc);
                    out << content;
                } else {
                    LVA_LOGW(kTag, "invalid config: %s", kSendspinConf);
                }
            }
        }

        if (::mkdir(kSendspinStateDir, 0700) != 0 && errno != EEXIST) {
            LVA_LOGW(kTag, "mkdir(%s) failed: %s", kSendspinStateDir,
                     std::strerror(errno));
        } else if (::chmod(kSendspinStateDir, 0700) != 0) {
            LVA_LOGW(kTag, "chmod(%s) failed: %s", kSendspinStateDir,
                     std::strerror(errno));
        }

        if (!load_blob(persistence_keys::OUTPUT_DELAY).has_value()) {
            const int old_delay = ReadJsonInt(kSendspinConf,
                                              "static_delay_ms");
            if (old_delay >= 0 && old_delay <= 5000) {
                const auto delay = static_cast<std::uint16_t>(old_delay);
                if (!save_blob(persistence_keys::OUTPUT_DELAY,
                               reinterpret_cast<const std::uint8_t*>(&delay),
                               sizeof(delay))) {
                    LVA_LOGW(kTag, "output-delay migration failed");
                }
            }
        }
    }

    std::optional<std::vector<std::uint8_t>> load_blob(
        const std::string& key) override {
        if (!ValidKey(key)) return std::nullopt;
        std::ifstream in(BlobPath(key), std::ios::binary | std::ios::ate);
        if (!in) return std::nullopt;
        const auto length = in.tellg();
        if (length < 0 || length > 4096) return std::nullopt;
        in.seekg(0);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                         std::istreambuf_iterator<char>());
    }

    bool save_blob(const std::string& key, const std::uint8_t* data,
                   std::size_t len) override {
        if (!ValidKey(key) || (data == nullptr && len != 0) || len > 4096) {
            return false;
        }
        if (::mkdir(kSendspinStateDir, 0700) != 0 && errno != EEXIST) {
            return false;
        }

        const std::string path = BlobPath(key);
        const std::string tmp = path + ".tmp";
        const int fd = ::open(tmp.c_str(),
                              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
                                  O_NOFOLLOW,
                              0600);
        if (fd < 0) return false;
        if (::fchmod(fd, 0600) != 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }

        bool ok = true;
        std::size_t written = 0;
        while (written < len) {
            const ssize_t n = ::write(fd, data + written, len - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            if (n == 0) {
                ok = false;
                break;
            }
            written += static_cast<std::size_t>(n);
        }
        if (ok && ::fsync(fd) != 0) ok = false;
        if (::close(fd) != 0) ok = false;
        if (!ok) {
            ::unlink(tmp.c_str());
            return false;
        }
        if (::rename(tmp.c_str(), path.c_str()) != 0) {
            ::unlink(tmp.c_str());
            return false;
        }

        const int dir_fd =
            ::open(kSendspinStateDir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd < 0) return false;
        const bool synced = (::fsync(dir_fd) == 0);
        ::close(dir_fd);
        return synced;
    }

   private:
    static bool ValidKey(const std::string& key) {
        if (key.empty()) return false;
        for (unsigned char c : key) {
            if (!std::isalnum(c) && c != '_') return false;
        }
        return true;
    }

    static std::string BlobPath(const std::string& key) {
        return std::string(kSendspinStateDir) + "/" + key;
    }
};

// ---------------------------------------------------------------------------
// LED: tr-ledring animations (D-Bus) plus direct sysfs for loudness.
// dbus-send runs through posix_spawn: LVA's SIGCHLD handler reaps children,
// which breaks system().
// ---------------------------------------------------------------------------

void SpawnQuiet(char* const argv[]) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                     O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, argv[0], &actions, nullptr, argv,
                                 environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        LVA_LOGW(kTag, "posix_spawn(%s) failed: %s", argv[0],
                 std::strerror(rc));
    }
}

void SendLedShow(bool to_idle, const std::string& array_arg) {
    char prog[]    = "/usr/bin/dbus-send";
    char system_a[] = "--system";
    char type_a[]  = "--type=signal";
    char path_a[]  = "/com/3r/EventBus";
    char iface_a[] = "com._3reality.EventBus.LedShow";
    char idle_t[]  = "boolean:true";
    char idle_f[]  = "boolean:false";
    std::string array_buf = array_arg;
    char* argv[] = {prog, system_a, type_a, path_a, iface_a,
                    to_idle ? idle_t : idle_f, array_buf.data(), nullptr};
    SpawnQuiet(argv);
}

class LedColorController {
   public:
    void SetColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        std::lock_guard<std::mutex> lock(mu_);
        if (r == base_r_ && g == base_g_ && b == base_b_) return;
        base_r_ = r;
        base_g_ = g;
        base_b_ = b;
        playing_ = true;
        if (!led_disabled_) StartBreathing();
    }

    // Track change: back to the default color until a new one arrives.
    void ResetColor() {
        std::lock_guard<std::mutex> lock(mu_);
        base_r_ = 0x40;
        base_g_ = 0x80;
        base_b_ = 0xFF;
        loudness_peak_ = 0;
        smooth_bright_ = 0.0f;
        if (playing_ && !led_disabled_) {
            sysfs_mode_ = false;
            StartBreathing();
        }
    }

    // Visualizer drain thread.
    void QueuePulse() { pulse_pending_.store(true, std::memory_order_release); }

    // Visualizer drain thread.
    void QueueLoudness(std::uint16_t loudness) {
        pending_loudness_.store(loudness, std::memory_order_relaxed);
        loudness_pending_.store(true, std::memory_order_release);
    }

    void ResetVisualizer() {
        pulse_pending_.store(false, std::memory_order_release);
        loudness_pending_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mu_);
        loudness_peak_ = 0;
        smooth_bright_ = 0.0f;
        sysfs_mode_ = false;
        if (playing_ && !led_disabled_) StartBreathing();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mu_);
        playing_ = false;
        sysfs_mode_ = false;
        SendLedIdle();
        WriteRgb(0, 0, 0);
    }

    void Resume() {
        std::lock_guard<std::mutex> lock(mu_);
        if (base_r_ == 0 && base_g_ == 0 && base_b_ == 0) {
            base_r_ = 0x40;
            base_g_ = 0x80;
            base_b_ = 0xFF;
        }
        playing_ = true;
        sysfs_mode_ = false;
        loudness_peak_ = 0;
        smooth_bright_ = 0.0f;
        if (led_disabled_) return;
        LVA_LOGD(kTag, "LED resume #%02x%02x%02x", base_r_, base_g_, base_b_);
        StartBreathing();
    }

    // Leave the ring to tr-ledring's volume animation for a while.
    void Suppress(std::int64_t duration_us) {
        std::lock_guard<std::mutex> lock(mu_);
        suppress_until_ = NowUs() + duration_us;
        sysfs_mode_ = false;
    }

    void ToggleEnabled() {
        std::lock_guard<std::mutex> lock(mu_);
        led_disabled_ = !led_disabled_;
        if (!WriteJsonInt(kSendspinConf, "led_disabled",
                          led_disabled_ ? 1 : 0)) {
            LVA_LOGW(kTag, "failed to persist LED setting");
        }
        LVA_LOGI(kTag, "music LED %s", led_disabled_ ? "disabled" : "enabled");
        if (led_disabled_) {
            SendLedIdle();
            WriteRgb(0, 0, 0);
        } else if (playing_) {
            sysfs_mode_ = false;
            StartBreathing();
        }
    }

    // Defaults to disabled: only a stored 0 (user enabled it) turns it on.
    void LoadState() {
        const int val = ReadJsonInt(kSendspinConf, "led_disabled");
        led_disabled_ = (val != 0);
    }

    // Main loop: all visualizer-driven LED I/O.
    void Tick() {
        const bool pulse =
            pulse_pending_.exchange(false, std::memory_order_acq_rel);
        const bool loudness =
            loudness_pending_.exchange(false, std::memory_order_acq_rel);
        const std::uint16_t level =
            pending_loudness_.load(std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(mu_);
        if (!playing_ || led_disabled_) return;

        const std::int64_t now = NowUs();
        if (suppress_until_ > 0 && now >= suppress_until_) {
            suppress_until_ = 0;
            SendLedIdle();
            sysfs_mode_ = true;
        }
        if (now < suppress_until_) return;

        if (pulse && now - last_pulse_time_ >= 250000) {
            last_pulse_time_ = now;
            GeneratePulse();
            return;
        }

        if (loudness) {
            if (!sysfs_mode_) {
                SendLedIdle();
                sysfs_mode_ = true;
            }
            if (level > loudness_peak_) {
                loudness_peak_ = level;
            } else {
                loudness_peak_ =
                    static_cast<std::uint16_t>(loudness_peak_ * 255 / 256);
            }
            const std::uint16_t peak =
                loudness_peak_ > 2000 ? loudness_peak_ : 2000;
            float norm = static_cast<float>(level) / peak;
            if (norm > 1.0f) norm = 1.0f;
            const float target = norm * norm;
            const float smoothing = target > smooth_bright_ ? 0.7f : 0.6f;
            smooth_bright_ += (target - smooth_bright_) * smoothing;

            const float scale = std::min(
                2.5f, 220.0f / std::max({base_r_, base_g_, base_b_,
                                         std::uint8_t{1}}));
            WriteRgb(Scaled(base_r_, scale * smooth_bright_, 255.0f),
                     Scaled(base_g_, scale * smooth_bright_, 255.0f),
                     Scaled(base_b_, scale * smooth_bright_, 255.0f));
        }
    }

   private:
    static std::uint8_t Scaled(std::uint8_t v, float k, float max) {
        return static_cast<std::uint8_t>(std::min(max, v * k));
    }

    void StartBreathing() {
        static constexpr const char* kPath = "/tmp/sendspin_breath.animation";
        FILE* f = std::fopen(kPath, "w");
        if (f == nullptr) return;

        const float scale = std::min(
            2.0f, 200.0f / std::max({base_r_, base_g_, base_b_,
                                     std::uint8_t{1}}));
        const std::uint8_t pr = Scaled(base_r_, scale, 255.0f);
        const std::uint8_t pg = Scaled(base_g_, scale, 255.0f);
        const std::uint8_t pb = Scaled(base_b_, scale, 255.0f);

        for (int i = 0; i < 32; i++) {
            const float t = 0.25f + 0.75f * i / 31.0f;
            WriteFrame(f, Scaled(pr, t, 255.0f), Scaled(pg, t, 255.0f),
                       Scaled(pb, t, 255.0f));
        }
        for (int i = 0; i < 31; i++) {
            const float t = 1.0f - 0.75f * (i + 1) / 31.0f;
            WriteFrame(f, Scaled(pr, t, 255.0f), Scaled(pg, t, 255.0f),
                       Scaled(pb, t, 255.0f));
        }
        std::fprintf(f, "loop\n");
        WriteFrame(f, Scaled(pr, 0.25f, 255.0f), Scaled(pg, 0.25f, 255.0f),
                   Scaled(pb, 0.25f, 255.0f));
        std::fclose(f);
        SendLedAnimation(kPath, kPath);
    }

    void GeneratePulse() {
        static constexpr const char* kPulsePath =
            "/tmp/sendspin_pulse.animation";
        static constexpr const char* kBreathPath =
            "/tmp/sendspin_breath.animation";
        FILE* f = std::fopen(kPulsePath, "w");
        if (f == nullptr) return;

        const float scale = std::min(
            2.0f, 200.0f / std::max({base_r_, base_g_, base_b_,
                                     std::uint8_t{1}}));
        const std::uint8_t pr = Scaled(base_r_, scale, 255.0f);
        const std::uint8_t pg = Scaled(base_g_, scale, 255.0f);
        const std::uint8_t pb = Scaled(base_b_, scale, 255.0f);

        WriteFrame(f, pr, pg, pb);
        WriteFrame(f, pr, pg, pb);
        for (int i = 1; i <= 8; i++) {
            const float t = 0.25f + 0.75f * (1.0f - static_cast<float>(i) / 8.0f);
            WriteFrame(f, Scaled(pr, t, 255.0f), Scaled(pg, t, 255.0f),
                       Scaled(pb, t, 255.0f));
        }
        std::fprintf(f, "loop\n");
        WriteFrame(f, Scaled(pr, 0.25f, 255.0f), Scaled(pg, 0.25f, 255.0f),
                   Scaled(pb, 0.25f, 255.0f));
        std::fclose(f);
        SendLedAnimation(kPulsePath, kBreathPath);
    }

    static void WriteFrame(FILE* f, std::uint8_t r, std::uint8_t g,
                           std::uint8_t b) {
        std::fprintf(f, "16:");
        for (int i = 0; i < 12; i++) {
            std::fprintf(f, i == 11 ? "%02x%02x%02x\n" : "%02x%02x%02x,",
                         r, g, b);
        }
    }

    static void WriteSysfs(const char* path, std::uint8_t value) {
        FILE* f = std::fopen(path, "w");
        if (f != nullptr) {
            std::fprintf(f, "%u", value);
            std::fclose(f);
        }
    }

    static void WriteRgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        WriteSysfs("/sys/class/leds/RGB_R/brightness", r);
        WriteSysfs("/sys/class/leds/RGB_G/brightness", g);
        WriteSysfs("/sys/class/leds/RGB_B/brightness", b);
    }

    // No shell here (posix_spawn), so no quotes around the paths: the
    // standalone client's quotes were stripped by system()'s shell.
    static void SendLedAnimation(const char* path, const char* next_path) {
        SendLedShow(false, std::string("array:string:") + path + "," +
                               next_path);
    }

    static void SendLedIdle() { SendLedShow(true, "array:string:"); }

    std::mutex mu_;
    std::uint8_t base_r_ = 0, base_g_ = 0, base_b_ = 0;
    std::int64_t suppress_until_ = 0;
    std::int64_t last_pulse_time_ = 0;
    std::uint16_t loudness_peak_ = 0;
    float smooth_bright_ = 0.0f;
    bool playing_ = false;
    bool sysfs_mode_ = false;
    bool led_disabled_ = true;
    std::atomic<bool> pulse_pending_{false};
    std::atomic<bool> loudness_pending_{false};
    std::atomic<std::uint16_t> pending_loudness_{0};
};

// ---------------------------------------------------------------------------
// PulseAudio sink. on_audio_write() runs on the sendspin-cpp sync thread;
// everything else on the main loop. The PA threaded mainloop lock serializes
// stream open / write / close.
// ---------------------------------------------------------------------------

class PulseSink {
   public:
    // PA target length; also the latency reported when PA has none yet.
    static constexpr pa_usec_t kTargetLatencyUs = 100000;
    // Five consecutive 20 ms write timeouts mean the sink made no progress for about 100 ms.
    // Drop that stale PCM and let Sendspin resynchronize instead of retrying it indefinitely.
    static constexpr unsigned kMaxConsecutiveZeroWrites = 5;

    PulseSink() {
        mainloop_ = pa_threaded_mainloop_new();
        if (mainloop_ == nullptr) {
            LVA_LOGE(kTag, "pa_threaded_mainloop_new failed");
            return;
        }
        if (pa_threaded_mainloop_start(mainloop_) < 0) {
            LVA_LOGE(kTag, "pa_threaded_mainloop_start failed");
            pa_threaded_mainloop_free(mainloop_);
            mainloop_ = nullptr;
            return;
        }
        pa_threaded_mainloop_lock(mainloop_);
        CreateContextLocked();
        pa_threaded_mainloop_unlock(mainloop_);
    }

    ~PulseSink() {
        shutting_down_.store(true, std::memory_order_release);
        if (mainloop_ == nullptr) return;
        pa_threaded_mainloop_lock(mainloop_);
        CloseStreamLocked();
        if (context_ != nullptr) {
            pa_context_disconnect(context_);
            pa_context_unref(context_);
            context_ = nullptr;
        }
        pa_threaded_mainloop_unlock(mainloop_);
        pa_threaded_mainloop_stop(mainloop_);
        pa_threaded_mainloop_free(mainloop_);
        mainloop_ = nullptr;
    }

    bool ok() const { return mainloop_ != nullptr; }

    void SetDucked(bool ducked) {
        ducked_.store(ducked, std::memory_order_relaxed);
    }
    void SetAnnouncementDucked(bool ducked) {
        announce_ducked_.store(ducked, std::memory_order_relaxed);
    }

    void Open(std::uint32_t sample_rate, std::uint8_t channels) {
        if (mainloop_ == nullptr) return;
        pa_threaded_mainloop_lock(mainloop_);
        stream_active_ = true;
        next_stream_attempt_us_ = 0;
        consecutive_zero_writes_ = 0;
        last_zero_write_log_us_ = 0;
        sample_rate_ = sample_rate;
        channels_ = channels;
        frame_size_ = static_cast<std::size_t>(channels_) * sizeof(std::int16_t);
        LVA_DIAG_LOGI(kTag, "PA open requested: %uHz %uch frame=%zu",
                      sample_rate, channels, frame_size_);
        CloseStreamLocked();
        EnsureStreamLocked();
        pa_threaded_mainloop_unlock(mainloop_);
    }

    void Close() {
        if (mainloop_ == nullptr) return;
        pa_threaded_mainloop_lock(mainloop_);
        stream_active_ = false;
        consecutive_zero_writes_ = 0;
        last_zero_write_log_us_ = 0;
        LVA_DIAG_LOGI(kTag, "PA close requested");
        CloseStreamLocked();
        pa_threaded_mainloop_unlock(mainloop_);
    }

    // Sync thread. Returns bytes written (whole frames) and the latency to
    // report with them.
    std::size_t Write(const std::uint8_t* data, std::size_t length,
                      std::uint32_t timeout_ms, std::size_t* frame_size,
                      pa_usec_t* latency_us, bool* drop_data) {
        if (drop_data != nullptr) *drop_data = false;
        if (frame_size != nullptr) *frame_size = frame_size_;
        if (length == 0) return 0;
        if (mainloop_ == nullptr) {
            if (drop_data != nullptr) *drop_data = true;
            return 0;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        do {
            if (shutting_down_.load(std::memory_order_acquire)) {
                if (drop_data != nullptr) *drop_data = true;
                return 0;
            }

            std::size_t written = 0;
#ifdef LVA_DIAG_LOG
            std::size_t diag_writable = 0;
            int diag_context_state = -1;
            int diag_stream_state = -1;
#endif
            *latency_us = kTargetLatencyUs;

            pa_threaded_mainloop_lock(mainloop_);
#ifdef LVA_DIAG_LOG
            if (context_ != nullptr) {
                diag_context_state =
                    static_cast<int>(pa_context_get_state(context_));
            }
            if (stream_ != nullptr) {
                diag_stream_state =
                    static_cast<int>(pa_stream_get_state(stream_));
            }
#endif
            if (stream_active_ && EnsureStreamLocked()) {
                const std::size_t writable = pa_stream_writable_size(stream_);
#ifdef LVA_DIAG_LOG
                diag_writable = writable;
                diag_context_state =
                    static_cast<int>(pa_context_get_state(context_));
                diag_stream_state =
                    static_cast<int>(pa_stream_get_state(stream_));
#endif
                if (writable == static_cast<std::size_t>(-1)) {
                    LVA_LOGW(kTag, "pa_stream_writable_size failed: %s; dropping stale audio",
                             pa_strerror(pa_context_errno(context_)));
                    CloseStreamLocked();
                    if (drop_data != nullptr) *drop_data = true;
                } else {
                    written = std::min(length, writable);
                    written -= written % frame_size_;
                    *frame_size = frame_size_;
                    if (written > 0) {
                        write_buffer_.assign(data, data + written);
                        ApplyGain(write_buffer_.data(), written);
                        if (pa_stream_write(stream_, write_buffer_.data(),
                                            written, nullptr, 0,
                                            PA_SEEK_RELATIVE) < 0) {
                            LVA_LOGW(kTag, "pa_stream_write failed: %s; dropping stale audio",
                                     pa_strerror(pa_context_errno(context_)));
                            CloseStreamLocked();
                            written = 0;
                            if (drop_data != nullptr) *drop_data = true;
                        } else {
                            consecutive_zero_writes_ = 0;
                            int negative = 0;
                            if (pa_stream_get_latency(stream_, latency_us,
                                                      &negative) < 0 ||
                                negative) {
                                *latency_us = kTargetLatencyUs;
                            }
                        }
                    }
                }
            }
            pa_threaded_mainloop_unlock(mainloop_);

            if (drop_data != nullptr && *drop_data) return length;
            if (written > 0) return written;
            if (timeout_ms == 0 ||
                std::chrono::steady_clock::now() >= deadline) {
                pa_threaded_mainloop_lock(mainloop_);
                if (stream_active_) {
                    ++consecutive_zero_writes_;
#ifdef LVA_DIAG_LOG
                    const std::int64_t now_us = NowUs();
                    if (last_zero_write_log_us_ == 0 ||
                        now_us - last_zero_write_log_us_ >= 1000000) {
                        last_zero_write_log_us_ = now_us;
                        LVA_LOGW(
                            kTag,
                            "PA no progress: request=%zu writable=%zu context_state=%d "
                            "stream_state=%d zero_streak=%u write_buffer_capacity=%zu",
                            length, diag_writable, diag_context_state,
                            diag_stream_state, consecutive_zero_writes_,
                            write_buffer_.capacity());
                    }
#endif
                    if (consecutive_zero_writes_ >= kMaxConsecutiveZeroWrites) {
                        LVA_LOGW(kTag,
                                 "PulseAudio made no write progress for about 100 ms; "
                                 "dropping stale audio and reopening the stream");
                        CloseStreamLocked();
                        consecutive_zero_writes_ = 0;
                        last_zero_write_log_us_ = 0;
                        if (drop_data != nullptr) *drop_data = true;
                    }
                }
                pa_threaded_mainloop_unlock(mainloop_);
                return (drop_data != nullptr && *drop_data) ? length : 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        return 0;
    }

   private:
    static constexpr std::int64_t kReconnectBackoffUs = 1000000;

    void CreateContextLocked() {
        if (mainloop_ == nullptr || context_ != nullptr) return;
        const std::int64_t now = NowUs();
        if (now < next_connect_attempt_us_) return;
        next_connect_attempt_us_ = now + kReconnectBackoffUs;

        context_ = pa_context_new(pa_threaded_mainloop_get_api(mainloop_),
                                  "sendspin");
        if (context_ == nullptr) {
            LVA_LOGW(kTag, "pa_context_new failed");
            return;
        }
        if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOFLAGS,
                               nullptr) < 0) {
            LVA_LOGW(kTag, "pa_context_connect failed: %s",
                     pa_strerror(pa_context_errno(context_)));
            pa_context_unref(context_);
            context_ = nullptr;
        }
    }

    bool EnsureContextLocked() {
        if (context_ != nullptr) {
            const auto state = pa_context_get_state(context_);
            if (state == PA_CONTEXT_READY) return true;
            if (state != PA_CONTEXT_FAILED && state != PA_CONTEXT_TERMINATED) {
                return false;
            }
            CloseStreamLocked();
            pa_context_disconnect(context_);
            pa_context_unref(context_);
            context_ = nullptr;
        }
        CreateContextLocked();
        return context_ != nullptr &&
               pa_context_get_state(context_) == PA_CONTEXT_READY;
    }

    bool EnsureStreamLocked() {
        if (!stream_active_ || !EnsureContextLocked()) return false;
        if (stream_ != nullptr) {
            const auto state = pa_stream_get_state(stream_);
            if (state == PA_STREAM_READY) return true;
            if (state != PA_STREAM_FAILED && state != PA_STREAM_TERMINATED) {
                return false;
            }
            CloseStreamLocked();
        }

        const std::int64_t now = NowUs();
        if (now < next_stream_attempt_us_) return false;
        next_stream_attempt_us_ = now + 100000;

        pa_sample_spec spec{PA_SAMPLE_S16LE, sample_rate_, channels_};
        if (!pa_sample_spec_valid(&spec)) {
            LVA_LOGW(kTag, "invalid PA format %uHz %uch", sample_rate_,
                     channels_);
            return false;
        }

        pa_proplist* props = pa_proplist_new();
        if (props != nullptr) {
            pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "sendspin");
            pa_proplist_sets(props, PA_PROP_MEDIA_NAME, "Sendspin");
        }
        stream_ = pa_stream_new_with_proplist(context_, "Sendspin", &spec,
                                              nullptr, props);
        if (props != nullptr) pa_proplist_free(props);
        if (stream_ == nullptr) {
            LVA_LOGW(kTag, "pa_stream_new failed: %s",
                     pa_strerror(pa_context_errno(context_)));
            return false;
        }

        // 100 ms keeps the ALSA sink near 25 ms. 20 ms drove it to ~5 ms
        // and caused underruns on the first play after PulseAudio start.
        // Sync is unaffected: notify_audio_played() reports real latency.
        const std::uint32_t target_length = static_cast<std::uint32_t>(
            pa_usec_to_bytes(kTargetLatencyUs, &spec));
        pa_buffer_attr attr{static_cast<std::uint32_t>(-1), target_length, 0,
                            static_cast<std::uint32_t>(-1),
                            static_cast<std::uint32_t>(-1)};
        const auto flags = static_cast<pa_stream_flags_t>(
            PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE |
            PA_STREAM_ADJUST_LATENCY);
        if (pa_stream_connect_playback(stream_, nullptr, &attr, flags,
                                       nullptr, nullptr) < 0) {
            LVA_LOGW(kTag, "pa_stream_connect_playback failed: %s",
                     pa_strerror(pa_context_errno(context_)));
            CloseStreamLocked();
            return false;
        }

        LVA_LOGI(kTag, "opening PA %uHz %uch", sample_rate_, channels_);
        return pa_stream_get_state(stream_) == PA_STREAM_READY;
    }

    void CloseStreamLocked() {
        if (stream_ == nullptr) return;
        pa_stream_disconnect(stream_);
        pa_stream_unref(stream_);
        stream_ = nullptr;
        LVA_LOGD(kTag, "closed PA stream");
    }

    // -12 dB while ducked, same as the standalone client.
    void ApplyGain(std::uint8_t* data, std::size_t length) {
        if (!ducked_.load(std::memory_order_relaxed) &&
            !announce_ducked_.load(std::memory_order_relaxed)) {
            return;
        }
        auto* samples = reinterpret_cast<std::int16_t*>(data);
        const std::size_t count = length / sizeof(std::int16_t);
        for (std::size_t i = 0; i < count; ++i) {
            samples[i] = static_cast<std::int16_t>(samples[i] >> 2);
        }
    }

    pa_threaded_mainloop* mainloop_ = nullptr;
    pa_context* context_ = nullptr;
    pa_stream* stream_ = nullptr;
    std::vector<std::uint8_t> write_buffer_;
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> ducked_{false};
    std::atomic<bool> announce_ducked_{false};
    bool stream_active_ = false;
    std::uint32_t sample_rate_ = 48000;
    std::uint8_t channels_ = 2;
    std::size_t frame_size_ = 4;
    unsigned consecutive_zero_writes_ = 0;
    std::int64_t last_zero_write_log_us_ = 0;  // LVA_DIAG_LOG throttle
    std::int64_t next_connect_attempt_us_ = 0;
    std::int64_t next_stream_attempt_us_ = 0;
};

class AlwaysReadyNetwork : public SendspinNetworkProvider {
   public:
    // LVA starts after the first NTP sync, i.e. with the network up.
    bool is_network_ready() override { return true; }
};

bool IsValidMac(const std::string& mac) {
    if (mac.size() != 17) return false;
    for (std::size_t i = 0; i < mac.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(mac[i]);
        if (i % 3 == 2) {
            if (c != ':') return false;
        } else if (!std::isxdigit(c) || std::isupper(c)) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl: all sendspin-cpp listeners in one place.
// ---------------------------------------------------------------------------

struct SendspinHub::Impl : public PlayerRoleListener,
                           public ControllerRoleListener,
                           public MetadataRoleListener,
                           public ColorRoleListener,
                           public SendspinClientListener {
    Impl(Config cfg, Callbacks cbs)
        : config(std::move(cfg)), callbacks(std::move(cbs)) {}

    Config    config;
    Callbacks callbacks;

    FilePersistenceProvider persistence;
    AlwaysReadyNetwork      network;
    LedColorController      led;
    PulseSink               sink;

    std::unique_ptr<SendspinClient> client;
    PlayerRole*     player     = nullptr;
    ControllerRole* controller = nullptr;

    // Read by the sync thread. active: this source owns the audio path.
    std::atomic<bool> active{false};
    // Between on_stream_start() and on_stream_end(); a yielded stream is
    // open but not active.
    bool stream_open = false;
    // Deactivate() sent client/leave at this time; Reactivate() asked to
    // resume the yielded stream if it outlives kLeaveGraceUs.
    std::int64_t left_at_us = 0;
    bool reactivate_pending = false;
    bool group_playing = false;
    int last_controller_volume = -1;
    std::string last_track;
    std::int64_t last_command_failure_us = 0;
#ifdef LVA_DIAG_LOG
    // Audio counters for the LVA_DIAG_LOG stats log.
    std::int64_t last_audio_stats_log_us = 0;
    std::int64_t last_audio_drop_log_us = 0;
    std::atomic<std::uint64_t> audio_written_bytes{0};
    std::atomic<std::uint64_t> audio_dropped_bytes{0};
    std::atomic<std::uint64_t> audio_inactive_dropped_bytes{0};
    std::atomic<std::uint64_t> audio_notify_frames{0};
#endif

    void SetActive(bool value) {
        if (active.exchange(value, std::memory_order_acq_rel) == value) return;
        LVA_LOGI(kTag, "stream %s", value ? "active" : "inactive");
        if (callbacks.on_active_changed) callbacks.on_active_changed(value);
    }

#ifdef LVA_DIAG_LOG
    void LogAudioStats() {
        const std::int64_t now = NowUs();
        if (last_audio_stats_log_us != 0 &&
            now - last_audio_stats_log_us < 60'000'000) {
            return;
        }
        last_audio_stats_log_us = now;
        LVA_LOGI(kTag,
                 "audio stats: written=%llu dropped=%llu inactive_dropped=%llu "
                 "notify_frames=%llu rss_anon_kb=%ld",
                 static_cast<unsigned long long>(
                     audio_written_bytes.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     audio_dropped_bytes.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     audio_inactive_dropped_bytes.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     audio_notify_frames.load(std::memory_order_relaxed)),
                 lva::memprobe::ReadRssAnonKb());
    }

    void LogAudioDrop(std::size_t length) {
        const std::int64_t now = NowUs();
        if (last_audio_drop_log_us != 0 &&
            now - last_audio_drop_log_us < 1'000'000) {
            return;
        }
        last_audio_drop_log_us = now;
        LVA_LOGW(kTag, "dropping audio after PulseAudio write failure: bytes=%zu total=%llu",
                 length,
                 static_cast<unsigned long long>(
                     audio_dropped_bytes.load(std::memory_order_relaxed)));
    }
#endif

    void SendCommand(SendspinControllerCommand command, const char* name) {
        if (controller == nullptr) return;
        if (controller->send_command({.command = command})) {
            LVA_LOGI(kTag, "controller %s", name);
            return;
        }
        const std::int64_t now = NowUs();
        if (now - last_command_failure_us >= 5000000) {
            last_command_failure_us = now;
            LVA_LOGW(kTag, "controller command rejected: %s", name);
        }
    }

    // --- PlayerRoleListener -------------------------------------------------

    // Sync thread. Like ESPHome's sendspin media source, audio is held back
    // while this source is not the active one.
    std::size_t on_audio_write(std::uint8_t* data, std::size_t length,
                               std::uint32_t timeout_ms) override {
        if (!active.load(std::memory_order_acquire) || player == nullptr) {
            // This source no longer owns the speaker. Do not keep retrying the same decoded
            // buffer while another media source is active; returning length tells SyncTask that
            // the stale PCM was intentionally discarded so it can consume newer chunks.
            LVA_DIAG(audio_inactive_dropped_bytes.fetch_add(length, std::memory_order_relaxed));
            return length;
        }
        std::size_t frame_size = 1;
        pa_usec_t latency_us = PulseSink::kTargetLatencyUs;
        bool drop_data = false;
        const std::size_t written =
            sink.Write(data, length, timeout_ms, &frame_size, &latency_us, &drop_data);
        if (drop_data) {
            // The bytes were intentionally discarded and never reached PulseAudio. Do not report
            // them as played: notify_audio_played() advances the real output timeline and fake
            // progress here causes buffered-frame underflow and repeated sync corrections.
#ifdef LVA_DIAG_LOG
            audio_dropped_bytes.fetch_add(length, std::memory_order_relaxed);
            LogAudioDrop(length);
#endif
            return length;
        }
        if (written > 0) {
            const auto frames = static_cast<std::uint32_t>(written / frame_size);
            LVA_DIAG(audio_written_bytes.fetch_add(written, std::memory_order_relaxed));
            LVA_DIAG(audio_notify_frames.fetch_add(frames, std::memory_order_relaxed));
            player->notify_audio_played(
                frames, NowUs() + static_cast<std::int64_t>(latency_us));
        }
        return written;
    }

    void on_stream_start() override {
        LVA_LOGI(kTag, "stream start");
        if (player == nullptr) return;
        const auto& params = player->get_current_stream_params();
        if (!params.sample_rate.has_value() || !params.channels.has_value()) {
            LVA_LOGW(kTag, "stream parameters unavailable");
            return;
        }
        stream_open = true;
        reactivate_pending = false;
        sink.Open(*params.sample_rate, *params.channels);
        SetActive(true);
    }

    void on_stream_end() override {
        LVA_LOGI(kTag, "stream end");
        stream_open = false;
        reactivate_pending = false;
        SetActive(false);
        sink.Close();
    }

    void on_volume_changed(std::uint8_t volume) override {
        const int percent = std::min(static_cast<int>(volume), 100);
        LVA_LOGI(kTag, "server volume %d%%", percent);
        led.Suppress(2000000);
        if (callbacks.on_volume_request) callbacks.on_volume_request(percent);
    }

    void on_mute_changed(bool muted) override {
        LVA_LOGI(kTag, "server mute %s", muted ? "on" : "off");
        if (callbacks.on_mute_request) callbacks.on_mute_request(muted);
    }

    void on_output_delay_changed(std::uint16_t delay_ms) override {
        LVA_LOGI(kTag, "output delay %u ms", delay_ms);
    }

    // --- ControllerRoleListener ---------------------------------------------

    void on_controller_state(const ServerStateControllerObject& state) override {
        LVA_LOGD(kTag, "controller state: vol=%u muted=%d", state.volume,
                 state.muted ? 1 : 0);
        if (last_controller_volume >= 0 &&
            state.volume != last_controller_volume) {
            led.Suppress(1500000);
        }
        last_controller_volume = state.volume;
    }

    // --- MetadataRoleListener -----------------------------------------------

    void on_metadata(const ServerMetadataStateObject& m) override {
        if (!m.artist || !m.title) return;
        std::string track = *m.artist + " - " + *m.title;
        if (track == last_track) return;
        last_track = std::move(track);
        led.ResetColor();
        LVA_LOGI(kTag, "now playing: %s", last_track.c_str());
    }

    void on_metadata_clear() override { last_track.clear(); }

    // --- ColorRoleListener --------------------------------------------------

    void on_color(const ServerColorStateObject& c) override {
        const std::optional<RgbColor>* pick = nullptr;
        if (c.accent) {
            pick = &c.accent;
        } else if (c.primary) {
            pick = &c.primary;
        } else if (c.background_dark) {
            pick = &c.background_dark;
        }
        if (pick != nullptr && *pick) {
            const auto& rgb = **pick;
            led.SetColor(rgb[0], rgb[1], rgb[2]);
        }
    }

    void on_color_clear() override { led.ResetColor(); }

    // --- SendspinClientListener ---------------------------------------------

    void on_group_update(const GroupUpdateObject& group) override {
        if (!group.playback_state.has_value()) return;
        group_playing = (*group.playback_state == SendspinPlaybackState::PLAYING);
        LVA_LOGI(kTag, "group playback: %s",
                 group_playing ? "playing" : "stopped");
        if (group_playing) {
            led.Resume();
        } else {
            led.Clear();
        }
    }
};

// ---------------------------------------------------------------------------
// SendspinHub
// ---------------------------------------------------------------------------

SendspinHub::SendspinHub(Config config, Callbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(callbacks))) {}

SendspinHub::~SendspinHub() { Stop(); }

bool SendspinHub::Start() {
    Impl& d = *impl_;
    if (d.client && d.client->is_started()) return true;
    if (!d.sink.ok()) {
        LVA_LOGE(kTag, "PulseAudio unavailable; Sendspin disabled");
        return false;
    }

    if (!d.client) {
        SendspinClient::set_log_level(LogLevel::INFO);
        d.led.LoadState();

        SendspinClientConfig cfg;
        cfg.name             = d.config.name;
        cfg.product_name     = d.config.product_name;
        cfg.manufacturer     = d.config.manufacturer;
        cfg.software_version = d.config.software_version;
        // Lets Music Assistant match this player to the HA ESPHome device
        // and hide the duplicate HA media player.
        if (IsValidMac(d.config.mac_address)) {
            cfg.mac_address = d.config.mac_address;
        } else {
            LVA_LOGW(kTag, "no valid MAC (%s); library auto-detects it",
                     d.config.mac_address.c_str());
        }

        d.client = std::make_unique<SendspinClient>(std::move(cfg));
        d.client->set_persistence_provider(&d.persistence);
        d.client->set_unpaired_access_enabled(kUnpairedAccess);
        d.client->set_network_provider(&d.network);
        d.client->set_listener(&d);

        PlayerRoleConfig player_config;
        player_config.audio_formats = {
            {SendspinCodecFormat::FLAC, 2, 44100, 16},
            {SendspinCodecFormat::FLAC, 2, 48000, 16},
            {SendspinCodecFormat::OPUS, 2, 48000, 16},
            {SendspinCodecFormat::OPUS, 1, 48000, 16},
            {SendspinCodecFormat::PCM, 2, 44100, 16},
            {SendspinCodecFormat::PCM, 2, 48000, 16},
        };
        auto& player = d.client->add_player(std::move(player_config));
        player.set_output_delay_adjustable(true);
        player.set_listener(&d);
        d.player = &player;

        d.client->add_metadata().set_listener(&d);
        auto& controller = d.client->add_controller();
        controller.set_listener(&d);
        d.controller = &controller;
        d.client->add_color().set_listener(&d);

        d.player->update_volume(static_cast<std::uint8_t>(
            std::clamp(d.config.initial_volume, 0, 100)));
    }

    if (!d.client->start()) {
        LVA_LOGE(kTag, "client start failed");
        return false;
    }
    LVA_LOGI(kTag, "listening as \"%s\" (id=%s, protocol 1.0.0-rc1)",
             d.config.name.c_str(), d.client->client_id().c_str());
    return true;
}

void SendspinHub::Stop() {
    Impl& d = *impl_;
    if (!d.client || !d.client->is_started()) return;
    LVA_LOGI(kTag, "stopping");
    d.client->stop();  // fires on_stream_end() for an open stream
    d.stream_open = false;
    d.reactivate_pending = false;
    d.SetActive(false);
    d.sink.Close();
    if (d.group_playing) d.led.Clear();
    d.group_playing = false;
}

bool SendspinHub::IsStarted() const {
    return impl_->client && impl_->client->is_started();
}

void SendspinHub::Loop() {
    Impl& d = *impl_;
    if (!d.client) return;
    d.client->loop();
    d.led.Tick();
#ifdef LVA_DIAG_LOG
    d.LogAudioStats();
#endif

    if (d.reactivate_pending && NowUs() - d.left_at_us >= kLeaveGraceUs) {
        d.reactivate_pending = false;
        if (!d.stream_open || d.active.load(std::memory_order_acquire) ||
            d.player == nullptr) {
            return;
        }
        const auto& params = d.player->get_current_stream_params();
        if (!params.sample_rate.has_value() || !params.channels.has_value()) {
            return;
        }
        LVA_LOGW(kTag, "server kept streaming after leave; resuming");
        d.sink.Open(*params.sample_rate, *params.channels);
        d.SetActive(true);
    }
}

bool SendspinHub::IsActive() const {
    return impl_->active.load(std::memory_order_acquire);
}

bool SendspinHub::IsGroupPlaying() const { return impl_->group_playing; }

void SendspinHub::Play() {
    if (!IsStarted()) return;
    impl_->SendCommand(SendspinControllerCommand::PLAY, "PLAY");
}

void SendspinHub::Pause() {
    if (!IsStarted()) return;
    impl_->SendCommand(SendspinControllerCommand::PAUSE, "PAUSE");
}

void SendspinHub::LeaveGroup() {
    Impl& d = *impl_;
    if (!IsStarted() || !d.group_playing) return;
    LVA_LOGI(kTag, "leaving group");
    d.client->leave();
}

void SendspinHub::Deactivate() {
    Impl& d = *impl_;
    if (!d.active.load(std::memory_order_acquire)) return;
    LVA_LOGI(kTag, "yielding the speaker; leaving group");
    d.SetActive(false);
    d.sink.Close();
    d.reactivate_pending = false;
    d.left_at_us = NowUs();
    if (IsStarted()) d.client->leave();
}

void SendspinHub::Reactivate() {
    Impl& d = *impl_;
    if (!d.stream_open || d.active.load(std::memory_order_acquire)) return;
    d.reactivate_pending = true;
}

void SendspinHub::CancelReactivate() { impl_->reactivate_pending = false; }

void SendspinHub::ConnectTo(const std::string& url) {
    if (!IsStarted()) {
        LVA_LOGW(kTag, "connect_to(%s) ignored: Sendspin disabled",
                 url.c_str());
        return;
    }
    LVA_LOGI(kTag, "connecting to %s", url.c_str());
    impl_->client->connect_to(url);
}

void SendspinHub::SetVolume(int percent) {
    Impl& d = *impl_;
    const int clamped = std::clamp(percent, 0, 100);
    if (d.player != nullptr && d.player->get_muted()) return;  // mute = vol 0
    d.config.initial_volume = clamped;
    if (d.player == nullptr || d.player->get_volume() == clamped) return;
    d.led.Suppress(2000000);
    d.player->update_volume(static_cast<std::uint8_t>(clamped));
}

void SendspinHub::SetMuted(bool muted) {
    Impl& d = *impl_;
    if (d.player == nullptr || d.player->get_muted() == muted) return;
    d.player->update_muted(muted);
}

void SendspinHub::SetDucked(bool ducked) { impl_->sink.SetDucked(ducked); }

void SendspinHub::SetAnnouncementDucked(bool ducked) {
    impl_->sink.SetAnnouncementDucked(ducked);
}

void SendspinHub::ToggleLed() { impl_->led.ToggleEnabled(); }

}  // namespace lva::sendspin
