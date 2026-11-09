#include "tr/DeviceEffects.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/Log.h"

extern char** environ;

namespace lva::tr {
namespace {
constexpr const char* kTag = "device-io";
using Clock = std::chrono::steady_clock;
constexpr auto kVolumeInterval = std::chrono::milliseconds(100);
constexpr auto kLedInterval = std::chrono::milliseconds(200);

pid_t Spawn(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0) {
        LVA_LOGW(kTag, "spawn actions failed: %s", std::strerror(rc));
        return -1;
    }
    rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (rc == 0) rc = posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (rc == 0) rc = posix_spawn_file_actions_addopen(
        &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    if (rc == 0) rc = ::posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        LVA_LOGW(kTag, "spawn %s failed: %s", args[0].c_str(), std::strerror(rc));
        return -1;
    }
    return pid;
}

struct Child {
    pid_t pid = -1;
    Clock::time_point started{};
    bool warned = false;

    void Poll(const char* name) {
        if (pid < 0) return;
        int status = 0;
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) {
            // The global SIGCHLD handler may have reaped this fire-and-forget
            // helper. ECHILD means finished, but its exit status is unknown.
            if (result == pid && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
                LVA_LOGW(kTag, "%s helper failed (pid=%d, status=%d)", name, pid, status);
            }
            pid = -1;
        } else if (!warned && Clock::now() - started >= std::chrono::seconds(5)) {
            warned = true;
            LVA_LOGW(kTag, "%s helper still running (pid=%d); keeping only latest request",
                     name, pid);
        }
    }

    void Started(pid_t value) { pid = value; started = Clock::now(); warned = false; }
};

struct LedCommand {
    bool to_idle;
    std::string array_arg;
    bool feedback;
    bool deduplicate;
    bool Same(const LedCommand& other) const {
        return to_idle == other.to_idle && array_arg == other.array_arg;
    }
};

class Effects {
   public:
    ~Effects() { Stop(); }

    bool Start() {
        std::lock_guard lock(mutex_);
        if (thread_.joinable()) return !stopping_;
        stopping_ = false;
        try {
            thread_ = std::thread([this] {
                try { Run(); }
                catch (const std::exception& error) {
                    LVA_LOGE(kTag, "worker failed: %s", error.what());
                    std::lock_guard lock(mutex_);
                    stopping_ = true;
                } catch (...) {
                    LVA_LOGE(kTag, "worker failed: unknown exception");
                    std::lock_guard lock(mutex_);
                    stopping_ = true;
                }
            });
        } catch (const std::exception& error) {
            stopping_ = true;
            LVA_LOGE(kTag, "thread creation failed: %s", error.what());
            return false;
        }
        return true;
    }

    void Stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    void Volume(int percent) {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            LVA_LOGW(kTag, "volume request rejected: worker stopped");
            return;
        }
        volume_ = std::clamp(percent, 0, 100);
        changed_.notify_one();
    }

    void Led(LedCommand command) {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            LVA_LOGW(kTag, "LED request rejected: worker stopped");
            return;
        }
        // A volume overlay is optional; do not displace a queued voice/mute
        // state or a music animation that has not been submitted yet.
        if (command.feedback && led_ && !led_->feedback) return;
        // Bound burst deduplication in time: later turns must still retrigger
        // animations after an external sender or the daemon changed the LED.
        if (command.deduplicate && last_led_ && command.Same(*last_led_) &&
            Clock::now() - last_led_at_ < kLedInterval) {
            // Intentional latest-wins semantics: the latest desired state is
            // already submitted, so cancel even a DIFFERENT pending state.
            // Short intermediate animations need not all be played.
            led_.reset();
            return;
        }
        led_ = std::move(command);
        changed_.notify_one();
    }

   private:
    static pid_t ApplyVolume(int percent) {
        const pid_t pid = Spawn({"/usr/bin/tr-volume", "set", std::to_string(percent)});
        LVA_LOGD(kTag, "apply volume=%d%% (pid=%d)", percent, pid);
        return pid;
    }

    static pid_t ApplyLed(const LedCommand& command) {
        return Spawn({"/usr/bin/dbus-send", "--system", "--type=signal",
                      "/com/3r/EventBus", "com._3reality.EventBus.LedShow",
                      command.to_idle ? "boolean:true" : "boolean:false", command.array_arg});
    }

    void Run() {
        ::pthread_setname_np(::pthread_self(), "lva-device-io");
        Child volume_child, led_child;
        auto next_volume = Clock::time_point::min();
        auto next_feedback = Clock::time_point::min();
        for (;;) {
            volume_child.Poll("volume");
            led_child.Poll("LED");
            std::optional<int> volume;
            std::optional<LedCommand> led;
            bool stopping;
            bool skipped_led = false;
            {
                std::unique_lock lock(mutex_);
                stopping = stopping_;
                const auto now = Clock::now();
                if (volume_ && (stopping || (volume_child.pid < 0 && now >= next_volume))) {
                    volume = std::exchange(volume_, std::nullopt);
                    next_volume = now + kVolumeInterval;
                }
                if (led_ && led_child.pid < 0 &&
                    (stopping || !led_->feedback || now >= next_feedback)) {
                    led = std::exchange(led_, std::nullopt);
                    last_led_ = led;
                    last_led_at_ = now;
                    if (led->feedback) next_feedback = now + kLedInterval;
                }
                if (stopping && led_) {
                    // Unlike tr-volume, dbus-send has no latest-request lock.
                    // A parallel final LED helper could overtake an older one.
                    skipped_led = true;
                    led_.reset();
                }
                if (!volume && !led && !stopping) {
                    if (volume_child.pid >= 0 || led_child.pid >= 0 || volume_ || led_) {
                        changed_.wait_for(lock, std::chrono::milliseconds(20));
                    } else {
                        changed_.wait(lock, [this] { return stopping_ || volume_ || led_; });
                    }
                    continue;
                }
            }
            if (skipped_led) {
                LVA_LOGW(kTag, "pending LED discarded on shutdown: previous helper still running");
            }
            if (volume) volume_child.Started(ApplyVolume(*volume));
            if (led) {
                const pid_t pid = ApplyLed(*led);
                led_child.Started(pid);
                if (pid < 0) {
                    std::lock_guard lock(mutex_);
                    last_led_.reset();  // Do not suppress a retry after spawn failure.
                }
            }
            // Helpers do not reference LVA memory. Final volume updates are
            // serialized by tr-volume's own latest-request/apply locks. A busy
            // LED slot drops its pending animation instead of spawning a
            // parallel helper. Do not wait for helper exit during shutdown.
            if (stopping) return;
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread thread_;
    bool stopping_ = true;
    std::optional<int> volume_;
    std::optional<LedCommand> led_, last_led_;
    Clock::time_point last_led_at_{};
};

Effects& Instance() { static Effects effects; return effects; }
}  // namespace

bool StartDeviceEffects() { return Instance().Start(); }
void StopDeviceEffects() { Instance().Stop(); }
void QueueSystemVolume(int percent) { Instance().Volume(percent); }
void QueueLedAnimation(bool to_idle, std::string array_arg,
                       bool volume_feedback, bool deduplicate) {
    Instance().Led({to_idle, std::move(array_arg), volume_feedback, deduplicate});
}

}  // namespace lva::tr
