#include "audio/ExternalWakeWord.h"

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include <curl/curl.h>

#include "util/Log.h"
#include "util/Sha256.h"

namespace lva::audio {

namespace {

constexpr const char* kTag = "wakeword";

// Replaces the last path segment of a URL; drops query and fragment.
std::string ReplaceUrlFilename(const std::string& url,
                               const std::string& filename) {
    std::size_t path_start = 0;
    const std::size_t scheme = url.find("://");
    if (scheme != std::string::npos) {
        path_start = url.find('/', scheme + 3);
        if (path_start == std::string::npos) {
            return url + "/" + filename;
        }
    }
    std::size_t end = url.find_first_of("?#", path_start);
    const std::string base =
        (end == std::string::npos) ? url : url.substr(0, end);

    const std::size_t last_slash = base.find_last_of('/');
    if (last_slash == std::string::npos || last_slash < path_start) {
        return base + "/" + filename;
    }
    return base.substr(0, last_slash + 1) + filename;
}

std::size_t WriteToFileCb(char* ptr, std::size_t size, std::size_t nmemb,
                          void* userdata) {
    auto* fp = static_cast<std::FILE*>(userdata);
    const std::size_t n = size * nmemb;
    if (n == 0) return 0;
    return std::fwrite(ptr, 1, n, fp);
}

bool DownloadToFile(const std::string& url,
                    const std::filesystem::path& dest, lva::tr::NetworkRequest& access) {
    if (access.Cancelled()) return false;
    std::FILE* fp = std::fopen(dest.c_str(), "wb");
    if (fp == nullptr) {
        LVA_LOGE(kTag, "cannot create %s", dest.c_str());
        return false;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        std::fclose(fp);
        ::unlink(dest.c_str());
        LVA_LOGE(kTag, "curl_easy_init failed");
        return false;
    }

    access.Configure(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteToFileCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CAINFO,
                     "/etc/ssl/certs/ca-certificates.crt");

    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    std::fclose(fp);

    if (rc != CURLE_OK) {
        LVA_LOGW(kTag, "download failed: %s (%s)", url.c_str(),
                 curl_easy_strerror(rc));
        ::unlink(dest.c_str());
        return false;
    }
    if (http_code != 200) {
        LVA_LOGW(kTag, "download failed: %s, status=%ld", url.c_str(),
                 http_code);
        ::unlink(dest.c_str());
        return false;
    }
    return true;
}

// Size/hash are optional in HA metadata. Check every supplied field for
// both cached and freshly downloaded files; never accept an empty file.
bool ModelMatches(const std::filesystem::path& path, const ExternalWakeWordInfo& info) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || (info.model_size != 0 && size != info.model_size)) return false;
    if (info.model_hash.empty()) return true;
    const auto actual = lva::util::Sha256FileHex(path);
    return !actual.empty() && actual == info.model_hash;
}

}  // namespace

std::filesystem::path DownloadExternalWakeWord(
    const ExternalWakeWordInfo& info,
    const std::filesystem::path& dest_dir,
    const lva::tr::NetworkPolicy& policy, const std::atomic<bool>* cancel) {
    lva::tr::NetworkRequest access(policy, lva::tr::RequestSource::LocalModel, cancel);
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    if (ec) {
        LVA_LOGE(kTag, "cannot create %s: %s", dest_dir.c_str(),
                 ec.message().c_str());
        return {};
    }

    const fs::path config_path = dest_dir / (info.id + ".json");
    const fs::path model_path  = dest_dir / (info.id + ".tflite");

    const bool config_missing = !fs::exists(config_path);

    const bool need_model = !ModelMatches(model_path, info);

    const fs::path config_part = config_path.string() + ".part";
    const fs::path model_part = model_path.string() + ".part";
    const bool fetch_config = config_missing || need_model;
    if (fetch_config) {
        LVA_LOGI(kTag, "downloading config %s -> %s", info.url.c_str(),
                 config_path.c_str());
        if (!DownloadToFile(info.url, config_part, access)) {
            return {};
        }
    }

    if (need_model) {
        const std::string model_url =
            ReplaceUrlFilename(info.url, model_path.filename().string());
        LVA_LOGI(kTag, "downloading model %s -> %s", model_url.c_str(),
                 model_path.c_str());
        if (!DownloadToFile(model_url, model_part, access)) {
            fs::remove(config_part, ec);
            return {};
        }
        if (!ModelMatches(model_part, info) || access.Cancelled()) {
            fs::remove(model_part, ec); fs::remove(config_part, ec);
            return {};
        }
        fs::rename(model_part, model_path, ec);
        if (ec) { fs::remove(config_part, ec); return {}; }
    }

    if (access.Cancelled()) { fs::remove(config_part, ec); return {}; }
    if (fetch_config) {
        fs::rename(config_part, config_path, ec);
        if (ec) return {};
    }
    return config_path;
}

}  // namespace lva::audio
