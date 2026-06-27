#include "storage/sd_card_storage.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <sys/stat.h>
#include <sys/utime.h>
#include <sys/unistd.h>
#include <unordered_map>
#include <utility>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "system/audream_power_manager.h"

#ifndef AUDREAM_SD_CS_GPIO
#define AUDREAM_SD_CS_GPIO GPIO_NUM_NC
#endif

#ifndef AUDREAM_SD_MOSI_GPIO
#define AUDREAM_SD_MOSI_GPIO GPIO_NUM_NC
#endif

#ifndef AUDREAM_SD_MISO_GPIO
#define AUDREAM_SD_MISO_GPIO GPIO_NUM_NC
#endif

#ifndef AUDREAM_SD_CLK_GPIO
#define AUDREAM_SD_CLK_GPIO GPIO_NUM_NC
#endif

#ifndef AUDREAM_SD_SPI_HOST
#define AUDREAM_SD_SPI_HOST SDSPI_DEFAULT_HOST
#endif

#ifndef AUDREAM_SD_USE_SDMMC
#define AUDREAM_SD_USE_SDMMC 0
#endif

namespace {

constexpr char kTag[] = "AudreamStorage";
constexpr char kMountPoint[] = "/sdcard";
constexpr char kRecordingsDirectory[] = "/sdcard/audream/recordings";
constexpr uint32_t kEstimatedOpusBytesPerMinute = 180000;
constexpr uint32_t kInitializationTaskStackSize = 8192;
constexpr UBaseType_t kInitializationTaskPriority = 4;
constexpr TickType_t kStartupProbeDelay = pdMS_TO_TICKS(1000);
constexpr TickType_t kProbeRetryDelay = pdMS_TO_TICKS(500);
constexpr uint32_t kProbeMaxAttempts = 3;
constexpr time_t kMinimumReasonableRecordingMtime = 1704067200;
constexpr uint64_t kEpochSortStrideMs = 1000000000000ULL;

struct RecordingScanEntry {
    StoredRecordingInfo info;
    uint64_t sort_key = 0;
    time_t modified_at = 0;
    std::string audio_path;
    bool metadata_loaded = false;
};

class ScopedStorageBusy {
public:
    ScopedStorageBusy() {
        AudreamPowerManager::GetInstance().SetStorageBusy(true);
    }

    ~ScopedStorageBusy() {
        AudreamPowerManager::GetInstance().SetStorageBusy(false);
    }
};

bool EndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsSafeRecordingID(const std::string& recording_id) {
    if (recording_id.empty() || recording_id.size() > 96) {
        return false;
    }
    return std::all_of(recording_id.begin(), recording_id.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-';
    });
}

std::string BuildRecordingPath(const std::string& recording_id) {
    return std::string(kRecordingsDirectory) + "/" + recording_id + ".opus";
}

uint32_t FileSizeBytes(const std::string& path) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<uint32_t>(st.st_size);
}

std::string ReadTextFile(const std::string& path) {
    std::ifstream stream(path);
    if (!stream.good()) {
        return "";
    }
    return std::string(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>()
    );
}

uint64_t ReadJsonUInt64(const std::string& content, const std::string& key) {
    const std::string marker = "\"" + key + "\":";
    const size_t start = content.find(marker);
    if (start == std::string::npos) {
        return 0;
    }
    size_t cursor = start + marker.size();
    while (cursor < content.size() && std::isspace(static_cast<unsigned char>(content[cursor]))) {
        cursor++;
    }
    uint64_t value = 0;
    while (cursor < content.size() && std::isdigit(static_cast<unsigned char>(content[cursor]))) {
        value = value * 10 + static_cast<uint64_t>(content[cursor] - '0');
        cursor++;
    }
    return value;
}

uint64_t RecordingSortKey(const std::string& metadata_content, time_t modified_at) {
    uint64_t value = ReadJsonUInt64(metadata_content, "started_at_unix_ms");
    if (value > 0) {
        return value;
    }
    const uint64_t epoch_seq = ReadJsonUInt64(metadata_content, "rtc_epoch_seq");
    const uint64_t rtc_ms = ReadJsonUInt64(metadata_content, "started_at_rtc_ms");
    if (epoch_seq > 0 && rtc_ms > 0) {
        return epoch_seq * kEpochSortStrideMs + std::min(rtc_ms, kEpochSortStrideMs - 1);
    }
    if (rtc_ms > 0) {
        return rtc_ms;
    }
    value = ReadJsonUInt64(metadata_content, "started_at_uptime_ms");
    if (value > 0) {
        return value;
    }
    return static_cast<uint64_t>(modified_at) * 1000;
}

void PopulateRecordingInfo(
    const std::string& metadata_content,
    StoredRecordingInfo& info
) {
    info.duration_ms = static_cast<uint32_t>(
        ReadJsonUInt64(metadata_content, "duration_ms")
    );
    info.started_at_unix_ms = ReadJsonUInt64(
        metadata_content,
        "started_at_unix_ms"
    );
    info.started_at_uptime_ms = ReadJsonUInt64(
        metadata_content,
        "started_at_uptime_ms"
    );
    info.started_at_rtc_ms = ReadJsonUInt64(
        metadata_content,
        "started_at_rtc_ms"
    );
}

std::string ReadJsonString(const std::string& content, const std::string& key) {
    const std::string marker = "\"" + key + "\":\"";
    const size_t start = content.find(marker);
    if (start == std::string::npos) {
        return "";
    }
    const size_t value_start = start + marker.size();
    const size_t value_end = content.find('"', value_start);
    if (value_end == std::string::npos) {
        return "";
    }
    return content.substr(value_start, value_end - value_start);
}

bool JsonBoolValue(const std::string& content, const std::string& key) {
    const std::string marker = "\"" + key + "\":";
    const size_t start = content.find(marker);
    if (start == std::string::npos) {
        return false;
    }
    size_t cursor = start + marker.size();
    while (cursor < content.size() && std::isspace(static_cast<unsigned char>(content[cursor]))) {
        cursor++;
    }
    return content.compare(cursor, 4, "true") == 0;
}

bool WriteRecordingMetadata(
    const std::string& metadata_path,
    const std::string& recording_id,
    const std::string& file_name,
    uint64_t rtc_epoch_seq,
    uint32_t duration_ms,
    uint32_t size_bytes,
    uint64_t started_at_unix_ms,
    uint64_t started_at_uptime_ms,
    uint64_t started_at_rtc_ms,
    bool time_synchronized
) {
    FILE* file = fopen(metadata_path.c_str(), "w");
    if (file == nullptr) {
        ESP_LOGE(kTag, "Failed to open recording metadata file: %s", metadata_path.c_str());
        return false;
    }
    std::fprintf(
        file,
        "{"
        "\"recording_id\":\"%s\","
        "\"file_name\":\"%s\","
        "\"rtc_epoch_seq\":%s,"
        "\"duration_ms\":%lu,"
        "\"size_bytes\":%lu,"
        "\"started_at_unix_ms\":%s,"
        "\"started_at_uptime_ms\":%s,"
        "\"started_at_rtc_ms\":%s,"
        "\"time_synchronized\":%s,"
        "\"codec\":\"opus\","
        "\"container\":\"ogg\","
        "\"sample_rate\":16000,"
        "\"channels\":1,"
        "\"sync_state\":\"pending\""
        "}\n",
        recording_id.c_str(),
        file_name.c_str(),
        std::to_string(rtc_epoch_seq).c_str(),
        static_cast<unsigned long>(duration_ms),
        static_cast<unsigned long>(size_bytes),
        std::to_string(started_at_unix_ms).c_str(),
        std::to_string(started_at_uptime_ms).c_str(),
        std::to_string(started_at_rtc_ms).c_str(),
        time_synchronized ? "true" : "false"
    );
    fclose(file);
    return true;
}

bool ApplyRecordingMtime(
    const std::string& audio_path,
    const std::string& metadata_path,
    uint64_t started_at_unix_ms
) {
    if (started_at_unix_ms == 0) {
        return true;
    }
    const time_t timestamp = static_cast<time_t>(started_at_unix_ms / 1000);
    struct utimbuf times = {};
    times.actime = timestamp;
    times.modtime = timestamp;
    bool success = true;
    if (utime(audio_path.c_str(), &times) != 0) {
        ESP_LOGW(kTag, "Failed to update recording mtime: %s", audio_path.c_str());
        success = false;
    }
    if (utime(metadata_path.c_str(), &times) != 0) {
        ESP_LOGW(kTag, "Failed to update metadata mtime: %s", metadata_path.c_str());
        success = false;
    }
    return success;
}

uint64_t DirectorySizeBytes(const char* path) {
    DIR* dir = opendir(path);
    if (dir == nullptr) {
        return 0;
    }
    uint64_t total = 0;
    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        std::string child_path = std::string(path) + "/" + entry->d_name;
        struct stat st = {};
        if (stat(child_path.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            total += DirectorySizeBytes(child_path.c_str());
        } else {
            total += static_cast<uint64_t>(st.st_size);
        }
    }
    closedir(dir);
    return total;
}

bool EnsureDirectory(const char* path) {
    struct stat st = {};
    if (stat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            ESP_LOGE(kTag, "Storage path exists but is not a directory: %s", path);
        }
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(path, 0775) == 0) {
        ESP_LOGI(kTag, "Created storage directory: %s", path);
        return true;
    }
    ESP_LOGE(kTag, "Failed to create directory %s: %s", path, std::strerror(errno));
    return false;
}

}  // namespace

SdCardStorage& SdCardStorage::GetInstance() {
    static SdCardStorage instance;
    return instance;
}

void SdCardStorage::SetInitializationCompleteCallback(
    std::function<void(const StorageStatus&)> callback
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    initialization_complete_callback_ = std::move(callback);
}

void SdCardStorage::StartInitializationTask() {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (initialization_task_started_) {
            return;
        }
        initialization_task_started_ = true;
    }

    BaseType_t created = xTaskCreate(
        [](void*) {
            vTaskDelay(kStartupProbeDelay);
            ESP_LOGI(kTag, "Starting startup SD card initialization");
            auto status = SdCardStorage::GetInstance().ProbeAndMountStatus();
            if (status.mounted) {
                ESP_LOGI(
                    kTag,
                    "Startup SD card initialization completed, free_mb=%lu remaining_minutes=%lu",
                    static_cast<unsigned long>(status.free_bytes / (1024 * 1024)),
                    static_cast<unsigned long>(status.estimated_remaining_minutes)
                );
            } else {
                ESP_LOGW(
                    kTag,
                    "Startup SD card initialization failed, error_code=%lu",
                    static_cast<unsigned long>(status.error_code)
                );
            }
            std::function<void(const StorageStatus&)> callback;
            {
                auto& storage = SdCardStorage::GetInstance();
                std::lock_guard<std::recursive_mutex> lock(storage.mutex_);
                callback = storage.initialization_complete_callback_;
            }
            if (callback) {
                callback(status);
            }
            vTaskDelete(nullptr);
        },
        "audream_sd_init",
        kInitializationTaskStackSize,
        nullptr,
        kInitializationTaskPriority,
        nullptr
    );

    if (created != pdPASS) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        initialization_task_started_ = false;
        last_error_code_ = StorageStatusErrorCode::kMountFailed;
        ESP_LOGE(kTag, "Failed to create startup SD card initialization task");
    }
}

bool SdCardStorage::Mount() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (mounted_) {
        ESP_LOGD(kTag, "SD card is already mounted");
        return true;
    }
    last_error_code_ = StorageStatusErrorCode::kNone;
    if (AUDREAM_SD_MOSI_GPIO == GPIO_NUM_NC ||
        AUDREAM_SD_MISO_GPIO == GPIO_NUM_NC ||
        AUDREAM_SD_CLK_GPIO == GPIO_NUM_NC ||
        (!AUDREAM_SD_USE_SDMMC && AUDREAM_SD_CS_GPIO == GPIO_NUM_NC)) {
        last_error_code_ = StorageStatusErrorCode::kPinsNotConfigured;
        ready_ = false;
        cached_status_ = {};
        cached_status_.error_code = last_error_code_;
        ESP_LOGE(kTag, "SD card pins are not configured for this board");
        return false;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_card_t* card = nullptr;
#if AUDREAM_SD_USE_SDMMC
    ESP_LOGI(
        kTag,
        "Mounting SD card in SDMMC 1-bit mode with clk=%d cmd=%d d0=%d",
        AUDREAM_SD_CLK_GPIO,
        AUDREAM_SD_MOSI_GPIO,
        AUDREAM_SD_MISO_GPIO
    );

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = AUDREAM_SD_CLK_GPIO;
    slot_config.cmd = AUDREAM_SD_MOSI_GPIO;
    slot_config.d0 = AUDREAM_SD_MISO_GPIO;
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
    slot_config.d4 = GPIO_NUM_NC;
    slot_config.d5 = GPIO_NUM_NC;
    slot_config.d6 = GPIO_NUM_NC;
    slot_config.d7 = GPIO_NUM_NC;
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.width = 1;
    slot_config.flags = 0;

    ESP_LOGI(kTag, "Calling esp_vfs_fat_sdmmc_mount at %s", kMountPoint);
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot_config, &mount_config, &card);
#else
    ESP_LOGI(
        kTag,
        "Mounting SD card in SDSPI mode on host=%d with pins cs=%d mosi=%d miso=%d clk=%d",
        AUDREAM_SD_SPI_HOST,
        AUDREAM_SD_CS_GPIO,
        AUDREAM_SD_MOSI_GPIO,
        AUDREAM_SD_MISO_GPIO,
        AUDREAM_SD_CLK_GPIO
    );

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = AUDREAM_SD_SPI_HOST;
    auto spi_host = static_cast<spi_host_device_t>(host.slot);
    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = AUDREAM_SD_MOSI_GPIO;
    bus_config.miso_io_num = AUDREAM_SD_MISO_GPIO;
    bus_config.sclk_io_num = AUDREAM_SD_CLK_GPIO;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = 4000;

    esp_err_t ret = spi_bus_initialize(spi_host, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        last_error_code_ = StorageStatusErrorCode::kSpiBusInitFailed;
        ready_ = false;
        cached_status_ = {};
        cached_status_.error_code = last_error_code_;
        ESP_LOGE(
            kTag,
            "Failed to initialize SD SPI bus: %s (%d)",
            esp_err_to_name(ret),
            static_cast<int>(ret)
        );
        return false;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "SD SPI bus was already initialized");
    } else {
        ESP_LOGI(kTag, "SD SPI bus initialized on host slot %d", static_cast<int>(spi_host));
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = AUDREAM_SD_CS_GPIO;
    slot_config.host_id = spi_host;

    ESP_LOGI(kTag, "Calling esp_vfs_fat_sdspi_mount at %s", kMountPoint);
    ret = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot_config, &mount_config, &card);
#endif
    if (ret != ESP_OK) {
        last_error_code_ = StorageStatusErrorCode::kMountFailed;
        ready_ = false;
        cached_status_ = {};
        cached_status_.error_code = last_error_code_;
        ESP_LOGE(
            kTag,
            "Failed to mount SD card: %s (%d). Check card insertion, FAT32 formatting, power, and bus wiring.",
            esp_err_to_name(ret),
            static_cast<int>(ret)
        );
        return false;
    }

    mounted_ = true;
    last_error_code_ = StorageStatusErrorCode::kNone;
    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(kTag, "SD card mounted at %s", kMountPoint);
    return true;
}

bool SdCardStorage::IsMounted() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ready_;
}

StorageStatus SdCardStorage::GetStatus() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!cached_status_.mounted) {
        ESP_LOGD(
            kTag,
            "Cached storage status unavailable, error_code=%lu",
            static_cast<unsigned long>(cached_status_.error_code)
        );
    } else {
        ESP_LOGD(
            kTag,
            "Cached storage status mounted free_mb=%lu remaining_minutes=%lu",
            static_cast<unsigned long>(cached_status_.free_bytes / (1024 * 1024)),
            static_cast<unsigned long>(cached_status_.estimated_remaining_minutes)
        );
    }
    return cached_status_;
}

StorageStatus SdCardStorage::ProbeAndMountStatus() {
    ScopedStorageBusy storage_busy;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    StorageStatus status;
    for (uint32_t attempt = 1; attempt <= kProbeMaxAttempts; ++attempt) {
        status.mounted = Mount();
        if (status.mounted) {
            break;
        }
        if (attempt < kProbeMaxAttempts) {
            ESP_LOGW(
                kTag,
                "SD card mount attempt %lu failed, retrying after a short delay",
                static_cast<unsigned long>(attempt)
            );
            vTaskDelay(kProbeRetryDelay);
        }
    }
    status.error_code = last_error_code_;
    if (!status.mounted) {
        ready_ = false;
        cached_status_ = status;
        ESP_LOGW(
            kTag,
            "Storage probe failed, error_code=%lu",
            static_cast<unsigned long>(status.error_code)
        );
        return status;
    }

    if (!EnsureRecordingsDirectory()) {
        ready_ = false;
        status.mounted = false;
        status.error_code = StorageStatusErrorCode::kDirectoryCreateFailed;
        last_error_code_ = status.error_code;
        cached_status_ = status;
        ESP_LOGE(
            kTag,
            "Storage probe failed because the recordings directory could not be prepared"
        );
        return status;
    }

    FATFS* fs = nullptr;
    DWORD free_clusters = 0;
    const FRESULT free_result = f_getfree(kMountPoint, &free_clusters, &fs);
    if (free_result != FR_OK || fs == nullptr) {
        ready_ = false;
        status.mounted = false;
        status.error_code = StorageStatusErrorCode::kFreeSpaceReadFailed;
        last_error_code_ = status.error_code;
        cached_status_ = status;
        ESP_LOGE(
            kTag,
            "Failed to read SD card free space: f_getfree=%d fs=%p",
            static_cast<int>(free_result),
            fs
        );
        return status;
    }

    const uint64_t total_clusters = static_cast<uint64_t>(fs->n_fatent - 2);
    const uint64_t sector_size = static_cast<uint64_t>(fs->ssize);
    const uint64_t sectors_per_cluster = static_cast<uint64_t>(fs->csize);
    status.total_bytes = total_clusters * sectors_per_cluster * sector_size;
    status.free_bytes = static_cast<uint64_t>(free_clusters) * sectors_per_cluster * sector_size;
    status.recording_bytes = DirectorySizeBytes(kRecordingsDirectory);
    status.estimated_remaining_minutes = static_cast<uint32_t>(
        status.free_bytes / kEstimatedOpusBytesPerMinute
    );
    status.mounted = true;
    status.error_code = StorageStatusErrorCode::kNone;
    ready_ = true;
    last_error_code_ = StorageStatusErrorCode::kNone;
    cached_status_ = status;
    ESP_LOGI(
        kTag,
        "Storage status mounted total_mb=%lu free_mb=%lu recordings_kb=%lu remaining_minutes=%lu",
        static_cast<unsigned long>(status.total_bytes / (1024 * 1024)),
        static_cast<unsigned long>(status.free_bytes / (1024 * 1024)),
        static_cast<unsigned long>(status.recording_bytes / 1024),
        static_cast<unsigned long>(status.estimated_remaining_minutes)
    );
    return status;
}

std::string SdCardStorage::RecordingsDirectory() const {
    return kRecordingsDirectory;
}

bool SdCardStorage::EnsureRecordingsDirectory() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!EnsureDirectory("/sdcard/audream")) {
        return false;
    }
    return EnsureDirectory(kRecordingsDirectory);
}

bool SdCardStorage::SetRecordingMtime(
    const std::string& recording_id,
    uint64_t started_at_unix_ms
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!IsSafeRecordingID(recording_id)) {
        return false;
    }
    if (started_at_unix_ms == 0) {
        return true;
    }
    if (!GetStatus().mounted) {
        return false;
    }
    const std::string audio_path = BuildRecordingPath(recording_id);
    const std::string metadata_path = audio_path + ".json";
    return ApplyRecordingMtime(audio_path, metadata_path, started_at_unix_ms);
}

std::vector<StoredRecordingInfo> SdCardStorage::ListRecordings(size_t max_count) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<StoredRecordingInfo> result;
    if (!GetStatus().mounted) {
        return result;
    }

    DIR* dir = opendir(kRecordingsDirectory);
    if (dir == nullptr) {
        ESP_LOGE(kTag, "Failed to open recordings directory: %s", std::strerror(errno));
        return result;
    }

    std::vector<RecordingScanEntry> entries;
    std::unordered_map<time_t, size_t> mtime_counts;
    while (dirent* entry = readdir(dir)) {
        const std::string file_name = entry->d_name;
        if (!EndsWith(file_name, ".opus")) {
            continue;
        }
        const std::string recording_id = file_name.substr(0, file_name.size() - 5);
        if (!IsSafeRecordingID(recording_id)) {
            continue;
        }
        const std::string path = std::string(kRecordingsDirectory) + "/" + file_name;
        struct stat st = {};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        StoredRecordingInfo info;
        info.recording_id = recording_id;
        info.file_name = file_name;
        info.size_bytes = static_cast<uint32_t>(st.st_size);
        if (info.size_bytes == 0) {
            continue;
        }
        mtime_counts[st.st_mtime]++;
        entries.push_back({
            .info = info,
            .sort_key = static_cast<uint64_t>(st.st_mtime) * 1000,
            .modified_at = st.st_mtime,
            .audio_path = path,
        });
    }
    closedir(dir);

    size_t corrected_count = 0;
    size_t repaired_mtime_count = 0;
    for (auto& entry : entries) {
        const bool invalid_mtime = entry.modified_at < kMinimumReasonableRecordingMtime;
        const bool duplicate_mtime = mtime_counts[entry.modified_at] > 1;
        if (!invalid_mtime && !duplicate_mtime) {
            continue;
        }
        const std::string metadata_content = ReadTextFile(entry.audio_path + ".json");
        if (metadata_content.empty()) {
            continue;
        }
        entry.metadata_loaded = true;
        PopulateRecordingInfo(metadata_content, entry.info);
        const uint64_t started_at_unix_ms = entry.info.started_at_unix_ms;
        if (started_at_unix_ms > 0) {
            const time_t corrected_mtime = static_cast<time_t>(started_at_unix_ms / 1000);
            if (corrected_mtime != entry.modified_at && ApplyRecordingMtime(
                    entry.audio_path,
                    entry.audio_path + ".json",
                    started_at_unix_ms
                )) {
                entry.modified_at = corrected_mtime;
                repaired_mtime_count++;
            }
        }
        const uint64_t corrected_sort_key = RecordingSortKey(
            metadata_content,
            entry.modified_at
        );
        if (corrected_sort_key != entry.sort_key) {
            entry.sort_key = corrected_sort_key;
            corrected_count++;
        }
    }
    if (corrected_count > 0) {
        ESP_LOGD(
            kTag,
            "Applied recording sort keys from metadata: %u",
            static_cast<unsigned>(corrected_count)
        );
    }
    if (repaired_mtime_count > 0) {
        ESP_LOGI(
            kTag,
            "Repaired recording mtimes from metadata: %u",
            static_cast<unsigned>(repaired_mtime_count)
        );
    }

    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        if (left.sort_key != right.sort_key) {
            return left.sort_key > right.sort_key;
        }
        return left.info.recording_id > right.info.recording_id;
    });

    result.reserve(std::min(max_count, entries.size()));
    for (auto& entry : entries) {
        if (result.size() >= max_count) {
            break;
        }
        if (!entry.metadata_loaded) {
            const std::string metadata_content = ReadTextFile(entry.audio_path + ".json");
            PopulateRecordingInfo(metadata_content, entry.info);
        }
        if (entry.info.duration_ms == 0) {
            continue;
        }
        result.push_back(entry.info);
    }
    return result;
}

bool SdCardStorage::GetRecordingFilePath(
    const std::string& recording_id,
    std::string& file_path,
    uint32_t& total_size
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    file_path.clear();
    total_size = 0;
    if (!IsSafeRecordingID(recording_id)) {
        return false;
    }
    if (!GetStatus().mounted) {
        return false;
    }
    const std::string path = BuildRecordingPath(recording_id);
    total_size = FileSizeBytes(path);
    if (total_size == 0) {
        return false;
    }
    file_path = path;
    return true;
}

bool SdCardStorage::DeleteRecording(const std::string& recording_id) {
    ScopedStorageBusy storage_busy;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!IsSafeRecordingID(recording_id) || !GetStatus().mounted) {
        return false;
    }

    const std::string audio_path = BuildRecordingPath(recording_id);
    struct stat audio_stat = {};
    if (stat(audio_path.c_str(), &audio_stat) != 0 || !S_ISREG(audio_stat.st_mode)) {
        return false;
    }
    if (unlink(audio_path.c_str()) != 0) {
        ESP_LOGE(
            kTag,
            "Failed to delete recording audio file: path=%s error=%s",
            audio_path.c_str(),
            std::strerror(errno)
        );
        return false;
    }

    const std::string metadata_path = audio_path + ".json";
    struct stat metadata_stat = {};
    const bool metadata_exists = stat(metadata_path.c_str(), &metadata_stat) == 0;
    if (metadata_exists && unlink(metadata_path.c_str()) != 0) {
        ESP_LOGW(
            kTag,
            "Recording audio was deleted but metadata deletion failed: path=%s error=%s",
            metadata_path.c_str(),
            std::strerror(errno)
        );
    }

    const uint64_t deleted_bytes = static_cast<uint64_t>(audio_stat.st_size) +
        (metadata_exists ? static_cast<uint64_t>(metadata_stat.st_size) : 0);
    cached_status_.recording_bytes =
        deleted_bytes >= cached_status_.recording_bytes
            ? 0
            : cached_status_.recording_bytes - deleted_bytes;
    cached_status_.free_bytes = std::min(
        cached_status_.total_bytes,
        cached_status_.free_bytes + deleted_bytes
    );
    cached_status_.estimated_remaining_minutes = static_cast<uint32_t>(
        cached_status_.free_bytes / kEstimatedOpusBytesPerMinute
    );
    ESP_LOGI(kTag, "Deleted synchronized recording: %s", recording_id.c_str());
    return true;
}

bool SdCardStorage::ReadRecordingChunk(
    const std::string& recording_id,
    uint32_t offset,
    size_t max_length,
    std::vector<uint8_t>& output,
    uint32_t& total_size
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    output.clear();
    total_size = 0;
    if (!IsSafeRecordingID(recording_id) || max_length == 0) {
        return false;
    }
    if (!GetStatus().mounted) {
        return false;
    }

    std::string path;
    if (!GetRecordingFilePath(recording_id, path, total_size) || offset > total_size) {
        return false;
    }
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        ESP_LOGE(kTag, "Failed to open recording file: %s", path.c_str());
        return false;
    }
    if (fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    const size_t length = std::min<size_t>(max_length, total_size - offset);
    output.resize(length);
    const size_t bytes_read = fread(output.data(), 1, length, file);
    fclose(file);
    output.resize(bytes_read);
    return bytes_read == length;
}

size_t SdCardStorage::BackfillUnsynchronizedRecordingTimes(
    uint64_t synced_unix_time_ms,
    uint64_t synced_rtc_time_ms,
    uint64_t rtc_epoch_seq
) {
    ScopedStorageBusy storage_busy;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!GetStatus().mounted || synced_unix_time_ms == 0 ||
        synced_rtc_time_ms == 0 || rtc_epoch_seq == 0) {
        return 0;
    }

    DIR* dir = opendir(kRecordingsDirectory);
    if (dir == nullptr) {
        ESP_LOGE(kTag, "Failed to open recordings directory: %s", std::strerror(errno));
        return 0;
    }

    size_t updated_count = 0;
    while (dirent* entry = readdir(dir)) {
        const std::string file_name = entry->d_name;
        if (!EndsWith(file_name, ".opus")) {
            continue;
        }
        const std::string recording_id = file_name.substr(0, file_name.size() - 5);
        if (!IsSafeRecordingID(recording_id)) {
            continue;
        }

        const std::string audio_path = std::string(kRecordingsDirectory) + "/" + file_name;
        const std::string metadata_path = audio_path + ".json";
        const std::string content = ReadTextFile(metadata_path);
        if (content.empty() || JsonBoolValue(content, "time_synchronized")) {
            continue;
        }
        if (ReadJsonUInt64(content, "rtc_epoch_seq") != rtc_epoch_seq) {
            continue;
        }

        const uint64_t started_at_rtc_ms = ReadJsonUInt64(content, "started_at_rtc_ms");
        if (started_at_rtc_ms == 0 || started_at_rtc_ms > synced_rtc_time_ms) {
            continue;
        }

        struct stat st = {};
        if (stat(audio_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        const uint64_t elapsed_before_sync_ms = synced_rtc_time_ms - started_at_rtc_ms;
        if (synced_unix_time_ms < elapsed_before_sync_ms) {
            continue;
        }
        const uint64_t started_at_unix_ms = synced_unix_time_ms - elapsed_before_sync_ms;
        const uint64_t started_at_uptime_ms = ReadJsonUInt64(content, "started_at_uptime_ms");
        const std::string metadata_recording_id = ReadJsonString(content, "recording_id");
        const std::string metadata_file_name = ReadJsonString(content, "file_name");
        const uint32_t duration_ms = static_cast<uint32_t>(
            ReadJsonUInt64(content, "duration_ms")
        );
        const uint32_t size_bytes = static_cast<uint32_t>(st.st_size);

        const bool metadata_written = WriteRecordingMetadata(
                metadata_path,
                metadata_recording_id.empty() ? recording_id : metadata_recording_id,
                metadata_file_name.empty() ? file_name : metadata_file_name,
                rtc_epoch_seq,
                duration_ms,
                size_bytes,
                started_at_unix_ms,
                started_at_uptime_ms,
                started_at_rtc_ms,
                true
            );
        if (metadata_written) {
            ApplyRecordingMtime(audio_path, metadata_path, started_at_unix_ms);
            updated_count++;
        }
    }
    closedir(dir);

    if (updated_count > 0) {
        ESP_LOGI(
            kTag,
            "Backfilled %u recording metadata timestamps",
            static_cast<unsigned>(updated_count)
        );
    }
    return updated_count;
}
