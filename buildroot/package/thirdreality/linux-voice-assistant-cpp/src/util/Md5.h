// Incremental MD5 for the existing HTTP signature and OTA checksum formats.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lva::util {

class Md5 {
   public:
    Md5();
    void Update(const unsigned char* data, std::size_t size);
    // Finalizes this instance. Do not call Update/HexDigest again afterwards.
    std::string HexDigest();

   private:
    void Final(unsigned char digest[16]);
    void Transform(const unsigned char block[64]);
    std::uint32_t state_[4];
    std::uint64_t count_;
    unsigned char buffer_[64];
};

// Lowercase, 32-character digest, including for empty input.
std::string Md5Hex(std::string_view data);

}  // namespace lva::util
