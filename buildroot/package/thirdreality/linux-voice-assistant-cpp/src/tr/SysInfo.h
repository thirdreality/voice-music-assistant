
#pragma once

#include <map>
#include <mutex>
#include <string>

namespace lva::tr {

struct DeviceInfo {
    std::string device_model;
    std::string model_id;
    std::string firmware_version;
    std::string mac_address;       // verbatim from JSON, may be "AA:BB:..." or "AABB..."
    std::string name;              // configured device name
    std::string status;            // wifi status: "connected" / "connected_no_internet" / ...
    std::string ssid;
    std::string ip;

    // AABBCCDDEEFF
    std::string normalized_mac() const;
    const std::string& serial_number() const { return mac_; }
    DeviceInfo();

   private:
    friend DeviceInfo ReadDeviceInfo();
    std::string mac_;  // cached normalized form
};

// Cached by mtime.
DeviceInfo ReadDeviceInfo();

std::string PreferredDeviceName();

}  // namespace lva::tr
