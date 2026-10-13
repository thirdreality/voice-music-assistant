#include "OpenSslCryptoBackend.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace muse {
using namespace musegadgets::noise::core;
namespace {
Status Invalid() { return Status::InvalidArgument(); }
Status Internal() { return Status::Internal(); }
bool Valid(ConstByteSpan b) { return b.data() || b.empty(); }
bool Valid(ByteSpan b) { return b.data() || b.empty(); }

Status Hmac256(ConstByteSpan key, ConstByteSpan data, ByteSpan out) {
    if (out.size() != 32 || !Valid(key) || !Valid(data) || !Valid(out)) return Invalid();
    unsigned int n = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data.data(),
              data.size(), out.data(), &n) || n != out.size()) return Internal();
    return OkStatus();
}

Status Gcm(bool seal, ConstByteSpan key, ConstByteSpan nonce, ConstByteSpan aad,
           ConstByteSpan input, ByteSpan output) {
    if (key.size() != 32 || nonce.size() != 12 || !Valid(aad) || !Valid(input) || !Valid(output))
        return Invalid();
    const size_t tag_size = 16;
    if ((seal && output.size() < input.size() + tag_size) ||
        (!seal && (input.size() < tag_size || output.size() < input.size() - tag_size)))
        return Status::ResourceExhausted();
    EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
    if (!raw) return Internal();
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(raw, EVP_CIPHER_CTX_free);
    int n = 0, total = 0;
    if (seal) {
        if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
            EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1 ||
            (!aad.empty() && EVP_EncryptUpdate(ctx.get(), nullptr, &n, aad.data(), aad.size()) != 1))
            return Internal();
        // OpenSSL reports AAD bytes in n, but they are not ciphertext bytes.
        // Empty Noise handshake payloads must put their tag at offset zero.
        n = 0;
        if (!input.empty() && EVP_EncryptUpdate(ctx.get(), output.data(), &n,
                                                input.data(), input.size()) != 1)
            return Internal();
        total = n;
        if (EVP_EncryptFinal_ex(ctx.get(), output.data() + total, &n) != 1) return Internal();
        total += n;
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, tag_size,
                                output.data() + total) != 1) return Internal();
    } else {
        const size_t cipher_size = input.size() - tag_size;
        if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
            EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1 ||
            (!aad.empty() && EVP_DecryptUpdate(ctx.get(), nullptr, &n, aad.data(), aad.size()) != 1))
            return Internal();
        n = 0;
        if (cipher_size && EVP_DecryptUpdate(ctx.get(), output.data(), &n,
                                             input.data(), cipher_size) != 1)
            return Internal();
        total = n;
        std::array<uint8_t, 16> final_output{};
        uint8_t* final_at = cipher_size ? output.data() + total : final_output.data();
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, tag_size,
                                const_cast<uint8_t*>(input.data() + cipher_size)) != 1 ||
            EVP_DecryptFinal_ex(ctx.get(), final_at, &n) != 1)
            return Status::Unauthenticated();
    }
    return OkStatus();
}
}  // namespace

Status OpenSslCryptoBackend::Random(ByteSpan out) noexcept {
    return Valid(out) && (out.empty() || RAND_bytes(out.data(), out.size()) == 1) ? OkStatus() : Internal();
}
Status OpenSslCryptoBackend::X25519GenerateKeypair(ByteSpan priv, ByteSpan pub) noexcept {
    if (priv.size() != 32 || pub.size() != 32) return Invalid();
    EVP_PKEY_CTX* cr = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!cr) return Internal();
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(cr, EVP_PKEY_CTX_free);
    EVP_PKEY* kr = nullptr;
    if (EVP_PKEY_keygen_init(ctx.get()) != 1 || EVP_PKEY_keygen(ctx.get(), &kr) != 1) return Internal();
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(kr, EVP_PKEY_free);
    size_t pn = priv.size(), qn = pub.size();
    return EVP_PKEY_get_raw_private_key(key.get(), priv.data(), &pn) == 1 &&
                   EVP_PKEY_get_raw_public_key(key.get(), pub.data(), &qn) == 1 && pn == 32 && qn == 32
               ? OkStatus() : Internal();
}
Status OpenSslCryptoBackend::X25519PublicFromPrivate(ConstByteSpan priv, ByteSpan pub) noexcept {
    if (priv.size() != 32 || pub.size() != 32) return Invalid();
    EVP_PKEY* kr = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), priv.size());
    if (!kr) return Internal();
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(kr, EVP_PKEY_free);
    size_t n = pub.size();
    return EVP_PKEY_get_raw_public_key(key.get(), pub.data(), &n) == 1 && n == 32 ? OkStatus() : Internal();
}
Status OpenSslCryptoBackend::X25519Dh(ConstByteSpan priv, ConstByteSpan peer, ByteSpan shared) noexcept {
    if (priv.size() != 32 || peer.size() != 32 || shared.size() != 32) return Invalid();
    EVP_PKEY* ar = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), priv.size());
    EVP_PKEY* br = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer.data(), peer.size());
    if (!ar || !br) { EVP_PKEY_free(ar); EVP_PKEY_free(br); return Internal(); }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> a(ar, EVP_PKEY_free), b(br, EVP_PKEY_free);
    EVP_PKEY_CTX* cr = EVP_PKEY_CTX_new(a.get(), nullptr);
    if (!cr) return Internal();
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(cr, EVP_PKEY_CTX_free);
    size_t n = shared.size();
    return EVP_PKEY_derive_init(ctx.get()) == 1 && EVP_PKEY_derive_set_peer(ctx.get(), b.get()) == 1 &&
                   EVP_PKEY_derive(ctx.get(), shared.data(), &n) == 1 && n == 32
               ? OkStatus() : Internal();
}
Status OpenSslCryptoBackend::Sha256(ConstByteSpan data, ByteSpan digest) noexcept {
    if (!Valid(data) || digest.size() != 32) return Invalid();
    EVP_MD_CTX* cr = EVP_MD_CTX_new(); if (!cr) return Internal();
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(cr, EVP_MD_CTX_free);
    unsigned int n = 0;
    return EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) == 1 &&
                   EVP_DigestUpdate(ctx.get(), data.data(), data.size()) == 1 &&
                   EVP_DigestFinal_ex(ctx.get(), digest.data(), &n) == 1 && n == 32
               ? OkStatus() : Internal();
}
Status OpenSslCryptoBackend::Sha256Concat(ConstByteSpan a, ConstByteSpan b, ByteSpan digest) noexcept {
    if (!Valid(a) || !Valid(b) || digest.size() != 32) return Invalid();
    EVP_MD_CTX* cr = EVP_MD_CTX_new(); if (!cr) return Internal();
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(cr, EVP_MD_CTX_free);
    unsigned int n = 0;
    return EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) == 1 &&
                   EVP_DigestUpdate(ctx.get(), a.data(), a.size()) == 1 &&
                   EVP_DigestUpdate(ctx.get(), b.data(), b.size()) == 1 &&
                   EVP_DigestFinal_ex(ctx.get(), digest.data(), &n) == 1 && n == 32
               ? OkStatus() : Internal();
}
Status OpenSslCryptoBackend::HkdfSha256(ConstByteSpan salt, ConstByteSpan ikm, ConstByteSpan info,
                                         Span<ByteSpan> outputs) noexcept {
    if (!Valid(salt) || !Valid(ikm) || !Valid(info) || outputs.size() > 3) return Invalid();
    std::array<uint8_t, 32> prk{}, previous{};
    if (!Hmac256(salt, ikm, ByteSpan(prk)).ok()) return Internal();
    size_t previous_size = 0;
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].size() != 32) return Invalid();
        std::vector<uint8_t> input;
        input.insert(input.end(), previous.begin(), previous.begin() + previous_size);
        input.insert(input.end(), info.data(), info.data() + info.size());
        input.push_back(static_cast<uint8_t>(i + 1));
        if (!Hmac256(ConstByteSpan(prk), ConstByteSpan(input.data(), input.size()), ByteSpan(previous)).ok())
            return Internal();
        std::memcpy(outputs[i].data(), previous.data(), previous.size());
        previous_size = previous.size();
    }
    OPENSSL_cleanse(prk.data(), prk.size()); OPENSSL_cleanse(previous.data(), previous.size());
    return OkStatus();
}
Status OpenSslCryptoBackend::Aes256GcmSeal(ConstByteSpan k, ConstByteSpan n, ConstByteSpan a,
                                            ConstByteSpan p, ByteSpan o) noexcept {
    return Gcm(true, k, n, a, p, o);
}
Status OpenSslCryptoBackend::Aes256GcmOpen(ConstByteSpan k, ConstByteSpan n, ConstByteSpan a,
                                            ConstByteSpan c, ByteSpan o) noexcept {
    return Gcm(false, k, n, a, c, o);
}
}  // namespace muse
