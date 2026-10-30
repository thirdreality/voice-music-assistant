
#include "entities/UpdateEntity.h"

#include <chrono>
#include <thread>
#include <stdexcept>
#include <utility>
#include <memory>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "state/ServerState.h"
#include "tr/OtaClient.h"
#include "tr/Supervisor.h"
#include "tr/SysInfo.h"
#include "util/Log.h"

namespace lva::entities {

namespace {

constexpr const char* kTag = "update";

constexpr const char* kCheckVersionUrl =
    "https://ota.cloud.3reality.com/reality/ota/client/checkVersion";
constexpr const char* kCaCertPath = "/etc/ssl/certs/ca-certificates.crt";
constexpr const char* kReleaseUrlBase =
    "https://github.com/thirdreality/voice-music-assistant/releases";
constexpr const char* kReleaseTagBase =
    "https://github.com/thirdreality/voice-music-assistant/releases/tag/";

std::string CloudVersionToTag(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    std::size_t start = 0;
    for (std::size_t i = 0; i <= v.size(); ++i) {
        if (i == v.size() || v[i] == '.') {
            std::size_t s = start;
            while (s + 1 < i && v[s] == '0') ++s;
            out.append(v, s, i - s);
            if (i < v.size()) out.push_back('.');
            start = i + 1;
        }
    }
    return out;
}

std::size_t WriteToString(char* ptr, std::size_t size, std::size_t nmemb,
                          void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    const std::size_t n = size * nmemb;
    constexpr std::size_t kMaxCheckResponse = 64 * 1024;
    if (s->size() + n > kMaxCheckResponse) {
        return 0;  // signals error to curl
    }
    s->append(ptr, n);
    return n;
}

std::string CheckVersionHttp(const std::string& body, lva::tr::NetworkRequest& access) {
    if (access.Cancelled()) return {};
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        throw std::runtime_error("curl_easy_init failed");
    }
    access.Configure(curl);
    std::string response;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            kCheckVersionUrl);
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  &WriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        20L);
    curl_easy_setopt(curl, CURLOPT_CAINFO,         kCaCertPath);

    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("curl: ") +
                                 curl_easy_strerror(rc));
    }
    if (http_code != 200) {
        throw std::runtime_error("HTTP " + std::to_string(http_code));
    }
    return response;
}

}  // namespace

void UpdateEntity::ScheduleCheck(lva::tr::RequestSource source) {
    if (check_.valid() || installing_) return;
    if (source != lva::tr::RequestSource::Explicit && !state_.network_policy.Allowed()) return;
    const auto dev = lva::tr::ReadDeviceInfo();
    if (dev.model_id.empty() || dev.serial_number().empty()) return;
    current_version_ = state_.version;
    const std::string body = nlohmann::json{
        {"modelId", dev.model_id}, {"version", current_version_},
        {"sno", dev.serial_number()}}.dump();
    check_source_ = source;
    cancel_.store(false, std::memory_order_release);
    auto access = std::make_shared<lva::tr::NetworkRequest>(state_.network_policy, source, &cancel_);
    LVA_LOGI(kTag, "%s OTA check: %s (model/version/serial)",
             source == lva::tr::RequestSource::Explicit ? "explicit" : "automatic", kCheckVersionUrl);
    try {
        check_ = std::async(std::launch::async, [access, body] {
            return CheckVersionHttp(body, *access);
        });
    } catch (const std::exception& e) {
        LVA_LOGW(kTag, "could not start OTA check: %s", e.what());
    }
}

void UpdateEntity::ScheduleInstall() {
    if (!state_.supervisor || check_.valid() || installing_) return;
    if (download_url_.empty() || expected_md5_.empty()) {
        ScheduleCheck(lva::tr::RequestSource::Explicit);
        return;
    }
    lva::tr::OtaRelease release;
    release.url = download_url_;
    release.version = latest_version_;
    release.expected_md5 = expected_md5_;
    release.source = lva::tr::RequestSource::Explicit;
    installing_ = !state_.supervisor->StartOtaUpdateAsync(release).empty();
    in_progress_ = installing_;
    BroadcastState();
}

void UpdateEntity::Poll() {
    if (check_.valid() && check_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            const auto body = check_.get();
            if (!body.empty()) ApplyCheckResult(body);
        } catch (const std::exception& e) {
            LVA_LOGW(kTag, "check_version failed: %s", e.what());
        }
    }
    if (installing_) {
        const bool running = state_.supervisor->IsOtaRunning();
        const auto status = state_.supervisor->GetOtaState();
        const bool changed = progress_ != status.progress || release_summary_ != status.message || !running;
        has_progress_ = true;
        progress_ = status.progress;
        release_summary_ = status.message;
        in_progress_ = running;
        if (!running) {
            installing_ = false;
            if (status.status != "failed") current_version_ = latest_version_;
        }
        if (changed) BroadcastState();
    }
}

void UpdateEntity::ApplyCheckResult(const std::string& body) {
    using nlohmann::json;
    json resp = json::parse(body, nullptr, false);
    if (resp.is_discarded() || !resp.is_object()) {
        LVA_LOGW(kTag, "OTA response not valid JSON");
        return;
    }
    if (!resp.contains("result") || !resp["result"].is_object()) {
        LVA_LOGW(kTag, "OTA response missing result");
        return;
    }
    const auto& result = resp["result"];
    const auto& data   = result.value("data", json::object());

    bool has_update = data.value("hasNewVersion", false);
    std::string latest = data.value("displayVersion", std::string());
    if (latest.empty()) latest = data.value("version", std::string());
    if (latest.empty()) latest = current_version_;

    expected_md5_ = data.value("md5", std::string());
    download_url_ = data.value("binUrl", std::string());
    if (download_url_.empty()) {
        download_url_ = data.value("altBinUrl", std::string());
    }

    latest_version_ = has_update ? latest : current_version_;
    if (has_update) {
        release_summary_ = "New firmware available";
    } else {
        release_summary_ = "";   // empty hides the line in HA's card
    }
    if (has_update && !latest_version_.empty()) {
        release_url_ = std::string(kReleaseTagBase) + "v" +
                       CloudVersionToTag(latest_version_);
    } else {
        release_url_ = kReleaseUrlBase;
    }

    LVA_LOGI(kTag, "current=%s latest=%s has_update=%d",
             current_version_.c_str(), latest_version_.c_str(),
             has_update ? 1 : 0);
    BroadcastState();
}

}  // namespace lva::entities
