#pragma once

#include <xplat/noise/core/CryptoBackend.h>

namespace muse {
class OpenSslCryptoBackend final : public musegadgets::noise::core::CryptoBackend {
 public:
    musegadgets::noise::core::Status Random(musegadgets::noise::core::ByteSpan out) noexcept override;
    musegadgets::noise::core::Status X25519GenerateKeypair(
        musegadgets::noise::core::ByteSpan private_key,
        musegadgets::noise::core::ByteSpan public_key) noexcept override;
    musegadgets::noise::core::Status X25519PublicFromPrivate(
        musegadgets::noise::core::ConstByteSpan private_key,
        musegadgets::noise::core::ByteSpan public_key) noexcept override;
    musegadgets::noise::core::Status X25519Dh(
        musegadgets::noise::core::ConstByteSpan private_key,
        musegadgets::noise::core::ConstByteSpan peer_public,
        musegadgets::noise::core::ByteSpan shared) noexcept override;
    musegadgets::noise::core::Status Sha256(
        musegadgets::noise::core::ConstByteSpan data,
        musegadgets::noise::core::ByteSpan digest) noexcept override;
    musegadgets::noise::core::Status Sha256Concat(
        musegadgets::noise::core::ConstByteSpan prefix,
        musegadgets::noise::core::ConstByteSpan suffix,
        musegadgets::noise::core::ByteSpan digest) noexcept override;
    musegadgets::noise::core::Status HkdfSha256(
        musegadgets::noise::core::ConstByteSpan salt,
        musegadgets::noise::core::ConstByteSpan ikm,
        musegadgets::noise::core::ConstByteSpan info,
        musegadgets::noise::core::Span<musegadgets::noise::core::ByteSpan> outputs) noexcept override;
    musegadgets::noise::core::Status Aes256GcmSeal(
        musegadgets::noise::core::ConstByteSpan key,
        musegadgets::noise::core::ConstByteSpan nonce,
        musegadgets::noise::core::ConstByteSpan aad,
        musegadgets::noise::core::ConstByteSpan plaintext,
        musegadgets::noise::core::ByteSpan output) noexcept override;
    musegadgets::noise::core::Status Aes256GcmOpen(
        musegadgets::noise::core::ConstByteSpan key,
        musegadgets::noise::core::ConstByteSpan nonce,
        musegadgets::noise::core::ConstByteSpan aad,
        musegadgets::noise::core::ConstByteSpan ciphertext_and_tag,
        musegadgets::noise::core::ByteSpan output) noexcept override;
};
}  // namespace muse
