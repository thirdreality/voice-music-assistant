#include "ConfigStore.h"
#include "PairingSession.h"
#include "WavWriter.h"

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cassert>
#include <filesystem>
#include <iostream>

int main() {
    const auto wav = muse::StreamingWavHeader(16000);
    assert(wav.size() == 44);
    assert(std::string(reinterpret_cast<const char*>(wav.data()), 4) == "RIFF");
    assert(wav[24] == 0x80 && wav[25] == 0x3e);

    muse::PairingSession session("homelink-aabbcc", "hatch-link:00:11:22:aa:bb:cc",
                                 "00:11:22:aa:bb:cc", "0.1.0", "mgst_test");
    assert(session.DeviceInfo().at("pairing_protocol") == 5);
    assert(session.DeviceInfo().at("pairing_policy") == "confirm_app");
    assert(session.Handle({{"action", "get_device_info"}}).at("pairing_protocol") == 5);
    assert(session.Handle({{"action", "pairing_client_hello"}}).at("status") ==
           "error_pairing_invalid_hello");

    EC_KEY* key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    assert(key && EC_KEY_generate_key(key) == 1);
    std::array<uint8_t, 65> pub{};
    assert(EC_POINT_point2oct(EC_KEY_get0_group(key), EC_KEY_get0_public_key(key),
                              POINT_CONVERSION_UNCOMPRESSED, pub.data(), pub.size(), nullptr) == pub.size());
    std::array<uint8_t, 16> nonce{};
    assert(RAND_bytes(nonce.data(), nonce.size()) == 1);
    auto b64 = [](const uint8_t* data, size_t size) {
        std::string out(4 * ((size + 2) / 3), '\0');
        out.resize(EVP_EncodeBlock(reinterpret_cast<uint8_t*>(out.data()), data, size));
        while (!out.empty() && out.back() == '=') out.pop_back();
        for (char& c : out) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
        return out;
    };
    const auto ready = session.Handle({{"action", "pairing_client_hello"}, {"version", 5},
                                       {"pairing_auth", "none"}, {"pairing_policy", "confirm_app"},
                                       {"mobile_pub", b64(pub.data(), pub.size())},
                                       {"mobile_nonce", b64(nonce.data(), nonce.size())}});
    assert(ready.at("type") == "pairing_ready");
    assert(ready.at("device_id") == "hatch-link:00:11:22:aa:bb:cc");
    EC_KEY_free(key);

    const auto dir = std::filesystem::temp_directory_path() / "muse-gadget-test";
    std::filesystem::remove_all(dir);
    muse::ConfigStore store(dir);
    store.Save({{"paired", true}});
    assert(store.Load().at("paired") == true);
    std::filesystem::remove_all(dir);

    std::cout << "PASS muse-gadget core tests\n";
    return 0;
}
