#include "satellite/LocalTts.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <utility>
#include <vector>

#include <pthread.h>
#include <unistd.h>

#include "audio/WavFile.h"
#include "util/Log.h"

namespace lva::satellite {
namespace {
constexpr const char* kTag = "local-tts";
constexpr const char* kTtsDir = "/tmp/lva-tts";

void RemoveFile(const std::string& path) {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
}  // namespace

LocalTts::LocalTts(lva::audio::SanoTts::Config config)
    : config_(std::move(config)), speed_(config_.speed) {}

LocalTts::~LocalTts() { Stop(); }

bool LocalTts::Start() {
    {
        std::lock_guard lock(mutex_);
        if (thread_.joinable()) return !stopping_;
        stopping_ = false;
        recovering_ = false;
        busy_ = true;  // Includes model initialization.
    }
    try {
        thread_ = std::thread([this] {
            try {
                ThreadMain();
                return;
            } catch (const std::exception& error) {
                LVA_LOGE(kTag, "worker failed: %s", error.what());
            } catch (...) {
                LVA_LOGE(kTag, "worker failed: unknown exception");
            }
            std::lock_guard lock(mutex_);
            busy_ = false;
            if (!stopping_) {
                DiscardResult();
                result_ = Result{generation_, {}};
                pending_.reset();
                stopping_ = true;
            }
        });
    } catch (const std::exception& error) {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        busy_ = false;
        LVA_LOGE(kTag, "thread creation failed: %s", error.what());
        return false;
    }
    return true;
}

void LocalTts::Stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        cancel_current_.store(true, std::memory_order_relaxed);
        pending_.reset();
        DiscardResult();
    }
    changed_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void LocalTts::SetSpeed(float speed) {
    std::lock_guard lock(mutex_);
    speed_ = speed;
}

std::uint64_t LocalTts::Submit(std::string text) {
    std::lock_guard lock(mutex_);
    if (stopping_ || recovering_ || text.empty()) return 0;
    cancel_current_.store(true, std::memory_order_relaxed);
    DiscardResult();
    pending_ = Request{++generation_, std::move(text)};
    progress_at_ = std::chrono::steady_clock::now();
    changed_.notify_one();
    return generation_;
}

void LocalTts::Cancel(bool timed_out) {
    std::lock_guard lock(mutex_);
    CancelLocked(timed_out);
}

bool LocalTts::TryCancelStalled(std::uint64_t generation,
                               std::chrono::steady_clock::duration timeout) {
    std::lock_guard lock(mutex_);
    if (generation != generation_ || result_ ||
        std::chrono::steady_clock::now() - progress_at_ < timeout) {
        return false;
    }
    CancelLocked(true);
    return true;
}

void LocalTts::CancelLocked(bool timed_out) {
    ++generation_;
    cancel_current_.store(true, std::memory_order_relaxed);
    pending_.reset();
    DiscardResult();
    if (timed_out) recovering_ = busy_;
}

bool LocalTts::Busy() const {
    std::lock_guard lock(mutex_);
    return busy_ || pending_.has_value();
}

std::optional<std::chrono::steady_clock::time_point> LocalTts::ProgressAt(
    std::uint64_t generation) const {
    std::lock_guard lock(mutex_);
    if (generation != generation_) return std::nullopt;
    return progress_at_;
}

void LocalTts::ReportProgress(std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    if (generation == generation_ && !stopping_ &&
        !cancel_current_.load(std::memory_order_relaxed)) {
        progress_at_ = std::chrono::steady_clock::now();
    }
}

void LocalTts::DiscardResult() {
    if (result_) RemoveFile(result_->path);
    result_.reset();
}

std::optional<LocalTts::Result> LocalTts::TakeResult() {
    std::lock_guard lock(mutex_);
    return std::exchange(result_, std::nullopt);
}

void LocalTts::ThreadMain() {
    ::pthread_setname_np(::pthread_self(), "lva-tts");
    // The API listener is already bound before this worker starts, so the
    // running LVA instance owns this directory. Keep crash leftovers out of
    // tmpfs, and perform the recursive cleanup off the main event loop.
    std::error_code cleanup_error;
    const auto removed = std::filesystem::remove_all(kTtsDir, cleanup_error);
    if (cleanup_error) {
        LVA_LOGW(kTag, "startup cleanup of %s failed: %s", kTtsDir,
                 cleanup_error.message().c_str());
    } else if (removed != 0) {
        LVA_LOGI(kTag, "startup cleanup of %s removed %llu entries", kTtsDir,
                 static_cast<unsigned long long>(removed));
    }
    lva::audio::SanoTts engine(config_);
    bool ready = false;
    try {
        ready = engine.Start();
    } catch (const std::exception& error) {
        LVA_LOGE(kTag, "initialization failed: %s", error.what());
    } catch (...) {
        LVA_LOGE(kTag, "initialization failed: unknown exception");
    }
    if (!ready) LVA_LOGE(kTag, "local TTS unavailable");
    {
        std::lock_guard lock(mutex_);
        busy_ = false;
        recovering_ = false;
    }

    for (;;) {
        Request request;
        float speed;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_) break;
            request = std::move(*pending_);
            pending_.reset();
            busy_ = true;
            speed = speed_;
            cancel_current_.store(false, std::memory_order_relaxed);
        }

        Result result{request.generation, {}};
        std::string path;
        try {
            std::vector<std::int16_t> pcm;
            const auto started = std::chrono::steady_clock::now();
            ReportProgress(request.generation);
            engine.SetSpeed(speed);
            if (ready && engine.Synthesize(request.text, pcm, cancel_current_,
                    [this, generation = request.generation] { ReportProgress(generation); }) &&
                !cancel_current_.load(std::memory_order_relaxed)) {
                std::error_code ec;
                std::filesystem::create_directories(kTtsDir, ec);
                path = std::string(kTtsDir) + "/reply-" + std::to_string(::getpid()) +
                       "-" + std::to_string(request.generation) + ".wav";
                if (!ec && lva::audio::WriteWavS16Mono(path, pcm, engine.SampleRate())) {
                    result.path = path;
                    LVA_LOGI(kTag, "job=%llu: %.2f s audio in %.0f ms -> %s",
                             static_cast<unsigned long long>(request.generation),
                             static_cast<double>(pcm.size()) / engine.SampleRate(),
                             std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - started).count(),
                             path.c_str());
                }
            }
        } catch (const std::exception& error) {
            LVA_LOGE(kTag, "synthesis failed: %s", error.what());
        } catch (...) {
            LVA_LOGE(kTag, "synthesis failed: unknown exception");
        }
        {
            std::lock_guard lock(mutex_);
            busy_ = false;
            recovering_ = false;
            if (!stopping_ && generation_ == request.generation &&
                !cancel_current_.load(std::memory_order_relaxed)) {
                if (result.path.empty()) {
                    RemoveFile(path);
                    LVA_LOGW(kTag, "job=%llu failed",
                             static_cast<unsigned long long>(request.generation));
                }
                result_ = std::move(result);
            } else {
                RemoveFile(path);
                LVA_LOGD(kTag, "discarded cancelled job=%llu",
                         static_cast<unsigned long long>(request.generation));
            }
        }
    }
    engine.Stop();
}

}  // namespace lva::satellite
