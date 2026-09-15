#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace watch_notification {
constexpr size_t kOwnerGrantBodySize = 92;
constexpr size_t kOwnerGrantPrefixSize = 94;
constexpr size_t kOwnerGrantMaxSize = 606;
constexpr size_t kPeerChallengeSize = 32;
constexpr size_t kOwnerEnvelopeMaxSize = kPeerChallengeSize + kOwnerGrantMaxSize;
constexpr uint32_t kOwnerGrantMaxLeaseSeconds = 300;

// The key must be a provisioned trust anchor, never supplied by the peer.
// A returned duration is anchored to challenge creation, not verification time.
std::optional<uint32_t> VerifyOwnerGrant(std::string_view trusted_spki_base64,
    std::string_view bound_device_id, uint64_t wire_session,
    const std::array<uint8_t, 32>& challenge, const uint8_t* proof, size_t size);

// Proves device credential possession to the phone, never account ownership to
// the watch. Call only after VerifyOwnerGrant accepted this exact signed proof.
std::optional<std::array<uint8_t, 32>> BuildPeerProof(std::string_view credential,
    const std::array<uint8_t, 32>& peer_challenge, const uint8_t* proof, size_t size);
}  // namespace watch_notification
