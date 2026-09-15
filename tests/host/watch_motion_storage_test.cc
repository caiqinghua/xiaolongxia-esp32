#include "watch/watch_motion_storage.h"
#include "system/audream_power_manager.h"
#include "nvs.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
std::vector<uint8_t> committed;
std::vector<uint8_t> wrist_committed;
std::vector<uint8_t>& Blob(const char* key) {
    return std::strcmp(key, "wrist_v1") == 0 ? wrist_committed : committed;
}
unsigned handles = 0, opens = 0, sets = 0, commits = 0;
bool fail_open = false, fail_read = false, fail_set = false, fail_commit = false;
void Reserved() {
    const auto& power = AudreamPowerManager::GetInstance();
    assert(power.reservations == 1 || power.sleeping);
}
void CheckHandle(nvs_handle_t handle, const char* key = nullptr) {
    Reserved();
    assert(handle == 7 && handles == 1);
    if (key) assert(std::strcmp(key, "activity_v1") == 0 || std::strcmp(key, "wrist_v1") == 0);
}
}  // namespace

extern "C" esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
    Reserved(); ++opens;
    assert(std::strcmp(name, "audream_watch") == 0);
    assert(mode == NVS_READWRITE || mode == NVS_READONLY);
    if (fail_open) return ESP_FAIL;
    if (mode == NVS_READONLY && committed.empty() && wrist_committed.empty()) return ESP_ERR_NVS_NOT_FOUND;
    ++handles; *handle = 7; return ESP_OK;
}
extern "C" void nvs_close(nvs_handle_t handle) { CheckHandle(handle); --handles; }
extern "C" esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* value, size_t* size) {
    CheckHandle(handle, key);
    assert(value && size && *size == (std::strcmp(key, "wrist_v1") == 0 ? 16 : 64));
    if (fail_read) { static_cast<uint8_t*>(value)[0] = 0xff; return ESP_FAIL; }
    const auto& blob = Blob(key);
    if (blob.empty()) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < blob.size()) { *size = blob.size(); return ESP_ERR_NVS_INVALID_LENGTH; }
    *size = blob.size(); std::memcpy(value, blob.data(), blob.size()); return ESP_OK;
}
extern "C" esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t size) {
    CheckHandle(handle, key); ++sets;
    assert(size == (std::strcmp(key, "wrist_v1") == 0 ? 16 : 64));
    if (fail_set) return ESP_FAIL;
    const auto* bytes = static_cast<const uint8_t*>(value);
    Blob(key).assign(bytes, bytes + size);
    return ESP_OK;
}
extern "C" esp_err_t nvs_commit(nvs_handle_t handle) {
    CheckHandle(handle); ++commits;
    // Simulate an ambiguous error after NVS has already changed the record.
    return fail_commit ? ESP_FAIL : ESP_OK;
}

int main() {
    auto& power = AudreamPowerManager::GetInstance();
    WatchActivityRecord output{};
    output.fill(0x5a);
    const auto sentinel = output;
    assert(LoadWatchActivityRecord(output, false) == WatchMotionLoad::kAbsent);
    assert(output == sentinel && power.reservations == 0 && handles == 0);
    power.allowed = false;
    const auto opened = opens;
    assert(LoadWatchActivityRecord(output, false) == WatchMotionLoad::kBusy);
    assert(!SaveWatchActivityRecord(output, false));
    assert(opens == opened && power.reservations == 0);
    power.allowed = true;
    const auto record = *EncodeWatchActivityCheckpoint({});
    assert(SaveWatchActivityRecord(record, false));
    assert(LoadWatchActivityRecord(output, false) == WatchMotionLoad::kLoaded && output == record);
    fail_open = true;
    assert(LoadWatchActivityRecord(output, false) == WatchMotionLoad::kFailed);
    assert(!SaveWatchActivityRecord(record, false));
    fail_open = false;
    fail_read = true;
    assert(LoadWatchActivityRecord(output, false) == WatchMotionLoad::kFailed && output == record);
    fail_read = false;
    for (size_t size : {63, 65}) {
        committed.resize(size);
        assert(LoadWatchActivityRecord(output, false) == WatchMotionLoad::kCorrupt && output == record);
    }
    fail_set = true;
    const auto before = commits;
    assert(!SaveWatchActivityRecord(record, false) && commits == before);
    fail_set = false;
    fail_commit = true;
    assert(!SaveWatchActivityRecord(record, false));
    fail_commit = false;
    assert(SaveWatchActivityRecord(record, false));
    power.allowed = false;
    power.sleeping = true;
    const auto acquired = power.acquires;
    assert(SaveWatchActivityRecord(record, true));
    assert(LoadWatchActivityRecord(output, true) == WatchMotionLoad::kLoaded && output == record);
    assert(power.acquires == acquired && power.reservations == 0 && handles == 0);
    power.sleeping = false;
    power.allowed = true;
    const auto activity_before = committed;
    WatchWristPreferences preferences;
    assert(LoadWatchWristPreferences(preferences) == WatchWristLoad::kAbsent && !preferences.enabled);
    preferences = {true, std::array<float, 3>{0, 0, -1}};
    assert(SaveWatchWristPreferences(preferences));
    WatchWristPreferences restored;
    assert(LoadWatchWristPreferences(restored) == WatchWristLoad::kLoaded && restored.enabled && restored.normal);
    assert(committed == activity_before);
    fail_open = true;
    assert(LoadWatchWristPreferences(restored) == WatchWristLoad::kFailed && !SaveWatchWristPreferences(preferences));
    fail_open = false; fail_read = true;
    assert(LoadWatchWristPreferences(restored) == WatchWristLoad::kFailed && restored.enabled);
    fail_read = false; fail_set = true;
    assert(!SaveWatchWristPreferences(preferences));
    fail_set = false; fail_commit = true;
    assert(!SaveWatchWristPreferences(preferences));
    fail_commit = false;
    for (size_t size : {15, 17}) {
        wrist_committed.resize(size);
        assert(LoadWatchWristPreferences(restored) == WatchWristLoad::kFailed);
    }
    assert(SaveWatchWristPreferences(preferences)); wrist_committed[0] = 255;
    assert(LoadWatchWristPreferences(restored) == WatchWristLoad::kFailed && restored.enabled);
    power.allowed = false;
    const auto wrist_opens = opens;
    assert(LoadWatchWristPreferences(restored) == WatchWristLoad::kFailed && !SaveWatchWristPreferences(preferences));
    assert(opens == wrist_opens && handles == 0 && power.reservations == 0 && committed == activity_before);
    std::puts("Watch activity and wrist NVS: errors, reservations, isolation, and exclusive sleep checks passed");
}
