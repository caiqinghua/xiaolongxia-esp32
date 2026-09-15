#include "blufi.h"

#include "esp_gap_ble_api.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "WatchBleAdvertising";

esp_ble_adv_params_t AdvertisingParameters(WatchBleAdvertisingAction action) {
    const auto desired = action == WatchBleAdvertisingAction::kStartFast
        ? WatchBleAdvertisingPolicy::kFast
        : WatchBleAdvertisingPolicy::kIdle;
    esp_ble_adv_params_t parameters{};
    parameters.adv_int_min = desired.minimum_interval;
    parameters.adv_int_max = desired.maximum_interval;
    parameters.adv_type = ADV_TYPE_IND;
    parameters.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    parameters.channel_map = ADV_CHNL_ALL;
    parameters.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    return parameters;
}

}  // namespace

void Blufi::BeginAdvertising() {
    advertising_power_.Begin(esp_timer_get_time() / 1000);
}

void Blufi::ApplyAdvertisingAction(WatchBleAdvertisingAction action, int64_t now_ms) {
    if (action == WatchBleAdvertisingAction::kNone) return;

    esp_err_t result = ESP_OK;
    if (action == WatchBleAdvertisingAction::kStop) {
        result = esp_ble_gap_stop_advertising();
        ESP_LOGI(kTag, "BLE advertising idle transition stop accepted=%d", result == ESP_OK);
    } else {
        auto parameters = AdvertisingParameters(action);
        result = esp_ble_gap_start_advertising(&parameters);
        ESP_LOGI(
            kTag,
            "BLE advertising start requested: mode=%s min_units=%u max_units=%u accepted=%d",
            action == WatchBleAdvertisingAction::kStartFast ? "fast" : "idle",
            static_cast<unsigned>(parameters.adv_int_min),
            static_cast<unsigned>(parameters.adv_int_max),
            result == ESP_OK
        );
    }
    if (result != ESP_OK) advertising_power_.OperationRejected(action, now_ms);
}

bool Blufi::HandleAdvertisingGapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    const auto now_ms = esp_timer_get_time() / 1000;
    WatchBleAdvertisingAction action = WatchBleAdvertisingAction::kNone;
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            action = advertising_power_.DataConfigured(
                param && param->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS,
                now_ms
            );
            ApplyAdvertisingAction(action, now_ms);
            return true;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            action = advertising_power_.StartCompleted(
                param && param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS,
                now_ms
            );
            break;
        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            action = advertising_power_.StopCompleted(
                param && param->adv_stop_cmpl.status == ESP_BT_STATUS_SUCCESS,
                now_ms
            );
            break;
        default:
            return false;
    }
    ApplyAdvertisingAction(action, now_ms);
    return false;
}
