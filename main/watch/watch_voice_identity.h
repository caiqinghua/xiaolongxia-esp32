#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace watch_voice {

enum class IdentityStatus { kOk, kAbsent, kProtectionRequired, kStorageError, kInvalid };
struct DeviceIdentity {
    std::string device_id;
    std::string hardware_id;
    std::array<uint8_t, 32> public_key{};
};

// Factory records are read-only at runtime. No private bytes leave this module.
// Call on an owned worker with the caller's storage/power reservation, not LVGL/GATT callbacks.
IdentityStatus LoadDeviceIdentity(DeviceIdentity& result);

class PairingSession;
class DevicePairingSigner {
    friend class PairingSession;
    static bool Sign(const DeviceIdentity& expected, std::string_view payload, std::array<uint8_t, 64>& proof);
};

}  // namespace watch_voice
