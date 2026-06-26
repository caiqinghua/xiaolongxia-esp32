#include "system/device_time_service.h"

#include "esp_attr.h"
#include "esp_log.h"
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
constexpr char kRtcEpochSeqKey[] = "rtc_epoch_seq";
constexpr uint64_t kMinimumValidUnixTimeMs = 1600000000000ULL;
constexpr uint32_t kRtcEpochMagic = 0xAD0E2026;

RTC_DATA_ATTR uint32_t s_rtc_epoch_magic = 0;
RTC_DATA_ATTR uint64_t s_rtc_epoch_seq = 0;

}  // namespace

DeviceTimeService& DeviceTimeService::GetInstance() {
    static DeviceTimeService instance;
    return instance;
}

void DeviceTimeService::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureRtcEpochSeqLocked();

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
    uint64_t rtc_epoch_seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureRtcEpochSeqLocked();
        rtc_epoch_seq = rtc_epoch_seq_;
        synced_unix_time_ms_ = unix_time_ms;
        synced_rtc_time_us_ = esp_rtc_get_time_us();
        synchronized_ = true;
        PersistLocked();
        ESP_LOGI(kTag, "Device time synchronized");
    }
    SdCardStorage::GetInstance().BackfillUnsynchronizedRecordingTimes(
        unix_time_ms,
        synced_rtc_time_ms,
        rtc_epoch_seq
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

uint64_t DeviceTimeService::RtcEpochSeq() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rtc_epoch_seq_;
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
        result = nvs_set_u64(handle, kRtcEpochSeqKey, rtc_epoch_seq_);
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

bool DeviceTimeService::PersistRtcEpochSeqLocked() const {
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Failed to open RTC epoch sequence storage: %s", esp_err_to_name(result));
        return false;
    }

    result = nvs_set_u64(handle, kRtcEpochSeqKey, rtc_epoch_seq_);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);

    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Failed to persist RTC epoch sequence: %s", esp_err_to_name(result));
        return false;
    }
    return true;
}

void DeviceTimeService::EnsureRtcEpochSeqLocked() {
    if (rtc_epoch_seq_ > 0) {
        return;
    }

    if (s_rtc_epoch_magic == kRtcEpochMagic && s_rtc_epoch_seq > 0) {
        rtc_epoch_seq_ = s_rtc_epoch_seq;
        ESP_LOGI(
            kTag,
            "RTC epoch sequence restored from RTC memory: %llu",
            static_cast<unsigned long long>(rtc_epoch_seq_)
        );
        return;
    }

    uint64_t stored_epoch_seq = 0;
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (result == ESP_OK) {
        result = nvs_get_u64(handle, kRtcEpochSeqKey, &stored_epoch_seq);
        nvs_close(handle);
    }
    if (result != ESP_OK) {
        stored_epoch_seq = 0;
    }

    rtc_epoch_seq_ = stored_epoch_seq + 1;
    s_rtc_epoch_seq = rtc_epoch_seq_;
    s_rtc_epoch_magic = kRtcEpochMagic;
    PersistRtcEpochSeqLocked();
    ESP_LOGI(
        kTag,
        "RTC epoch sequence initialized: %llu",
        static_cast<unsigned long long>(rtc_epoch_seq_)
    );
}
