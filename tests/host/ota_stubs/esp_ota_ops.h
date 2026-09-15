#pragma once
#include <esp_app_desc.h>
#include <cstddef>
#include <cstdint>
constexpr unsigned ESP_PARTITION_TYPE_APP = 0;
constexpr unsigned ESP_PARTITION_SUBTYPE_APP_FACTORY = 0;
struct esp_partition_t {
    size_t size;
    unsigned type = ESP_PARTITION_TYPE_APP;
    unsigned subtype = 0x10;
};
enum esp_ota_img_states_t {
    ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID,
    ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED = -1
};
const esp_partition_t* esp_ota_get_running_partition();
esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t*);
esp_err_t esp_ota_mark_app_valid_cancel_rollback();
using esp_ota_handle_t = uint32_t;
constexpr size_t OTA_WITH_SEQUENTIAL_WRITES = static_cast<size_t>(-2);
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*);
esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t*);
esp_err_t esp_ota_write(esp_ota_handle_t, const void*, size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*);
