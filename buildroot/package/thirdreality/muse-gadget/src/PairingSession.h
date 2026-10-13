#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace muse {

class PairingSession {
 public:
    enum class State { Idle, WaitClientFinished, ConfirmRequired, Ready };

    PairingSession(std::string node_id, std::string device_id,
                   std::string mac, std::string firmware_version,
                   std::string sdk_token);
    ~PairingSession();

    PairingSession(const PairingSession&) = delete;
    PairingSession& operator=(const PairingSession&) = delete;

    nlohmann::json DeviceInfo() const;
    nlohmann::json Handle(const nlohmann::json& command);
    // Seal a device-to-mobile message with the active session keys.
    nlohmann::json Seal(const nlohmann::json& value);
    bool ready() const { return state_ == State::Ready; }
    nlohmann::json Confirm();
    nlohmann::json CompleteProvisioning(bool success);
    void Reset();
    State state() const { return state_; }

 private:
    struct Crypto;

    nlohmann::json HandleHello(const nlohmann::json& command);
    nlohmann::json DecryptEnvelope(const nlohmann::json& command);
    nlohmann::json Encrypt(const nlohmann::json& plain);
    bool Expired();

    std::string node_id_;
    std::string device_id_;
    std::string mac_;
    std::string firmware_version_;
    std::string sdk_token_;
    std::unique_ptr<Crypto> crypto_;
    State state_{State::Idle};
    uint64_t rx_counter_{0};
    uint64_t tx_counter_{0};
    std::chrono::steady_clock::time_point deadline_{};
};

}  // namespace muse
