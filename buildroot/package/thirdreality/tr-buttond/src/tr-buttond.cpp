// tr-buttond: ThirdReality button daemon.
//
// Owns the gpio_keypad input device for the speaker. Replaces the policy
// role of Amlogic's input_eventd (which is no longer started). Runs
// independently of linux-voice-assistant (lva) so keys keep working even when
// lva is down.
//
// Key map (from /etc/gpio_key.kl):
//   Volup 115, Voldown 114, Mute 113, Home 102, Tap 353 (KEY_SELECT).
//
// Dispatch:
//   Volup / Voldown / Mute short press  -> adckey_function.sh Volup/Voldown/Mute
//   Volup / Voldown / Mute long press   -> adckey_function.sh longpress<Type>
//   Home long press                     -> adckey_function.sh longpressHome
//   Tap  long press                     -> adckey_function.sh longpressTap
//   Home single / double / triple       -> lva socket HOME_SINGLE/DOUBLE/TRIPLE
//   Tap  single                         -> lva socket TAP (lva decides: abort
//                                          TTS, else toggle music); if lva is
//                                          down, fall back to sendspin
//                                          MUSIC_TOGGLE
//   Tap  double                         -> sendspin socket LED_TOGGLE
//
// System actions (volume / mute / factory-reset / wifi) stay in the shell
// script /etc/adckey/adckey_function.sh, which is ThirdReality-owned. Only the
// lva/sendspin semantic actions go over Unix datagram sockets.

#include <dirent.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr const char* kInputDeviceName = "gpio_keypad";
constexpr const char* kAdckeyScript   = "/etc/adckey/adckey_function.sh";
constexpr const char* kLvaSocket      = "/tmp/lva-control.sock";
constexpr const char* kSendspinSocket = "/tmp/sendspin-control.sock";
constexpr const char* kMuseSocket     = "/tmp/muse-gadget-control.sock";
constexpr const char* kMuseState      = "/data/conf/muse/pairing.json";

// Key codes (see /etc/gpio_key.kl).
constexpr int kKeyVoldown = 114;
constexpr int kKeyVolup   = 115;
constexpr int kKeyMute    = 113;
constexpr int kKeyHome    = 102;
constexpr int kKeyTap     = 353;

// Timing.
constexpr int kLongPressMs     = 10000;  // press held this long = long press (10s)
constexpr int kTapWindowTapMs  = 350;    // tap-key multi-tap grouping window
constexpr int kTapWindowHomeMs = 500;    // home-key multi-tap grouping window
constexpr int kPollTimeoutMs   = 100;

volatile std::sig_atomic_t g_running = 1;

void HandleSignal(int) { g_running = 0; }

int64_t NowMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

// Find and open the input device whose EVIOCGNAME matches kInputDeviceName.
// The event* number is assigned by kernel probe order and is not stable, so
// match by driver-reported name instead of a hardcoded /dev/input/event0.
// Returns an O_CLOEXEC fd, or -1 if not found.
int OpenInputDevice() {
    DIR* dir = ::opendir("/dev/input");
    if (dir == nullptr) {
        fprintf(stderr, "[buttond] opendir(/dev/input) failed: %s\n",
                std::strerror(errno));
        return -1;
    }

    int found = -1;
    struct dirent* de;
    while ((de = ::readdir(dir)) != nullptr) {
        if (std::strncmp(de->d_name, "event", 5) != 0) continue;

        std::string path = std::string("/dev/input/") + de->d_name;
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;

        char name[64] = {0};
        if (::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
            std::strcmp(name, kInputDeviceName) == 0) {
            found = fd;
            break;
        }
        ::close(fd);
    }

    ::closedir(dir);
    return found;
}

// Run adckey_function.sh with one argument.
//
// Uses system() exactly like Amlogic's input_eventd did. This matters for
// factory_reset's `killall5 -9`, which spares only processes in the caller's
// own session: the shell started by system() is a direct child of this daemon
// and shares its session, so killall5 leaves both alive long enough to reach
// `reboot`. system() blocks until the script returns; the long-press scripts
// (factory_reset / change_wifi) are terminal actions, and the short-press
// scripts return quickly.
void RunScript(const char* arg) {
    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "%s %s", kAdckeyScript, arg);
    const int rc = std::system(cmd);
    if (rc != 0) {
        fprintf(stderr, "[buttond] '%s' exited with %d\n", cmd, rc);
    }
}

// Send a text command to a Unix datagram socket. Returns true on success.
// Failure (e.g. ENOENT when the peer is not running) is expected and quiet.
bool SendTo(const char* socket_path, const char* cmd) {
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    const ssize_t n = ::sendto(fd, cmd, std::strlen(cmd), MSG_NOSIGNAL,
                               reinterpret_cast<struct sockaddr*>(&addr),
                               sizeof(addr));
    fprintf(stderr, "[buttond] SendTo %s cmd=%s n=%zd errno=%d\n",
            socket_path, cmd, n, (n < 0 ? errno : 0));
    ::close(fd);
    return n >= 0;
}

void OnTap(int taps) {
    if (taps == 1) {
        // lva owns the semantics: abort TTS if speaking, else toggle music.
        // If lva is down the datagram cannot be delivered, so fall back to
        // telling sendspin to toggle music directly.
        if (!SendTo(kLvaSocket, "TAP")) {
            SendTo(kSendspinSocket, "MUSIC_TOGGLE");
        }
    } else if (taps >= 2) {
        SendTo(kSendspinSocket, "LED_TOGGLE");
    }
}

void OnHome(int taps) {
    if (::access(kMuseState, R_OK) == 0) return;
    const char* cmd = (taps == 1)   ? "HOME_SINGLE"
                      : (taps == 2)  ? "HOME_DOUBLE"
                                     : "HOME_TRIPLE";
    SendTo(kLvaSocket, cmd);
}

// Tracks tap grouping and long-press detection for one key.
class GestureKey {
   public:
    GestureKey(int tap_window_ms, const char* long_press_arg,
               void (*on_taps)(int taps))
        : tap_window_ms_(tap_window_ms),
          long_press_arg_(long_press_arg),
          on_taps_(on_taps) {}

    void OnDown(int64_t now_ms) {
        down_ms_      = now_ms;
        long_fired_   = false;
        press_active_ = true;
    }

    void OnUp(int64_t now_ms) {
        press_active_ = false;
        if (long_fired_) return;  // long press already handled on timeout
        ++tap_count_;
        last_up_ms_  = now_ms;
        window_open_ = true;
    }

    // Fire long-press and window expiry. Call every loop iteration.
    void Tick(int64_t now_ms) {
        if (press_active_ && !long_fired_ &&
            now_ms - down_ms_ >= kLongPressMs) {
            long_fired_  = true;
            window_open_ = false;
            tap_count_   = 0;
            RunScript(long_press_arg_);
        }
        if (window_open_ && now_ms - last_up_ms_ >= tap_window_ms_) {
            Flush();
        }
    }

    // Shortest timeout the daemon should poll for, or -1 if nothing pending.
    int PendingTimeoutMs(int64_t now_ms) const {
        int timeout = -1;
        if (press_active_ && !long_fired_) {
            const int t = static_cast<int>(kLongPressMs - (now_ms - down_ms_));
            timeout = (t < 1) ? 1 : t;
        }
        if (window_open_) {
            const int t = static_cast<int>(tap_window_ms_ - (now_ms - last_up_ms_));
            const int tc = (t < 1) ? 1 : t;
            if (timeout < 0 || tc < timeout) timeout = tc;
        }
        return timeout;
    }

   private:
    void Flush() {
        const int count = tap_count_;
        tap_count_   = 0;
        window_open_ = false;
        if (count >= 1 && on_taps_) on_taps_(count);
    }

    const int   tap_window_ms_;
    const char* long_press_arg_;
    void (*on_taps_)(int taps);

    int64_t down_ms_      = 0;
    int64_t last_up_ms_   = 0;
    bool    press_active_ = false;
    bool    long_fired_   = false;
    bool    window_open_  = false;
    int     tap_count_    = 0;
};

// Volume / mute keys: no multi-tap. Matches Amlogic input_eventd for a
// keymode-0 key: a short press fires <Type> on release; a press held to the
// long-press threshold fires longpress<Type> immediately (a no-op in
// adckey_function.sh) and still fires <Type> on release.
class SimpleKey {
   public:
    SimpleKey(const char* short_arg, const char* long_arg)
        : short_arg_(short_arg), long_arg_(long_arg) {}

    void OnDown(int64_t now_ms) {
        down_ms_      = now_ms;
        long_fired_   = false;
        press_active_ = true;
    }

    void OnUp() {
        press_active_ = false;
        RunScript(short_arg_);
    }

    void Tick(int64_t now_ms) {
        if (press_active_ && !long_fired_ &&
            now_ms - down_ms_ >= kLongPressMs) {
            long_fired_ = true;
            RunScript(long_arg_);
        }
    }

    int PendingTimeoutMs(int64_t now_ms) const {
        if (!press_active_ || long_fired_) return -1;
        const int t = static_cast<int>(kLongPressMs - (now_ms - down_ms_));
        return (t < 1) ? 1 : t;
    }

   private:
    const char* short_arg_;
    const char* long_arg_;
    int64_t     down_ms_      = 0;
    bool        press_active_ = false;
    bool        long_fired_   = false;
};

void HandleKey(const struct input_event& ev, int64_t now_ms,
               GestureKey& tap_key, GestureKey& home_key,
               SimpleKey& volup_key, SimpleKey& voldown_key,
               SimpleKey& mute_key) {
    // Keep a paired Home gesture exclusively in Muse, including release.
    static bool muse_home_active = false;
    // ev.value: 1 = down, 0 = up, 2 = autorepeat (ignored).
    const int code = ev.code;

    if (ev.value == 2) return;  // ignore kernel autorepeat
    const bool down = (ev.value == 1);
    fprintf(stderr, "[buttond] key code=%d value=%d\n", code, ev.value);
    if (code == kKeyHome) {
        if (down && ::access(kMuseState, R_OK) == 0 && SendTo(kMuseSocket, "BUTTON_DOWN")) {
            muse_home_active = true;
            return;
        }
        if (!down && muse_home_active) {
            SendTo(kMuseSocket, "BUTTON_UP");
            muse_home_active = false;
            return;
        }
    }

    switch (code) {
        case kKeyTap:
        case kKeyHome: {
            GestureKey& g = (code == kKeyTap) ? tap_key : home_key;
            if (down) {
                if (code == kKeyTap) SendTo(kLvaSocket, "TAP_DOWN");
                g.OnDown(now_ms);
            } else {
                g.OnUp(now_ms);
            }
            break;
        }
        case kKeyVolup:
            if (down) volup_key.OnDown(now_ms); else volup_key.OnUp();
            break;
        case kKeyVoldown:
            if (down) voldown_key.OnDown(now_ms); else voldown_key.OnUp();
            break;
        case kKeyMute:
            if (down) mute_key.OnDown(now_ms); else mute_key.OnUp();
            break;
        default:
            break;
    }
}

}  // namespace

int main() {
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGPIPE, SIG_IGN);

    GestureKey tap_key(kTapWindowTapMs, "longpressTap", OnTap);
    GestureKey home_key(kTapWindowHomeMs, "longpressHome", OnHome);
    SimpleKey  volup_key("Volup", "longpressVolup");
    SimpleKey  voldown_key("Voldown", "longpressVoldown");
    SimpleKey  mute_key("Mute", "longpressMute");

    int fd = -1;
    while (g_running) {
        if (fd < 0) {
            fd = OpenInputDevice();
            if (fd < 0) {
                fprintf(stderr, "[buttond] input device \"%s\" not found; "
                        "retrying\n", kInputDeviceName);
                ::sleep(1);
                continue;
            }
            fprintf(stderr, "[buttond] watching input device \"%s\"\n",
                    kInputDeviceName);
        }

        const int64_t now = NowMs();
        int timeout = kPollTimeoutMs;
        const int pending[] = {
            tap_key.PendingTimeoutMs(now),
            home_key.PendingTimeoutMs(now),
            volup_key.PendingTimeoutMs(now),
            voldown_key.PendingTimeoutMs(now),
            mute_key.PendingTimeoutMs(now),
        };
        for (const int t : pending) {
            if (t >= 0 && t < timeout) timeout = t;
        }

        struct pollfd pfd{};
        pfd.fd     = fd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, timeout);

        if (pr < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[buttond] poll failed: %s\n", std::strerror(errno));
            ::close(fd);
            fd = -1;
            continue;
        }

        // Device vanished (unplug / driver reload): reopen instead of spinning.
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            fprintf(stderr, "[buttond] input device gone; reopening\n");
            ::close(fd);
            fd = -1;
            continue;
        }

        if (pr > 0 && (pfd.revents & POLLIN)) {
            struct input_event ev;
            const ssize_t n = ::read(fd, &ev, sizeof(ev));
            if (n == static_cast<ssize_t>(sizeof(ev))) {
                if (ev.type == EV_KEY) {
                    HandleKey(ev, NowMs(), tap_key, home_key,
                              volup_key, voldown_key, mute_key);
                }
            } else if (n == 0 ||
                       (n < 0 && errno != EAGAIN && errno != EINTR)) {
                // EOF or hard read error: drop the fd and reopen.
                if (n < 0) {
                    fprintf(stderr, "[buttond] read failed: %s\n",
                            std::strerror(errno));
                }
                ::close(fd);
                fd = -1;
                continue;
            }
        }

        const int64_t t = NowMs();
        tap_key.Tick(t);
        home_key.Tick(t);
        volup_key.Tick(t);
        voldown_key.Tick(t);
        mute_key.Tick(t);
    }

    if (fd >= 0) ::close(fd);
    fprintf(stderr, "[buttond] exiting\n");
    return 0;
}
