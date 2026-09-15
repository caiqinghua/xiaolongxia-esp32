#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace watch_voice {

enum class BindingStatus { kAbsent, kBound, kRemoved, kStorageError, kProtectionRequired,
                           kConflict, kInvalid, kCancelled };

struct Binding {
    uint64_t generation = 0;
    std::string device_id;
    std::string credential;
};

// Only kAbsent permits legacy configuration. A removal leaves a tombstone.
BindingStatus LoadBinding(Binding& result);
// Call only after authenticated provisioning, with a generation from LoadBinding.
// These synchronous operations need the caller's storage/power reservation.
BindingStatus SaveBinding(uint64_t expected_generation, const std::string& device_id,
                          const std::string& credential);
// Final admission runs under the storage lock, immediately before the first
// write. It must be bounded, must not call binding APIs, and must not do I/O.
// Once admitted, cancellation cannot undo a write that may already be durable.
BindingStatus SaveBindingAuthorized(uint64_t expected_generation, const std::string& device_id,
                                    const std::string& credential,
                                    const std::function<bool()>& admit);
BindingStatus RemoveBinding(uint64_t expected_generation);
BindingStatus RemoveBindingAuthorized(uint64_t expected_generation, const std::function<bool()>& admit);

// Runs a bounded verifier while rebind/removal is excluded. The callback must
// not call another binding operation; it receives no device credential.
bool WithBoundIdentity(const std::function<bool(std::string_view, uint64_t)>& verifier);
// For bounded device-possession proof only. Do not retain or log the credential.
bool WithBoundDevice(const std::function<bool(const Binding&)>& verifier);
// Memory-only invalidation fence for already admitted private-data sessions.
uint64_t ActiveBindingGeneration();

enum class ReceiptStatus { kAbsent, kOk, kConflict, kStorageError, kProtectionRequired, kInvalid };

struct SettingsReceipt {
    uint64_t revision = 0;
    uint64_t applied_revision = 0;
    bool enabled = false;
    std::string error;
    std::string power_mode = "watch_standby";
    std::string applied_power_mode = "watch_standby";
};

// Historical application results, never an authorization or a microphone permit.
// Both operations revalidate the binding under the same storage mutex as rebind.
ReceiptStatus LoadSettingsReceipt(const Binding& binding, SettingsReceipt& result);
ReceiptStatus SaveSettingsReceipt(const Binding& binding, uint64_t revision, bool enabled,
                                  std::string_view error, SettingsReceipt& result,
                                  std::string_view power_mode = "watch_standby");

}  // namespace watch_voice
