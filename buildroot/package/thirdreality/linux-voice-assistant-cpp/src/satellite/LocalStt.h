#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lva::audio { class ISpeechToText; class PcmRingBuffer; }

namespace lva::satellite {

// Runs an ISpeechToText engine on its own thread so the ~1 s decode does not
// block the main loop. Public methods are main-thread only; the engine is
// only touched by the worker.
class LocalStt {
   public:
    struct Options {
        double max_utterance_seconds = 15.0;
        std::string dump_wav_path;  // optional, for mic diagnostics
    };

    LocalStt(std::unique_ptr<lva::audio::ISpeechToText> engine,
             lva::audio::PcmRingBuffer& ring, Options options);
    ~LocalStt();

    LocalStt(const LocalStt&)            = delete;
    LocalStt& operator=(const LocalStt&) = delete;

    void Start();
    void Stop();

    bool Ready() const noexcept {
        return ready_.load(std::memory_order_acquire);
    }

    void BeginUtterance();
    void Cancel();

    // Returned once; empty string = nothing recognized.
    std::optional<std::string> TakeResult();

   private:
    void ThreadMain();
    void DrainRing();
    void WriteDump(const std::vector<std::int16_t>& pcm) const;

    std::unique_ptr<lva::audio::ISpeechToText> engine_;
    lva::audio::PcmRingBuffer&                 ring_;
    Options                                    options_;

    std::thread       thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> ready_{false};

    // generation_ bumps on every BeginUtterance()/Cancel() so a result from
    // an older utterance is never delivered.
    std::mutex                 mutex_;
    std::uint64_t              generation_ = 0;
    bool                       active_     = false;
    std::optional<std::string> result_;
    std::uint64_t              result_generation_ = 0;
};

}  // namespace lva::satellite
