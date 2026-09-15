#pragma once

#include "esp_gatts_api.h"

// Static service callbacks share the one public Bluedroid GATTS callback.
// Each service retains responsibility for its interface, peer and host lifetime.
bool RegisterWatchGattCallback(uint16_t app_id, esp_gatts_cb_t callback);
