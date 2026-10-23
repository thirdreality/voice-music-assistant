// Publishes one DNS-SD service: _esphomelib._tcp so HA discovers the
// device, or _sendspin._tcp so Music Assistant finds the Sendspin player.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lva::proto {

class MdnsPublisher {
   public:
    struct Options {
        std::string name;        // service instance name (device name)
        std::string type = "_esphomelib._tcp";
        std::uint16_t port = 6053;
        std::string mac;         // colon-separated MAC
        std::string version;     // esphome_version string
        // TXT records. Empty = the ESPHome records built from mac/version.
        std::vector<std::string> txt;
    };

    MdnsPublisher() = default;
    ~MdnsPublisher();

    MdnsPublisher(const MdnsPublisher&) = delete;
    MdnsPublisher& operator=(const MdnsPublisher&) = delete;

    bool Start(const Options& opts);
    void Stop();

   private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace lva::proto
