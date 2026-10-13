#include "OpenSslCryptoBackend.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL " << message << '\n';
        std::exit(1);
    }
}
}  // namespace

int main() {
    using namespace musegadgets::noise::core;
    muse::OpenSslCryptoBackend crypto;
    const std::array<uint8_t, 32> key{};
    const std::array<uint8_t, 12> nonce{};
    std::array<uint8_t, 32> aad{};
    for (size_t i = 0; i < aad.size(); ++i) aad[i] = static_cast<uint8_t>(i);

    // Independent AESGCM reference: zero key/nonce, AAD bytes 0..31,
    // empty plaintext. Noise XX message 3 ends with exactly this case.
    const std::array<uint8_t, 16> expected{{
        0x22, 0xa1, 0x70, 0xff, 0xba, 0xda, 0x16, 0x3f,
        0x54, 0x7f, 0x88, 0x65, 0xab, 0xf9, 0x90, 0x9f}};
    std::array<uint8_t, 64> sealed{};
    sealed.fill(0xa5);
    Check(crypto.Aes256GcmSeal(ConstByteSpan(key), ConstByteSpan(nonce),
                              ConstByteSpan(aad), ConstByteSpan(),
                              ByteSpan(sealed.data(), expected.size())).ok(),
          "seal empty Noise handshake payload");
    Check(std::equal(expected.begin(), expected.end(), sealed.begin()),
          "empty payload tag must start at offset zero");
    Check(std::all_of(sealed.begin() + expected.size(), sealed.end(),
                      [](uint8_t b) { return b == 0xa5; }),
          "seal must not overwrite output guard bytes");

    Check(crypto.Aes256GcmOpen(ConstByteSpan(key), ConstByteSpan(nonce),
                              ConstByteSpan(aad), ConstByteSpan(expected),
                              ByteSpan()).ok(),
          "open empty handshake payload with no output buffer");
    auto invalid = expected;
    invalid[0] ^= 1;
    Check(crypto.Aes256GcmOpen(ConstByteSpan(key), ConstByteSpan(nonce),
                              ConstByteSpan(aad), ConstByteSpan(invalid),
                              ByteSpan()).IsUnauthenticated(),
          "reject modified empty payload tag");

    // Ensure resetting the AAD byte count does not break nonempty records.
    const std::array<uint8_t, 13> plain{{
        'N', 'o', 'i', 's', 'e', ' ', 'p', 'a', 'y', 'l', 'o', 'a', 'd'}};
    const std::array<uint8_t, 29> expected_record{{
        0x80, 0xc8, 0x29, 0x4e, 0x28, 0x40, 0x1b, 0x0f,
        0x7e, 0x22, 0xaa, 0xb2, 0xde, 0xac, 0x98, 0xfc,
        0x5d, 0xf8, 0xc2, 0x67, 0x00, 0xa2, 0x73, 0x74,
        0x1c, 0x4d, 0xba, 0x00, 0xfc}};
    std::array<uint8_t, 29> record{};
    Check(crypto.Aes256GcmSeal(ConstByteSpan(key), ConstByteSpan(nonce),
                              ConstByteSpan(aad), ConstByteSpan(plain),
                              ByteSpan(record)).ok() && record == expected_record,
          "nonempty payload matches independent reference");
    std::array<uint8_t, 13> opened{};
    Check(crypto.Aes256GcmOpen(ConstByteSpan(key), ConstByteSpan(nonce),
                              ConstByteSpan(aad), ConstByteSpan(record),
                              ByteSpan(opened)).ok() && opened == plain,
          "open nonempty payload");
    std::cout << "PASS muse-gadget crypto tests\n";
    return 0;
}
