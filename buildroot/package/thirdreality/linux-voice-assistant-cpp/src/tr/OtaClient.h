
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include "tr/NetworkPolicy.h"

namespace lva::tr {

struct OtaRelease {
    RequestSource source = RequestSource::Automatic;
    std::string version;        // semver-ish, used in HA UI
    std::string url;            // download URL
    std::string expected_md5;   // hex, lower-cased on compare
};

class OtaClient {
   public:
    static constexpr const char* kDefaultDownloadPath = "/data/software.swu";
    static constexpr const char* kCaCertPath = "/etc/ssl/certs/ca-certificates.crt";
    static constexpr const char* kSwupdatePath = "/usr/bin/swupdate";
    static constexpr const char* kSwupdatePublicKey = "/etc/swupdate-public.pem";
    static constexpr const char* kSwupdateHardware = "S420:1.0";

    using ProgressCallback = std::function<void(float progress,
                                                bool has_progress)>;

    OtaClient() = default;

    OtaClient(const OtaClient&)            = delete;
    OtaClient& operator=(const OtaClient&) = delete;

    void DownloadFirmware(const OtaRelease& release, const NetworkPolicy& policy,
                          ProgressCallback progress,
                          const std::atomic<bool>* cancel = nullptr);

    void InstallFirmware();

    static std::string CalculateFileMd5(const std::string& path);

    const std::string& download_path() const noexcept { return download_path_; }

   private:
    std::string download_path_ = kDefaultDownloadPath;
};

}  // namespace lva::tr
