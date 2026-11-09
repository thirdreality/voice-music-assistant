#include "app/Signals.h"

#include <sys/wait.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <thread>

#include "audio/WakeWordEngine.h"
#include "protocol/ApiServer.h"
#include "util/Log.h"
namespace lva::app {
namespace {
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<lva::proto::ApiServer*>::is_always_lock_free);
static_assert(std::atomic<lva::audio::WakeWordEngine*>::is_always_lock_free);
std::atomic<int> g_shutdown_signal{0};
std::atomic<lva::proto::ApiServer*> g_server{nullptr};
std::atomic<lva::audio::WakeWordEngine*> g_wakeword_engine{nullptr};
std::atomic<unsigned> g_active_handlers{0};

extern "C" void OnSignal(int signo) {
    const int saved_errno = errno;
    g_active_handlers.fetch_add(1);
    g_shutdown_signal.store(signo, std::memory_order_relaxed);
    if (auto* engine = g_wakeword_engine.load()) {
        engine->RequestStop();
    }
    if (auto* server = g_server.load()) {
        server->Stop();
    }
    g_active_handlers.fetch_sub(1);
    errno = saved_errno;
}

extern "C" void OnSigchld(int /*signo*/) {
    int saved_errno = errno;
    while (::waitpid(-1, nullptr, WNOHANG) > 0) {
    }
    errno = saved_errno;
}

// A handler may run on another worker. Clearing a target must wait for handlers
// that already borrowed it before its owner can destroy it. The counter and
// targets use sequential consistency so a later handler observes null.
void WaitForHandlers() {
    const auto started = std::chrono::steady_clock::now();
    auto next_warning = started + std::chrono::seconds(5);
    while (g_active_handlers.load() != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_warning) {
            // Ordinary shutdown context, never the signal handler itself.
            LVA_LOGW("signals",
                     "still waiting for active signal handlers before releasing targets");
            next_warning = now + std::chrono::seconds(5);
        }
        // Briefly yield for the normal fast path, then avoid burning CPU if a
        // handler's worker is descheduled. Do not return while it owns a target:
        // a timeout would allow that worker to access a destroyed object.
        if (now - started < std::chrono::milliseconds(10))
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}  // namespace

void InstallSignalHandlers() {
    struct sigaction sa{};
    sa.sa_handler = &OnSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    struct sigaction sc{};
    sc.sa_handler = &OnSigchld;
    sigemptyset(&sc.sa_mask);
    sc.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    ::sigaction(SIGCHLD, &sc, nullptr);
}

void SetSignalServer(lva::proto::ApiServer* server) {
    g_server.store(server);
    if (!server) WaitForHandlers();
}
void SetSignalWakeEngine(lva::audio::WakeWordEngine* engine) {
    g_wakeword_engine.store(engine);
    if (!engine) WaitForHandlers();
}
int ShutdownSignal() { return g_shutdown_signal.load(std::memory_order_relaxed); }

}  // namespace lva::app
