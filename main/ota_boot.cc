#include "ota.h"

#include <esp_log.h>
#include <esp_ota_ops.h>

bool Ota::MarkCurrentVersionValid() {
    const auto* partition = esp_ota_get_running_partition();
    if (!partition || partition->type != ESP_PARTITION_TYPE_APP) {
        ESP_LOGE("Ota", "Running application partition is unavailable");
        return false;
    }
    if (partition->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) return true;

    esp_ota_img_states_t state;
    const auto queried = esp_ota_get_state_partition(partition, &state);
    if (queried != ESP_OK) {
        ESP_LOGE("Ota", "Running firmware state could not be read: %s", esp_err_to_name(queried));
        return false;
    }
    if (state == ESP_OTA_IMG_VALID) return true;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE("Ota", "Running firmware is not pending verification: state=%d", static_cast<int>(state));
        return false;
    }
    const auto confirmed = esp_ota_mark_app_valid_cancel_rollback();
    if (confirmed != ESP_OK) {
        ESP_LOGE("Ota", "Firmware boot confirmation failed: %s", esp_err_to_name(confirmed));
        return false;
    }
    // A successful write must also be observable before reporting confirmation.
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK || state != ESP_OTA_IMG_VALID) {
        ESP_LOGE("Ota", "Firmware boot confirmation could not be verified");
        return false;
    }
    ESP_LOGI("Ota", "Firmware boot confirmed");
    return true;
}
