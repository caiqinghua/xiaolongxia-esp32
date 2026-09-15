#include "watch_control_bond.h"
#include "esp_gap_ble_api.h"
#include "nvs.h"
#include "sdkconfig.h"
#include <sodium.h>
#if CONFIG_NVS_ENCRYPTION && CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC
#include "esp_flash_encrypt.h"
#endif
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>

namespace watch_control {
namespace {
constexpr size_t kMaxBonds = 32;
constexpr size_t kRecordSize = 40;
// ESP-IDF 5.5.4 stack/smp_api.h: SMP_SEC_AUTHENTICATED.
constexpr uint8_t kAuthenticated = 1 << 2;
std::mutex storage_mutex;
bool uncertain_write = false;

bool Nonzero(const uint8_t* bytes, size_t length) {
    return std::any_of(bytes, bytes + length, [](uint8_t byte) { return byte != 0; });
}
auto NativeAddressType(const esp_ble_addr_type_t& type) {
    // The C SDK can write 0xff on storage failure, outside the C++ enum range.
    std::underlying_type_t<esp_ble_addr_type_t> raw = 0;
    std::memcpy(&raw, &type, sizeof(raw));
    return raw;
}
struct NativeBonds {
    explicit NativeBonds(size_t count) : count(count), values(new (std::nothrow) esp_ble_bond_dev_t[count]{}) {}
    size_t count;
    std::unique_ptr<esp_ble_bond_dev_t[]> values;
    ~NativeBonds() { if (values) sodium_memzero(values.get(), count * sizeof(esp_ble_bond_dev_t)); }
};
bool Fingerprint(const esp_ble_bond_dev_t& bond, BondFingerprint& result) {
    const auto& keys = bond.bond_key;
    if (!(keys.key_mask & ESP_BLE_ENC_KEY_MASK) || keys.penc_key.key_size != 16 ||
        keys.penc_key.sec_level != kAuthenticated || !Nonzero(keys.penc_key.ltk, 16)) return false;
    const bool identity = keys.key_mask & ESP_BLE_ID_KEY_MASK;
    const auto type = NativeAddressType(identity ? keys.pid_key.addr_type : bond.bd_addr_type);
    const auto* address = identity ? keys.pid_key.static_addr : bond.bd_addr;
    if ((type != BLE_ADDR_TYPE_PUBLIC && type != BLE_ADDR_TYPE_RANDOM) || !Nonzero(address, 6) ||
        (identity && !Nonzero(keys.pid_key.irk, 16))) return false;
    crypto_hash_sha256_state hash;
    crypto_hash_sha256_init(&hash);
    constexpr unsigned char domain[] = "Audream control bond v1";
    crypto_hash_sha256_update(&hash, domain, sizeof(domain) - 1);
    const uint8_t flags[]{static_cast<uint8_t>(type), static_cast<uint8_t>(identity)};
    crypto_hash_sha256_update(&hash, flags, sizeof(flags));
    crypto_hash_sha256_update(&hash, address, 6);
    crypto_hash_sha256_update(&hash, keys.penc_key.ltk, 16);
    if (identity) crypto_hash_sha256_update(&hash, keys.pid_key.irk, 16);
    crypto_hash_sha256_final(&hash, result.data());
    sodium_memzero(&hash, sizeof(hash));
    return true;
}
bool ProtectedStorage() {
#if CONFIG_NVS_ENCRYPTION && CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC
    return esp_flash_encryption_enabled();
#else
    return false;
#endif
}
std::array<char, 16> Key(const BondFingerprint& fingerprint) {
    // NVS keys are at most 15 bytes. The full fingerprint is checked in the blob;
    // a prefix collision fails closed instead of overwriting another approval.
    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 16> key{};
    key[0] = 'c';
    for (size_t i = 0; i < 7; ++i) {
        key[1 + 2 * i] = digits[fingerprint[i] >> 4];
        key[2 + 2 * i] = digits[fingerprint[i] & 15];
    }
    return key;
}
struct Handle {
    nvs_handle_t value = 0;
    bool open = false;
    ~Handle() { if (open) nvs_close(value); }
};
BondStatus ReadUnlocked(const BondFingerprint& fingerprint) {
    if (!Nonzero(fingerprint.data(), fingerprint.size())) return BondStatus::kInvalidBond;
    if (uncertain_write) return BondStatus::kStorageError;
    if (!ProtectedStorage()) return BondStatus::kProtectionRequired;
    Handle handle;
    auto status = nvs_open("audream_ble", NVS_READONLY, &handle.value);
    if (status == ESP_ERR_NVS_NOT_FOUND) return BondStatus::kAbsent;
    if (status != ESP_OK) return BondStatus::kStorageError;
    handle.open = true;
    const auto key = Key(fingerprint);
    size_t size = 0;
    status = nvs_get_blob(handle.value, key.data(), nullptr, &size);
    if (status == ESP_ERR_NVS_NOT_FOUND) return BondStatus::kAbsent;
    if (status != ESP_OK || size != kRecordSize) return BondStatus::kStorageError;
    std::array<uint8_t, kRecordSize> record{};
    status = nvs_get_blob(handle.value, key.data(), record.data(), &size);
    if (status != ESP_OK || size != record.size() ||
        std::memcmp(record.data(), "ACB1", 4) != 0 || (record[4] != 1 && record[4] != 2) ||
        record[5] || record[6] || record[7] ||
        sodium_memcmp(record.data() + 8, fingerprint.data(), fingerprint.size()) != 0)
        return BondStatus::kStorageError;
    return record[4] == 1 ? BondStatus::kApproved : BondStatus::kRemoved;
}
BondStatus Write(const BondFingerprint& fingerprint, bool approve, const std::function<bool()>& admit) {
    std::lock_guard<std::mutex> lock(storage_mutex);
    const auto previous = ReadUnlocked(fingerprint);
    if (previous != BondStatus::kAbsent && previous != BondStatus::kApproved && previous != BondStatus::kRemoved)
        return previous;
    if (approve && !admit) return BondStatus::kCancelled;
    Handle handle;
    if (nvs_open("audream_ble", NVS_READWRITE, &handle.value) != ESP_OK) return BondStatus::kStorageError;
    handle.open = true;
    if (approve && !admit()) return BondStatus::kCancelled;
    const auto desired = approve ? BondStatus::kApproved : BondStatus::kRemoved;
    if (previous == desired) return desired;
    std::array<uint8_t, kRecordSize> record{};
    std::memcpy(record.data(), "ACB1", 4);
    record[4] = approve ? 1 : 2;
    std::copy(fingerprint.begin(), fingerprint.end(), record.begin() + 8);
    const auto key = Key(fingerprint);
    auto result = nvs_set_blob(handle.value, key.data(), record.data(), record.size());
    if (result == ESP_OK) result = nvs_commit(handle.value);
    if (result != ESP_OK) {
        // A failed write can still have changed NVS. Deny both old and new grants
        // for this boot until authoritative storage is reloaded after restart.
        uncertain_write = true;
        return BondStatus::kStorageError;
    }
    return desired;
}
}

BondStatus FindNativeBond(const uint8_t* address, BondFingerprint& fingerprint) {
    std::array<uint8_t, 6> stored_address{};
    return FindNativeBond(address, fingerprint, stored_address);
}
BondStatus FindNativeBond(const uint8_t* address, BondFingerprint& fingerprint,
                          std::array<uint8_t, 6>& stored_address) {
    fingerprint = {};
    stored_address = {};
    if (!address || !Nonzero(address, 6)) return BondStatus::kInvalidBond;
    const int count = esp_ble_get_bond_device_num();
    if (count < 0 || count > static_cast<int>(kMaxBonds)) return BondStatus::kUnavailable;
    if (!count) return BondStatus::kAbsent;
    NativeBonds bonds(count);
    if (!bonds.values) return BondStatus::kUnavailable;
    int copied = count;
    if (esp_ble_get_bond_device_list(&copied, bonds.values.get()) != ESP_OK || copied != count ||
        esp_ble_get_bond_device_num() != count) return BondStatus::kUnavailable;
    const esp_ble_bond_dev_t* found = nullptr;
    for (int i = 0; i < copied; ++i) {
        const auto& bond = bonds.values[i];
        const bool identity_match = (bond.bond_key.key_mask & ESP_BLE_ID_KEY_MASK) &&
            std::equal(address, address + 6, bond.bond_key.pid_key.static_addr);
        if (!identity_match && !std::equal(address, address + 6, bond.bd_addr)) continue;
        if (found) return BondStatus::kAmbiguous;
        found = &bond;
    }
    if (!found) return BondStatus::kAbsent;
    if (!Fingerprint(*found, fingerprint) || !Nonzero(found->bd_addr, 6)) return BondStatus::kInvalidBond;
    std::copy_n(found->bd_addr, stored_address.size(), stored_address.begin());
    return BondStatus::kFound;
}
BondStatus ReadApproval(const BondFingerprint& fingerprint) {
    std::lock_guard<std::mutex> lock(storage_mutex);
    return ReadUnlocked(fingerprint);
}
BondStatus ListApprovedBonds(std::vector<StoredControlBond>& output) {
    output.clear();
    if (!ProtectedStorage()) return BondStatus::kProtectionRequired;
    const int count = esp_ble_get_bond_device_num();
    if (count < 0 || count > static_cast<int>(kMaxBonds)) return BondStatus::kUnavailable;
    if (!count) return BondStatus::kAbsent;
    NativeBonds native(count);
    if (!native.values) return BondStatus::kUnavailable;
    int copied = count;
    if (esp_ble_get_bond_device_list(&copied, native.values.get()) != ESP_OK || copied != count ||
        esp_ble_get_bond_device_num() != count) return BondStatus::kUnavailable;
    std::vector<StoredControlBond> approved;
    for (int index = 0; index < count; ++index) {
        const auto& value = native.values[index];
        StoredControlBond bond;
        if (!Fingerprint(value, bond.fingerprint) || !Nonzero(value.bd_addr, 6)) return BondStatus::kInvalidBond;
        std::copy_n(value.bd_addr, bond.address.size(), bond.address.begin());
        BondFingerprint verified{};
        std::array<uint8_t, 6> address{};
        const auto found = FindNativeBond(bond.address.data(), verified, address);
        if (found != BondStatus::kFound) return found == BondStatus::kAbsent ? BondStatus::kUnavailable : found;
        if (verified != bond.fingerprint || address != bond.address) return BondStatus::kUnavailable;
        const auto approval = ReadApproval(bond.fingerprint);
        if (approval == BondStatus::kAbsent || approval == BondStatus::kRemoved) continue;
        if (approval != BondStatus::kApproved) return approval;
        if (std::any_of(approved.begin(), approved.end(), [&](const auto& prior) {
                return prior.address == bond.address || prior.fingerprint == bond.fingerprint;
            })) return BondStatus::kAmbiguous;
        approved.push_back(bond);
    }
    if (esp_ble_get_bond_device_num() != count) return BondStatus::kUnavailable;
    output = std::move(approved);
    return output.empty() ? BondStatus::kAbsent : BondStatus::kFound;
}
BondStatus Approve(const BondFingerprint& fingerprint, const std::function<bool()>& admit) {
    return Write(fingerprint, true, admit);
}
BondStatus Revoke(const BondFingerprint& fingerprint) { return Write(fingerprint, false, {}); }
BondStatus RevokeAll() {
    std::lock_guard<std::mutex> lock(storage_mutex);
    if (uncertain_write) return BondStatus::kStorageError;
    if (!ProtectedStorage()) return BondStatus::kProtectionRequired;
    Handle handle;
    if (nvs_open("audream_ble", NVS_READWRITE, &handle.value) != ESP_OK) return BondStatus::kStorageError;
    handle.open = true;
    auto status = nvs_erase_all(handle.value);
    if (status == ESP_OK) status = nvs_commit(handle.value);
    if (status != ESP_OK) { uncertain_write = true; return BondStatus::kStorageError; }
    return BondStatus::kRemoved;
}
}
