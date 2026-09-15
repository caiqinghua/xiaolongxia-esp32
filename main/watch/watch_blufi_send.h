#pragma once

#include <stdint.h>
#include "esp_blufi_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Async replies retain the originating request's grant across SDK queue boundaries.
esp_err_t audream_blufi_send_custom_data_for_lease(uint64_t lease, uint8_t* data, uint32_t length);
esp_err_t audream_blufi_send_wifi_conn_report_for_lease(uint64_t lease, wifi_mode_t mode,
    esp_blufi_sta_conn_state_t state, uint8_t softap_connections, esp_blufi_extra_info_t* info);
esp_err_t audream_blufi_send_wifi_list_for_lease(uint64_t lease, uint16_t count, esp_blufi_ap_record_t* list);

#ifdef __cplusplus
}
#endif
