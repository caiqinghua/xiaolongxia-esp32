#pragma once
#include "esp_gap_ble_api.h"
class WatchAncsClient {
public:
    static WatchAncsClient& Get() { static WatchAncsClient client; return client; }
    bool NeedsResponsiveLink() const { return busy; }
    bool GapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t*) {
        if (event == ESP_GAP_BLE_SEC_REQ_EVT) { ++security_calls; return true; }
        return false;
    }
    bool busy = false;
    unsigned security_calls = 0;
};
