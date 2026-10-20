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
                    const std::filesystem::path& dest) {
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

}  // namespace

std::filesystem::path DownloadExternalWakeWord(
    const ExternalWakeWordInfo& info,
    const std::filesystem::path& dest_dir) {
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

    bool need_model = true;
    if (fs::exists(model_path)) {
        const auto cached_size = fs::file_size(model_path, ec);
        if (!ec && cached_size == info.model_size) {
            const std::string cached_hash = lva::util::Sha256FileHex(model_path);
            if (!cached_hash.empty() && cached_hash == info.model_hash) {
                need_model = false;
                LVA_LOGD(kTag,
                         "model size and hash match for %s; skipping download",
                         info.id.c_str());
            }
        }
    }

    if (config_missing || need_model) {
        LVA_LOGI(kTag, "downloading config %s -> %s", info.url.c_str(),
                 config_path.c_str());
        if (!DownloadToFile(info.url, config_path)) {
            return {};
        }
    }

    if (need_model) {
        const std::string model_url =
            ReplaceUrlFilename(info.url, model_path.filename().string());
        LVA_LOGI(kTag, "downloading model %s -> %s", model_url.c_str(),
                 model_path.c_str());
        if (!DownloadToFile(model_url, model_path)) {
            return {};
        }
    }

    return config_path;
}

}  // namespace lva::audio
