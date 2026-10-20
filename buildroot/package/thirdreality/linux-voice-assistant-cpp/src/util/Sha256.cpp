#include "util/Sha256.h"

#include <array>
#include <cstdint>
#include <fstream>

namespace lva::util {

namespace {

// Minimal SHA-256 to avoid an OpenSSL dependency.
class Sha256 {
   public:
    Sha256() { Reset(); }

    void Update(const unsigned char* data, std::size_t len) {
        for (std::size_t i = 0; i < len; ++i) {
            buffer_[buffer_len_++] = data[i];
            if (buffer_len_ == 64) {
                Transform(buffer_);
                bit_len_ += 512;
                buffer_len_ = 0;
            }
        }
    }

    void Final(unsigned char digest[32]) {
        std::size_t i = buffer_len_;
        bit_len_ += static_cast<std::uint64_t>(buffer_len_) * 8;

        buffer_[i++] = 0x80;
        if (i > 56) {
            while (i < 64) buffer_[i++] = 0x00;
            Transform(buffer_);
            i = 0;
        }
        while (i < 56) buffer_[i++] = 0x00;

        for (int j = 7; j >= 0; --j) {
            buffer_[i++] = static_cast<unsigned char>(
                (bit_len_ >> (j * 8)) & 0xFF);
        }
        Transform(buffer_);

        for (int j = 0; j < 8; ++j) {
            digest[j * 4 + 0] = static_cast<unsigned char>((state_[j] >> 24) & 0xFF);
            digest[j * 4 + 1] = static_cast<unsigned char>((state_[j] >> 16) & 0xFF);
            digest[j * 4 + 2] = static_cast<unsigned char>((state_[j] >> 8) & 0xFF);
            digest[j * 4 + 3] = static_cast<unsigned char>(state_[j] & 0xFF);
        }
    }

   private:
    void Reset() {
        state_[0] = 0x6a09e667; state_[1] = 0xbb67ae85;
        state_[2] = 0x3c6ef372; state_[3] = 0xa54ff53a;
        state_[4] = 0x510e527f; state_[5] = 0x9b05688c;
        state_[6] = 0x1f83d9ab; state_[7] = 0x5be0cd19;
        bit_len_ = 0;
        buffer_len_ = 0;
    }

    static std::uint32_t Rotr(std::uint32_t x, std::uint32_t n) {
        return (x >> n) | (x << (32 - n));
    }

    void Transform(const unsigned char block[64]) {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
            0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 =
                Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 =
                Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 =
                Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;

            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::uint32_t state_[8];
    std::uint64_t bit_len_;
    unsigned char buffer_[64];
    std::size_t buffer_len_;
};

}  // namespace

std::string Sha256FileHex(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    Sha256 sha;
    std::array<char, 64 * 1024> buf;
    while (f) {
        f.read(buf.data(), buf.size());
        const std::streamsize n = f.gcount();
        if (n > 0) {
            sha.Update(reinterpret_cast<const unsigned char*>(buf.data()),
                       static_cast<std::size_t>(n));
        }
    }
    unsigned char digest[32];
    sha.Final(digest);

    static const char kHex[] = "0123456789abcdef";
    std::string out(64, '\0');
    for (int i = 0; i < 32; ++i) {
        out[2 * i + 0] = kHex[(digest[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[digest[i] & 0xF];
    }
    return out;
}

}  // namespace lva::util
