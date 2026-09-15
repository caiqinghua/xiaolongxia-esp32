#include "watch/watch_control_bond.h"
#include "esp_gap_ble_api.h"
#include "nvs.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace watch_control;
std::vector<esp_ble_bond_dev_t> bonds;
std::map<std::string, std::vector<uint8_t>> records;
std::map<std::string, std::vector<uint8_t>> pending;
bool protected_storage = true, fail_open = false, fail_read = false, fail_set = false, fail_commit = false;
bool fail_list = false, change_count = false;
unsigned count_calls = 0, handles = 0, commits = 0;
esp_ble_bond_dev_t NativeBond() {
    esp_ble_bond_dev_t bond{};
    bond.bd_addr_type = BLE_ADDR_TYPE_RANDOM;
    std::copy_n("\x41\x02\x03\x04\x05\x06", 6, bond.bd_addr);
    bond.bond_key.key_mask = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    auto& enc = bond.bond_key.penc_key;
    enc.key_size = 16; enc.sec_level = 4;
    for (size_t i = 0; i < 16; ++i) enc.ltk[i] = i + 1;
    auto& id = bond.bond_key.pid_key;
    id.addr_type = BLE_ADDR_TYPE_PUBLIC;
    std::copy_n("\x01\x02\x03\x04\x05\x06", 6, id.static_addr);
    for (size_t i = 0; i < 16; ++i) id.irk[i] = 32 + i;
    return bond;
}
BondFingerprint Find(const uint8_t* address) {
    BondFingerprint result{};
    assert(FindNativeBond(address, result) == BondStatus::kFound);
    assert(std::any_of(result.begin(), result.end(), [](uint8_t byte) { return byte != 0; }));
    return result;
}
void TestNativeIdentity() {
    const auto original = NativeBond();
    bonds = {original};
    const auto fingerprint = Find(original.bd_addr);
    constexpr BondFingerprint expected{0xf6,0xd2,0x3d,0x31,0xa2,0xc3,0xc3,0x20,
        0x1b,0xad,0x72,0xef,0x52,0xe5,0x24,0x4d,0x21,0xe9,0x1b,0x1c,0xcb,0x24,0xae,0xa1,
        0x53,0xad,0x07,0x56,0xc9,0xf6,0x44,0x20};
    assert(fingerprint == expected);
    assert(Find(original.bond_key.pid_key.static_addr) == fingerprint);
    bonds[0].bd_addr[0] ^= 1;
    assert(Find(bonds[0].bd_addr) == fingerprint);
    bonds[0].bond_key.penc_key.ltk[0] ^= 1;
    assert(Find(bonds[0].bd_addr) != fingerprint);
    bonds[0] = original;
    bonds[0].bond_key.pid_key.irk[0] ^= 1;
    assert(Find(bonds[0].bd_addr) != fingerprint);
    for (int invalid = 0; invalid < 6; ++invalid) {
        bonds = {original};
        if (invalid == 0) bonds[0].bond_key.penc_key.key_size = 15;
        if (invalid == 1) bonds[0].bond_key.penc_key.sec_level = 1;
        if (invalid == 2) bonds[0].bond_key.key_mask = ESP_BLE_ID_KEY_MASK;
        if (invalid == 3) std::memset(bonds[0].bond_key.penc_key.ltk, 0, 16);
        if (invalid == 4) std::memset(bonds[0].bond_key.pid_key.irk, 0, 16);
        if (invalid == 5) bonds[0].bond_key.pid_key.addr_type = static_cast<esp_ble_addr_type_t>(0xff);
        BondFingerprint result{}; result.fill(0xff);
        assert(FindNativeBond(original.bd_addr, result) == BondStatus::kInvalidBond);
        assert(result == BondFingerprint{});
    }
    bonds = {original, original};
    BondFingerprint result{};
    assert(FindNativeBond(original.bd_addr, result) == BondStatus::kAmbiguous);
    bonds = {original}; fail_list = true;
    assert(FindNativeBond(original.bd_addr, result) == BondStatus::kUnavailable);
    fail_list = false; count_calls = 0; change_count = true;
    assert(FindNativeBond(original.bd_addr, result) == BondStatus::kUnavailable);
    change_count = false;
    bonds.resize(33);
    assert(FindNativeBond(original.bd_addr, result) == BondStatus::kUnavailable);
    bonds.clear();
    assert(FindNativeBond(original.bd_addr, result) == BondStatus::kAbsent);
    assert(FindNativeBond(nullptr, result) == BondStatus::kInvalidBond);
    bonds = {original};
}
void TestStorage(const BondFingerprint& fingerprint) {
    assert(ReadApproval(fingerprint) == BondStatus::kAbsent);
    assert(Approve(fingerprint, {}) == BondStatus::kCancelled);
    assert(Approve(fingerprint, [] { return false; }) == BondStatus::kCancelled);
    assert(records.empty() && !handles && !commits);
    assert(Approve(fingerprint, [] { return true; }) == BondStatus::kApproved);
    assert(ReadApproval(fingerprint) == BondStatus::kApproved);
    const auto original = bonds[0];
    bonds[0].bond_key.penc_key.ltk[0] ^= 1;
    assert(ReadApproval(Find(bonds[0].bd_addr)) == BondStatus::kAbsent);
    bonds[0] = original;
    bonds[0].bond_key.pid_key.irk[0] ^= 1;
    assert(ReadApproval(Find(bonds[0].bd_addr)) == BondStatus::kAbsent);
    bonds[0] = original;
    assert(records.size() == 1 && commits == 1);
    assert(Approve(fingerprint, [] { return true; }) == BondStatus::kApproved && commits == 1);
    assert(Approve(fingerprint, [] { return false; }) == BondStatus::kCancelled);
    const auto record = records.begin()->second;
    assert(record.size() == 40);
    auto collision = fingerprint; collision[31] ^= 1;
    assert(ReadApproval(collision) == BondStatus::kStorageError);
    assert(Approve(collision, [] { return true; }) == BondStatus::kStorageError);
    assert(Revoke(collision) == BondStatus::kStorageError);
    assert(records.begin()->second == record);
    for (size_t offset : {size_t(0), size_t(4), size_t(5), size_t(8)}) {
        records.begin()->second = record;
        records.begin()->second[offset] ^= 0x80;
        assert(ReadApproval(fingerprint) == BondStatus::kStorageError);
        assert(Approve(fingerprint, [] { return true; }) == BondStatus::kStorageError);
    }
    records.begin()->second = record;
    records.begin()->second.push_back(0);
    assert(ReadApproval(fingerprint) == BondStatus::kStorageError);
    records.begin()->second = record;
    protected_storage = false;
    assert(ReadApproval(fingerprint) == BondStatus::kProtectionRequired);
    assert(Revoke(fingerprint) == BondStatus::kProtectionRequired);
    assert(RevokeAll() == BondStatus::kProtectionRequired);
    protected_storage = true;
    fail_open = true; assert(ReadApproval(fingerprint) == BondStatus::kStorageError); fail_open = false;
    fail_read = true; assert(ReadApproval(fingerprint) == BondStatus::kStorageError); fail_read = false;
    assert(Revoke(fingerprint) == BondStatus::kRemoved);
    assert(ReadApproval(fingerprint) == BondStatus::kRemoved);
    assert(Revoke(fingerprint) == BondStatus::kRemoved && commits == 2);
    assert(Approve(fingerprint, [] { return true; }) == BondStatus::kApproved);
    assert(ReadApproval(BondFingerprint{}) == BondStatus::kInvalidBond);
    assert(RevokeAll() == BondStatus::kRemoved && records.empty());
    assert(ReadApproval(fingerprint) == BondStatus::kAbsent);
    assert(!handles);
}
void TestApprovedListing(const BondFingerprint& fingerprint) {
    std::vector<StoredControlBond> listed(1);
    assert(ListApprovedBonds(listed) == BondStatus::kAbsent && listed.empty());
    assert(Approve(fingerprint, [] { return true; }) == BondStatus::kApproved);
    assert(ListApprovedBonds(listed) == BondStatus::kFound && listed.size() == 1);
    assert(listed[0].fingerprint == fingerprint);
    assert(std::equal(listed[0].address.begin(), listed[0].address.end(), bonds[0].bd_addr));
    protected_storage = false;
    assert(ListApprovedBonds(listed) == BondStatus::kProtectionRequired && listed.empty());
    protected_storage = true;
    fail_list = true;
    listed.resize(1);
    assert(ListApprovedBonds(listed) == BondStatus::kUnavailable && listed.empty());
    fail_list = false;
    fail_read = true;
    assert(ListApprovedBonds(listed) == BondStatus::kStorageError && listed.empty());
    fail_read = false;
    count_calls = 0; change_count = true;
    assert(ListApprovedBonds(listed) == BondStatus::kUnavailable && listed.empty());
    change_count = false;
    const auto original = bonds;
    bonds.push_back(bonds[0]);
    assert(ListApprovedBonds(listed) == BondStatus::kAmbiguous && listed.empty());
    bonds.resize(33);
    assert(ListApprovedBonds(listed) == BondStatus::kUnavailable && listed.empty());
    bonds = original;
    assert(Revoke(fingerprint) == BondStatus::kRemoved);
    assert(ListApprovedBonds(listed) == BondStatus::kAbsent && listed.empty());
    assert(RevokeAll() == BondStatus::kRemoved);
    assert(records.empty() && !handles);
}
}

int esp_ble_get_bond_device_num() {
    if (change_count && ++count_calls > 1) return bonds.size() + 1;
    return bonds.size();
}
esp_err_t esp_ble_get_bond_device_list(int* count, esp_ble_bond_dev_t* output) {
    if (fail_list) return ESP_FAIL;
    *count = std::min(*count, static_cast<int>(bonds.size()));
    std::copy_n(bonds.begin(), *count, output);
    return ESP_OK;
}
bool esp_flash_encryption_enabled() { return protected_storage; }
esp_err_t nvs_open(const char* name, nvs_open_mode_t, nvs_handle_t* handle) {
    assert(std::strcmp(name, "audream_ble") == 0);
    if (fail_open) return ESP_FAIL;
    *handle = ++handles;
    return ESP_OK;
}
void nvs_close(nvs_handle_t) { assert(handles); --handles; pending.clear(); }
esp_err_t nvs_get_blob(nvs_handle_t, const char* key, void* output, size_t* size) {
    assert(std::strlen(key) == 15);
    if (fail_read) return ESP_FAIL;
    const auto found = records.find(key);
    if (found == records.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (output && *size < found->second.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    *size = found->second.size();
    if (output) std::memcpy(output, found->second.data(), *size);
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char* key, const void* input, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(input);
    pending[key] = {bytes, bytes + size};
    if (fail_set) { records[key] = pending[key]; return ESP_FAIL; }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
    ++commits;
    for (const auto& record : pending) records[record.first] = record.second;
    return fail_commit ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t) { records.clear(); pending.clear(); return ESP_OK; }
int main(int argc, char** argv) {
    TestNativeIdentity();
    const auto fingerprint = Find(bonds[0].bd_addr);
#if !CONFIG_NVS_ENCRYPTION
    (void)argc; (void)argv;
    assert(ReadApproval(fingerprint) == BondStatus::kProtectionRequired);
    assert(Approve(fingerprint, [] { return true; }) == BondStatus::kProtectionRequired);
    assert(Revoke(fingerprint) == BondStatus::kProtectionRequired);
    std::vector<StoredControlBond> listed(1);
    assert(ListApprovedBonds(listed) == BondStatus::kProtectionRequired && listed.empty());
    assert(records.empty());
    // Keep this function type-checked in the unprotected configuration.
    (void)&TestStorage;
    (void)&TestApprovedListing;
#else
    if (argc > 1) {
        const std::string failure(argv[1]);
        const bool revoke = failure.find("revoke") != std::string::npos;
        if (revoke) assert(Approve(fingerprint, [] { return true; }) == BondStatus::kApproved);
        fail_set = failure == "set" || failure == "set-revoke";
        fail_commit = failure == "commit" || failure == "commit-revoke";
        assert(fail_set || fail_commit);
        assert((revoke ? Revoke(fingerprint) : Approve(fingerprint, [] { return true; })) == BondStatus::kStorageError);
        fail_set = fail_commit = false;
        assert(ReadApproval(fingerprint) == BondStatus::kStorageError);
        assert(Revoke(fingerprint) == BondStatus::kStorageError);
        assert(Approve(fingerprint, [] { return true; }) == BondStatus::kStorageError);
        std::vector<StoredControlBond> listed(1);
        assert(ListApprovedBonds(listed) == BondStatus::kStorageError && listed.empty());
        assert(!handles);
    } else {
        TestStorage(fingerprint);
        TestApprovedListing(fingerprint);
    }
#endif
    std::puts("PASS: native bond identity, authenticated key fingerprint, protected approval storage and write-error denial");
}
