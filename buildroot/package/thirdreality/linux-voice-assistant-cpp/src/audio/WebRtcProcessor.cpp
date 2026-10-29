#include "audio/WebRtcProcessor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

#include "modules/audio_processing/include/audio_processing.h"
#include "rtc_base/ref_count.h"

#include "util/Log.h"
#ifdef LVA_DIAG_LOG
#include "util/MemProbe.h"
#endif

namespace lva::audio {

namespace {

constexpr const char* kTag = "webrtc";

// Conservative post-AEC residual suppression. The ref is much hotter than
// the mic, so match on normalized correlation, not level.
constexpr double kResidualMinRefRms  = 120.0;
constexpr double kResidualMinAbsCorr = 0.30;
constexpr double kResidualStrength   = 0.70;
constexpr double kResidualMaxCoeff   = 0.12;

// Suppress the shadow wake detector this long after the echo canceller
// turns on, while AEC3 converges and its linear output is still dirty.
constexpr std::int64_t kLinearResumeGuardNs = 1'500'000'000LL;

::webrtc::AudioProcessing::Config::NoiseSuppression::Level
ToNsLevel(int n) {
    using L = ::webrtc::AudioProcessing::Config::NoiseSuppression::Level;
    switch (n) {
        case 1:  return L::kLow;
        case 2:  return L::kModerate;
        case 3:  return L::kHigh;
        case 4:  return L::kVeryHigh;
        default: return L::kModerate;  // unused if disabled
    }
}

}  // namespace

WebRtcProcessor::WebRtcProcessor(int agc_level, int ns_level, bool aec_enabled)
    : agc_level_(agc_level),
      ns_level_(ns_level),
      aec_enabled_(aec_enabled) {
    stream_cfg_ = std::make_unique<::webrtc::StreamConfig>(
        kSampleRate, /*num_channels=*/1, /*has_keyboard=*/false);
    std::lock_guard<std::mutex> lk(apm_mutex_);
    RebuildLocked();
}

WebRtcProcessor::~WebRtcProcessor() {
    std::lock_guard<std::mutex> lk(apm_mutex_);
    if (apm_ != nullptr) {
        static_cast<::webrtc::AudioProcessing*>(apm_)->Release();
        apm_ = nullptr;
    }
}

void WebRtcProcessor::SetLevels(int agc_level, int ns_level) {
    std::lock_guard<std::mutex> lk(apm_mutex_);
    if (agc_level == agc_level_ && ns_level == ns_level_) return;
    agc_level_ = agc_level;
    ns_level_  = ns_level;
    LVA_LOGI(kTag, "levels updated: agc=%d ns=%d aec=%d",
             agc_level_, ns_level_, aec_enabled_ ? 1 : 0);
    RebuildLocked();
}

void WebRtcProcessor::SetFarEndActive(bool active) {
    if (!aec_enabled_) return;  // AEC not configured; nothing to gate
    far_end_active_.store(active, std::memory_order_relaxed);
}

void WebRtcProcessor::SetExternalFarEndActive(bool active) {
    if (!aec_enabled_) return;  // AEC not configured; nothing to gate
    external_far_end_active_.store(active, std::memory_order_relaxed);
}

void WebRtcProcessor::ApplyConfigLocked(bool echo_on) {
    auto* apm = static_cast<::webrtc::AudioProcessing*>(apm_);
    if (apm == nullptr) return;

    ::webrtc::AudioProcessing::Config cfg;
    if (aec_enabled_ && echo_on) {
        cfg.echo_canceller.enabled = true;
        cfg.echo_canceller.mobile_mode = false;

        cfg.echo_canceller.export_linear_aec_output = true;

        cfg.echo_canceller.enforce_high_pass_filtering = true;
        cfg.high_pass_filter.enabled = true;  // aids AEC convergence
    }
    if (agc_level_ > 0) {
        cfg.gain_controller1.enabled = true;
        cfg.gain_controller1.mode =
            ::webrtc::AudioProcessing::Config::GainController1::kFixedDigital;
        cfg.gain_controller1.target_level_dbfs = 3;  // -3 dBFS target
        cfg.gain_controller1.compression_gain_db =
            std::clamp(agc_level_, 1, 31);
        cfg.gain_controller1.enable_limiter = true;
    }
    // NS is disabled while AEC is active.
    if (ns_level_ > 0 && !echo_on) {
        cfg.noise_suppression.enabled = true;
        cfg.noise_suppression.level = ToNsLevel(ns_level_);
    }
    apm->ApplyConfig(cfg);
    echo_on_applied_ = (aec_enabled_ && echo_on);
}

void WebRtcProcessor::ReconcileGateLocked() {
    if (!aec_enabled_) return;
    const bool want_echo =
        far_end_active_.load(std::memory_order_relaxed) ||
        external_far_end_active_.load(std::memory_order_relaxed);
    if (want_echo != echo_on_applied_) {
        ApplyConfigLocked(want_echo);

        if (echo_on_applied_) {
            const auto now_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()).count();
            linear_guard_until_ns_.store(now_ns + kLinearResumeGuardNs,
                                         std::memory_order_relaxed);
        } else {
            linear_guard_until_ns_.store(0, std::memory_order_relaxed);
        }

        LVA_LOGD(kTag, "echo canceller %s (far-end %s)%s",
                 echo_on_applied_ ? "enabled" : "disabled",
                 want_echo ? "active" : "idle",
                 echo_on_applied_ ? " [linear guard armed 1500 ms]" : "");
    }
}

void WebRtcProcessor::RebuildLocked() {
    if (apm_ != nullptr) {
        static_cast<::webrtc::AudioProcessing*>(apm_)->Release();
        apm_ = nullptr;
    }

    auto* apm = ::webrtc::AudioProcessingBuilder().Create();
    if (apm == nullptr) {
        LVA_LOGE(kTag, "AudioProcessingBuilder::Create() returned null");
        return;
    }
    // A new RefCountedObject starts at refcount 0 (see rtc_base/ref_count.h):
    // the creator must AddRef() it. Without this, Release() below drops the
    // count to -1 and never deletes, leaking a whole AEC3 instance per rebuild.
    apm->AddRef();
    apm_ = apm;
    ++rebuild_count_;

    const bool echo_on =
        far_end_active_.load(std::memory_order_relaxed) ||
        external_far_end_active_.load(std::memory_order_relaxed);
    ApplyConfigLocked(echo_on);

    // A rebuild drops AEC3's converged state; re-arm the shadow guard.
    if (echo_on_applied_) {
        const auto now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now()
                    .time_since_epoch()).count();
        linear_guard_until_ns_.store(now_ns + kLinearResumeGuardNs,
                                     std::memory_order_relaxed);
    } else {
        linear_guard_until_ns_.store(0, std::memory_order_relaxed);
    }

    LVA_LOGI(kTag,
             "initialized (aec_cfg=%d, echo_on=%d, agc=%d gain_db=%d, ns=%d, hpf=%d)",
             aec_enabled_ ? 1 : 0,
             echo_on_applied_ ? 1 : 0,
             agc_level_,
             agc_level_ > 0 ? std::clamp(agc_level_, 1, 31) : 0,
             ns_level_,
             echo_on_applied_ ? 1 : 0);
}

bool WebRtcProcessor::ProcessReverse(std::int16_t* buf,
                                     std::size_t n) {
    std::lock_guard<std::mutex> lk(apm_mutex_);
    if (apm_ == nullptr) return false;
    if (!aec_enabled_)   return true;   // no-op fast path
    // Reconcile here too so ref and mic agree within a period.
    ReconcileGateLocked();
    if (!echo_on_applied_) return true;
    if (n == 0)          return true;
    if ((n % kFrameSamples) != 0) {
        LVA_LOGE(kTag, "ProcessReverse: n=%zu not a multiple of %d",
                 n, kFrameSamples);
        return false;
    }
    auto* apm = static_cast<::webrtc::AudioProcessing*>(apm_);
    for (std::size_t off = 0; off < n; off += kFrameSamples) {
        const int err = apm->ProcessReverseStream(
            buf + off, *stream_cfg_, *stream_cfg_, buf + off);
        if (err != ::webrtc::AudioProcessing::kNoError) {
            LVA_LOGE(kTag, "ProcessReverseStream failed: %d", err);
            return false;
        }
    }
    return true;
}

void WebRtcProcessor::SuppressSendspinResidual(
    std::int16_t* mic,
    const std::int16_t* ref,
    std::size_t n) {

    std::lock_guard<std::mutex> lk(apm_mutex_);

    if (mic == nullptr || ref == nullptr || n == 0) return;
    if (!aec_enabled_ || !echo_on_applied_) return;

    // External (Sendspin) playback only.
    if (!external_far_end_active_.load(std::memory_order_relaxed)) {
        return;
    }

    if ((n % kFrameSamples) != 0) {
        LVA_LOGW(kTag,
                 "SuppressSendspinResidual: n=%zu not multiple of %d",
                 n, kFrameSamples);
        return;
    }

    for (std::size_t off = 0; off < n; off += kFrameSamples) {
        double sum_m2 = 0.0;
        double sum_r2 = 0.0;
        double sum_mr = 0.0;

        for (std::size_t i = 0; i < kFrameSamples; ++i) {
            const double m = static_cast<double>(mic[off + i]);
            const double r = static_cast<double>(ref[off + i]);

            sum_m2 += m * m;
            sum_r2 += r * r;
            sum_mr += m * r;
        }

        const double ref_rms =
            std::sqrt(sum_r2 / static_cast<double>(kFrameSamples));

        double corr = 0.0;
        double raw_coeff = 0.0;
        double applied_coeff = 0.0;

        if (ref_rms >= kResidualMinRefRms &&
            sum_m2 > 1.0 &&
            sum_r2 > 1.0) {

            corr = sum_mr / std::sqrt(sum_m2 * sum_r2);

            if (std::abs(corr) >= kResidualMinAbsCorr) {
                raw_coeff = sum_mr / sum_r2;

                const double limited =
                    std::clamp(raw_coeff,
                               -kResidualMaxCoeff,
                                kResidualMaxCoeff);

                applied_coeff = limited * kResidualStrength;

                for (std::size_t i = 0; i < kFrameSamples; ++i) {
                    const double cleaned =
                        static_cast<double>(mic[off + i]) -
                        applied_coeff *
                            static_cast<double>(ref[off + i]);

                    const long sample =
                        std::lround(std::clamp(cleaned,
                                             -32768.0,
                                              32767.0));

                    mic[off + i] =
                        static_cast<std::int16_t>(sample);
                }

            }
        }
    }
}

bool WebRtcProcessor::GetLinearAecOutput(std::int16_t* buf,
                                             std::size_t n) {
    std::lock_guard<std::mutex> lk(apm_mutex_);

    if (buf == nullptr ||
        n != static_cast<std::size_t>(kFrameSamples) ||
        apm_ == nullptr ||
        !echo_on_applied_) {
        return false;
    }

    auto* apm = static_cast<::webrtc::AudioProcessing*>(apm_);

    std::array<std::array<float, kFrameSamples>, 1> linear{};

    auto view =
        rtc::ArrayView<std::array<float, kFrameSamples>>(
            linear.data(), linear.size());

    if (!apm->GetLinearAecOutput(view)) {
        return false;
    }

    // FloatS16 scale, not [-1, 1].
    for (std::size_t i = 0; i < n; ++i) {
        const float sample =
            std::clamp(linear[0][i], -32768.0f, 32767.0f);

        buf[i] = static_cast<std::int16_t>(std::lround(sample));
    }

    return true;
}

void WebRtcProcessor::ResetEcho() {
    std::lock_guard<std::mutex> lk(apm_mutex_);
    if (!aec_enabled_) return;
#ifdef LVA_DIAG_LOG
    const auto now = std::chrono::steady_clock::now();
    const long since_last_ms =
        last_reset_.time_since_epoch().count() == 0
            ? -1
            : static_cast<long>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - last_reset_).count());
    last_reset_ = now;
    const long rss_before = lva::memprobe::ReadRssAnonKb();
    LVA_LOGW(kTag,
             "resetting echo canceller (post-xrun re-converge) "
             "rebuilds=%u since_last=%ld ms RssAnon=%ld kB",
             rebuild_count_, since_last_ms, rss_before);
    RebuildLocked();
    LVA_LOGI(kTag, "echo canceller rebuilt: RssAnon %ld -> %ld kB",
             rss_before, lva::memprobe::ReadRssAnonKb());
#else
    LVA_LOGW(kTag, "resetting echo canceller (post-xrun re-converge)");
    RebuildLocked();
#endif
}

bool WebRtcProcessor::Process(std::int16_t* buf, std::size_t n) {
    std::lock_guard<std::mutex> lk(apm_mutex_);
    if (apm_ == nullptr) return false;
    if (n == 0) return true;
    if ((n % kFrameSamples) != 0) {
        LVA_LOGE(kTag, "Process: n=%zu not a multiple of %d",
                 n, kFrameSamples);
        return false;
    }

    ReconcileGateLocked();

    if (!echo_on_applied_ && agc_level_ == 0 && ns_level_ == 0) {
        return true;
    }
    auto* apm = static_cast<::webrtc::AudioProcessing*>(apm_);
    if (echo_on_applied_) {
        // mic and ref come from the same ALSA period.
        apm->set_stream_delay_ms(0);
    }
    for (std::size_t off = 0; off < n; off += kFrameSamples) {
        const int err = apm->ProcessStream(
            buf + off, *stream_cfg_, *stream_cfg_, buf + off);
        if (err != ::webrtc::AudioProcessing::kNoError) {
            LVA_LOGE(kTag, "ProcessStream failed: %d", err);
            return false;
        }
    }

    return true;
}

}  // namespace lva::audio
