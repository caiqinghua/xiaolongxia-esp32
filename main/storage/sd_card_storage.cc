#include "storage/sd_card_storage.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
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
