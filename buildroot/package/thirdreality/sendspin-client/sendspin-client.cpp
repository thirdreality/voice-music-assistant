// Sendspin client for ThirdReality speaker — PulseAudio backend
// SPDX-License-Identifier: Apache-2.0
//
// Integrates sendspin-cpp with PulseAudio for audio output, and drives the
// LED ring via tr-ledring D-Bus animations and direct sysfs writes.
//
// Volume conventions (do not mix without converting):
//   - Sendspin wire (MA 2.8+): 0-100 percent
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
#include <pulse/simple.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace sendspin;

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
  explicit ScopedSoundLock(bool required) {
    if (!required) return;
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
  const bool is_sound_config = std::strcmp(path, SOUND_CONF) == 0;
  ScopedSoundLock lock(is_sound_config);
  if (is_sound_config && !lock.locked()) return false;
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

// Notify lva of external Sendspin playback transitions so it can gate the
// AEC echo canceller (and the linear-AEC shadow wake path) on actual Music
// Assistant playback. Fire-and-forget: lva binds the socket and may be down.
static void send_playback_state_to_lva(bool playing) {
  const int fd = ::socket(AF_UNIX,
                          SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return;

  struct sockaddr_un peer{};
  peer.sun_family = AF_UNIX;
  std::strncpy(peer.sun_path, LVA_CONTROL_PATH, sizeof(peer.sun_path) - 1);

  const char *command = playing ? "SENDSPIN_PLAYING:1" : "SENDSPIN_PLAYING:0";
  if (::sendto(fd, command, std::strlen(command), MSG_NOSIGNAL,
               reinterpret_cast<struct sockaddr *>(&peer),
               sizeof(peer)) < 0) {
    // Expected when lva is not running (no bound socket); drop silently.
  }
  ::close(fd);
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
// Persistence Provider — saves state to /data/conf/sendspin.json
// ============================================================================

class FilePersistenceProvider : public SendspinPersistenceProvider {
 public:
  FilePersistenceProvider() {
    // Ensure all required fields exist in config file
    std::ifstream test(SENDSPIN_CONF);
    if (!test) {
      std::ofstream out(SENDSPIN_CONF, std::ios::trunc);
      out << "{\n  \"last_server_hash\": 0,\n  \"static_delay_ms\": 0,\n  \"led_disabled\": 1\n}\n";
    } else {
      // Check if led_disabled field exists, add it if missing
      std::string content((std::istreambuf_iterator<char>(test)),
                          std::istreambuf_iterator<char>());
      test.close();
      if (content.find("\"led_disabled\"") == std::string::npos) {
        auto pos = content.rfind('}');
        if (pos != std::string::npos) {
          content.insert(pos, ",\n  \"led_disabled\": 1\n");
          std::ofstream out(SENDSPIN_CONF, std::ios::trunc);
          out << content;
        }
      }
    }
  }

  bool save_last_server_hash(uint32_t hash) override {
    write_json_int(SENDSPIN_CONF, "last_server_hash", static_cast<int>(hash));
    fprintf(stderr, "[sendspin] persisted last_server_hash: %u\n", hash);
    return true;
  }

  std::optional<uint32_t> load_last_server_hash() override {
    int val = read_json_int(SENDSPIN_CONF, "last_server_hash");
    if (val < 0) return std::nullopt;
    return static_cast<uint32_t>(val);
  }

  bool save_static_delay(uint16_t delay_ms) override {
    write_json_int(SENDSPIN_CONF, "static_delay_ms", delay_ms);
    fprintf(stderr, "[sendspin] persisted static_delay: %u ms\n", delay_ms);
    return true;
  }

  std::optional<uint16_t> load_static_delay() override {
    int val = read_json_int(SENDSPIN_CONF, "static_delay_ms");
    if (val < 0) return std::nullopt;
    return static_cast<uint16_t>(val);
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

  // Beat flash: briefly shows full brightness then fades back
  void pulse() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!playing_ || led_disabled_) return;
    int64_t now = now_us();
    if (now < suppress_until_ || now - last_pulse_time_ < 250000) return;
    last_pulse_time_ = now;
    generate_pulse();
  }

  // Continuous loudness-driven brightness via direct sysfs writes
  void set_loudness(uint16_t loudness) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!playing_ || now_us() < suppress_until_) return;
    if (led_disabled_) return;

    // On first loudness frame, stop tr-ledring so we own sysfs exclusively
    if (!sysfs_mode_) {
      send_led_idle();
      sysfs_mode_ = true;
    }

    // Auto-scale to observed peak with minimum floor
    if (loudness > loudness_peak_) loudness_peak_ = loudness;
    else loudness_peak_ = loudness_peak_ * 255 / 256;
    uint16_t peak = (loudness_peak_ > 2000) ? loudness_peak_ : 2000;

    // Normalize and apply squared curve for contrast
    float norm = static_cast<float>(loudness) / peak;
    if (norm > 1.0f) norm = 1.0f;
    float target = norm * norm;

    // Smooth: fast attack (0.7), fast decay (0.6)
    if (target > smooth_bright_)
      smooth_bright_ += (target - smooth_bright_) * 0.7f;
    else
      smooth_bright_ += (target - smooth_bright_) * 0.6f;

    // Scale color for visibility (cap at 2.5x)
    float scale = std::min(2.5f, 220.0f / std::max({base_r_, base_g_, base_b_, (uint8_t)1}));
    write_sysfs("/sys/class/leds/RGB_R/brightness",
                static_cast<uint8_t>(std::min(255.0f, base_r_ * scale * smooth_bright_)));
    write_sysfs("/sys/class/leds/RGB_G/brightness",
                static_cast<uint8_t>(std::min(255.0f, base_g_ * scale * smooth_bright_)));
    write_sysfs("/sys/class/leds/RGB_B/brightness",
                static_cast<uint8_t>(std::min(255.0f, base_b_ * scale * smooth_bright_)));
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
    write_json_int(SENDSPIN_CONF, "led_disabled", led_disabled_ ? 1 : 0);
    fprintf(stderr, "[sendspin] LED %s\n", led_disabled_ ? "disabled" : "enabled");
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

  // Call from main loop to handle suppress expiry
  void tick() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!playing_ || led_disabled_) return;
    if (suppress_until_ > 0 && now_us() >= suppress_until_) {
      suppress_until_ = 0;
      send_led_idle();
      sysfs_mode_ = true;
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
    (void)system(cmd);
  }

  void send_led_idle() {
    (void)system("dbus-send --system --type=signal /com/3r/EventBus "
                 "com._3reality.EventBus.LedShow boolean:true "
                 "array:string:");
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
};

// ============================================================================
// Player listener — blocking PA audio output
// ============================================================================

class PulsePlayerListener : public PlayerRoleListener {
 public:
  ~PulsePlayerListener() override { close_pa(true); }

  void set_player(PlayerRole *p) { player_ = p; }
  void set_volume_controller(PulseVolumeController *vc) { vol_ctrl_ = vc; }
  void set_led_controller(LedColorController *lc) { led_ctrl_ = lc; }

  size_t on_audio_write(uint8_t *data, size_t length, uint32_t /*timeout_ms*/) override {
    if (length == 0) return 0;

    if (!pa_.load()) {
      int64_t now = now_us();
      if (now - last_open_attempt_us_ < backoff_us_) return length;
      last_open_attempt_us_ = now;

      pa_sample_spec ss{PA_SAMPLE_S16LE, 48000, 2};
      if (player_) {
        auto &params = player_->get_current_stream_params();
        if (params.sample_rate.has_value()) ss.rate = params.sample_rate.value();
        if (params.channels.has_value()) ss.channels = static_cast<uint8_t>(params.channels.value());
      }

      uint32_t bytes_per_frame = pa_frame_size(&ss);
      uint32_t tlength_bytes = ss.rate * bytes_per_frame * 20 / 1000;
      pa_buffer_attr ba{tlength_bytes * 2, tlength_bytes, 0, (uint32_t)-1, (uint32_t)-1};

      int err = 0;
      auto *pa = pa_simple_new(nullptr, "sendspin-client", PA_STREAM_PLAYBACK,
                               nullptr, "Sendspin", &ss, nullptr, &ba, &err);
      if (pa) {
        frame_size_ = bytes_per_frame;
        current_rate_ = ss.rate;
        current_channels_ = ss.channels;
        pa_.store(pa);
        backoff_us_ = kInitialBackoffUs;
        fprintf(stderr, "[sendspin] opened PA %uHz %uch (frame=%zu, tlength=%ums)\n",
                ss.rate, ss.channels, frame_size_,
                tlength_bytes * 1000 / (ss.rate * bytes_per_frame));
      } else {
        fprintf(stderr, "[sendspin] pa_simple_new failed: %s\n", pa_strerror(err));
        backoff_us_ = std::min(backoff_us_ * 2, kMaxBackoffUs);
        return length;
      }
    }

    auto *pa = pa_.load();
    size_t aligned = length - (length % frame_size_);
    if (aligned == 0) return length;

    if (g_ducked.load(std::memory_order_relaxed)) {
      auto *samples = reinterpret_cast<int16_t *>(data);
      const size_t count = aligned / sizeof(int16_t);
      for (size_t i = 0; i < count; ++i)
        samples[i] = static_cast<int16_t>(samples[i] >> 2);
    }

    int err = 0;
    if (pa_simple_write(pa, data, aligned, &err) < 0) {
      fprintf(stderr, "pa_simple_write: %s\n", pa_strerror(err));
      close_pa(false);
      return length;
    }

    uint32_t frames = static_cast<uint32_t>(aligned / frame_size_);
    int64_t latency_us = pa_simple_get_latency(pa, nullptr);
    if (latency_us < 0) latency_us = 0;
    player_->notify_audio_played(frames, now_us() + latency_us);
    return aligned;
  }

  void on_stream_start() override {
    fprintf(stderr, "[sendspin] on_stream_start\n");
    uint32_t new_rate = 0;
    uint8_t new_channels = 0;
    if (player_) {
      auto &p = player_->get_current_stream_params();
      if (p.sample_rate.has_value()) new_rate = *p.sample_rate;
      if (p.channels.has_value()) new_channels = static_cast<uint8_t>(*p.channels);
    }
    auto *pa = pa_.load();
    if (pa && new_rate == current_rate_ && new_channels == current_channels_) {
      pa_simple_flush(pa, nullptr);
    } else {
      close_pa(false);
    }
  }

  void on_stream_end() override {
    fprintf(stderr, "[sendspin] on_stream_end\n");
    close_pa(false);
  }

  void on_mute_changed(bool muted) override {
    fprintf(stderr, "[sendspin] mute: %s\n", muted ? "on" : "off");
    if (vol_ctrl_) vol_ctrl_->set_mute(muted);
  }

  void on_volume_changed(uint8_t volume_percent) override {
    int percent = std::min((int)volume_percent, 100);
    last_volume_ = percent;
    if (led_ctrl_) led_ctrl_->suppress(2000000);
    persist_volume(percent);
    fprintf(stderr, "[sendspin] volume: %d%%\n", percent);
  }

  void on_static_delay_changed(uint16_t delay_ms) override {
    fprintf(stderr, "[sendspin] static_delay: %u ms\n", delay_ms);
  }

  void sync_local_state(PlayerRole &player) {
    int percent = read_device_volume();
    if (percent < 0) return;
    if (percent != last_volume_) {
      player.update_volume(static_cast<uint8_t>(percent));
      last_volume_ = percent;
      if (led_ctrl_) led_ctrl_->suppress(2000000);
      fprintf(stderr, "[sendspin] local volume synced: %d%%\n", percent);
    }
  }

 private:
  static constexpr int64_t kInitialBackoffUs = 100'000;
  static constexpr int64_t kMaxBackoffUs = 5'000'000;

  void close_pa(bool drain) {
    auto *pa = pa_.exchange(nullptr);
    if (pa) {
      if (drain) pa_simple_drain(pa, nullptr);
      else pa_simple_flush(pa, nullptr);
      pa_simple_free(pa);
      fprintf(stderr, "[sendspin] closed PulseAudio (%s)\n", drain ? "drain" : "flush");
    }
    current_rate_ = 0;
    current_channels_ = 0;
  }

  std::atomic<pa_simple *> pa_{nullptr};
  PlayerRole *player_{nullptr};
  PulseVolumeController *vol_ctrl_{nullptr};
  LedColorController *led_ctrl_{nullptr};
  size_t frame_size_{4};
  uint32_t current_rate_{0};
  uint8_t current_channels_{0};
  int last_volume_{-1};
  int64_t last_open_attempt_us_{0};
  int64_t backoff_us_{kInitialBackoffUs};
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
    if (led_) led_->clear();
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
  }

  void on_visualizer_stream_end() override {
    fprintf(stderr, "[sendspin] visualizer stream ended\n");
  }

  void on_beat(int64_t, bool) override {
    if (led_) led_->pulse();
  }

  void on_loudness(int64_t, uint16_t loudness) override {
    if (led_) led_->set_loudness(loudness);
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
      const bool playing = (state == SendspinPlaybackState::PLAYING);
      send_playback_state_to_lva(playing);
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
  config.client_id = friendly_name;
  config.name = friendly_name;
  config.product_name = "Voice & Music Assistant";
  config.manufacturer = "ThirdReality";
  config.software_version = "1.0.0";

  SendspinClient client(std::move(config));

  FilePersistenceProvider persistence;
  client.set_persistence_provider(&persistence);

  PulseVolumeController vol_ctrl;
  LedColorController led_ctrl;
  led_ctrl.load_led_state();

  // Player
  PulsePlayerListener player_listener;
  PlayerRoleConfig player_config;
  player_config.audio_formats = {
      {SendspinCodecFormat::FLAC, 2, 44100, 16},
      {SendspinCodecFormat::FLAC, 2, 48000, 16},
      {SendspinCodecFormat::OPUS, 2, 48000, 16},
      {SendspinCodecFormat::OPUS, 1, 48000, 16},
      {SendspinCodecFormat::PCM, 2, 44100, 16},
      {SendspinCodecFormat::PCM, 2, 48000, 16},
  };
  auto saved_delay = persistence.load_static_delay();
  if (saved_delay.has_value()) player_config.initial_static_delay_ms = *saved_delay;
  auto &player = client.add_player(std::move(player_config));
  player.set_static_delay_adjustable(true);
  player_listener.set_player(&player);
  player_listener.set_volume_controller(&vol_ctrl);
  player_listener.set_led_controller(&led_ctrl);
  player.set_listener(&player_listener);
  const int initial_volume = read_device_volume();
  if (initial_volume >= 0 && initial_volume <= 100) {
    player.update_volume(static_cast<uint8_t>(initial_volume));
  }

  // Metadata
  SimpleMetadataListener metadata_listener;
  metadata_listener.set_led(&led_ctrl);
  auto &metadata = client.add_metadata();
  metadata.set_listener(&metadata_listener);

  // Controller
  ControllerListener controller_listener;
  controller_listener.set_led(&led_ctrl);
  auto &controller = client.add_controller();
  controller.set_listener(&controller_listener);

  // Color
  ColorListener color_listener;
  color_listener.set_led(&led_ctrl);
  auto &color = client.add_color();
  color.set_listener(&color_listener);

  // Visualizer
  VisualizerListener visualizer_listener;
  visualizer_listener.set_led(&led_ctrl);
  VisualizerRoleConfig viz_config;
  viz_config.support.types = {VisualizerDataType::BEAT, VisualizerDataType::LOUDNESS};
  viz_config.support.buffer_capacity = 8192;
  viz_config.support.rate_max = 30;
  auto &visualizer = client.add_visualizer(std::move(viz_config));
  visualizer.set_listener(&visualizer_listener);

  // Client
  HostNetworkProvider network;
  MainClientListener client_listener;
  client_listener.set_led(&led_ctrl);
  client.set_network_provider(&network);
  client.set_listener(&client_listener);

  client.start_server();
  fprintf(stderr, "[sendspin] listening as \"%s\"\n", friendly_name.c_str());
  if (!connect_url.empty()) client.connect_to(connect_url);

  // Control socket: tap commands arrive from lva, which owns the tap key.
  ControlSocket control;
  if (!control.open()) {
    fprintf(stderr, "[sendspin] control socket unavailable; "
                    "tap commands disabled\n");
  }

  int poll_count = 0;
  while (g_running) {
    client.loop();
    led_ctrl.tick();

    control.poll([&](const char *cmd) {
      if (strcmp(cmd, "MUSIC_TOGGLE") == 0) {
        if (client.get_group_state().playback_state ==
            SendspinPlaybackState::PLAYING)
          controller.send_command({.command = SendspinControllerCommand::PAUSE});
        else
          controller.send_command({.command = SendspinControllerCommand::PLAY});
      } else if (strcmp(cmd, "PAUSE") == 0) {
        controller.send_command({.command = SendspinControllerCommand::PAUSE});
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
  client.disconnect(SendspinGoodbyeReason::SHUTDOWN);
  return 0;
}
