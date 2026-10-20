// Downloads a model file to /data in the background: resumes a partial
// download, checks free space, and installs the file only after its
// SHA-256 matches. Used for the on-device STT model, which is too large
// to ship in the rootfs.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace lva::tr {

struct ModelSpec {
    std::string           url;
    std::filesystem::path path;    // final install path
    std::uintmax_t        size = 0;
    std::string           sha256;  // lowercase hex
};

class ModelDownloader {
   public:
    enum class Phase { kMissing, kDownloading, kVerifying, kReady, kFailed };

    struct Status {
        Phase       phase   = Phase::kMissing;
        int         percent = 0;
        std::string error;
    };

    explicit ModelDownloader(ModelSpec spec);
    ~ModelDownloader();

    ModelDownloader(const ModelDownloader&)            = delete;
    ModelDownloader& operator=(const ModelDownloader&) = delete;

    // The installed file exists and has the expected size.
    bool Installed() const;

    // Starts the worker unless it is running or the model is installed.
    // The status is kDownloading when this returns.
    void Start();

    // Stops the worker and waits for it. The partial file is kept.
    void Cancel();

    Status GetStatus() const;

   private:
    enum class Result { kOk, kRetry, kFatal, kCancelled };

    void Run();
    Result DownloadOnce();
    Result VerifyAndInstall();
    void SetStatus(Phase phase, int percent, std::string error = {});
    void SetPercent(std::uintmax_t bytes);
    bool SleepUnlessCancelled(int seconds);

    const ModelSpec             spec_;
    const std::filesystem::path part_path_;

    mutable std::mutex      mu_;
    std::condition_variable cv_;
    Status                  status_;

    std::atomic<bool>          cancel_{false};
    std::atomic<bool>          running_{false};
    std::thread                worker_;
};

}  // namespace lva::tr
