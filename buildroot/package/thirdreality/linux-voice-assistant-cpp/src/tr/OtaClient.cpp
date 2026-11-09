#include "tr/OtaClient.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "util/Log.h"
#include "util/Md5.h"

extern char** environ;

namespace lva::tr {

namespace {

constexpr const char* kTag = "ota";
constexpr off_t kCacheDropWindow = 1 << 20;  // 1 MiB

void DropFileCache(int fd, off_t offset, off_t length) {
    if (length <= 0) return;
#ifdef POSIX_FADV_DONTNEED
    posix_fadvise(fd, offset, length, POSIX_FADV_DONTNEED);
#else
    (void)fd; (void)offset; (void)length;
#endif
}

}  // namespace

std::string OtaClient::CalculateFileMd5(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        throw std::runtime_error("md5: cannot open " + path);
    }
    lva::util::Md5 md5;
    std::vector<unsigned char> buf(64 * 1024);
    int fd = -1;
    {
        fd = ::open(path.c_str(), O_RDONLY);
    }
    off_t processed = 0, dropped = 0;
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()),
               static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got <= 0) break;
        md5.Update(buf.data(), static_cast<std::size_t>(got));
        processed += got;
        if (fd >= 0 && processed - dropped >= kCacheDropWindow) {
            DropFileCache(fd, dropped, processed - dropped);
            dropped = processed;
        }
    }
    if (fd >= 0) ::close(fd);
    return md5.HexDigest();
}

namespace {

struct DownloadCtx {
    NetworkRequest* request = nullptr;
    std::FILE*                              fp = nullptr;
    lva::util::Md5                          md5;
    OtaClient::ProgressCallback             progress;
    bool                                    has_progress = false;
    curl_off_t                              total_bytes = 0;
    curl_off_t                              written = 0;
    int                                     fd = -1;
    off_t                                   dropped = 0;
};

std::size_t WriteCb(char* ptr, std::size_t size, std::size_t nmemb,
                    void* userdata) {
    auto* ctx = static_cast<DownloadCtx*>(userdata);
    if (ctx->request->Cancelled()) {
        return 0;  // tell curl to abort
    }
    const std::size_t n = size * nmemb;
    if (n == 0) return 0;
    if (std::fwrite(ptr, 1, n, ctx->fp) != n) return 0;
    ctx->md5.Update(reinterpret_cast<const unsigned char*>(ptr), n);
    ctx->written += static_cast<curl_off_t>(n);
    if (ctx->fd >= 0 &&
        ctx->written - ctx->dropped >= kCacheDropWindow) {
        std::fflush(ctx->fp);
        ::fsync(ctx->fd);
        DropFileCache(ctx->fd, ctx->dropped, ctx->written - ctx->dropped);
        ctx->dropped = ctx->written;
    }
    return n;
}

int ProgressCb(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
               curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<DownloadCtx*>(clientp);
    if (ctx->request->Cancelled()) {
        return 1;  // abort
    }
    if (dltotal > 0) {
        ctx->total_bytes  = dltotal;
        ctx->has_progress = true;
    }
    if (ctx->progress) {
        const float pct = ctx->total_bytes > 0
            ? std::min(100.0f,
                       static_cast<float>(dlnow) /
                           static_cast<float>(ctx->total_bytes) * 100.0f)
            : 0.0f;
        ctx->progress(pct, ctx->has_progress);
    }
    return 0;
}

}  // namespace

void OtaClient::DownloadFirmware(const OtaRelease& release, const NetworkPolicy& policy,
                                 ProgressCallback progress,
                                 const std::atomic<bool>* cancel) {
    if (release.url.empty()) {
        throw std::runtime_error("OTA: empty download URL");
    }
    if (release.expected_md5.empty()) {
        throw std::runtime_error("OTA: empty expected MD5");
    }

    const std::string final_path = download_path_;
    const std::string temp_path  = final_path + ".part";

    struct stat st{};
    if (::stat(final_path.c_str(), &st) == 0) {
        try {
            const std::string md5 = CalculateFileMd5(final_path);
            std::string want = release.expected_md5;
            std::transform(want.begin(), want.end(), want.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            if (md5 == want) {
                LVA_LOGI(kTag,
                         "reusing already-downloaded matching firmware: %s",
                         final_path.c_str());
                if (progress) progress(100.0f, true);
                return;
            }
            LVA_LOGI(kTag, "removing stale firmware (md5 mismatch)");
            ::unlink(final_path.c_str());
        } catch (...) {
            ::unlink(final_path.c_str());
        }
    }
    ::unlink(temp_path.c_str());

    {
        const auto slash = final_path.find_last_of('/');
        if (slash != std::string::npos) {
            const std::string dir = final_path.substr(0, slash);
            ::mkdir(dir.c_str(), 0755);  // best-effort
        }
    }

    NetworkRequest access(policy, release.source, cancel);
    if (access.Cancelled()) throw std::runtime_error("OTA: background internet disabled");
    DownloadCtx ctx;
    ctx.request = &access;
    ctx.fp = std::fopen(temp_path.c_str(), "wb");
    if (ctx.fp == nullptr) {
        throw std::runtime_error("OTA: cannot create " + temp_path +
                                 ": " + std::strerror(errno));
    }
    ctx.fd       = ::fileno(ctx.fp);
    ctx.progress = std::move(progress);

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        std::fclose(ctx.fp);
        ::unlink(temp_path.c_str());
        throw std::runtime_error("OTA: curl_easy_init failed");
    }

    access.Configure(curl);
    curl_easy_setopt(curl, CURLOPT_URL,            release.url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  &WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,     0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &ProgressCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,   &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);   // 1 KB/s
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,  60L);     // for 60 s
    curl_easy_setopt(curl, CURLOPT_CAINFO,         kCaCertPath);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE,     32L * 1024L);

    LVA_LOGI(kTag, "downloading %s -> %s", release.url.c_str(),
             temp_path.c_str());
    if (ctx.progress) ctx.progress(0.0f, false);

    const CURLcode rc = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    std::fflush(ctx.fp);
    ::fsync(ctx.fd);
    if (ctx.written > ctx.dropped) {
        DropFileCache(ctx.fd, ctx.dropped, ctx.written - ctx.dropped);
    }
    std::fclose(ctx.fp);

    if (rc != CURLE_OK) {
        ::unlink(temp_path.c_str());
        throw std::runtime_error(std::string("OTA: curl failed: ") +
                                 curl_easy_strerror(rc));
    }
    if (http_status != 200) {
        ::unlink(temp_path.c_str());
        throw std::runtime_error("OTA: HTTP status " +
                                 std::to_string(http_status));
    }

    const std::string actual = ctx.md5.HexDigest();
    std::string want = release.expected_md5;
    std::transform(want.begin(), want.end(), want.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (actual != want) {
        ::unlink(temp_path.c_str());
        throw std::runtime_error("OTA: MD5 mismatch (expected=" + want +
                                 ", got=" + actual + ")");
    }

    if (::rename(temp_path.c_str(), final_path.c_str()) != 0) {
        ::unlink(temp_path.c_str());
        throw std::runtime_error("OTA: rename to final path failed: " +
                                 std::string(std::strerror(errno)));
    }
    LVA_LOGI(kTag, "download OK (%lld bytes), MD5 verified",
             static_cast<long long>(ctx.written));
    if (ctx.progress) ctx.progress(100.0f, true);
}

void OtaClient::InstallFirmware() {
    struct stat st{};
    if (::stat(download_path_.c_str(), &st) != 0) {
        throw std::runtime_error("OTA: firmware file missing: " +
                                 download_path_);
    }

    char prog[]  = "/usr/bin/swupdate";
    char a1[]    = "-G";
    char a2[]    = "-k";
    char pubkey[] = "/etc/swupdate-public.pem";
    char a3[]    = "-H";
    char hwver[] = "S420:1.0";
    char* argv[] = { prog, a1, a2, pubkey, a3, hwver, nullptr };

    LVA_LOGI(kTag, "starting swupdate");

    // Keep the global SIGCHLD reaper from stealing our child.
    sigset_t block_chld, old_mask;
    sigemptyset(&block_chld);
    sigaddset(&block_chld, SIGCHLD);
    ::sigprocmask(SIG_BLOCK, &block_chld, &old_mask);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                     "/dev/null", O_RDONLY, 0);

    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, prog, &actions, nullptr,
                                 argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        ::sigprocmask(SIG_SETMASK, &old_mask, nullptr);
        throw std::runtime_error("OTA: posix_spawn(swupdate) failed: " +
                                 std::string(std::strerror(rc)));
    }

    int status = 0;
    while (true) {
        const pid_t r = ::waitpid(pid, &status, 0);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) {
            ::sigprocmask(SIG_SETMASK, &old_mask, nullptr);
            throw std::runtime_error("OTA: waitpid failed: " +
                                     std::string(std::strerror(errno)));
        }
    }
    ::sigprocmask(SIG_SETMASK, &old_mask, nullptr);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("OTA: swupdate exited with status " +
                                 std::to_string(WEXITSTATUS(status)));
    }
    LVA_LOGI(kTag, "swupdate completed; expecting reboot");
}

}  // namespace lva::tr
