#include "blufi.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include <algorithm>
#if CONFIG_AUDREAM_WATCH_ANCS
#include "watch/watch_ancs_client.h"
#endif

void Blufi::NotifyLinkActivity() {
    link_power_.Activity(esp_timer_get_time() / 1000);
}

void Blufi::HandleLinkPowerEvent(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
    if (event == ESP_BLUFI_EVENT_BLE_CONNECT && param) {
        link_power_.Connected(param->connect.remote_bda, esp_timer_get_time() / 1000);
    } else if (event == ESP_BLUFI_EVENT_BLE_DISCONNECT || event == ESP_BLUFI_EVENT_DEINIT_FINISH) {
        link_power_.Disconnected();
    }
}

void Blufi::HandleLinkParameterUpdate(const uint8_t* address, uint16_t minimum, uint16_t maximum,
                                    uint16_t interval, uint16_t latency, uint16_t timeout, bool success) {
    if (link_power_.Updated(address, minimum, maximum, success, esp_timer_get_time() / 1000)) {
        ESP_LOGI("WatchBleLink", "BLE parameter response: success=%d interval_units=%u latency=%u timeout_units=%u",
            success, static_cast<unsigned>(interval), static_cast<unsigned>(latency), static_cast<unsigned>(timeout));
    }
}

void Blufi::PollLinkPower() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!session_.IsActive() || shutdown_failed_) {
        link_power_.Disconnected();
        return;
    }
    bool busy = session_.AllowsProvisioning() || control_lease_ != 0;
#if CONFIG_AUDREAM_WATCH_ANCS
    busy = busy || WatchAncsClient::Get().NeedsResponsiveLink();
#endif
    link_power_.Poll(esp_timer_get_time() / 1000, busy,
        [](const WatchBleLinkPolicy::Address& address, const WatchBleLinkParameters& desired) {
            esp_ble_conn_update_params_t request{};
            std::copy(address.begin(), address.end(), request.bda);
            request.min_int = desired.minimum_interval;
            request.max_int = desired.maximum_interval;
            request.latency = desired.latency;
            request.timeout = desired.supervision_timeout;
            const auto result = esp_ble_gap_update_conn_params(&request);
            ESP_LOGI("WatchBleLink", "BLE parameter request: min_units=%u max_units=%u latency=%u timeout_units=%u accepted=%d",
                static_cast<unsigned>(request.min_int), static_cast<unsigned>(request.max_int),
                static_cast<unsigned>(request.latency), static_cast<unsigned>(request.timeout), result == ESP_OK);
            return result == ESP_OK;
        });
}
