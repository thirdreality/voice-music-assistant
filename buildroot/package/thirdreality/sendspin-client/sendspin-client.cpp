// Sendspin client for ThirdReality speaker — PulseAudio backend
// SPDX-License-Identifier: Apache-2.0
//
// Integrates sendspin-cpp with PulseAudio for audio output, and drives the
// LED ring via tr-ledring D-Bus animations and direct sysfs writes.
//
// Volume conventions (do not mix without converting):
//   - Sendspin wire: 0-100 percent
//   - PulseAudio linear: 0.0-1.0
//   - /data/conf/sound.json: 0-100 percent

#include <sendspin/client.h>
#include <sendspin/color_role.h>
#include <sendspin/controller_role.h>
#include <sendspin/metadata_role.h>
#include <sendspin/player_role.h>
#include <sendspin/visualizer_role.h>

#include <pulse/error.h>
#include <pulse/pulseaudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace sendspin;

static constexpr bool kUnpairedAccess = true;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_ducked{false};
static void signal_handler(int) { g_running = false; }

// ============================================================================
// Control socket — receives tap commands from lva over a Unix domain
// datagram socket. lva owns the tap key; this process no longer reads the
// input device directly.
// ============================================================================

class ControlSocket {
 public:
  static constexpr const char *PATH = "/tmp/sendspin-control.sock";

  // Binds the datagram socket. Returns false on failure (commands are then
  // simply never received; the rest of the client keeps working).
  bool open() {
    fd_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd_ < 0) {
      fprintf(stderr, "[sendspin] control socket() failed\n");
      return false;
    }
    unlink(PATH);  // clear a stale socket from a previous run

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, PATH, sizeof(addr.sun_path) - 1);
    if (bind(fd_, reinterpret_cast<struct sockaddr *>(&addr),
             sizeof(addr)) < 0) {
      fprintf(stderr, "[sendspin] control bind(%s) failed\n", PATH);
      close(fd_);
      fd_ = -1;
      return false;
    }
    if (::chmod(PATH, 0600) != 0) {
      fprintf(stderr, "[sendspin] control chmod(%s) failed: %s\n",
              PATH, strerror(errno));
      close(fd_);
      fd_ = -1;
      unlink(PATH);
      return false;
    }
    return true;
  }

  ~ControlSocket() {
    if (fd_ >= 0) {
      close(fd_);
      unlink(PATH);
    }
  }

  // Drain all pending datagrams; dispatch recognized commands.
  void poll(const std::function<void(const char *)> &on_command) {
    if (fd_ < 0) return;
    char buf[64];
    while (true) {
      ssize_t n = recv(fd_, buf, sizeof(buf) - 1, MSG_DONTWAIT);
      if (n <= 0) break;
      buf[n] = '\0';
      on_command(buf);
    }
  }

 private:
  int fd_ = -1;
};

static constexpr const char *SOUND_CONF = "/data/conf/sound.json";
static constexpr const char *LVA_CONTROL_PATH = "/tmp/lva-control.sock";
static constexpr const char *SOUND_LOCK_PATH = "/tmp/sound_config.lock";
static constexpr const char *SENDSPIN_CONF = "/data/conf/sendspin.json";
static constexpr const char *SENDSPIN_STATE_DIR = "/data/conf/sendspin-state";

// ============================================================================
// JSON helpers
// ============================================================================

static int read_json_int(const char *path, const char *field) {
  std::ifstream f(path);
  if (!f) return -1;
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  std::string key = std::string("\"") + field + "\"";
  auto pos = content.find(key);
  if (pos == std::string::npos) return -1;
  pos = content.find(':', pos);
  if (pos == std::string::npos) return -1;
  const char *start = content.c_str() + pos + 1;
  while (*start == ' ' || *start == '\t' || *start == '\r' ||
         *start == '\n') {
    ++start;
  }
  errno = 0;
  char *end = nullptr;
  const long value = std::strtol(start, &end, 10);
  if (end == start || errno == ERANGE || value < INT_MIN || value > INT_MAX) {
    return -1;
  }
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
    ++end;
  }
  if (*end != ',' && *end != '}') return -1;
  return static_cast<int>(value);
}

class ScopedSoundLock {
 public:
  ScopedSoundLock() {
    fd_ = ::open(SOUND_LOCK_PATH, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ >= 0 && ::flock(fd_, LOCK_EX) != 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  ~ScopedSoundLock() {
    if (fd_ < 0) return;
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
  }

  bool locked() const { return fd_ >= 0; }

 private:
  int fd_{-1};
};

static bool write_json_int(const char *path, const char *field, int value) {
  ScopedSoundLock lock;
  if (!lock.locked()) return false;
  std::ifstream in(path);
  if (!in) return false;
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  in.close();

  std::string key = std::string("\"") + field + "\"";
  auto pos = content.find(key);
  if (pos == std::string::npos) return false;
  pos = content.find(':', pos);
  if (pos == std::string::npos) return false;
  pos++;
  while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) pos++;
  auto end = pos;
  if (end < content.size() && content[end] == '-') end++;
  while (end < content.size() && (content[end] >= '0' && content[end] <= '9')) end++;
  if (end == pos) return false;

  char buf[16];
  snprintf(buf, sizeof(buf), "%d", value);
  content.replace(pos, end - pos, buf);

  std::string tmp = std::string(path) + ".tmp";
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

static int read_device_volume() {
  return read_json_int(SOUND_CONF, "volume");
}

static bool send_volume_to_lva(int volume) {
  const int fd = ::socket(AF_UNIX,
                          SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return false;

  struct sockaddr_un local{};
  local.sun_family = AF_UNIX;
  local.sun_path[0] = '\0';
  std::snprintf(local.sun_path + 1, sizeof(local.sun_path) - 1,
                "sendspin-%ld-%d", static_cast<long>(::getpid()), fd);
  const socklen_t local_len = static_cast<socklen_t>(
      offsetof(struct sockaddr_un, sun_path) + 1 +
      std::strlen(local.sun_path + 1));
  if (::bind(fd, reinterpret_cast<struct sockaddr *>(&local), local_len) < 0) {
    ::close(fd);
    return false;
  }

  char command[32];
  std::snprintf(command, sizeof(command), "CONFIG_SET_VOLUME:%d", volume);
  struct sockaddr_un peer{};
  peer.sun_family = AF_UNIX;
  std::strncpy(peer.sun_path, LVA_CONTROL_PATH, sizeof(peer.sun_path) - 1);
  if (::sendto(fd, command, std::strlen(command), MSG_NOSIGNAL,
               reinterpret_cast<struct sockaddr *>(&peer),
               sizeof(peer)) < 0) {
    ::close(fd);
    return false;
  }

  struct pollfd pfd{fd, POLLIN, 0};
  const int result = ::poll(&pfd, 1, 100);
  if (result <= 0 || !(pfd.revents & POLLIN)) {
    ::close(fd);
    return false;
  }

  char reply[8] = {};
  const ssize_t n = ::recv(fd, reply, sizeof(reply) - 1, 0);
  ::close(fd);
  return n == 2 && std::memcmp(reply, "OK", 2) == 0;
}

static void persist_volume(int vol) {
  if (vol < 0) vol = 0;
  if (vol > 100) vol = 100;
  if (send_volume_to_lva(vol)) return;
  if (!write_json_int(SOUND_CONF, "volume", vol)) {
    fprintf(stderr, "[sendspin] failed to persist volume\n");
    return;
  }
  char command[64];
  std::snprintf(command, sizeof(command),
                "/usr/bin/tr-volume set %d >/dev/null 2>&1", vol);
  if (std::system(command) != 0) {
    fprintf(stderr, "[sendspin] failed to apply fallback volume\n");
  }
}

static int64_t now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ============================================================================
// Persistence provider for identity, trust, delay, and LED settings
// ============================================================================

class FilePersistenceProvider : public SendspinPersistenceProvider {
 public:
  FilePersistenceProvider() {
    std::ifstream test(SENDSPIN_CONF);
    if (!test) {
      std::ofstream out(SENDSPIN_CONF, std::ios::trunc);
      out << "{\n  \"led_disabled\": 1\n}\n";
    } else {
      std::string content((std::istreambuf_iterator<char>(test)),
                          std::istreambuf_iterator<char>());
      test.close();
      if (content.find("\"led_disabled\"") == std::string::npos) {
        const auto open = content.find('{');
        const auto close = content.rfind('}');
        if (open != std::string::npos && close != std::string::npos &&
            open < close) {
          const bool has_fields = std::any_of(
              content.begin() + static_cast<ptrdiff_t>(open + 1),
              content.begin() + static_cast<ptrdiff_t>(close),
              [](unsigned char c) { return !std::isspace(c); });
          content.insert(close, has_fields
                                    ? ",\n  \"led_disabled\": 1\n"
                                    : "\n  \"led_disabled\": 1\n");
          std::ofstream out(SENDSPIN_CONF, std::ios::trunc);
          out << content;
        } else {
          fprintf(stderr, "[sendspin] invalid config: %s\n", SENDSPIN_CONF);
        }
      }
    }

    if (::mkdir(SENDSPIN_STATE_DIR, 0700) != 0 && errno != EEXIST) {
      fprintf(stderr, "[sendspin] mkdir(%s) failed: %s\n",
              SENDSPIN_STATE_DIR, strerror(errno));
    } else if (::chmod(SENDSPIN_STATE_DIR, 0700) != 0) {
      fprintf(stderr, "[sendspin] chmod(%s) failed: %s\n",
              SENDSPIN_STATE_DIR, strerror(errno));
    }

    if (!load_blob(persistence_keys::OUTPUT_DELAY).has_value()) {
      const int old_delay = read_json_int(SENDSPIN_CONF, "static_delay_ms");
      if (old_delay >= 0 && old_delay <= 5000) {
        const auto delay = static_cast<uint16_t>(old_delay);
        if (!save_blob(persistence_keys::OUTPUT_DELAY,
                       reinterpret_cast<const uint8_t *>(&delay),
                       sizeof(delay))) {
          fprintf(stderr, "[sendspin] output-delay migration failed\n");
        }
      }
    }
  }

  std::optional<std::vector<uint8_t>> load_blob(
      const std::string &key) override {
    if (!valid_key(key)) return std::nullopt;

    std::ifstream in(blob_path(key), std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const auto length = in.tellg();
    if (length < 0 || length > 4096) return std::nullopt;
    in.seekg(0);

    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
  }

  bool save_blob(const std::string &key, const uint8_t *data,
                 size_t len) override {
    if (!valid_key(key) || (data == nullptr && len != 0) || len > 4096) {
      return false;
    }

    if (::mkdir(SENDSPIN_STATE_DIR, 0700) != 0 && errno != EEXIST) {
      return false;
    }

    const std::string path = blob_path(key);
    const std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                    0600);
    if (fd < 0) return false;

    if (::fchmod(fd, 0600) != 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      return false;
    }

    bool ok = true;
    size_t written = 0;
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
      written += static_cast<size_t>(n);
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
        ::open(SENDSPIN_STATE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) return false;
    const bool synced = (::fsync(dir_fd) == 0);
    ::close(dir_fd);
    return synced;
  }

 private:
  static bool valid_key(const std::string &key) {
    if (key.empty()) return false;
    for (unsigned char c : key) {
      if (!std::isalnum(c) && c != '_') return false;
    }
    return true;
  }

  static std::string blob_path(const std::string &key) {
    return std::string(SENDSPIN_STATE_DIR) + "/" + key;
  }
};
// ============================================================================
// PulseAudio volume/mute controller (async, non-blocking)
// ============================================================================

class PulseVolumeController {
 public:
  PulseVolumeController() {
    ml_ = pa_threaded_mainloop_new();
    if (!ml_) return;
    ctx_ = pa_context_new(pa_threaded_mainloop_get_api(ml_), "sendspin-vol");
    if (!ctx_) {
      pa_threaded_mainloop_free(ml_);
      ml_ = nullptr;
      return;
    }
    if (pa_context_connect(ctx_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
      pa_context_unref(ctx_);
      ctx_ = nullptr;
      pa_threaded_mainloop_free(ml_);
      ml_ = nullptr;
      return;
    }
    if (pa_threaded_mainloop_start(ml_) < 0) {
      pa_context_disconnect(ctx_);
      pa_context_unref(ctx_);
      ctx_ = nullptr;
      pa_threaded_mainloop_free(ml_);
      ml_ = nullptr;
      return;
    }
    started_ = true;
  }

  ~PulseVolumeController() {
    if (!ml_) return;
    if (started_) pa_threaded_mainloop_stop(ml_);
    if (ctx_) {
      pa_context_disconnect(ctx_);
      pa_context_unref(ctx_);
    }
    pa_threaded_mainloop_free(ml_);
  }

  void set_mute(bool muted) {
    if (!ctx_ || !ml_ || !started_) return;
    pa_threaded_mainloop_lock(ml_);
    if (pa_context_get_state(ctx_) == PA_CONTEXT_READY) {
      auto *op = pa_context_set_sink_mute_by_name(ctx_, "@DEFAULT_SINK@", muted ? 1 : 0, nullptr, nullptr);
      if (op) pa_operation_unref(op);
    }
    pa_threaded_mainloop_unlock(ml_);
  }

 private:
  pa_threaded_mainloop *ml_{nullptr};
  pa_context *ctx_{nullptr};
  bool started_{false};
};

// ============================================================================
// LED Controller — drives tr-ledring animations and direct sysfs for loudness
// ============================================================================

class LedColorController {
 public:
  // Set base color from server and start breathing animation
  void set_color(uint8_t r, uint8_t g, uint8_t b) {
    std::lock_guard<std::mutex> lock(mu_);
    if (r == base_r_ && g == base_g_ && b == base_b_) return;
    base_r_ = r; base_g_ = g; base_b_ = b;
    playing_ = true;
    if (!led_disabled_) start_breathing();
  }

  // Reset to default color (called on track change before color arrives)
  void reset_color() {
    std::lock_guard<std::mutex> lock(mu_);
    base_r_ = 0x40; base_g_ = 0x80; base_b_ = 0xFF;
    loudness_peak_ = 0;
    smooth_bright_ = 0.0f;
    if (playing_ && !led_disabled_) {
      sysfs_mode_ = false;
      start_breathing();
    }
  }

  void queue_pulse() {
    pulse_pending_.store(true, std::memory_order_release);
  }

  void queue_loudness(uint16_t loudness) {
    pending_loudness_.store(loudness, std::memory_order_relaxed);
    loudness_pending_.store(true, std::memory_order_release);
  }

  void reset_visualizer() {
    pulse_pending_.store(false, std::memory_order_release);
    loudness_pending_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mu_);
    loudness_peak_ = 0;
    smooth_bright_ = 0.0f;
    sysfs_mode_ = false;
    if (playing_ && !led_disabled_) start_breathing();
  }

  // Stop all LED output
  void clear() {
    std::lock_guard<std::mutex> lock(mu_);
    playing_ = false;
    sysfs_mode_ = false;
    send_led_idle();
    write_sysfs("/sys/class/leds/RGB_R/brightness", 0);
    write_sysfs("/sys/class/leds/RGB_G/brightness", 0);
    write_sysfs("/sys/class/leds/RGB_B/brightness", 0);
  }

  // Restart LED with last known color (or default light blue)
  void resume() {
    std::lock_guard<std::mutex> lock(mu_);
    if (base_r_ == 0 && base_g_ == 0 && base_b_ == 0) {
      base_r_ = 0x40; base_g_ = 0x80; base_b_ = 0xFF;
    }
    playing_ = true;
    sysfs_mode_ = false;
    loudness_peak_ = 0;
    smooth_bright_ = 0.0f;
    if (led_disabled_) return;
    fprintf(stderr, "[sendspin] LED resume #%02x%02x%02x\n", base_r_, base_g_, base_b_);
    start_breathing();
  }

  // Pause LED output to avoid conflicting with tr-ledring volume animations
  void suppress(int64_t duration_us) {
    std::lock_guard<std::mutex> lock(mu_);
    suppress_until_ = now_us() + duration_us;
    sysfs_mode_ = false;
  }

  // Toggle LED effect on/off
  void toggle_enabled() {
    std::lock_guard<std::mutex> lock(mu_);
    led_disabled_ = !led_disabled_;
    if (!write_json_int(SENDSPIN_CONF, "led_disabled",
                        led_disabled_ ? 1 : 0)) {
      fprintf(stderr, "[sendspin] failed to persist LED setting\n");
    }
    fprintf(stderr, "[sendspin] LED %s\n",
            led_disabled_ ? "disabled" : "enabled");
    if (led_disabled_) {
      send_led_idle();
      write_sysfs("/sys/class/leds/RGB_R/brightness", 0);
      write_sysfs("/sys/class/leds/RGB_G/brightness", 0);
      write_sysfs("/sys/class/leds/RGB_B/brightness", 0);
    } else if (playing_) {
      sysfs_mode_ = false;
      start_breathing();
    }
  }

  // Load persisted LED state
  void load_led_state() {
    // Music LED defaults to disabled (see FilePersistenceProvider, which
    // seeds /data/conf/sendspin.json with led_disabled=1). Only an explicit
    // stored 0 (user enabled via double-tap) turns the music LED on.
    int val = read_json_int(SENDSPIN_CONF, "led_disabled");
    led_disabled_ = (val != 0);
  }

  // Call from the main loop for all Visualizer-driven LED I/O.
  void tick() {
    const bool pulse = pulse_pending_.exchange(false, std::memory_order_acq_rel);
    const bool loudness =
        loudness_pending_.exchange(false, std::memory_order_acq_rel);
    const uint16_t level =
        pending_loudness_.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mu_);
    if (!playing_ || led_disabled_) return;

    const int64_t now = now_us();
    if (suppress_until_ > 0 && now >= suppress_until_) {
      suppress_until_ = 0;
      send_led_idle();
      sysfs_mode_ = true;
    }
    if (now < suppress_until_) return;

    if (pulse && now - last_pulse_time_ >= 250000) {
      last_pulse_time_ = now;
      generate_pulse();
      return;
    }

    if (loudness) {
      if (!sysfs_mode_) {
        send_led_idle();
        sysfs_mode_ = true;
      }
      if (level > loudness_peak_) loudness_peak_ = level;
      else loudness_peak_ = loudness_peak_ * 255 / 256;
      const uint16_t peak = loudness_peak_ > 2000 ? loudness_peak_ : 2000;
      float norm = static_cast<float>(level) / peak;
      if (norm > 1.0f) norm = 1.0f;
      const float target = norm * norm;
      const float smoothing = target > smooth_bright_ ? 0.7f : 0.6f;
      smooth_bright_ += (target - smooth_bright_) * smoothing;

      const float scale =
          std::min(2.5f, 220.0f /
                    std::max({base_r_, base_g_, base_b_, uint8_t{1}}));
      write_sysfs("/sys/class/leds/RGB_R/brightness",
                  static_cast<uint8_t>(
                      std::min(255.0f, base_r_ * scale * smooth_bright_)));
      write_sysfs("/sys/class/leds/RGB_G/brightness",
                  static_cast<uint8_t>(
                      std::min(255.0f, base_g_ * scale * smooth_bright_)));
      write_sysfs("/sys/class/leds/RGB_B/brightness",
                  static_cast<uint8_t>(
                      std::min(255.0f, base_b_ * scale * smooth_bright_)));
    }
  }

 private:
  void start_breathing() {
    static constexpr const char *PATH = "/tmp/sendspin_breath.animation";
    FILE *f = fopen(PATH, "w");
    if (!f) return;

    float scale = std::min(2.0f, 200.0f / std::max({base_r_, base_g_, base_b_, (uint8_t)1}));
    uint8_t pr = static_cast<uint8_t>(std::min(255.0f, base_r_ * scale));
    uint8_t pg = static_cast<uint8_t>(std::min(255.0f, base_g_ * scale));
    uint8_t pb = static_cast<uint8_t>(std::min(255.0f, base_b_ * scale));

    for (int i = 0; i < 32; i++) {
      float t = 0.25f + 0.75f * i / 31.0f;
      write_frame(f, static_cast<uint8_t>(pr * t),
                     static_cast<uint8_t>(pg * t),
                     static_cast<uint8_t>(pb * t));
    }
    for (int i = 0; i < 31; i++) {
      float t = 1.0f - 0.75f * (i + 1) / 31.0f;
      write_frame(f, static_cast<uint8_t>(pr * t),
                     static_cast<uint8_t>(pg * t),
                     static_cast<uint8_t>(pb * t));
    }
    fprintf(f, "loop\n");
    write_frame(f, static_cast<uint8_t>(pr * 0.25f),
                   static_cast<uint8_t>(pg * 0.25f),
                   static_cast<uint8_t>(pb * 0.25f));
    fclose(f);
    send_led_animation(PATH, PATH);
  }

  void generate_pulse() {
    static constexpr const char *PULSE_PATH = "/tmp/sendspin_pulse.animation";
    static constexpr const char *BREATH_PATH = "/tmp/sendspin_breath.animation";
    FILE *f = fopen(PULSE_PATH, "w");
    if (!f) return;

    float scale = std::min(2.0f, 200.0f / std::max({base_r_, base_g_, base_b_, (uint8_t)1}));
    uint8_t pr = static_cast<uint8_t>(std::min(255.0f, base_r_ * scale));
    uint8_t pg = static_cast<uint8_t>(std::min(255.0f, base_g_ * scale));
    uint8_t pb = static_cast<uint8_t>(std::min(255.0f, base_b_ * scale));

    write_frame(f, pr, pg, pb);
    write_frame(f, pr, pg, pb);
    for (int i = 1; i <= 8; i++) {
      float t = 1.0f - (float)i / 8.0f;
      write_frame(f, static_cast<uint8_t>(pr * (0.25f + 0.75f * t)),
                     static_cast<uint8_t>(pg * (0.25f + 0.75f * t)),
                     static_cast<uint8_t>(pb * (0.25f + 0.75f * t)));
    }
    fprintf(f, "loop\n");
    write_frame(f, static_cast<uint8_t>(pr * 0.25f),
                   static_cast<uint8_t>(pg * 0.25f),
                   static_cast<uint8_t>(pb * 0.25f));
    fclose(f);
    send_led_animation(PULSE_PATH, BREATH_PATH);
  }

  static void write_frame(FILE *f, uint8_t r, uint8_t g, uint8_t b) {
    fprintf(f, "16:%02x%02x%02x,%02x%02x%02x,%02x%02x%02x,%02x%02x%02x,"
               "%02x%02x%02x,%02x%02x%02x,%02x%02x%02x,%02x%02x%02x,"
               "%02x%02x%02x,%02x%02x%02x,%02x%02x%02x,%02x%02x%02x\n",
            r, g, b, r, g, b, r, g, b, r, g, b,
            r, g, b, r, g, b, r, g, b, r, g, b,
            r, g, b, r, g, b, r, g, b, r, g, b);
  }

  static void write_sysfs(const char *path, uint8_t value) {
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%u", value); fclose(f); }
  }

  void send_led_animation(const char *path, const char *next_path = nullptr) {
    char cmd[512];
    if (next_path) {
      snprintf(cmd, sizeof(cmd),
        "dbus-send --system --type=signal /com/3r/EventBus "
        "com._3reality.EventBus.LedShow boolean:false "
        "array:string:\"%s\",\"%s\"", path, next_path);
    } else {
      snprintf(cmd, sizeof(cmd),
        "dbus-send --system --type=signal /com/3r/EventBus "
        "com._3reality.EventBus.LedShow boolean:false "
        "array:string:\"%s\"", path);
    }
    const int rc = ::system(cmd);
    if (rc != 0) {
      fprintf(stderr, "[sendspin] LED animation command failed: %d\n", rc);
    }
  }

  void send_led_idle() {
    const int rc =
        ::system("dbus-send --system --type=signal /com/3r/EventBus "
                 "com._3reality.EventBus.LedShow boolean:true "
                 "array:string:");
    if (rc != 0) {
      fprintf(stderr, "[sendspin] LED idle command failed: %d\n", rc);
    }
  }

  std::mutex mu_;
  uint8_t base_r_{0}, base_g_{0}, base_b_{0};
  int64_t suppress_until_{0};
  int64_t last_pulse_time_{0};
  uint16_t loudness_peak_{0};
  float smooth_bright_{0.0f};
  bool playing_{false};
  bool sysfs_mode_{false};
  bool led_disabled_{false};
  std::atomic<bool> pulse_pending_{false};
  std::atomic<bool> loudness_pending_{false};
  std::atomic<uint16_t> pending_loudness_{0};
};

// ============================================================================
// Player listener — bounded asynchronous PulseAudio output
// ============================================================================

class PulsePlayerListener : public PlayerRoleListener {
 public:
  PulsePlayerListener() {
    mainloop_ = pa_threaded_mainloop_new();
    if (!mainloop_) {
      fprintf(stderr, "[sendspin] pa_threaded_mainloop_new failed\n");
      return;
    }
    if (pa_threaded_mainloop_start(mainloop_) < 0) {
      fprintf(stderr, "[sendspin] pa_threaded_mainloop_start failed\n");
      pa_threaded_mainloop_free(mainloop_);
      mainloop_ = nullptr;
      return;
    }

    pa_threaded_mainloop_lock(mainloop_);
    create_context_locked();
    pa_threaded_mainloop_unlock(mainloop_);

    const int volume = read_device_volume();
    if (volume >= 0 && volume <= 100) {
      last_volume_.store(volume, std::memory_order_relaxed);
    }
  }

  ~PulsePlayerListener() override {
    shutting_down_.store(true, std::memory_order_release);
    if (!mainloop_) return;

    pa_threaded_mainloop_lock(mainloop_);
    close_stream_locked();
    if (context_) {
      pa_context_disconnect(context_);
      pa_context_unref(context_);
      context_ = nullptr;
    }
    pa_threaded_mainloop_unlock(mainloop_);
    pa_threaded_mainloop_stop(mainloop_);
    pa_threaded_mainloop_free(mainloop_);
    mainloop_ = nullptr;
  }

  void set_player(PlayerRole *p) { player_ = p; }
  void set_volume_controller(PulseVolumeController *vc) { vol_ctrl_ = vc; }
  void set_led_controller(LedColorController *lc) { led_ctrl_ = lc; }

  size_t on_audio_write(uint8_t *data, size_t length,
                        uint32_t timeout_ms) override {
    if (length == 0 || !mainloop_ || !player_) return 0;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    do {
      if (shutting_down_.load(std::memory_order_acquire)) return 0;

      size_t written = 0;
      size_t written_frame_size = 1;
      pa_usec_t latency_us = 20000;

      pa_threaded_mainloop_lock(mainloop_);
      if (stream_active_ && ensure_stream_locked()) {
        const size_t writable = pa_stream_writable_size(stream_);
        if (writable == static_cast<size_t>(-1)) {
          fprintf(stderr, "[sendspin] pa_stream_writable_size failed: %s\n",
                  pa_strerror(pa_context_errno(context_)));
          close_stream_locked();
        } else {
          written = std::min(length, writable);
          written -= written % frame_size_;
          written_frame_size = frame_size_;
          if (written > 0) {
            write_buffer_.assign(data, data + written);
            apply_stream_gain(write_buffer_.data(), written);
            if (pa_stream_write(stream_, write_buffer_.data(), written,
                                nullptr, 0, PA_SEEK_RELATIVE) < 0) {
              fprintf(stderr, "[sendspin] pa_stream_write failed: %s\n",
                      pa_strerror(pa_context_errno(context_)));
              close_stream_locked();
              written = 0;
            } else {
              int negative = 0;
              if (pa_stream_get_latency(stream_, &latency_us, &negative) < 0 ||
                  negative) {
                latency_us = 20000;
              }
            }
          }
        }
      }
      pa_threaded_mainloop_unlock(mainloop_);

      if (written > 0) {
        const uint32_t frames =
            static_cast<uint32_t>(written / written_frame_size);
        player_->notify_audio_played(frames, now_us() + latency_us);
        return written;
      }

      if (timeout_ms == 0 || std::chrono::steady_clock::now() >= deadline) {
        return 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);

    return 0;
  }

  void on_stream_start() override {
    fprintf(stderr, "[sendspin] on_stream_start\n");
    if (!mainloop_ || !player_) return;

    const auto &params = player_->get_current_stream_params();
    if (!params.sample_rate.has_value() || !params.channels.has_value()) {
      fprintf(stderr, "[sendspin] stream parameters unavailable\n");
      return;
    }

    pa_threaded_mainloop_lock(mainloop_);
    stream_active_ = true;
    next_stream_attempt_us_ = 0;
    sample_rate_ = *params.sample_rate;
    channels_ = *params.channels;
    frame_size_ = static_cast<size_t>(channels_) * sizeof(int16_t);
    close_stream_locked();
    ensure_stream_locked();
    pa_threaded_mainloop_unlock(mainloop_);
  }

  void on_stream_end() override {
    fprintf(stderr, "[sendspin] on_stream_end\n");
    if (!mainloop_) return;

    pa_threaded_mainloop_lock(mainloop_);
    stream_active_ = false;
    close_stream_locked();
    pa_threaded_mainloop_unlock(mainloop_);
  }

  void on_mute_changed(bool muted) override {
    fprintf(stderr, "[sendspin] mute: %s\n", muted ? "on" : "off");
    if (vol_ctrl_) vol_ctrl_->set_mute(muted);
  }

  void on_volume_changed(uint8_t volume_percent) override {
    const int percent = std::min(static_cast<int>(volume_percent), 100);
    last_volume_.store(percent, std::memory_order_release);
    if (led_ctrl_) led_ctrl_->suppress(2000000);
    persist_volume(percent);
    fprintf(stderr, "[sendspin] volume: %d%%\n", percent);
  }

  void on_output_delay_changed(uint16_t delay_ms) override {
    fprintf(stderr, "[sendspin] output_delay: %u ms\n", delay_ms);
  }

  void sync_local_state(PlayerRole &player) {
    const int percent = read_device_volume();
    if (percent < 0 || percent > 100) return;
    const bool changed =
        percent != last_volume_.load(std::memory_order_acquire);
    if (changed) {
      last_volume_.store(percent, std::memory_order_release);
      if (led_ctrl_) led_ctrl_->suppress(2000000);
    }
    if (changed || player.get_volume() != percent) {
      player.update_volume(static_cast<uint8_t>(percent));
      fprintf(stderr, "[sendspin] local volume synced: %d%%\n", percent);
    }
  }

 private:
  static constexpr int64_t kReconnectBackoffUs = 1000000;

  void create_context_locked() {
    if (!mainloop_ || context_) return;
    const int64_t now = now_us();
    if (now < next_connect_attempt_us_) return;
    next_connect_attempt_us_ = now + kReconnectBackoffUs;

    context_ = pa_context_new(pa_threaded_mainloop_get_api(mainloop_),
                              "sendspin-client");
    if (!context_) {
      fprintf(stderr, "[sendspin] pa_context_new failed\n");
      return;
    }
    if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
      fprintf(stderr, "[sendspin] pa_context_connect failed: %s\n",
              pa_strerror(pa_context_errno(context_)));
      pa_context_unref(context_);
      context_ = nullptr;
    }
  }

  bool ensure_context_locked() {
    if (context_) {
      const auto state = pa_context_get_state(context_);
      if (state == PA_CONTEXT_READY) return true;
      if (state != PA_CONTEXT_FAILED && state != PA_CONTEXT_TERMINATED) {
        return false;
      }
      close_stream_locked();
      pa_context_disconnect(context_);
      pa_context_unref(context_);
      context_ = nullptr;
    }
    create_context_locked();
    return context_ && pa_context_get_state(context_) == PA_CONTEXT_READY;
  }

  bool ensure_stream_locked() {
    if (!stream_active_ || !ensure_context_locked()) return false;
    if (stream_) {
      const auto state = pa_stream_get_state(stream_);
      if (state == PA_STREAM_READY) return true;
      if (state != PA_STREAM_FAILED && state != PA_STREAM_TERMINATED) {
        return false;
      }
      close_stream_locked();
    }

    const int64_t now = now_us();
    if (now < next_stream_attempt_us_) return false;
    next_stream_attempt_us_ = now + 100000;

    pa_sample_spec spec{PA_SAMPLE_S16LE, sample_rate_, channels_};
    if (!pa_sample_spec_valid(&spec)) {
      fprintf(stderr, "[sendspin] invalid PA format %uHz %uch\n",
              sample_rate_, channels_);
      return false;
    }

    pa_proplist *props = pa_proplist_new();
    if (props) {
      pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "sendspin-client");
      pa_proplist_sets(props, PA_PROP_MEDIA_NAME, "Sendspin");
    }
    stream_ = pa_stream_new_with_proplist(context_, "Sendspin", &spec,
                                          nullptr, props);
    if (props) pa_proplist_free(props);
    if (!stream_) {
      fprintf(stderr, "[sendspin] pa_stream_new failed: %s\n",
              pa_strerror(pa_context_errno(context_)));
      return false;
    }

    const uint32_t bytes_per_frame = pa_frame_size(&spec);
    const uint32_t target_length = spec.rate * bytes_per_frame * 20 / 1000;
    pa_buffer_attr attr{target_length * 2, target_length, 0,
                        static_cast<uint32_t>(-1),
                        static_cast<uint32_t>(-1)};
    const auto flags = static_cast<pa_stream_flags_t>(
        PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE |
        PA_STREAM_ADJUST_LATENCY);
    if (pa_stream_connect_playback(stream_, nullptr, &attr, flags, nullptr,
                                   nullptr) < 0) {
      fprintf(stderr, "[sendspin] pa_stream_connect_playback failed: %s\n",
              pa_strerror(pa_context_errno(context_)));
      close_stream_locked();
      return false;
    }

    fprintf(stderr, "[sendspin] opening PA %uHz %uch (frame=%zu)\n",
            sample_rate_, channels_, frame_size_);
    return pa_stream_get_state(stream_) == PA_STREAM_READY;
  }

  void close_stream_locked() {
    if (!stream_) return;
    pa_stream_disconnect(stream_);
    pa_stream_unref(stream_);
    stream_ = nullptr;
    fprintf(stderr, "[sendspin] closed PulseAudio stream\n");
  }

  void apply_stream_gain(uint8_t *data, size_t length) {
    if (!g_ducked.load(std::memory_order_relaxed)) return;
    auto *samples = reinterpret_cast<int16_t *>(data);
    const size_t count = length / sizeof(int16_t);
    for (size_t i = 0; i < count; ++i) {
      samples[i] = static_cast<int16_t>(samples[i] >> 2);
    }
  }

  pa_threaded_mainloop *mainloop_{nullptr};
  pa_context *context_{nullptr};
  pa_stream *stream_{nullptr};
  PlayerRole *player_{nullptr};
  PulseVolumeController *vol_ctrl_{nullptr};
  LedColorController *led_ctrl_{nullptr};
  std::vector<uint8_t> write_buffer_;
  std::atomic<int> last_volume_{100};
  std::atomic<bool> shutting_down_{false};
  bool stream_active_{false};
  uint32_t sample_rate_{48000};
  uint8_t channels_{2};
  size_t frame_size_{4};
  int64_t next_connect_attempt_us_{0};
  int64_t next_stream_attempt_us_{0};
};
// ============================================================================
// Color listener — applies album color to LED
// ============================================================================

class ColorListener : public ColorRoleListener {
 public:
  void set_led(LedColorController *led) { led_ = led; }

  void on_color(const ServerColorStateObject &c) override {
    if (!led_) return;
    const std::optional<RgbColor> *pick = nullptr;
    if (c.accent) pick = &c.accent;
    else if (c.primary) pick = &c.primary;
    else if (c.background_dark) pick = &c.background_dark;
    if (pick && *pick) {
      auto &rgb = **pick;
      led_->set_color(rgb[0], rgb[1], rgb[2]);
      fprintf(stderr, "[sendspin] color: #%02x%02x%02x\n", rgb[0], rgb[1], rgb[2]);
    }
  }

  void on_color_clear() override {
    if (led_) led_->reset_color();
  }

 private:
  LedColorController *led_{nullptr};
};

// ============================================================================
// Visualizer listener — drives LED from loudness and beat data
// ============================================================================

class VisualizerListener : public VisualizerRoleListener {
 public:
  void set_led(LedColorController *led) { led_ = led; }

  void on_visualizer_stream_start(const ServerVisualizerStreamObject &) override {
    fprintf(stderr, "[sendspin] visualizer stream started\n");
    if (led_) led_->reset_visualizer();
  }

  void on_visualizer_stream_end() override {
    fprintf(stderr, "[sendspin] visualizer stream ended\n");
    if (led_) led_->reset_visualizer();
  }

  void on_visualizer_stream_clear() override {
    fprintf(stderr, "[sendspin] visualizer stream cleared\n");
    if (led_) led_->reset_visualizer();
  }

  void on_beat(int64_t, bool) override {
    if (led_) led_->queue_pulse();
  }

  void on_loudness(int64_t, uint16_t loudness) override {
    if (led_) led_->queue_loudness(loudness);
  }

 private:
  LedColorController *led_{nullptr};
};

// ============================================================================
// Controller listener — tracks playback state, suppresses LED on volume change
// ============================================================================

class ControllerListener : public ControllerRoleListener {
 public:
  void set_led(LedColorController *led) { led_ = led; }

  void on_controller_state(const ServerStateControllerObject &state) override {
    fprintf(stderr, "[sendspin] controller state: vol=%u muted=%d repeat=%u shuffle=%d cmds=%zu\n",
            state.volume, state.muted, static_cast<unsigned>(state.repeat),
            state.shuffle, state.supported_commands.size());
    if (last_vol_ >= 0 && state.volume != last_vol_ && led_)
      led_->suppress(1500000);
    last_vol_ = state.volume;
  }

  void on_controller_state_clear() override {
    fprintf(stderr, "[sendspin] controller state cleared\n");
  }

 private:
  LedColorController *led_{nullptr};
  int last_vol_{-1};
};

// ============================================================================
// Metadata listener
// ============================================================================

class SimpleMetadataListener : public MetadataRoleListener {
 public:
  void set_led(LedColorController *led) { led_ = led; }

  void on_metadata(const ServerMetadataStateObject &m) override {
    if (m.artist && m.title) {
      std::string track = *m.artist + " - " + *m.title;
      if (track != last_track_) {
        last_track_ = track;
        if (led_) led_->reset_color();
        fprintf(stderr, "[sendspin] now playing: %s\n", track.c_str());
      }
    }
  }

  void on_metadata_clear() override {
    fprintf(stderr, "[sendspin] metadata cleared\n");
    last_track_.clear();
  }

 private:
  LedColorController *led_{nullptr};
  std::string last_track_;
};

// ============================================================================
// Client listener — group state, high-performance networking
// ============================================================================

class MainClientListener : public SendspinClientListener {
 public:
  void set_led(LedColorController *led) { led_ = led; }

  void on_time_sync_updated(float error) override {
    fprintf(stderr, "[sendspin] time sync: error=%.1f us\n", error);
  }

  void on_group_update(const GroupUpdateObject &group) override {
    if (group.playback_state.has_value()) {
      auto state = *group.playback_state;
      fprintf(stderr, "[sendspin] group playback: %s\n",
              state == SendspinPlaybackState::PLAYING ? "playing" : "stopped");
      if (state == SendspinPlaybackState::STOPPED && led_) led_->clear();
      else if (state == SendspinPlaybackState::PLAYING && led_) led_->resume();
    }
  }

  void on_request_high_performance() override {
    fprintf(stderr, "[sendspin] high-performance requested\n");
  }
  void on_release_high_performance() override {
    fprintf(stderr, "[sendspin] high-performance released\n");
  }

 private:
  LedColorController *led_{nullptr};
};

class HostNetworkProvider : public SendspinNetworkProvider {
 public:
  bool is_network_ready() override { return true; }
};

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
  std::string connect_url;
  std::string friendly_name = "3RSPK Speaker";
  auto log_level = LogLevel::INFO;

  int opt;
  while ((opt = getopt(argc, argv, "u:vqh")) != -1) {
    switch (opt) {
      case 'u': connect_url = optarg; break;
      case 'v': log_level = LogLevel::DEBUG; break;
      case 'q': log_level = LogLevel::ERROR; break;
      default: return 1;
    }
  }
  if (optind < argc) friendly_name = argv[optind];

  struct sigaction sa_quit{};
  sigemptyset(&sa_quit.sa_mask);
  sa_quit.sa_handler = signal_handler;
  sigaction(SIGINT, &sa_quit, nullptr);
  sigaction(SIGTERM, &sa_quit, nullptr);

  SendspinClient::set_log_level(log_level);

  SendspinClientConfig config;
  config.name = friendly_name;
  config.product_name = "Voice & Music Assistant";
  config.manufacturer = "ThirdReality";
  config.software_version = SENDSPIN_CLIENT_VERSION;

  FilePersistenceProvider persistence;
  PulseVolumeController vol_ctrl;
  LedColorController led_ctrl;
  PulsePlayerListener player_listener;
  SimpleMetadataListener metadata_listener;
  ControllerListener controller_listener;
  ColorListener color_listener;
  VisualizerListener visualizer_listener;
  HostNetworkProvider network;
  MainClientListener client_listener;

  led_ctrl.load_led_state();
  player_listener.set_volume_controller(&vol_ctrl);
  player_listener.set_led_controller(&led_ctrl);
  metadata_listener.set_led(&led_ctrl);
  controller_listener.set_led(&led_ctrl);
  color_listener.set_led(&led_ctrl);
  visualizer_listener.set_led(&led_ctrl);
  client_listener.set_led(&led_ctrl);

  SendspinClient client(std::move(config));
  client.set_persistence_provider(&persistence);
  client.set_unpaired_access_enabled(kUnpairedAccess);
  client.set_network_provider(&network);
  client.set_listener(&client_listener);

  PlayerRoleConfig player_config;
  player_config.audio_formats = {
      {SendspinCodecFormat::FLAC, 2, 44100, 16},
      {SendspinCodecFormat::FLAC, 2, 48000, 16},
      {SendspinCodecFormat::OPUS, 2, 48000, 16},
      {SendspinCodecFormat::PCM, 2, 44100, 16},
      {SendspinCodecFormat::PCM, 2, 48000, 16},
  };
  auto &player = client.add_player(std::move(player_config));
  player.set_output_delay_adjustable(true);
  player_listener.set_player(&player);
  player.set_listener(&player_listener);
  const int initial_volume = read_device_volume();
  if (initial_volume >= 0 && initial_volume <= 100) {
    player.update_volume(static_cast<uint8_t>(initial_volume));
  }

  auto &metadata = client.add_metadata();
  metadata.set_listener(&metadata_listener);

  auto &controller = client.add_controller();
  controller.set_listener(&controller_listener);

  auto &color = client.add_color();
  color.set_listener(&color_listener);

  VisualizerRoleConfig viz_config;
  viz_config.support.buffer_capacity = 8192;
  viz_config.stream.types = {
      VisualizerDataType::BEAT,
      VisualizerDataType::LOUDNESS,
  };
  viz_config.stream.rate_max = 30;
  auto &visualizer = client.add_visualizer(std::move(viz_config));
  visualizer.set_listener(&visualizer_listener);

  fprintf(stderr,
          "[sendspin] client %s starting (protocol=1.0.0-rc1 access=%s)\n",
          SENDSPIN_CLIENT_VERSION,
          kUnpairedAccess ? "unpaired" : "paired-only");
  if (!client.start()) {
    fprintf(stderr, "[sendspin] client start failed\n");
    return 1;
  }
  fprintf(stderr, "[sendspin] listening as \"%s\" (id=%s)\n",
          friendly_name.c_str(), client.client_id().c_str());
  if (!connect_url.empty()) client.connect_to(connect_url);

  // Control socket: tap commands arrive from lva, which owns the tap key.
  ControlSocket control;
  if (!control.open()) {
    fprintf(stderr, "[sendspin] control socket unavailable; "
                    "tap commands disabled\n");
  }

  int64_t last_controller_failure_us = 0;
  auto send_controller_command =
      [&](SendspinControllerCommand command, const char *name) {
        if (controller.send_command({.command = command})) return;
        const int64_t now = now_us();
        if (now - last_controller_failure_us >= 5000000) {
          last_controller_failure_us = now;
          fprintf(stderr, "[sendspin] controller command rejected: %s\n",
                  name);
        }
      };

  int poll_count = 0;
  while (g_running) {
    client.loop();
    led_ctrl.tick();

    control.poll([&](const char *cmd) {
      if (strcmp(cmd, "MUSIC_TOGGLE") == 0) {
        if (client.get_group_state().playback_state ==
            SendspinPlaybackState::PLAYING)
          send_controller_command(SendspinControllerCommand::PAUSE, "PAUSE");
        else
          send_controller_command(SendspinControllerCommand::PLAY, "PLAY");
      } else if (strcmp(cmd, "PAUSE") == 0) {
        send_controller_command(SendspinControllerCommand::PAUSE, "PAUSE");
      } else if (strcmp(cmd, "LED_TOGGLE") == 0) {
        led_ctrl.toggle_enabled();
      } else if (strcmp(cmd, "DUCK") == 0) {
        g_ducked = true;
      } else if (strcmp(cmd, "UNDUCK") == 0) {
        g_ducked = false;
      }
    });

    if (++poll_count >= 30) {
      poll_count = 0;
      player_listener.sync_local_state(player);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }

  fprintf(stderr, "[sendspin] shutting down\n");
  led_ctrl.clear();
  client.stop();
  return 0;
}
