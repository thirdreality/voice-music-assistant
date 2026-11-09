#include <cctype>
#include <chrono>
#include <vector>

#include "app/Application.h"
#include "app/Signals.h"
#include "audio/MicroWakeWord.h"
#include "audio/OpenWakeWord.h"
#include "audio/OpenWakeWordFeatures.h"
#include "audio/PcmRingBuffer.h"
#include "audio/WakeWordEngine.h"
#include "audio/WebRtcProcessor.h"
#include "satellite/Satellite.h"
#include "util/Log.h"

namespace lva::app {
namespace {
constexpr const char* kTag = "main";
}

void Application::LoadWakeModels() {
    wakeword_engine = std::make_unique<lva::audio::WakeWordEngine>(*wake_ring);

    {
        std::vector<std::string> ids;
        for (const auto& slot : state.preferences.active_wake_words) {
            if (slot.has_value() && !slot->empty()) ids.push_back(*slot);
        }
        if (ids.empty()) {
            const std::string& list = cli.wakeword_models;
            std::size_t start = 0;
            while (start < list.size()) {
                std::size_t comma = list.find(',', start);
                if (comma == std::string::npos) comma = list.size();
                std::string id(list, start, comma - start);
                while (!id.empty() && std::isspace(static_cast<unsigned char>(id.front())))
                    id.erase(0, 1);
                while (!id.empty() && std::isspace(static_cast<unsigned char>(id.back())))
                    id.pop_back();
                if (!id.empty()) ids.push_back(std::move(id));
                start = comma + 1;
            }
            LVA_LOGI(kTag, "no persisted wake words; using CLI default '%s'",
                     cli.wakeword_models.c_str());
        } else {
            LVA_LOGI(kTag, "loading %zu persisted wake word(s)", ids.size());
        }

        for (const auto& id : ids) {
            const auto json_path = wakeword_dir / (id + ".json");
            if (use_openwakeword) {
                auto model = lva::audio::OpenWakeWord::FromConfig(json_path);
                if (model && model->Ok()) {
                    wakeword_engine->AddOpenModel(std::move(model));
                } else {
                    LVA_LOGW(kTag, "failed to load OWW '%s'", id.c_str());
                }
            } else {
                auto model = lva::audio::MicroWakeWord::FromConfig(json_path);
                if (model && model->Ok()) {
                    model->SetSourceTag("post_apm");
                    wakeword_engine->AddModel(std::move(model));
                } else {
                    LVA_LOGW(kTag, "failed to load wake word '%s'", id.c_str());
                }
            }
        }
    }

    if (use_openwakeword) {
        auto oww_feat = std::make_unique<lva::audio::OpenWakeWordFeatures>();
        if (oww_feat->Load(wakeword_dir)) {
            wakeword_engine->SetOpenFeatures(std::move(oww_feat));
        } else {
            LVA_LOGE(kTag, "OpenWakeWord features failed to load");
        }
    }

    if (!use_openwakeword) {
        // The stop model only exists for microWakeWord.
        const std::filesystem::path micro_dir = "/usr/share/thirdreality/wakewords/microwakeword";
        const auto stop_json = micro_dir / "stop.json";
        auto stop_model = lva::audio::MicroWakeWord::FromConfig(stop_json);
        if (stop_model && stop_model->Ok()) {
            wakeword_engine->AddModel(std::move(stop_model));
        } else {
            LVA_LOGW(kTag, "stop word model not loaded");
        }
    }

    // Models load with their built-in cutoff; apply the saved ones. Slot
    // order must match PersistWakeWordSensitivity(): micro, then open.
    {
        std::size_t wake_idx = 0;
        const auto micro_models = wakeword_engine->SnapshotModels();
        for (const auto& m : micro_models) {
            if (m->config().id == "stop") {
                if (state.preferences.stop_word_sensitivity.has_value()) {
                    m->SetProbabilityCutoff(
                        static_cast<float>(*state.preferences.stop_word_sensitivity));
                }
                continue;
            }
            const auto& slot = (wake_idx == 0) ? state.preferences.wake_word_1_sensitivity
                                               : state.preferences.wake_word_2_sensitivity;
            if (wake_idx < 2 && slot.has_value()) {
                m->SetProbabilityCutoff(static_cast<float>(*slot));
            }
            ++wake_idx;
        }
        const auto open_models = wakeword_engine->SnapshotOpenModels();
        for (const auto& m : open_models) {
            const auto& slot = (wake_idx == 0) ? state.preferences.wake_word_1_sensitivity
                                               : state.preferences.wake_word_2_sensitivity;
            if (wake_idx < 2 && slot.has_value()) {
                m->SetProbabilityCutoff(static_cast<float>(*slot));
            }
            ++wake_idx;
        }
    }

    // First-boot defaults, only for slots with a loaded model.
    {
        const double wake_default =
            use_openwakeword ? kDefaultOpenWakeSensitivity : kDefaultMicroWakeSensitivity;
        std::size_t loaded_wake_words = wakeword_engine->SnapshotOpenModels().size();
        for (const auto& m : wakeword_engine->SnapshotModels()) {
            if (m->config().id != "stop") ++loaded_wake_words;
        }

        if (loaded_wake_words >= 1 && !state.preferences.wake_word_1_sensitivity.has_value()) {
            state.preferences.wake_word_1_sensitivity = wake_default;
            prefs_dirty = true;
        }
        if (loaded_wake_words >= 2 && !state.preferences.wake_word_2_sensitivity.has_value()) {
            state.preferences.wake_word_2_sensitivity = wake_default;
            prefs_dirty = true;
        }
        if (!use_openwakeword && !state.preferences.stop_word_sensitivity.has_value()) {
            state.preferences.stop_word_sensitivity = kDefaultStopSensitivity;
            prefs_dirty = true;
        }
        if (!state.preferences.continue_conversation_delay.has_value()) {
            state.preferences.continue_conversation_delay = cli.continue_conversation_delay;
            prefs_dirty = true;
        }
    }

    if (prefs_dirty) {
        const bool queued =
            preferences_corrupt ? state.RequestPreferencesRecovery() : state.SavePreferences(true);
        if (queued) {
            LVA_LOGI(kTag, "queued complete config for %s", state.preferences_path.c_str());
        } else {
            LVA_LOGW(kTag, "failed to persist first-boot config to %s",
                     state.preferences_path.c_str());
        }
    }
}

void Application::StartWakeDetection() {
    wakeword_engine->SetDetectionCallback([this](const std::string& model_id, float prob) {
        if (model_id == "stop") {
            LVA_LOGD("wake", "stop word detected prob=%.3f", prob);
            satellite->OnStopDetected();
            return;
        }

        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();

        constexpr std::int64_t kWakeDedupNs = 1'000'000'000LL;

        auto previous = last_wake_dispatch_ns.load(std::memory_order_relaxed);

        while (true) {
            if (previous != 0 && now_ns - previous < kWakeDedupNs) {
                LVA_LOGD("wake",
                         "duplicate wake suppressed source=post_apm "
                         "id=%s prob=%.3f",
                         model_id.c_str(), prob);
                return;
            }

            if (last_wake_dispatch_ns.compare_exchange_weak(
                    previous, now_ns, std::memory_order_relaxed, std::memory_order_relaxed)) {
                break;
            }
        }

        LVA_LOGD("wake",
                 "wake candidate source=post_apm "
                 "id=%s prob=%.3f",
                 model_id.c_str(), prob);

        satellite->OnWakeDetected(model_id, prob);
    });

    linear_wakeword_engine->SetDetectionCallback([this](const std::string& model_id, float prob) {
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();

        if (model_id == "stop") {
            const auto guard_until_ns =
                webrtc_processor != nullptr ? webrtc_processor->LinearGuardUntilNs() : 0;

            if (guard_until_ns != 0 && now_ns < guard_until_ns) {
                const auto remaining_ms = (guard_until_ns - now_ns) / 1'000'000LL;

                LVA_LOGD("wake",
                         "linear stop guard suppressed "
                         "prob=%.3f remaining_ms=%lld",
                         prob, static_cast<long long>(remaining_ms));
                return;
            }

            LVA_LOGD("wake", "stop word detected source=linear_aec prob=%.3f", prob);
            satellite->OnStopDetected();
            return;
        }

        const auto guard_until_ns =
            webrtc_processor != nullptr ? webrtc_processor->LinearGuardUntilNs() : 0;

        if (guard_until_ns != 0 && now_ns < guard_until_ns) {
            const auto remaining_ms = (guard_until_ns - now_ns) / 1'000'000LL;

            LVA_LOGD("wake",
                     "linear resume guard suppressed "
                     "id=%s prob=%.3f remaining_ms=%lld",
                     model_id.c_str(), prob, static_cast<long long>(remaining_ms));

            return;
        }

        constexpr std::int64_t kWakeDedupNs = 1'000'000'000LL;

        auto previous = last_wake_dispatch_ns.load(std::memory_order_relaxed);

        while (true) {
            if (previous != 0 && now_ns - previous < kWakeDedupNs) {
                LVA_LOGD("wake",
                         "duplicate wake suppressed source=linear_aec "
                         "id=%s prob=%.3f",
                         model_id.c_str(), prob);
                return;
            }

            if (last_wake_dispatch_ns.compare_exchange_weak(
                    previous, now_ns, std::memory_order_relaxed, std::memory_order_relaxed)) {
                break;
            }
        }

        LVA_LOGD("wake",
                 "wake candidate source=linear_aec "
                 "id=%s prob=%.3f",
                 model_id.c_str(), prob);

        satellite->OnWakeDetected(model_id, prob);
    });

    SetSignalWakeEngine(wakeword_engine.get());

    if (!wakeword_engine->Start()) {
        LVA_LOGW(kTag,
                 "wake-word engine failed to start; voice "
                 "pipeline disabled");
    }

    if (!use_openwakeword && !linear_wakeword_engine->SnapshotModels().empty()) {
        if (!linear_wakeword_engine->Start()) {
            LVA_LOGW(kTag, "linear-AEC shadow wake engine failed to start");
        } else {
            LVA_LOGI(kTag,
                     "dual-path wake active "
                     "(primary=post_apm secondary=linear_aec)");

            LVA_LOGI(kTag, "wake guards active: linear_resume=1500 ms stop_after_wake=1000 ms");
        }
    }
}

}  // namespace lva::app
