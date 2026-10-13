#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace muse {
class MuseClient {
 public:
    explicit MuseClient(std::string noise_host = "hatch.metaaivm.com");
    static std::string NewVoiceSessionId();
    using ReplyCallback = std::function<bool(const std::string&, std::atomic<bool>&, std::string&)>;
    bool SendVoiceNote(const std::string& device_token,
                       const std::vector<int16_t>& pcm,
                       std::string& error,
                       const ReplyCallback& reply = {},
                       std::atomic<bool>* cancel = nullptr,
                       const std::function<void()>& waiting = {},
                       const std::string& voice_session = "",
                       bool main_chat = false);
    // Validate the device token against the Muse account and bring the device
    // online (GET /fetch_vms). Returns false with a reason on failure.
    bool VerifyAccount(const std::string& device_token, std::string& error);
    // Keep the device registered and online with its Muse. Connects to the
    // leased VM, sends link.register, then serves link.invoke requests until
    // the connection ends or *stop becomes true. Returns on stop; reconnection
    // and backoff are handled internally.
    void RunLinkService(const std::string& device_token,
                        const std::string& node_id,
                        const std::string& display_name,
                        std::atomic<bool>& stop);
    // Rotate the device token pair and report the SDK token (POST
    // /device_token/refresh). Required before the VM will accept the device.
    // device_id is the node_id. On success returns true and fills new tokens.
    bool RefreshDeviceToken(const std::string& refresh_token,
                            const std::string& device_id,
                            const std::string& sdk_token,
                            const std::string& api_url_v2,
                            std::string& new_access_token,
                            std::string& new_refresh_token,
                            std::string& error);
 private:
    std::string noise_host_;
};
}  // namespace muse
