#include "control_bond_fixture.h"
#include "nvs.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace bond_fixture {
std::vector<esp_ble_bond_dev_t> bonds;
std::function<void()> on_list;
std::function<void()> on_commit;
bool fail_commit = false;
unsigned handles = 0;
std::map<std::string, std::vector<uint8_t>> records, pending;
void Reset(const uint8_t* peer) {
    assert(!handles);
    records.clear(); pending.clear(); on_list = {}; on_commit = {}; fail_commit = false;
    esp_ble_bond_dev_t bond{};
    std::copy_n(peer, 6, bond.bd_addr);
    bond.bd_addr_type = BLE_ADDR_TYPE_RANDOM;
    bond.bond_key.key_mask = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    bond.bond_key.penc_key.key_size = 16;
    bond.bond_key.penc_key.sec_level = 4;
    bond.bond_key.pid_key.addr_type = BLE_ADDR_TYPE_PUBLIC;
    std::copy_n(peer, 6, bond.bond_key.pid_key.static_addr);
    for (unsigned i = 0; i < 16; ++i) {
        bond.bond_key.penc_key.ltk[i] = i + 1;
        bond.bond_key.pid_key.irk[i] = i + 32;
    }
    bonds = {bond};
}
}
int esp_ble_get_bond_device_num() { return bond_fixture::bonds.size(); }
esp_err_t esp_ble_get_bond_device_list(int* count, esp_ble_bond_dev_t* output) {
    *count = std::min(*count, static_cast<int>(bond_fixture::bonds.size()));
    std::copy_n(bond_fixture::bonds.begin(), *count, output);
    if (auto action = std::exchange(bond_fixture::on_list, {})) action();
    return ESP_OK;
}
bool esp_flash_encryption_enabled() { return true; }
esp_err_t nvs_open(const char* name, nvs_open_mode_t, nvs_handle_t* handle) {
    assert(std::strcmp(name, "audream_ble") == 0);
    *handle = ++bond_fixture::handles;
    return ESP_OK;
}
void nvs_close(nvs_handle_t) { assert(bond_fixture::handles); --bond_fixture::handles; bond_fixture::pending.clear(); }
esp_err_t nvs_get_blob(nvs_handle_t, const char* key, void* output, size_t* size) {
    const auto found = bond_fixture::records.find(key);
    if (found == bond_fixture::records.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (output && *size < found->second.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    *size = found->second.size();
    if (output) std::memcpy(output, found->second.data(), *size);
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char* key, const void* input, size_t size) {
    const auto* data = static_cast<const uint8_t*>(input);
    bond_fixture::pending[key] = {data, data + size};
    return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t) { bond_fixture::records.clear(); bond_fixture::pending.clear(); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t) {
    for (const auto& item : bond_fixture::pending) bond_fixture::records[item.first] = item.second;
    if (auto action = std::exchange(bond_fixture::on_commit, {})) action();
    return bond_fixture::fail_commit ? ESP_FAIL : ESP_OK;
}
