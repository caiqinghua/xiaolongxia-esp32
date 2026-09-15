#include "watch_voice_identity.h"
#include "watch_voice_cloud.h"
#include "sdkconfig.h"
#include "nvs.h"
#include <sodium.h>
#if CONFIG_NVS_ENCRYPTION && CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC
#include "esp_flash_encrypt.h"
#endif
#include <algorithm>
#include <mutex>

namespace watch_voice {
namespace {
constexpr size_t kHardwareOffset = 41, kPublicOffset = 169, kSeedOffset = 201, kRecordSize = 233;
std::mutex identity_mutex;
struct Secret {
    std::array<uint8_t, kRecordSize> record{};
    std::array<uint8_t, crypto_sign_SECRETKEYBYTES> key{};
    ~Secret() { sodium_memzero(record.data(), record.size()); sodium_memzero(key.data(), key.size()); }
};
struct Handle {
    nvs_handle_t value = 0;
    bool opened = false;
    ~Handle() { if (opened) nvs_close(value); }
};

IdentityStatus Read(DeviceIdentity& identity, Secret& secret) {
    identity = {};
#if CONFIG_NVS_ENCRYPTION && CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC
    if (!esp_flash_encryption_enabled()) return IdentityStatus::kProtectionRequired;
#else
    return IdentityStatus::kProtectionRequired;
#endif
    Handle handle;
    const auto opened = nvs_open("audream_ident", NVS_READONLY, &handle.value);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return IdentityStatus::kAbsent;
    if (opened != ESP_OK) return IdentityStatus::kStorageError;
    handle.opened = true;
    size_t size = 0;
    auto status = nvs_get_blob(handle.value, "identity_v1", nullptr, &size);
    if (status == ESP_ERR_NVS_NOT_FOUND) return IdentityStatus::kAbsent;
    if (status != ESP_OK || size != kRecordSize) return IdentityStatus::kStorageError;
    status = nvs_get_blob(handle.value, "identity_v1", secret.record.data(), &size);
    if (status != ESP_OK || size != kRecordSize) return IdentityStatus::kStorageError;
    const auto& data = secret.record;
    if (!std::equal(data.begin(), data.begin() + 4, "AID1") || !data[40] || data[40] > 128 ||
        !std::all_of(data.begin() + kHardwareOffset + data[40], data.begin() + kPublicOffset,
                     [](uint8_t byte) { return byte == 0; }) ||
        std::all_of(data.begin() + kSeedOffset, data.end(), [](uint8_t byte) { return byte == 0; }))
        return IdentityStatus::kInvalid;
    DeviceIdentity decoded;
    decoded.device_id.assign(reinterpret_cast<const char*>(data.data() + 4), 36);
    decoded.hardware_id.assign(reinterpret_cast<const char*>(data.data() + kHardwareOffset), data[40]);
    if (!ValidDeviceId(decoded.device_id) || !std::all_of(decoded.hardware_id.begin(), decoded.hardware_id.end(),
            [](unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == ':'; }))
        return IdentityStatus::kInvalid;
    if (sodium_init() < 0 || crypto_sign_seed_keypair(decoded.public_key.data(), secret.key.data(),
                                                    data.data() + kSeedOffset) != 0 ||
        sodium_memcmp(decoded.public_key.data(), data.data() + kPublicOffset, decoded.public_key.size()) != 0)
        return IdentityStatus::kInvalid;
    identity = std::move(decoded);
    return IdentityStatus::kOk;
}
}

IdentityStatus LoadDeviceIdentity(DeviceIdentity& result) {
    std::lock_guard<std::mutex> lock(identity_mutex);
    Secret secret;
    return Read(result, secret);
}

bool DevicePairingSigner::Sign(const DeviceIdentity& expected, std::string_view payload, std::array<uint8_t, 64>& proof) {
    proof.fill(0);
    if (payload.empty() || payload.size() > 1024) return false;
    std::lock_guard<std::mutex> lock(identity_mutex);
    Secret secret;
    DeviceIdentity current;
    if (Read(current, secret) != IdentityStatus::kOk || current.device_id != expected.device_id ||
        current.hardware_id != expected.hardware_id || current.public_key != expected.public_key) return false;
    unsigned long long size = 0;
    if (crypto_sign_detached(proof.data(), &size, reinterpret_cast<const uint8_t*>(payload.data()),
                             payload.size(), secret.key.data()) != 0 || size != proof.size()) {
        proof.fill(0);
        return false;
    }
    return true;
}

}  // namespace watch_voice
