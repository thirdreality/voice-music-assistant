#include "tr/ModelDownloader.h"

#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>

#include <curl/curl.h>

#include "util/Log.h"
#include "util/Sha256.h"

namespace lva::tr {

namespace {

constexpr const char*    kTag           = "model_dl";
constexpr const char*    kCaCertPath    = "/etc/ssl/certs/ca-certificates.crt";
constexpr int            kMaxAttempts   = 5;
constexpr std::uintmax_t kSpaceMargin   = 8u << 20;  // keep /data usable

std::uintmax_t FileSizeOrZero(const std::filesystem::path& path) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(path, ec);
    return ec ? 0 : n;
}

struct DownloadCtx {
    CURL*                              curl   = nullptr;
    std::FILE*                         fp     = nullptr;
    std::uintmax_t                     offset = 0;  // bytes in the part file
    bool                               checked_status = false;
    const std::atomic<bool>*           cancel = nullptr;
    std::function<void(std::uintmax_t)> on_bytes;
};

std::size_t WriteCb(char* ptr, std::size_t size, std::size_t nmemb,
                    void* userdata) {
    auto* ctx = static_cast<DownloadCtx*>(userdata);
    if (!ctx->checked_status) {
        ctx->checked_status = true;
        long code = 0;
        curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &code);
        if (code == 200 && ctx->offset > 0) {
            // The server ignored the Range header; start over.
            LVA_LOGW(kTag, "server sent the full file; restarting at 0");
            if (::ftruncate(::fileno(ctx->fp), 0) != 0 ||
                std::fseek(ctx->fp, 0, SEEK_SET) != 0) {
                return 0;
            }
            ctx->offset = 0;
        }
    }
    return std::fwrite(ptr, 1, size * nmemb, ctx->fp);
}

int ProgressCb(void* userdata, curl_off_t /*dltotal*/, curl_off_t dlnow,
               curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<DownloadCtx*>(userdata);
    if (ctx->cancel->load(std::memory_order_relaxed)) return 1;
    ctx->on_bytes(ctx->offset + static_cast<std::uintmax_t>(dlnow));
    return 0;
}

}  // namespace

ModelDownloader::ModelDownloader(ModelSpec spec)
    : spec_(std::move(spec)),
      part_path_(spec_.path.string() + ".part") {
    status_.phase   = Installed() ? Phase::kReady : Phase::kMissing;
    status_.percent = status_.phase == Phase::kReady ? 100 : 0;
}

ModelDownloader::~ModelDownloader() { Cancel(); }

bool ModelDownloader::Installed() const {
    return FileSizeOrZero(spec_.path) == spec_.size && spec_.size > 0;
}

void ModelDownloader::Start() {
    if (running_.load(std::memory_order_acquire) || Installed()) return;
    if (worker_.joinable()) worker_.join();
    cancel_.store(false, std::memory_order_relaxed);
    // Set before the worker runs, so a caller never sees a stale kFailed.
    SetStatus(Phase::kDownloading,
              static_cast<int>(FileSizeOrZero(part_path_) * 100 / spec_.size));
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] {
        Run();
        running_.store(false, std::memory_order_release);
    });
}

void ModelDownloader::Cancel() {
    cancel_.store(true, std::memory_order_relaxed);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

ModelDownloader::Status ModelDownloader::GetStatus() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

void ModelDownloader::SetStatus(Phase phase, int percent, std::string error) {
    std::lock_guard<std::mutex> lock(mu_);
    status_.phase   = phase;
    status_.percent = percent;
    status_.error   = std::move(error);
}

void ModelDownloader::SetPercent(std::uintmax_t bytes) {
    const int percent = static_cast<int>(
        std::min<std::uintmax_t>(bytes * 100 / spec_.size, 99));
    std::lock_guard<std::mutex> lock(mu_);
    status_.percent = percent;
}

bool ModelDownloader::SleepUnlessCancelled(int seconds) {
    std::unique_lock<std::mutex> lock(mu_);
    return !cv_.wait_for(lock, std::chrono::seconds(seconds), [this] {
        return cancel_.load(std::memory_order_relaxed);
    });
}

void ModelDownloader::Run() {
    LVA_LOGI(kTag, "downloading %s -> %s", spec_.url.c_str(),
             spec_.path.c_str());
    std::string last_error = "download failed";
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        SetStatus(Phase::kDownloading,
                  static_cast<int>(FileSizeOrZero(part_path_) * 100 /
                                   spec_.size));
        Result r = DownloadOnce();
        if (r == Result::kOk) r = VerifyAndInstall();
        switch (r) {
            case Result::kOk:
                LVA_LOGI(kTag, "installed %s", spec_.path.c_str());
                SetStatus(Phase::kReady, 100);
                return;
            case Result::kCancelled:
                LVA_LOGI(kTag, "download cancelled");
                SetStatus(Phase::kMissing, 0);
                return;
            case Result::kFatal:
                return;  // status already set
            case Result::kRetry:
                last_error = GetStatus().error;
                break;
        }
        if (attempt == kMaxAttempts) break;
        const int backoff = 5 << (attempt - 1);  // 5, 10, 20, 40 s
        LVA_LOGW(kTag, "attempt %d/%d failed (%s); retry in %d s", attempt,
                 kMaxAttempts, last_error.c_str(), backoff);
        if (!SleepUnlessCancelled(backoff)) {
            SetStatus(Phase::kMissing, 0);
            return;
        }
    }
    LVA_LOGE(kTag, "giving up: %s", last_error.c_str());
    SetStatus(Phase::kFailed, 0, last_error);
}

ModelDownloader::Result ModelDownloader::DownloadOnce() {
    std::error_code ec;
    std::filesystem::create_directories(spec_.path.parent_path(), ec);
    if (ec) {
        SetStatus(Phase::kFailed, 0, "cannot create " +
                  spec_.path.parent_path().string());
        return Result::kFatal;
    }

    std::uintmax_t offset = FileSizeOrZero(part_path_);
    if (offset > spec_.size) {
        std::filesystem::remove(part_path_, ec);
        offset = 0;
    }
    if (offset == spec_.size) return Result::kOk;

    struct statvfs vfs{};
    if (::statvfs(spec_.path.parent_path().c_str(), &vfs) == 0) {
        const std::uintmax_t avail =
            static_cast<std::uintmax_t>(vfs.f_bavail) * vfs.f_frsize;
        const std::uintmax_t need = spec_.size - offset + kSpaceMargin;
        if (avail < need) {
            LVA_LOGE(kTag, "need %ju bytes free, have %ju", need, avail);
            SetStatus(Phase::kFailed, 0, "not enough free space");
            return Result::kFatal;
        }
    }

    DownloadCtx ctx;
    ctx.offset   = offset;
    ctx.cancel   = &cancel_;
    ctx.on_bytes = [this](std::uintmax_t bytes) { SetPercent(bytes); };
    ctx.fp     = std::fopen(part_path_.c_str(), offset > 0 ? "ab" : "wb");
    if (ctx.fp == nullptr) {
        SetStatus(Phase::kFailed, 0, std::string("cannot write: ") +
                  std::strerror(errno));
        return Result::kFatal;
    }
    ctx.curl = curl_easy_init();
    if (ctx.curl == nullptr) {
        std::fclose(ctx.fp);
        SetStatus(Phase::kDownloading, 0, "curl_easy_init failed");
        return Result::kRetry;
    }
    if (offset > 0) {
        LVA_LOGI(kTag, "resuming at %ju of %ju bytes", offset, spec_.size);
    }

    CURL* curl = ctx.curl;
    curl_easy_setopt(curl, CURLOPT_URL,              spec_.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR,      1L);
    curl_easy_setopt(curl, CURLOPT_CAINFO,           kCaCertPath);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,   10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT,  1024L);  // 1 KB/s
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,   60L);    // for 60 s
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,         1L);
    curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                     static_cast<curl_off_t>(offset));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    &WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &ProgressCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,     &ctx);

    const auto attempt_started = std::chrono::steady_clock::now();
    LVA_LOGI(kTag, "download attempt started (offset=%ju)", offset);
    const CURLcode rc = curl_easy_perform(curl);
    const auto attempt_elapsed = std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                   attempt_started).count();
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    LVA_LOGI(kTag, "download attempt finished rc=%d http=%ld elapsed_ms=%lld",
             static_cast<int>(rc), http_code,
             static_cast<long long>(attempt_elapsed));
    std::fflush(ctx.fp);
    ::fsync(::fileno(ctx.fp));
    std::fclose(ctx.fp);

    if (rc == CURLE_ABORTED_BY_CALLBACK &&
        cancel_.load(std::memory_order_relaxed)) {
        return Result::kCancelled;
    }
    if (http_code == 416) {
        // Range past the end: the part file is bad. Start over next time.
        std::filesystem::remove(part_path_, ec);
    }
    if (rc != CURLE_OK) {
        if (http_code >= 400 && http_code < 500 && http_code != 408 &&
            http_code != 416 && http_code != 429) {
            // Wrong URL or no access; retrying will not help. 408 and 429
            // (rate limited) are retried with backoff.
            const std::string err = "HTTP " + std::to_string(http_code);
            LVA_LOGE(kTag, "%s for %s", err.c_str(), spec_.url.c_str());
            SetStatus(Phase::kFailed, 0, err);
            return Result::kFatal;
        }
        const std::string err = http_code >= 400
            ? "HTTP " + std::to_string(http_code)
            : std::string(curl_easy_strerror(rc));
        SetStatus(Phase::kDownloading, GetStatus().percent, err);
        return Result::kRetry;
    }
    if (FileSizeOrZero(part_path_) != spec_.size) {
        SetStatus(Phase::kDownloading, GetStatus().percent,
                  "size mismatch");
        std::filesystem::remove(part_path_, ec);
        return Result::kRetry;
    }
    return Result::kOk;
}

ModelDownloader::Result ModelDownloader::VerifyAndInstall() {
    SetStatus(Phase::kVerifying, 99);
    const std::string actual = lva::util::Sha256FileHex(part_path_);
    std::error_code ec;
    if (actual != spec_.sha256) {
        LVA_LOGE(kTag, "sha256 mismatch: got %s, want %s", actual.c_str(),
                 spec_.sha256.c_str());
        std::filesystem::remove(part_path_, ec);
        SetStatus(Phase::kDownloading, 0, "checksum mismatch");
        return Result::kRetry;
    }
    std::filesystem::rename(part_path_, spec_.path, ec);
    if (ec) {
        SetStatus(Phase::kFailed, 0, "install failed: " + ec.message());
        return Result::kFatal;
    }
    ::sync();
    return Result::kOk;
}

}  // namespace lva::tr
