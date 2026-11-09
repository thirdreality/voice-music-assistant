#include "util/Md5.h"

#include <cstring>

namespace lva::util {
namespace {

std::uint32_t F(std::uint32_t x, std::uint32_t y, std::uint32_t z)
    { return (x & y) | (~x & z); }
std::uint32_t G(std::uint32_t x, std::uint32_t y, std::uint32_t z)
    { return (x & z) | (y & ~z); }
std::uint32_t H(std::uint32_t x, std::uint32_t y, std::uint32_t z)
    { return x ^ y ^ z; }
std::uint32_t I(std::uint32_t x, std::uint32_t y, std::uint32_t z)
    { return y ^ (x | ~z); }
std::uint32_t Rot(std::uint32_t x, int n)
    { return (x << n) | (x >> (32 - n)); }
void Step(std::uint32_t (*f)(std::uint32_t,std::uint32_t,std::uint32_t),
                 std::uint32_t& a, std::uint32_t b, std::uint32_t c,
                 std::uint32_t d, std::uint32_t x, int s, std::uint32_t t) {
    a = b + Rot(a + f(b, c, d) + x + t, s);
}

std::string Md5HexLower(const unsigned char digest[16]) {
    static const char kHex[] = "0123456789abcdef";
    std::string out(32, '\0');
    for (int i = 0; i < 16; ++i) {
        out[2 * i + 0] = kHex[(digest[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[digest[i] & 0xF];
    }
    return out;
}

}  // namespace

Md5::Md5() {
    state_[0] = 0x67452301;
    state_[1] = 0xEFCDAB89;
    state_[2] = 0x98BADCFE;
    state_[3] = 0x10325476;
    count_   = 0;
}

void Md5::Update(const unsigned char* data, std::size_t n) {
    std::size_t idx = static_cast<std::size_t>((count_ >> 3) & 0x3F);
    count_ += static_cast<std::uint64_t>(n) << 3;
    std::size_t part = 64 - idx;
    std::size_t i = 0;
    if (n >= part) {
        std::memcpy(buffer_ + idx, data, part);
        Transform(buffer_);
        for (i = part; i + 63 < n; i += 64) Transform(data + i);
        idx = 0;
    }
    std::memcpy(buffer_ + idx, data + i, n - i);
}

void Md5::Final(unsigned char digest[16]) {
    unsigned char bits[8];
    for (int i = 0; i < 8; ++i) {
        bits[i] = static_cast<unsigned char>((count_ >> (i * 8)) & 0xFF);
    }
    std::size_t idx = static_cast<std::size_t>((count_ >> 3) & 0x3F);
    std::size_t pad_len = (idx < 56) ? (56 - idx) : (120 - idx);
    static const unsigned char kPad[64] = {0x80};
    Update(kPad, pad_len);
    Update(bits, 8);
    for (int i = 0; i < 4; ++i) {
        digest[i * 4 + 0] = static_cast<unsigned char>(state_[i] & 0xFF);
        digest[i * 4 + 1] = static_cast<unsigned char>((state_[i] >> 8) & 0xFF);
        digest[i * 4 + 2] = static_cast<unsigned char>((state_[i] >> 16) & 0xFF);
        digest[i * 4 + 3] = static_cast<unsigned char>((state_[i] >> 24) & 0xFF);
    }
}

void Md5::Transform(const unsigned char block[64]) {
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t x[16];
    for (int i = 0; i < 16; ++i) {
        x[i] = static_cast<std::uint32_t>(block[i * 4 + 0])
             | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8)
             | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16)
             | (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
    }
    Step(F, a, b, c, d, x[0],  7,  0xD76AA478);
    Step(F, d, a, b, c, x[1],  12, 0xE8C7B756);
    Step(F, c, d, a, b, x[2],  17, 0x242070DB);
    Step(F, b, c, d, a, x[3],  22, 0xC1BDCEEE);
    Step(F, a, b, c, d, x[4],  7,  0xF57C0FAF);
    Step(F, d, a, b, c, x[5],  12, 0x4787C62A);
    Step(F, c, d, a, b, x[6],  17, 0xA8304613);
    Step(F, b, c, d, a, x[7],  22, 0xFD469501);
    Step(F, a, b, c, d, x[8],  7,  0x698098D8);
    Step(F, d, a, b, c, x[9],  12, 0x8B44F7AF);
    Step(F, c, d, a, b, x[10], 17, 0xFFFF5BB1);
    Step(F, b, c, d, a, x[11], 22, 0x895CD7BE);
    Step(F, a, b, c, d, x[12], 7,  0x6B901122);
    Step(F, d, a, b, c, x[13], 12, 0xFD987193);
    Step(F, c, d, a, b, x[14], 17, 0xA679438E);
    Step(F, b, c, d, a, x[15], 22, 0x49B40821);
    Step(G, a, b, c, d, x[1],  5,  0xF61E2562);
    Step(G, d, a, b, c, x[6],  9,  0xC040B340);
    Step(G, c, d, a, b, x[11], 14, 0x265E5A51);
    Step(G, b, c, d, a, x[0],  20, 0xE9B6C7AA);
    Step(G, a, b, c, d, x[5],  5,  0xD62F105D);
    Step(G, d, a, b, c, x[10], 9,  0x02441453);
    Step(G, c, d, a, b, x[15], 14, 0xD8A1E681);
    Step(G, b, c, d, a, x[4],  20, 0xE7D3FBC8);
    Step(G, a, b, c, d, x[9],  5,  0x21E1CDE6);
    Step(G, d, a, b, c, x[14], 9,  0xC33707D6);
    Step(G, c, d, a, b, x[3],  14, 0xF4D50D87);
    Step(G, b, c, d, a, x[8],  20, 0x455A14ED);
    Step(G, a, b, c, d, x[13], 5,  0xA9E3E905);
    Step(G, d, a, b, c, x[2],  9,  0xFCEFA3F8);
    Step(G, c, d, a, b, x[7],  14, 0x676F02D9);
    Step(G, b, c, d, a, x[12], 20, 0x8D2A4C8A);
    Step(H, a, b, c, d, x[5],  4,  0xFFFA3942);
    Step(H, d, a, b, c, x[8],  11, 0x8771F681);
    Step(H, c, d, a, b, x[11], 16, 0x6D9D6122);
    Step(H, b, c, d, a, x[14], 23, 0xFDE5380C);
    Step(H, a, b, c, d, x[1],  4,  0xA4BEEA44);
    Step(H, d, a, b, c, x[4],  11, 0x4BDECFA9);
    Step(H, c, d, a, b, x[7],  16, 0xF6BB4B60);
    Step(H, b, c, d, a, x[10], 23, 0xBEBFBC70);
    Step(H, a, b, c, d, x[13], 4,  0x289B7EC6);
    Step(H, d, a, b, c, x[0],  11, 0xEAA127FA);
    Step(H, c, d, a, b, x[3],  16, 0xD4EF3085);
    Step(H, b, c, d, a, x[6],  23, 0x04881D05);
    Step(H, a, b, c, d, x[9],  4,  0xD9D4D039);
    Step(H, d, a, b, c, x[12], 11, 0xE6DB99E5);
    Step(H, c, d, a, b, x[15], 16, 0x1FA27CF8);
    Step(H, b, c, d, a, x[2],  23, 0xC4AC5665);
    Step(I, a, b, c, d, x[0],  6,  0xF4292244);
    Step(I, d, a, b, c, x[7],  10, 0x432AFF97);
    Step(I, c, d, a, b, x[14], 15, 0xAB9423A7);
    Step(I, b, c, d, a, x[5],  21, 0xFC93A039);
    Step(I, a, b, c, d, x[12], 6,  0x655B59C3);
    Step(I, d, a, b, c, x[3],  10, 0x8F0CCC92);
    Step(I, c, d, a, b, x[10], 15, 0xFFEFF47D);
    Step(I, b, c, d, a, x[1],  21, 0x85845DD1);
    Step(I, a, b, c, d, x[8],  6,  0x6FA87E4F);
    Step(I, d, a, b, c, x[15], 10, 0xFE2CE6E0);
    Step(I, c, d, a, b, x[6],  15, 0xA3014314);
    Step(I, b, c, d, a, x[13], 21, 0x4E0811A1);
    Step(I, a, b, c, d, x[4],  6,  0xF7537E82);
    Step(I, d, a, b, c, x[11], 10, 0xBD3AF235);
    Step(I, c, d, a, b, x[2],  15, 0x2AD7D2BB);
    Step(I, b, c, d, a, x[9],  21, 0xEB86D391);
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
}

std::string Md5::HexDigest() {
    unsigned char digest[16];
    Final(digest);
    return Md5HexLower(digest);
}

std::string Md5Hex(std::string_view data) {
    Md5 md5;
    if (!data.empty()) {
        md5.Update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    }
    return md5.HexDigest();
}

}  // namespace lva::util
