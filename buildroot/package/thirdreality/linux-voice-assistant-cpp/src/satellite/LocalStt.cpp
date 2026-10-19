#include "satellite/LocalStt.h"

#include <algorithm>
#include <cctype>
#include <chrono>

#include <malloc.h>

#include "audio/ISpeechToText.h"
#include "audio/PcmRingBuffer.h"
#include "audio/WavFile.h"
#include "util/Log.h"

namespace lva::satellite {

namespace {

constexpr const char* kTag = "local-stt";

constexpr int         kSampleRate  = 16000;
constexpr std::size_t kChunkFrames = 1600;  // 100 ms

std::string Normalize(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    const auto first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const auto last = out.find_last_not_of(' ');
    return out.substr(first, last - first + 1);
}

}  // namespace

LocalStt::LocalStt(std::unique_ptr<lva::audio::ISpeechToText> engine,
                   lva::audio::PcmRingBuffer& ring, Options options)
    : engine_(std::move(engine)), ring_(ring), options_(std::move(options)) {}

LocalStt::~LocalStt() { Stop(); }

void LocalStt::Start() {
    if (thread_.joinable()) return;
    stop_requested_.store(false, std::memory_order_relaxed);
    thread_ = std::thread(&LocalStt::ThreadMain, this);
}

void LocalStt::Stop() {
    if (!thread_.joinable()) return;
    stop_requested_.store(true, std::memory_order_relaxed);
    thread_.join();
}

void LocalStt::BeginUtterance() {
    std::lock_guard<std::mutex> lk(mutex_);
    ++generation_;
    active_ = true;
    result_.reset();
}

void LocalStt::Cancel() {
    std::lock_guard<std::mutex> lk(mutex_);
    ++generation_;
    active_ = false;
    result_.reset();
}

std::optional<std::string> LocalStt::TakeResult() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!result_ || result_generation_ != generation_) return std::nullopt;
    auto out = std::move(result_);
    result_.reset();
    return out;
}

void LocalStt::DrainRing() {
    std::int16_t scratch[kChunkFrames];
    while (ring_.Read(scratch, kChunkFrames) > 0) {
    }
}

void LocalStt::WriteDump(const std::vector<std::int16_t>& pcm) const {
    if (options_.dump_wav_path.empty()) return;
    if (lva::audio::WriteWavS16Mono(options_.dump_wav_path, pcm,
                                    kSampleRate)) {
        LVA_LOGI(kTag, "utterance audio written to %s (%.1f s)",
                 options_.dump_wav_path.c_str(),
                 static_cast<double>(pcm.size()) / kSampleRate);
    }
}

void LocalStt::ThreadMain() {
    const auto load_start = std::chrono::steady_clock::now();
    if (!engine_->Start()) {
        LVA_LOGE(kTag, "model load failed; local STT unavailable");
        return;
    }
    malloc_trim(0);
    LVA_LOGI(kTag, "model loaded in %.1f s",
             std::chrono::duration<double>(
                 std::chrono::steady_clock::now() - load_start).count());
    ready_.store(true, std::memory_order_release);

    const auto max_samples = static_cast<std::size_t>(
        options_.max_utterance_seconds * kSampleRate);

    std::vector<std::int16_t> chunk(kChunkFrames);
    std::vector<std::int16_t> dump;
    std::uint64_t decoding_generation = 0;
    bool          decoding            = false;
    std::size_t   fed_samples         = 0;

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        std::uint64_t want_generation = 0;
        bool          want_active     = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            want_generation = generation_;
            want_active     = active_;
        }

        if (!want_active) {
            decoding = false;
            DrainRing();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (!decoding || decoding_generation != want_generation) {
            engine_->Reset();
            DrainRing();
            decoding            = true;
            decoding_generation = want_generation;
            fed_samples         = 0;
            dump.clear();
            LVA_LOGD(kTag, "utterance %llu started",
                     static_cast<unsigned long long>(decoding_generation));
        }

        const std::size_t got = ring_.Read(chunk.data(), chunk.size());
        if (got == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        engine_->AcceptWaveform(chunk.data(), got);
        fed_samples += got;
        if (!options_.dump_wav_path.empty()) {
            dump.insert(dump.end(), chunk.begin(), chunk.begin() + got);
        }

        const bool timed_out = fed_samples >= max_samples;
        if (!engine_->IsFinal() && !timed_out) continue;
        if (timed_out && !engine_->IsFinal()) {
            LVA_LOGI(kTag, "utterance hit %.0f s cap; finishing",
                     options_.max_utterance_seconds);
            engine_->InputFinished();
        }

        std::string text = Normalize(engine_->Result());
        LVA_LOGI(kTag, "final (%.1f s audio): '%s'",
                 static_cast<double>(fed_samples) / kSampleRate,
                 text.c_str());
        WriteDump(dump);

        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (generation_ == decoding_generation) {
                result_            = std::move(text);
                result_generation_ = decoding_generation;
                active_            = false;
            }
        }
        decoding = false;
        dump.clear();
        dump.shrink_to_fit();
        engine_->Reset();
        malloc_trim(0);
    }

    engine_->Stop();
    ready_.store(false, std::memory_order_release);
}

}  // namespace lva::satellite
