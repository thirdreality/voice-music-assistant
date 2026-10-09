#include "tr/MusicAssistantControl.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "util/Log.h"

namespace lva::tr {

namespace {

constexpr const char* kTag = "music-assistant";

constexpr const char* kApiUrlFile =
    "/data/conf/music-assistant-url";
constexpr const char* kPlayerIdFile =
    "/data/conf/music-assistant-player-id";
constexpr const char* kTokenFile =
    "/data/conf/music-assistant-token";

constexpr long kConnectTimeoutMs = 1500;
constexpr long kRequestTimeoutMs = 2500;
constexpr std::size_t kMaxResponseBytes = 64 * 1024;

constexpr std::uint64_t kSendspinObservedActiveBit = 1ULL;
std::atomic<std::uint64_t> g_sendspin_observed_state{0};

struct MusicAssistantConfig {
    std::string api_url;
    std::string player_id;
    std::string token;
};

struct ApiResult {
    bool        ok = false;
    bool        timed_out = false;
    bool        request_body_sent = false;
    long        http_status = 0;
    std::string body;
};

struct PlayerSnapshot {
    std::string playback_state;
    std::string active_source;
    std::string queue_item_id;
    double      media_elapsed_time = -1.0;
};

struct MusicAssistantPauseLease {
    std::string   active_source;
    std::string   queue_item_id;
    double        media_elapsed_time = -1.0;
    std::uint64_t sendspin_transition_generation = 0;
};

enum class MusicAssistantResumeResult {
    Complete,
    Pending,
    RetryableFailure,
};

enum class PlayerCommandResult {
    Success,
    TimeoutPending,
    Failure,
};

std::mutex g_control_mutex;
bool g_voice_active = false;
bool g_worker_running = false;
std::optional<MusicAssistantPauseLease> g_pause_lease;

std::uint64_t SendspinObservedGeneration(std::uint64_t state) {
    return state >> 1;
}

bool SendspinObservedActive(std::uint64_t state) {
    return (state & kSendspinObservedActiveBit) != 0;
}

std::optional<std::string> ReadConfigValue(
    const char* path,
    const char* label) {

    std::ifstream in(path);
    if (!in) {
        LVA_LOGW(kTag, "%s file unavailable: %s", label, path);
        return std::nullopt;
    }

    std::string value((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());

    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }

    auto first = std::find_if_not(
        value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c); });
    value.erase(value.begin(), first);

    if (value.empty()) {
        LVA_LOGW(kTag, "%s file is empty: %s", label, path);
        return std::nullopt;
    }

    return value;
}

std::optional<MusicAssistantConfig> LoadConfig() {
    const auto api_url =
        ReadConfigValue(kApiUrlFile, "API URL");
    const auto player_id =
        ReadConfigValue(kPlayerIdFile, "player ID");
    const auto token =
        ReadConfigValue(kTokenFile, "token");

    if (!api_url || !player_id || !token) {
        return std::nullopt;
    }

    MusicAssistantConfig config;
    config.api_url = *api_url;
    config.player_id = *player_id;
    config.token = *token;
    return config;
}

std::size_t WriteToString(char* ptr,
                          std::size_t size,
                          std::size_t nmemb,
                          void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const std::size_t n = size * nmemb;

    if (out->size() + n > kMaxResponseBytes) {
        return 0;
    }

    out->append(ptr, n);
    return n;
}

ApiResult PostApi(const MusicAssistantConfig& config,
                  const nlohmann::json& request) {
    ApiResult result;

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        LVA_LOGW(kTag, "curl_easy_init failed");
        return result;
    }

    const std::string body = request.dump();
    const std::string auth =
        "Authorization: Bearer " + config.token;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(
        headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, config.api_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(
        curl, CURLOPT_POSTFIELDSIZE,
        static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(
        curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(
        curl, CURLOPT_TIMEOUT_MS, kRequestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(
        curl, CURLINFO_RESPONSE_CODE, &result.http_status);

    curl_off_t uploaded = 0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD_T, &uploaded);
    result.request_body_sent =
        uploaded >= static_cast<curl_off_t>(body.size());

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc == CURLE_OPERATION_TIMEDOUT) {
        result.timed_out = true;

        if (result.request_body_sent) {
            LVA_LOGW(
                kTag,
                "Music Assistant request timed out after full request "
                "body was sent; server-side outcome unknown");
        } else {
            LVA_LOGW(
                kTag,
                "Music Assistant request timed out before full request "
                "body was sent");
        }

        return result;
    }

    if (rc != CURLE_OK) {
        LVA_LOGW(
            kTag,
            "Music Assistant request failed: %s",
            curl_easy_strerror(rc));
        return result;
    }

    if (result.http_status != 200) {
        LVA_LOGW(
            kTag,
            "Music Assistant returned HTTP %ld",
            result.http_status);
        return result;
    }

    result.ok = true;
    return result;
}

std::optional<PlayerSnapshot> GetPlayerSnapshot(
    const MusicAssistantConfig& config) {

    const nlohmann::json request = {
        {"command", "players/get"},
        {"args", {
            {"player_id", config.player_id},
            {"raise_unavailable", false},
        }},
    };

    const ApiResult result = PostApi(config, request);
    if (!result.ok) {
        return std::nullopt;
    }

    const auto data =
        nlohmann::json::parse(result.body, nullptr, false);

    if (data.is_discarded() || !data.is_object()) {
        LVA_LOGW(kTag, "invalid players/get response");
        return std::nullopt;
    }

    PlayerSnapshot snapshot;
    snapshot.playback_state =
        data.value(
            "playback_state",
            data.value("state", std::string{}));
    snapshot.active_source =
        data.value("active_source", std::string{});

    const auto media = data.find("current_media");
    if (media != data.end() && media->is_object()) {
        snapshot.queue_item_id =
            media->value("queue_item_id", std::string{});

        const auto elapsed = media->find("elapsed_time");
        if (elapsed != media->end() &&
            elapsed->is_number()) {
            snapshot.media_elapsed_time =
                elapsed->get<double>();
        }
    }

    return snapshot;
}

PlayerCommandResult SendPlayerCommandDetailed(
    const MusicAssistantConfig& config,
    const char* command) {

    const nlohmann::json request = {
        {"command", command},
        {"args", {
            {"player_id", config.player_id},
        }},
    };

    const ApiResult result = PostApi(config, request);

    if (result.ok) {
        return PlayerCommandResult::Success;
    }

    // A fully transmitted PLAY may continue executing server-side even
    // when the HTTP response itself times out.
    if (result.timed_out && result.request_body_sent) {
        return PlayerCommandResult::TimeoutPending;
    }

    return PlayerCommandResult::Failure;
}

bool SendPlayerCommand(const MusicAssistantConfig& config,
                       const char* command) {
    return SendPlayerCommandDetailed(config, command) ==
           PlayerCommandResult::Success;
}

bool ResumeConfirmedByLocalSendspin(
    const MusicAssistantPauseLease& lease) {

    const std::uint64_t observed =
        g_sendspin_observed_state.load(
            std::memory_order_acquire);

    const std::uint64_t generation =
        SendspinObservedGeneration(observed);

    if (!SendspinObservedActive(observed) ||
        generation <=
            lease.sendspin_transition_generation) {
        return false;
    }

    LVA_LOGI(
        kTag,
        "voice resume confirmed by local Sendspin playback "
        "(generation=%llu pause_generation=%llu)",
        static_cast<unsigned long long>(generation),
        static_cast<unsigned long long>(
            lease.sendspin_transition_generation));

    return true;
}

std::optional<MusicAssistantPauseLease>
PauseIfPlayingSync() {
    const auto config = LoadConfig();
    if (!config) {
        return std::nullopt;
    }

    const auto snapshot = GetPlayerSnapshot(*config);
    if (!snapshot) {
        return std::nullopt;
    }

    if (snapshot->playback_state != "playing") {
        LVA_LOGD(
            kTag,
            "voice pause skipped: player state=%s",
            snapshot->playback_state.c_str());
        return std::nullopt;
    }

    if (!SendPlayerCommand(
            *config, "players/cmd/pause")) {
        LVA_LOGW(kTag, "voice pause command failed");
        return std::nullopt;
    }

    MusicAssistantPauseLease lease;
    lease.active_source =
        snapshot->active_source;
    lease.queue_item_id =
        snapshot->queue_item_id;
    lease.media_elapsed_time =
        snapshot->media_elapsed_time;

    const std::uint64_t observed =
        g_sendspin_observed_state.load(
            std::memory_order_acquire);

    lease.sendspin_transition_generation =
        SendspinObservedGeneration(observed);

    LVA_LOGI(
        kTag,
        "paused Music Assistant for voice interaction "
        "(source=%s queue_item=%s)",
        lease.active_source.empty()
            ? "(none)"
            : lease.active_source.c_str(),
        lease.queue_item_id.empty()
            ? "(none)"
            : lease.queue_item_id.c_str());

    return lease;
}

MusicAssistantResumeResult ResumeIfStillPausedSync(
    const MusicAssistantPauseLease& lease) {

    if (ResumeConfirmedByLocalSendspin(lease)) {
        return MusicAssistantResumeResult::Complete;
    }

    const auto config = LoadConfig();
    if (!config) {
        return MusicAssistantResumeResult::RetryableFailure;
    }

    const auto snapshot = GetPlayerSnapshot(*config);
    if (!snapshot) {
        return MusicAssistantResumeResult::RetryableFailure;
    }

    // The Universal Player may report idle after the speaker is paused.
    if (snapshot->playback_state != "paused" &&
        snapshot->playback_state != "idle") {
        LVA_LOGI(
            kTag,
            "voice resume skipped: current state=%s",
            snapshot->playback_state.c_str());
        return MusicAssistantResumeResult::Complete;
    }

    if (!lease.active_source.empty() &&
        snapshot->active_source != lease.active_source) {
        LVA_LOGI(
            kTag,
            "voice resume skipped: active source changed");
        return MusicAssistantResumeResult::Complete;
    }

    if (!lease.queue_item_id.empty() &&
        snapshot->queue_item_id != lease.queue_item_id) {
        LVA_LOGI(
            kTag,
            "voice resume skipped: queue item changed");
        return MusicAssistantResumeResult::Complete;
    }

    if (ResumeConfirmedByLocalSendspin(lease)) {
        return MusicAssistantResumeResult::Complete;
    }

    const PlayerCommandResult command_result =
        SendPlayerCommandDetailed(
            *config, "players/cmd/play");

    if (command_result ==
        PlayerCommandResult::TimeoutPending) {
        LVA_LOGW(
            kTag,
            "voice resume PLAY timed out after full request body "
            "was sent; server-side command remains pending");
        return MusicAssistantResumeResult::Pending;
    }

    if (command_result !=
        PlayerCommandResult::Success) {
        LVA_LOGW(kTag, "voice resume command failed");
        return MusicAssistantResumeResult::RetryableFailure;
    }

    LVA_LOGI(
        kTag,
        "resumed Music Assistant after voice interaction");
    return MusicAssistantResumeResult::Complete;
}

void ResumeLeaseWithPolicy(
    const MusicAssistantPauseLease& lease) {

    constexpr int kFailureRetryAttempts = 2;
    constexpr auto kFailureRetryDelay =
        std::chrono::milliseconds(3000);
    constexpr auto kPendingConfirmWindow =
        std::chrono::milliseconds(22000);
    constexpr auto kLocalConfirmPoll =
        std::chrono::milliseconds(25);

    for (int attempt = 1;
         attempt <= kFailureRetryAttempts;
         ++attempt) {

        const auto result =
            ResumeIfStillPausedSync(lease);

        if (result ==
            MusicAssistantResumeResult::Complete) {
            if (attempt > 1) {
                LVA_LOGI(
                    kTag,
                    "Music Assistant resume recovered on "
                    "genuine-failure retry %d",
                    attempt);
            }
            return;
        }

        if (result ==
            MusicAssistantResumeResult::Pending) {

            LVA_LOGW(
                kTag,
                "PLAY outcome pending; duplicate PLAY suppressed; "
                "waiting up to %lld ms for local Sendspin confirmation",
                static_cast<long long>(
                    kPendingConfirmWindow.count()));

            const auto deadline =
                std::chrono::steady_clock::now() +
                kPendingConfirmWindow;

            while (std::chrono::steady_clock::now() <
                   deadline) {

                if (ResumeConfirmedByLocalSendspin(lease)) {
                    LVA_LOGI(
                        kTag,
                        "pending PLAY confirmed by local Sendspin; "
                        "no duplicate PLAY was issued");
                    return;
                }

                std::this_thread::sleep_for(
                    kLocalConfirmPoll);
            }

            LVA_LOGW(
                kTag,
                "pending PLAY confirmation window expired; "
                "duplicate PLAY remained suppressed");
            return;
        }

        if (ResumeConfirmedByLocalSendspin(lease)) {
            LVA_LOGI(
                kTag,
                "Music Assistant resume recovered locally "
                "after genuine failure");
            return;
        }

        if (attempt < kFailureRetryAttempts) {
            LVA_LOGW(
                kTag,
                "resume failed before a pending PLAY was "
                "established; retrying verification in %lld ms",
                static_cast<long long>(
                    kFailureRetryDelay.count()));

            std::this_thread::sleep_for(
                kFailureRetryDelay);
        }
    }

    LVA_LOGW(
        kTag,
        "Music Assistant resume failed after %d "
        "genuine-failure attempts; giving up",
        kFailureRetryAttempts);
}

void WorkerLoop();

void EnsureWorkerLocked() {
    if (g_worker_running) {
        return;
    }

    g_worker_running = true;

    std::thread([] {
        WorkerLoop();
    }).detach();
}

void WorkerLoop() {
    for (;;) {
        bool want_voice_active = false;
        std::optional<MusicAssistantPauseLease>
            lease_to_resume;

        {
            std::lock_guard<std::mutex>
                lock(g_control_mutex);

            want_voice_active = g_voice_active;

            if (want_voice_active) {
                if (g_pause_lease) {
                    g_worker_running = false;
                    return;
                }
            } else {
                if (!g_pause_lease) {
                    g_worker_running = false;
                    return;
                }

                lease_to_resume =
                    std::move(g_pause_lease);
                g_pause_lease.reset();
            }
        }

        if (lease_to_resume) {
            ResumeLeaseWithPolicy(*lease_to_resume);
            continue;
        }

        // Voice became active and there is no lease yet. Perform the
        // potentially blocking query/pause entirely on this worker.
        auto new_lease = PauseIfPlayingSync();

        if (!new_lease) {
            std::lock_guard<std::mutex>
                lock(g_control_mutex);

            // If the desired state changed while the HTTP request was
            // running, loop once more to reconcile it.
            if (!g_voice_active) {
                continue;
            }

            g_worker_running = false;
            return;
        }

        bool restore_immediately = false;

        {
            std::lock_guard<std::mutex>
                lock(g_control_mutex);

            if (g_voice_active && !g_pause_lease) {
                g_pause_lease = *new_lease;
            } else {
                // The voice interaction ended while PAUSE was in flight,
                // or another lease already won the race.
                restore_immediately = true;
            }
        }

        if (restore_immediately) {
            ResumeLeaseWithPolicy(*new_lease);
            continue;
        }

        // The pause lease is installed. No further work is needed until
        // ResumeMusicForVoiceAsync() changes the desired state.
        std::lock_guard<std::mutex> lock(g_control_mutex);
        g_worker_running = false;
        return;
    }
}

}  // namespace

void MusicAssistantObserveSendspinPlayback(bool active) {
    std::uint64_t observed =
        g_sendspin_observed_state.load(
            std::memory_order_acquire);

    while (SendspinObservedActive(observed) != active) {
        const std::uint64_t generation =
            SendspinObservedGeneration(observed) + 1;

        const std::uint64_t desired =
            (generation << 1) |
            (active ? kSendspinObservedActiveBit : 0ULL);

        if (g_sendspin_observed_state.compare_exchange_weak(
                observed,
                desired,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {

            LVA_LOGD(
                kTag,
                "Sendspin observer transition: "
                "active=%d generation=%llu",
                active ? 1 : 0,
                static_cast<unsigned long long>(
                    generation));
            return;
        }
    }
}

void MusicAssistantPauseForVoiceAsync() {
    std::lock_guard<std::mutex> lock(g_control_mutex);
    g_voice_active = true;
    EnsureWorkerLocked();
}

void MusicAssistantResumeForVoiceAsync() {
    std::lock_guard<std::mutex> lock(g_control_mutex);
    g_voice_active = false;
    EnsureWorkerLocked();
}

}  // namespace lva::tr
