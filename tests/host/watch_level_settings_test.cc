#include "watch/watch_level_settings.h"
#include "watch/watch_voice_settings.h"
#include "watch/watch_notification_settings.h"
#include "watch/watch_wifi_settings.h"
#include "audio/audio_codec.h"
#include "settings.h"
#include "nvs.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>

namespace {
std::map<std::string, int32_t> disk;
std::map<std::string, int32_t> pending;
std::string current;
int open_error = 0, read_error = 0, write_error = 0, commit_error = 0;
int opens = 0, closes = 0, commits = 0;
bool writable = false;
class TestCodec : public AudioCodec {
    int Read(int16_t*, int count) override { return count; }
    int Write(const int16_t*, int count) override { return count; }
};
}

int nvs_open(const char* name, int mode, nvs_handle_t* handle) {
    if (open_error) return open_error;
    assert(current.empty());
    current = name;
    writable = mode == NVS_READWRITE;
    pending.clear();
    *handle = 1;
    ++opens;
    return ESP_OK;
}
int nvs_get_i32(nvs_handle_t handle, const char* key, int32_t* value) {
    assert(handle == 1 && !current.empty());
    if (read_error) return read_error;
    auto it = disk.find(current + "/" + key);
    if (it == disk.end()) return ESP_ERR_NVS_NOT_FOUND;
    *value = it->second;
    return ESP_OK;
}
int nvs_set_i32(nvs_handle_t handle, const char* key, int32_t value) {
    assert(handle == 1 && writable);
    if (write_error) return write_error;
    pending[current + "/" + key] = value;
    return ESP_OK;
}
int nvs_commit(nvs_handle_t handle) {
    assert(handle == 1 && writable);
    ++commits;
    if (commit_error) return commit_error;
    for (const auto& entry : pending) disk[entry.first] = entry.second;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) {
    assert(handle == 1 && !current.empty());
    current.clear();
    pending.clear();
    ++closes;
}

int main() {
    using Level = WatchLevel;
    assert(ReadWatchLevel(Level::kBrightness) == 75);
    assert(ReadWatchLevel(Level::kVolume) == 70);
    assert(commits == 0);
    open_error = ESP_ERR_NVS_NOT_FOUND;
    assert(ReadWatchLevel(Level::kBrightness) == 75);
    open_error = 0;
    disk["display/brightness"] = 44;
    disk["audio/output_volume"] = 33;
    assert(ReadWatchLevel(Level::kBrightness) == 44);
    assert(ReadWatchLevel(Level::kVolume) == 33);
    std::puts("PASS: watch controls reuse legacy keys without writes during load");

    assert(SaveWatchLevel(Level::kBrightness, 10));
    assert(SaveWatchLevel(Level::kVolume, 0));
    assert(ReadWatchLevel(Level::kVolume) == 0);
    assert(ReadWatchLevel(Level::kBrightness) == 10);
    assert(SaveWatchLevel(Level::kBrightness, 100));
    assert(SaveWatchLevel(Level::kVolume, 100));
    const auto writes = commits;
    assert(!SaveWatchLevel(Level::kBrightness, 0));
    assert(!SaveWatchLevel(Level::kBrightness, 9));
    assert(!SaveWatchLevel(Level::kVolume, -1));
    assert(!SaveWatchLevel(Level::kVolume, 101));
    assert(!SaveWatchLevel(static_cast<Level>(99), 50));
    assert(ReadWatchLevel(static_cast<Level>(99)) == -1);
    assert(commits == writes);
    std::puts("PASS: brightness floor and mute values are validated before persistence");

    disk["audio/output_volume"] = 500;
    disk["display/brightness"] = -1;
    assert(ReadWatchLevel(Level::kVolume) == 70);
    assert(ReadWatchLevel(Level::kBrightness) == 75);
    assert(commits == writes);
    std::puts("PASS: invalid legacy values do not trigger implicit writes");

    open_error = 7;
    assert(ReadWatchLevel(Level::kBrightness) == -1);
    assert(!SaveWatchLevel(Level::kBrightness, 50));
    open_error = 0;
    read_error = 7;
    assert(ReadWatchLevel(Level::kBrightness) == -1);
    read_error = 0;
    write_error = 7;
    assert(!SaveWatchLevel(Level::kBrightness, 50));
    assert(commits == writes);
    write_error = 0;
    commit_error = 7;
    assert(!SaveWatchLevel(Level::kBrightness, 50));
    commit_error = 0;
    assert(opens == closes);
    assert(SaveWatchLevel(Level::kBrightness, 50));
    assert(ReadWatchLevel(Level::kBrightness) == 50);
    std::puts("PASS: NVS open read write and commit failures propagate without leaked handles");

    TestCodec codec;
    assert(SaveWatchLevel(Level::kVolume, 0));
    codec.Start();
    assert(codec.output_volume() == 0);
    assert(!codec.output_enabled() && !codec.input_enabled());
    assert(SaveWatchLevel(Level::kVolume, 42));
    codec.Start();
    assert(codec.output_volume() == 42);
    read_error = 7;
    codec.Start();
    assert(codec.output_volume() == 0);
    read_error = 0;
    for (auto profile : {BoardRuntimeProfile::kAssistant, BoardRuntimeProfile::kRecordingOnly}) {
        Board::profile = profile;
        Settings::legacy_volume = 0;
        codec.Start();
        assert(codec.output_volume() == 10);
        Settings::legacy_volume = 66;
        codec.Start();
        assert(codec.output_volume() == 66);
    }
    std::puts("PASS: actual codec startup preserves watch mute and legacy restoration behavior");

    assert(ReadWatchVoiceEnabled() == 0);
    assert(SaveWatchVoiceEnabled(true));
    assert(ReadWatchVoiceEnabled() == 1);
    assert(SaveWatchVoiceEnabled(false));
    assert(ReadWatchVoiceEnabled() == 0);
    disk["audream_watch/voice_enabled"] = 2;
    assert(ReadWatchVoiceEnabled() == -1);
    std::puts("PASS: voice permission is opt in and rejects corrupt enabled values");

    open_error = 7;
    assert(ReadWatchVoiceEnabled() == -1);
    assert(!SaveWatchVoiceEnabled(true));
    open_error = 0;
    read_error = 7;
    assert(ReadWatchVoiceEnabled() == -1);
    read_error = 0;
    write_error = 7;
    assert(!SaveWatchVoiceEnabled(true));
    write_error = 0;
    commit_error = 7;
    assert(!SaveWatchVoiceEnabled(false));
    commit_error = 0;
    assert(SaveWatchVoiceEnabled(false));
    assert(ReadWatchVoiceEnabled() == 0);
    assert(opens == closes);
    std::puts("PASS: voice permission propagates storage errors and supports retry");

    disk["wifi/ssid_count"] = 3;
    assert(ReadWatchNotificationsEnabled() == 0);
    open_error = ESP_ERR_NVS_NOT_FOUND;
    assert(ReadWatchNotificationsEnabled() == 0);
    open_error = 0;
    assert(SaveWatchNotificationsEnabled(true));
    assert(ReadWatchNotificationsEnabled() == 1);
    assert(SaveWatchNotificationsEnabled(false));
    assert(ReadWatchNotificationsEnabled() == 0);
    disk["audream_watch/notify_enabled"] = 2;
    assert(ReadWatchNotificationsEnabled() == -1);
    open_error = 7;
    assert(ReadWatchNotificationsEnabled() == -1);
    assert(!SaveWatchNotificationsEnabled(true));
    open_error = 0;
    read_error = 7;
    assert(ReadWatchNotificationsEnabled() == -1);
    read_error = 0;
    write_error = 7;
    assert(!SaveWatchNotificationsEnabled(true));
    write_error = 0;
    assert(SaveWatchNotificationsEnabled(true));
    commit_error = 7;
    assert(!SaveWatchNotificationsEnabled(false));
    commit_error = 0;
    assert(ReadWatchNotificationsEnabled() == 1);
    assert(SaveWatchNotificationsEnabled(false));
    assert(ReadWatchNotificationsEnabled() == 0 && opens == closes);
    std::puts("PASS: notifications require persisted opt in and report every NVS failure");

    assert(ReadWatchWifiEnabled() == 1);
    assert(SaveWatchWifiEnabled(false));
    assert(ReadWatchWifiEnabled() == 0);
    assert(disk["wifi/ssid_count"] == 3);
    assert(SaveWatchWifiEnabled(true));
    assert(ReadWatchWifiEnabled() == 1);
    std::puts("PASS: WiFi preference preserves upgrade behavior and existing credential keys");

    disk["audream_watch/wifi_enabled"] = 2;
    assert(ReadWatchWifiEnabled() == -1);
    open_error = ESP_ERR_NVS_NOT_FOUND;
    assert(ReadWatchWifiEnabled() == 1);
    open_error = 7;
    assert(ReadWatchWifiEnabled() == -1);
    assert(!SaveWatchWifiEnabled(false));
    open_error = 0;
    read_error = 7;
    assert(ReadWatchWifiEnabled() == -1);
    read_error = 0;
    write_error = 7;
    assert(!SaveWatchWifiEnabled(false));
    write_error = 0;
    commit_error = 7;
    assert(!SaveWatchWifiEnabled(false));
    commit_error = 0;
    assert(SaveWatchWifiEnabled(false));
    assert(ReadWatchWifiEnabled() == 0 && opens == closes);
    std::puts("PASS: WiFi storage failures and corrupt values remain explicit and retryable");
}
