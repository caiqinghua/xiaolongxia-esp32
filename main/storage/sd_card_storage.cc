#include "storage/sd_card_storage.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

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

namespace {

constexpr char kTag[] = "AudreamStorage";
constexpr char kMountPoint[] = "/sdcard";
constexpr char kRecordingsDirectory[] = "/sdcard/audream/recordings";
constexpr uint32_t kEstimatedOpusBytesPerMinute = 180000;

struct RecordingScanEntry {
    StoredRecordingInfo info;
    time_t modified_at = 0;
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

uint32_t ReadMetadataUInt32(const std::string& metadata_path, const std::string& key) {
    std::ifstream stream(metadata_path);
    if (!stream.good()) {
        return 0;
    }
    const std::string content(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>()
    );
    const std::string marker = "\"" + key + "\":";
    const size_t start = content.find(marker);
    if (start == std::string::npos) {
        return 0;
    }
    size_t cursor = start + marker.size();
    while (cursor < content.size() && std::isspace(static_cast<unsigned char>(content[cursor]))) {
        cursor++;
    }
    uint32_t value = 0;
    while (cursor < content.size() && std::isdigit(static_cast<unsigned char>(content[cursor]))) {
        value = value * 10 + static_cast<uint32_t>(content[cursor] - '0');
        cursor++;
    }
    return value;
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
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(path, 0775) == 0) {
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

bool SdCardStorage::Mount() {
    if (mounted_) {
        return true;
    }
    if (AUDREAM_SD_CS_GPIO == GPIO_NUM_NC ||
        AUDREAM_SD_MOSI_GPIO == GPIO_NUM_NC ||
        AUDREAM_SD_MISO_GPIO == GPIO_NUM_NC ||
        AUDREAM_SD_CLK_GPIO == GPIO_NUM_NC) {
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
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    auto spi_host = static_cast<spi_host_device_t>(host.slot);
    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = AUDREAM_SD_MOSI_GPIO;
    bus_config.miso_io_num = AUDREAM_SD_MISO_GPIO;
    bus_config.sclk_io_num = AUDREAM_SD_CLK_GPIO;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = 4000;

    esp_err_t ret = spi_bus_initialize(spi_host, &bus_config, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "Failed to initialize SD SPI bus: %s", esp_err_to_name(ret));
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = AUDREAM_SD_CS_GPIO;
    slot_config.host_id = spi_host;

    ret = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return false;
    }

    mounted_ = true;
    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(kTag, "SD card mounted at %s", kMountPoint);
    return EnsureRecordingsDirectory();
}

bool SdCardStorage::IsMounted() const {
    return mounted_;
}

StorageStatus SdCardStorage::GetStatus() {
    StorageStatus status;
    status.mounted = Mount();
    if (!status.mounted) {
        return status;
    }

    FATFS* fs = nullptr;
    DWORD free_clusters = 0;
    if (f_getfree(kMountPoint, &free_clusters, &fs) != FR_OK || fs == nullptr) {
        ESP_LOGE(kTag, "Failed to read SD card free space");
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
    return status;
}

std::string SdCardStorage::RecordingsDirectory() const {
    return kRecordingsDirectory;
}

bool SdCardStorage::EnsureRecordingsDirectory() {
    if (!EnsureDirectory("/sdcard/audream")) {
        return false;
    }
    return EnsureDirectory(kRecordingsDirectory);
}

std::vector<StoredRecordingInfo> SdCardStorage::ListRecordings(size_t max_count) {
    std::vector<StoredRecordingInfo> result;
    if (!Mount() || !EnsureRecordingsDirectory()) {
        return result;
    }

    DIR* dir = opendir(kRecordingsDirectory);
    if (dir == nullptr) {
        ESP_LOGE(kTag, "Failed to open recordings directory: %s", std::strerror(errno));
        return result;
    }

    std::vector<RecordingScanEntry> entries;
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
        info.duration_ms = ReadMetadataUInt32(path + ".json", "duration_ms");
        entries.push_back({
            .info = info,
            .modified_at = st.st_mtime,
        });
    }
    closedir(dir);

    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.modified_at > right.modified_at;
    });

    result.reserve(std::min(max_count, entries.size()));
    for (const auto& entry : entries) {
        if (result.size() >= max_count) {
            break;
        }
        result.push_back(entry.info);
    }
    return result;
}

bool SdCardStorage::ReadRecordingChunk(
    const std::string& recording_id,
    uint32_t offset,
    size_t max_length,
    std::vector<uint8_t>& output,
    uint32_t& total_size
) {
    output.clear();
    total_size = 0;
    if (!IsSafeRecordingID(recording_id) || max_length == 0) {
        return false;
    }
    if (!Mount() || !EnsureRecordingsDirectory()) {
        return false;
    }

    const std::string path = BuildRecordingPath(recording_id);
    total_size = FileSizeBytes(path);
    if (total_size == 0 || offset > total_size) {
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
