#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace watch_control {
using BondFingerprint = std::array<uint8_t, 32>;
enum class BondStatus {
    kAbsent, kFound, kApproved, kRemoved, kInvalidBond, kAmbiguous,
    kUnavailable, kProtectionRequired, kStorageError, kCancelled
};
struct StoredControlBond {
    BondFingerprint fingerprint{};
    std::array<uint8_t, 6> address{};
};
// Local owner only. Returns approved native bonds without exporting key bytes.
BondStatus ListApprovedBonds(std::vector<StoredControlBond>& bonds);

// The address must come from the native connection, never an application payload.
// A found bond is an identity candidate, not evidence of current-link encryption.
BondStatus FindNativeBond(const uint8_t* address, BondFingerprint& fingerprint);
// Resolve the SDK's stored address, which can differ from the live privacy address.
BondStatus FindNativeBond(const uint8_t* address, BondFingerprint& fingerprint,
                          std::array<uint8_t, 6>& stored_address);
BondStatus ReadApproval(const BondFingerprint& fingerprint);
// The native owner must recheck physical consent/generation in admit immediately
// before persistence. Neither cloud ownership nor notification opt-in can approve.
BondStatus Approve(const BondFingerprint& fingerprint, const std::function<bool()>& admit);
BondStatus Revoke(const BondFingerprint& fingerprint);
// Only for successful native clear-all-bonds or explicit local reset.
BondStatus RevokeAll();
}
