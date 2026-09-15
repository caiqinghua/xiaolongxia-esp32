#include "watch_voice_binding.h"
#include "watch_voice_cloud.h"

#include "sdkconfig.h"
#include "nvs.h"
#if CONFIG_NVS_ENCRYPTION && CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC
#include "esp_flash_encrypt.h"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <iterator>
#include <mutex>

namespace watch_voice {
namespace {
constexpr char kNamespace[] = "audream_voice";
constexpr char kRecordKey[] = "binding_v1";
constexpr size_t kDeviceOffset = 12;
constexpr size_t kCredentialOffset = kDeviceOffset + 36;
constexpr size_t kRecordSize = kCredentialOffset + 128;
std::mutex storage_mutex;
std::atomic<uint64_t> active_binding_generation{0};
bool uncertain_write = false;
bool uncertain_receipt_write = false;
constexpr char kReceiptKey[] = "settings_v1";
constexpr size_t kReceiptSize = 64;
constexpr std::string_view kReceiptErrors[] = {
    "", "unsupported_mode", "unsupported_voice", "application_failed", "local_disabled"
};

struct Record {
    std::array<uint8_t, kRecordSize> bytes{};
    ~Record() {
        volatile uint8_t* cursor = bytes.data();
        for (size_t i = 0; i < bytes.size(); ++i) cursor[i] = 0;
    }
};

struct Handle {
    nvs_handle_t value = 0;
    bool opened = false;
    ~Handle() { if (opened) nvs_close(value); }
    bool Open(nvs_open_mode_t mode) {
        opened = nvs_open(kNamespace, mode, &value) == ESP_OK;
        return opened;
    }
};

bool ProtectedStorage() {
    // app_main initializes the default partition with nvs_flash_init(), which
    // selects encrypted NVS under these flags. Never burn keys/eFuses here.
#if CONFIG_NVS_ENCRYPTION && CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC
    return esp_flash_encryption_enabled();
#else
    return false;
#endif
}

BindingStatus LoadUnlocked(Binding& result) {
    result = {};
    if (uncertain_write) return BindingStatus::kStorageError;
    Handle handle;
    const auto opened = nvs_open(kNamespace, NVS_READONLY, &handle.value);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return BindingStatus::kAbsent;
    if (opened != ESP_OK) return BindingStatus::kStorageError;
    handle.opened = true;
    size_t size = 0;
    auto error = nvs_get_blob(handle.value, kRecordKey, nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) return BindingStatus::kAbsent;
    if (error != ESP_OK || size != kRecordSize) return BindingStatus::kStorageError;
    // Do not read credential bytes from an unprotected deployment.
    if (!ProtectedStorage()) return BindingStatus::kProtectionRequired;
    Record record;
    error = nvs_get_blob(handle.value, kRecordKey, record.bytes.data(), &size);
    const auto& data = record.bytes;
    if (error != ESP_OK || size != data.size() || data[0] != 1 || data[3] != 0 ||
        (data[1] != 1 && data[1] != 2)) return BindingStatus::kStorageError;
    uint64_t generation = 0;
    for (size_t i = 4; i < kDeviceOffset; ++i) generation = (generation << 8) | data[i];
    if (!generation) return BindingStatus::kStorageError;
    if (data[1] == 2) {
        if (data[2] != 0 || !std::all_of(data.begin() + kDeviceOffset, data.end(),
                                        [](uint8_t byte) { return byte == 0; })) {
            return BindingStatus::kStorageError;
        }
        result.generation = generation;
        return BindingStatus::kRemoved;
    }
    const auto length = data[2];
    if (length < 47 || length > 128 ||
        !std::all_of(data.begin() + kCredentialOffset + length, data.end(),
                     [](uint8_t byte) { return byte == 0; })) return BindingStatus::kStorageError;
    std::string device(reinterpret_cast<const char*>(data.data() + kDeviceOffset), 36);
    std::string credential(reinterpret_cast<const char*>(data.data() + kCredentialOffset), length);
    if (!ValidDeviceId(device) || !ValidCredential(credential)) return BindingStatus::kStorageError;
    result = {generation, std::move(device), std::move(credential)};
    return BindingStatus::kBound;
}

BindingStatus Write(uint64_t expected, const std::string& device, const std::string& credential,
                    bool remove, const std::function<bool()>& admit = {}) {
    std::lock_guard<std::mutex> lock(storage_mutex);
    if (!ProtectedStorage()) return BindingStatus::kProtectionRequired;
    Binding previous;
    const auto status = LoadUnlocked(previous);
    if (status != BindingStatus::kBound && status != BindingStatus::kRemoved &&
        status != BindingStatus::kAbsent) return status;
    if (previous.generation != expected || expected == std::numeric_limits<uint64_t>::max()) {
        return BindingStatus::kConflict;
    }
    Record record;
    auto& data = record.bytes;
    data[0] = 1;
    data[1] = remove ? 2 : 1;
    data[2] = static_cast<uint8_t>(credential.size());
    auto generation = expected + 1;
    for (size_t i = kDeviceOffset; i > 4;) {
        data[--i] = static_cast<uint8_t>(generation & 0xff);
        generation >>= 8;
    }
    std::copy(device.begin(), device.end(), data.begin() + kDeviceOffset);
    std::copy(credential.begin(), credential.end(), data.begin() + kCredentialOffset);
    Handle handle;
    if (!handle.Open(NVS_READWRITE)) return BindingStatus::kStorageError;
    if (admit && !admit()) return BindingStatus::kCancelled;
    active_binding_generation.store(0);
    auto error = nvs_set_blob(handle.value, kRecordKey, data.data(), data.size());
    if (error == ESP_OK) error = nvs_commit(handle.value);
    if (error != ESP_OK) {
        // NVS may have changed before an error is reported. Do not use either
        // old or new credentials again until reboot and authoritative reload.
        uncertain_write = true;
        return BindingStatus::kStorageError;
    }
    active_binding_generation.store(remove ? 0 : expected + 1);
    return remove ? BindingStatus::kRemoved : BindingStatus::kBound;
}

uint64_t Read64(const uint8_t* data) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value = (value << 8) | data[i];
    return value;
}

void Write64(uint8_t* data, uint64_t value) {
    for (size_t i = 8; i > 0;) {
        data[--i] = static_cast<uint8_t>(value & 0xff);
        value >>= 8;
    }
}

ReceiptStatus CheckReceiptBinding(const Binding& expected) {
    if (!expected.generation || !ValidDeviceId(expected.device_id)) return ReceiptStatus::kInvalid;
    if (!ProtectedStorage()) return ReceiptStatus::kProtectionRequired;
    Binding current;
    const auto status = LoadUnlocked(current);
    if (status == BindingStatus::kStorageError) return ReceiptStatus::kStorageError;
    if (status != BindingStatus::kBound || current.device_id != expected.device_id ||
        current.generation != expected.generation || current.credential != expected.credential) {
        return ReceiptStatus::kConflict;
    }
    return ReceiptStatus::kOk;
}

ReceiptStatus ReadReceipt(const Binding& binding, SettingsReceipt& result) {
    if (uncertain_receipt_write) return ReceiptStatus::kStorageError;
    Handle handle;
    if (!handle.Open(NVS_READONLY)) return ReceiptStatus::kStorageError;
    size_t size = 0;
    auto status = nvs_get_blob(handle.value, kReceiptKey, nullptr, &size);
    if (status == ESP_ERR_NVS_NOT_FOUND) return ReceiptStatus::kAbsent;
    if (status != ESP_OK || size != kReceiptSize) return ReceiptStatus::kStorageError;
    std::array<uint8_t, kReceiptSize> data{};
    status = nvs_get_blob(handle.value, kReceiptKey, data.data(), &size);
    if (status != ESP_OK || size != data.size() || (data[0] != 1 && data[0] != 2) || data[1] >= std::size(kReceiptErrors) ||
        data[2] > 1 || (data[0] == 1 && data[3] != 0) || (data[3] & 0xf0) ||
        (data[3] & 3) > 2 || ((data[3] >> 2) & 3) > 2) return ReceiptStatus::kStorageError;
    const auto generation = Read64(data.data() + 4);
    const std::string device(reinterpret_cast<const char*>(data.data() + 12), 36);
    const auto revision = Read64(data.data() + 48);
    const auto applied = Read64(data.data() + 56);
    if (!generation || !ValidDeviceId(device) || !revision || applied > revision ||
        (data[1] == 0 && (applied != revision || (data[3] & 3) != (data[3] >> 2))) ||
        (!applied && (data[3] >> 2) != 0)) return ReceiptStatus::kStorageError;
    if (generation != binding.generation || device != binding.device_id) return ReceiptStatus::kAbsent;
    result = {revision, applied, data[2] != 0, std::string(kReceiptErrors[data[1]])};
    constexpr const char* modes[] = {"watch_standby", "battery_saver", "always_available"};
    result.power_mode = modes[data[3] & 3];
    result.applied_power_mode = modes[data[3] >> 2];
    return ReceiptStatus::kOk;
}
}  // namespace

BindingStatus LoadBinding(Binding& result) {
    std::lock_guard<std::mutex> lock(storage_mutex);
    const auto status = LoadUnlocked(result);
    active_binding_generation.store(status == BindingStatus::kBound ? result.generation : 0);
    return status;
}

bool WithBoundIdentity(const std::function<bool(std::string_view, uint64_t)>& verifier) {
    return WithBoundDevice([&](const Binding& binding) {
        return verifier && verifier(binding.device_id, binding.generation);
    });
}

bool WithBoundDevice(const std::function<bool(const Binding&)>& verifier) {
    std::lock_guard<std::mutex> lock(storage_mutex);
    Binding current;
    const auto status = LoadUnlocked(current);
    active_binding_generation.store(status == BindingStatus::kBound ? current.generation : 0);
    return status == BindingStatus::kBound && verifier && verifier(current);
}

uint64_t ActiveBindingGeneration() { return active_binding_generation.load(); }

BindingStatus SaveBinding(uint64_t expected, const std::string& device, const std::string& credential) {
    if (!ValidDeviceId(device) || !ValidCredential(credential)) return BindingStatus::kInvalid;
    return Write(expected, device, credential, false);
}

BindingStatus SaveBindingAuthorized(uint64_t expected, const std::string& device,
                                    const std::string& credential, const std::function<bool()>& admit) {
    if (!admit || !ValidDeviceId(device) || !ValidCredential(credential)) return BindingStatus::kInvalid;
    return Write(expected, device, credential, false, admit);
}

BindingStatus RemoveBinding(uint64_t expected) { return Write(expected, {}, {}, true); }
BindingStatus RemoveBindingAuthorized(uint64_t expected, const std::function<bool()>& admit) {
    if (!admit) return BindingStatus::kInvalid;
    return Write(expected, {}, {}, true, admit);
}

ReceiptStatus LoadSettingsReceipt(const Binding& binding, SettingsReceipt& result) {
    result = {};
    std::lock_guard<std::mutex> lock(storage_mutex);
    const auto status = CheckReceiptBinding(binding);
    return status == ReceiptStatus::kOk ? ReadReceipt(binding, result) : status;
}

ReceiptStatus SaveSettingsReceipt(const Binding& binding, uint64_t revision, bool enabled,
                                  std::string_view error, SettingsReceipt& result, std::string_view power_mode) {
    result = {};
    const auto found = std::find(std::begin(kReceiptErrors), std::end(kReceiptErrors), error);
    constexpr std::string_view modes[] = {"watch_standby", "battery_saver", "always_available"};
    const auto mode = std::find(std::begin(modes), std::end(modes), power_mode);
    if (!revision || found == std::end(kReceiptErrors) || mode == std::end(modes)) return ReceiptStatus::kInvalid;
    std::lock_guard<std::mutex> lock(storage_mutex);
    const auto bound = CheckReceiptBinding(binding);
    if (bound != ReceiptStatus::kOk) return bound;
    SettingsReceipt previous;
    const auto loaded = ReadReceipt(binding, previous);
    if (loaded != ReceiptStatus::kOk && loaded != ReceiptStatus::kAbsent) return loaded;
    if (revision < previous.revision) return ReceiptStatus::kConflict;
    // An observed revision cannot change its settings; its application result may change.
    if (revision == previous.revision && (enabled != previous.enabled || power_mode != previous.power_mode))
        return ReceiptStatus::kConflict;
    const SettingsReceipt next{revision, error.empty() ? revision : previous.applied_revision, enabled, std::string(error),
        std::string(power_mode), error.empty() ? std::string(power_mode) : previous.applied_power_mode};
    if (loaded == ReceiptStatus::kOk && next.revision == previous.revision &&
        next.applied_revision == previous.applied_revision && next.enabled == previous.enabled &&
        next.error == previous.error && next.power_mode == previous.power_mode && next.applied_power_mode == previous.applied_power_mode) {
        result = next;
        return ReceiptStatus::kOk;
    }
    std::array<uint8_t, kReceiptSize> data{};
    data[0] = 2;
    data[1] = static_cast<uint8_t>(found - std::begin(kReceiptErrors));
    data[2] = enabled ? 1 : 0;
    const auto applied_mode = std::find(std::begin(modes), std::end(modes), next.applied_power_mode);
    data[3] = static_cast<uint8_t>((mode - std::begin(modes)) | ((applied_mode - std::begin(modes)) << 2));
    Write64(data.data() + 4, binding.generation);
    std::copy(binding.device_id.begin(), binding.device_id.end(), data.begin() + 12);
    Write64(data.data() + 48, next.revision);
    Write64(data.data() + 56, next.applied_revision);
    Handle handle;
    if (!handle.Open(NVS_READWRITE)) return ReceiptStatus::kStorageError;
    auto status = nvs_set_blob(handle.value, kReceiptKey, data.data(), data.size());
    if (status == ESP_OK) status = nvs_commit(handle.value);
    if (status != ESP_OK) {
        // Do not infer which revision reached flash after an uncertain write.
        // Credentials were not changed and remain independently usable.
        uncertain_receipt_write = true;
        return ReceiptStatus::kStorageError;
    }
    result = next;
    return ReceiptStatus::kOk;
}

}  // namespace watch_voice
