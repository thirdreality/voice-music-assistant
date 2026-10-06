#include "audio/WebRtcProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "modules/audio_processing/include/audio_processing.h"
#include "rtc_base/ref_count.h"

#include "util/Log.h"

namespace lva::audio {

namespace {

constexpr const char* kTag = "webrtc";

// Post-AEC residual suppression is deliberately conservative.
//
// The reference channel is normally much hotter than the microphone.
// We therefore use normalized correlation to decide whether a component
// really resembles the outgoing Sendspin audio instead of using a simple
// dB/noise gate.
//
// A 0.30 correlation threshold avoids acting on weak accidental matches.
// Only 70% of the estimated correlated component is removed, and the
// reference coefficient is hard-limited to 0.12.
constexpr double kResidualMinRefRms  = 120.0;
constexpr double kResidualMinAbsCorr = 0.30;
constexpr double kResidualStrength   = 0.70;
constexpr double kResidualMaxCoeff   = 0.12;

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

        // Ask WebRTC to retain the continuous linear AEC output
        // independently of the normal EchoRemover output path.
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
    // Keep the AEC path isolated from noise suppression:
    // Keep the configured noise suppressor during normal listening, but
    // disable only NS while far-end playback/AEC is active. This leaves
    // AEC3, HPF, AGC1, wake-word settings, and all other processing intact.
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
        LVA_LOGD(kTag, "echo canceller %s (far-end %s)",
                 echo_on_applied_ ? "enabled" : "disabled",
                 want_echo ? "active" : "idle");
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
    apm_ = apm;

    // Rebuild preserves the current far-end gate: the echo canceller is
    // only actually enabled when far-end playback is active. On a fresh
    // build (startup / level change) start from the desired gate state.
    const bool echo_on =
        far_end_active_.load(std::memory_order_relaxed) ||
        external_far_end_active_.load(std::memory_order_relaxed);
    ApplyConfigLocked(echo_on);

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
    // ProcessReverse runs before Process each period; reconcile the gate
    // here too so the reference and mic stages agree within a period.
    ReconcileGateLocked();
    // Skip reverse processing while the echo canceller is gated off:
    // feeding the far-end stream is pointless (and only churns AEC
    // state) when there is no local playback to cancel.
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

    // IMPORTANT: this extra suppression is ONLY for external Sendspin /
    // Music Assistant playback. Local MPV/TTS audio keeps the normal
    // WebRTC processing path unchanged.
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

    // WebRTC's linear AEC buffer uses FloatS16 scale, not normalized
    // [-1, 1]. Clamp/round directly back into signed 16-bit PCM.
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
    LVA_LOGW(kTag, "resetting echo canceller (post-xrun re-converge)");
    RebuildLocked();
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

    // Reconcile the far-end gate: enable the echo canceller only while
    // there is genuine local playback (music/TTS). Toggling it via
    // ApplyConfig avoids destroying near-end speech during plain
    // listening, where the hardware-loopback reference is just crosstalk.
    ReconcileGateLocked();

    if (!echo_on_applied_ && agc_level_ == 0 && ns_level_ == 0) {
        // Nothing enabled; cheap no-op.
        return true;
    }
    auto* apm = static_cast<::webrtc::AudioProcessing*>(apm_);
    if (echo_on_applied_) {
        // mic and ref are the same ALSA capture period → ~0 delay.
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
