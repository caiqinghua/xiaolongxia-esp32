#include "system/device_time_service.h"

#include <cstdio>
#include <cstring>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "storage/sd_card_storage.h"

namespace {

constexpr char kTag[] = "DeviceTime";
constexpr char kNamespace[] = "audream_time";
constexpr char kUnixTimeMsKey[] = "unix_ms";
constexpr char kRtcTimeUsKey[] = "rtc_us";
constexpr uint64_t kMinimumValidUnixTimeMs = 1600000000000ULL;
constexpr uint32_t kRtcEpochMagic = 0xAD0E2026;

RTC_DATA_ATTR uint32_t s_rtc_epoch_magic = 0;
RTC_DATA_ATTR char s_rtc_epoch_id[33] = {};

std::string BuildRtcEpochID() {
    uint8_t bytes[16] = {};
    esp_fill_random(bytes, sizeof(bytes));

    uint8_t mac[6] = {};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        for (size_t i = 0; i < sizeof(mac); ++i) {
            bytes[i % sizeof(bytes)] ^= mac[i];
        }
    }

    const uint64_t rtc_time_us = esp_rtc_get_time_us();
    for (size_t i = 0; i < sizeof(rtc_time_us); ++i) {
        bytes[i % sizeof(bytes)] ^= static_cast<uint8_t>((rtc_time_us >> (i * 8)) & 0xff);
    }

    char buffer[33] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
        static_cast<unsigned int>(bytes[0]),
        static_cast<unsigned int>(bytes[1]),
        static_cast<unsigned int>(bytes[2]),
        static_cast<unsigned int>(bytes[3]),
        static_cast<unsigned int>(bytes[4]),
        static_cast<unsigned int>(bytes[5]),
        static_cast<unsigned int>(bytes[6]),
        static_cast<unsigned int>(bytes[7]),
        static_cast<unsigned int>(bytes[8]),
        static_cast<unsigned int>(bytes[9]),
        static_cast<unsigned int>(bytes[10]),
        static_cast<unsigned int>(bytes[11]),
        static_cast<unsigned int>(bytes[12]),
        static_cast<unsigned int>(bytes[13]),
        static_cast<unsigned int>(bytes[14]),
        static_cast<unsigned int>(bytes[15])
    );
    return std::string(buffer);
}

}  // namespace

DeviceTimeService& DeviceTimeService::GetInstance() {
    static DeviceTimeService instance;
    return instance;
}

void DeviceTimeService::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureRtcEpochIDLocked();

    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        ESP_LOGI(kTag, "No persisted device time is available");
        return;
    }

    uint64_t stored_unix_time_ms = 0;
    uint64_t stored_rtc_time_us = 0;
    result = nvs_get_u64(handle, kUnixTimeMsKey, &stored_unix_time_ms);
    if (result == ESP_OK) {
        result = nvs_get_u64(handle, kRtcTimeUsKey, &stored_rtc_time_us);
    }
    nvs_close(handle);

    if (result != ESP_OK || stored_unix_time_ms < kMinimumValidUnixTimeMs) {
        ESP_LOGI(kTag, "Persisted device time is not valid");
        return;
    }

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(kTag, "Skipping persisted device time on cold boot");
        return;
    }

    const uint64_t current_rtc_time_us = esp_rtc_get_time_us();
    if (current_rtc_time_us < stored_rtc_time_us) {
        ESP_LOGW(kTag, "Persisted device time is older than the current RTC base");
        return;
    }

    synced_unix_time_ms_ = stored_unix_time_ms;
    synced_rtc_time_us_ = stored_rtc_time_us;
    synchronized_ = true;
    ESP_LOGI(kTag, "Restored persisted device time after sleep wakeup");
}

void DeviceTimeService::SyncUnixTimeMs(uint64_t unix_time_ms) {
    const uint64_t synced_rtc_time_ms = esp_rtc_get_time_us() / 1000;
    std::string rtc_epoch_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureRtcEpochIDLocked();
        rtc_epoch_id = rtc_epoch_id_;
        synced_unix_time_ms_ = unix_time_ms;
        synced_rtc_time_us_ = esp_rtc_get_time_us();
        synchronized_ = true;
        PersistLocked();
        ESP_LOGI(kTag, "Device time synchronized");
    }
    SdCardStorage::GetInstance().BackfillUnsynchronizedRecordingTimes(
        unix_time_ms,
        synced_rtc_time_ms,
        rtc_epoch_id
    );
}

bool DeviceTimeService::IsSynchronized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return synchronized_;
}

uint64_t DeviceTimeService::CurrentUnixTimeMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synchronized_) {
        return 0;
    }
    const uint64_t current_rtc_time_us = esp_rtc_get_time_us();
    if (current_rtc_time_us <= synced_rtc_time_us_) {
        return synced_unix_time_ms_;
    }
    const uint64_t elapsed_us = current_rtc_time_us - synced_rtc_time_us_;
    return synced_unix_time_ms_ + static_cast<uint64_t>(elapsed_us / 1000);
}

std::string DeviceTimeService::RtcEpochID() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rtc_epoch_id_;
}

bool DeviceTimeService::PersistLocked() const {
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Failed to open device time storage: %s", esp_err_to_name(result));
        return false;
    }

    result = nvs_set_u64(handle, kUnixTimeMsKey, synced_unix_time_ms_);
    if (result == ESP_OK) {
        result = nvs_set_u64(handle, kRtcTimeUsKey, synced_rtc_time_us_);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);

    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Failed to persist device time: %s", esp_err_to_name(result));
        return false;
    }
    return true;
}

void DeviceTimeService::EnsureRtcEpochIDLocked() {
    if (!rtc_epoch_id_.empty()) {
        return;
    }

    if (s_rtc_epoch_magic != kRtcEpochMagic ||
        strnlen(s_rtc_epoch_id, sizeof(s_rtc_epoch_id)) != 32) {
        const std::string generated_id = BuildRtcEpochID();
        std::memset(s_rtc_epoch_id, 0, sizeof(s_rtc_epoch_id));
        std::memcpy(s_rtc_epoch_id, generated_id.c_str(), generated_id.size());
        s_rtc_epoch_magic = kRtcEpochMagic;
    }

    rtc_epoch_id_ = std::string(s_rtc_epoch_id);
    ESP_LOGI(kTag, "RTC epoch ID initialized");
}
