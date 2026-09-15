#include "watch_gatt_dispatcher.h"
#include <array>
#include <mutex>

namespace {
struct Entry { uint16_t app_id = 0; esp_gatts_cb_t callback = nullptr; };
std::array<Entry, 2> services;
std::mutex mutex;
void Dispatch(esp_gatts_cb_event_t event, esp_gatt_if_t interface, esp_ble_gatts_cb_param_t* param) {
    if (!param) return;
    // Transactions must identify their registered interface. Broadcast lifecycle
    // events are valid, but broadcasting a write could produce two responses.
    if (interface == ESP_GATT_IF_NONE && (event == ESP_GATTS_READ_EVT ||
        event == ESP_GATTS_WRITE_EVT || event == ESP_GATTS_EXEC_WRITE_EVT)) return;
    std::array<Entry, 2> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot = services;
    }
    for (const auto& entry : snapshot) {
        if (entry.callback && (event != ESP_GATTS_REG_EVT || param->reg.app_id == entry.app_id))
            entry.callback(event, interface, param);
    }
}
}

bool RegisterWatchGattCallback(uint16_t app_id, esp_gatts_cb_t callback) {
    if (!callback) return false;
    std::lock_guard<std::mutex> lock(mutex);
    Entry* available = nullptr;
    for (auto& entry : services) {
        if (entry.callback && entry.app_id == app_id) {
            if (entry.callback != callback) return false;
            return esp_ble_gatts_register_callback(Dispatch) == ESP_OK;
        }
        if (!entry.callback && !available) available = &entry;
    }
    if (!available || esp_ble_gatts_register_callback(Dispatch) != ESP_OK) return false;
    *available = {app_id, callback};
    return true;
}
