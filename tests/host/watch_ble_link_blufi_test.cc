#include "blufi.h"
#include "esp_gap_ble_api.h"
#include "watch/watch_ancs_client.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>
#include "gap_method.inc"

static int64_t now_us = 0;
static std::vector<esp_ble_conn_update_params_t> requests;
static std::vector<esp_ble_adv_params_t> advertising_requests;
static esp_gap_ble_cb_t gap_callback = nullptr;
static unsigned forwarded = 0;
static unsigned advertising_stops = 0;
extern "C" esp_err_t esp_ble_gap_register_callback(esp_gap_ble_cb_t callback) {
    gap_callback = callback; return ESP_OK;
}
extern "C" esp_err_t esp_blufi_profile_init() { return ESP_OK; }
void esp_blufi_gap_event_handler(esp_gap_ble_cb_event_t, esp_ble_gap_cb_param_t*) { ++forwarded; }
extern "C" int64_t esp_timer_get_time() { return now_us; }
extern "C" esp_err_t esp_ble_gap_update_conn_params(esp_ble_conn_update_params_t* params) {
    requests.push_back(*params);
    return ESP_OK;
}
extern "C" esp_err_t esp_ble_gap_start_advertising(esp_ble_adv_params_t* params) {
    advertising_requests.push_back(*params);
    return ESP_OK;
}
extern "C" esp_err_t esp_ble_gap_stop_advertising() {
    ++advertising_stops;
    return ESP_OK;
}
static void Poll(Blufi& blufi, int64_t now_ms) {
    now_us = now_ms * 1000; blufi.PollLinkPower();
}
static void Complete(Blufi&) {
    const auto& p = requests.back();
    esp_ble_gap_cb_param_t event{};
    auto& update = event.update_conn_params;
    std::copy_n(p.bda, 6, update.bda);
    update.min_int = p.min_int; update.max_int = p.max_int; update.conn_int = p.max_int;
    update.latency = p.latency; update.timeout = p.timeout; update.status = ESP_BT_STATUS_SUCCESS;
    gap_callback(ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT, &event);
}
int main() {
    Blufi blufi;
    assert(Blufi::_gap_register_callback() == ESP_OK && gap_callback);
    blufi.session_.Activate(BlufiRole::kControlOnly);
    blufi.BeginAdvertising();
    esp_ble_gap_cb_param_t advertising{};
    advertising.adv_data_cmpl.status = ESP_BT_STATUS_SUCCESS;
    gap_callback(ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT, &advertising);
    assert(advertising_requests.size() == 1 && advertising_requests.back().adv_int_min == 160 &&
        advertising_requests.back().adv_int_max == 240 && forwarded == 0);
    advertising.adv_start_cmpl.status = ESP_BT_STATUS_SUCCESS;
    gap_callback(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, &advertising);
    Poll(blufi, 29999); assert(advertising_stops == 0);
    Poll(blufi, 30000); assert(advertising_stops == 1);
    advertising.adv_stop_cmpl.status = ESP_BT_STATUS_SUCCESS;
    gap_callback(ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT, &advertising);
    assert(advertising_requests.size() == 2 && advertising_requests.back().adv_int_min == 1600 &&
        advertising_requests.back().adv_int_max == 2000);
    gap_callback(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, &advertising);
    const auto forwarded_before_security = forwarded;
    esp_ble_gap_cb_param_t security{};
    gap_callback(ESP_GAP_BLE_SEC_REQ_EVT, &security);
#if CONFIG_AUDREAM_WATCH_ANCS
    assert(WatchAncsClient::Get().security_calls == 1 && forwarded == forwarded_before_security);
#else
    assert(forwarded == forwarded_before_security + 1);
#endif
    esp_blufi_cb_param_t event{};
    const uint8_t peer[]{1, 2, 3, 4, 5, 6};
    std::copy_n(peer, 6, event.connect.remote_bda);
    blufi.HandleLinkPowerEvent(ESP_BLUFI_EVENT_BLE_CONNECT, &event);
    Poll(blufi, 35000);
    assert(requests.size() == 1 && requests.back().min_int == 24 && requests.back().max_int == 36 &&
        requests.back().latency == 0 && requests.back().timeout == 400 &&
        std::equal(peer, peer + 6, requests.back().bda));
    Complete(blufi);
    blufi.control_lease_ = 1; Poll(blufi, 50000); assert(requests.size() == 1);
    blufi.control_lease_ = 0; Poll(blufi, 65000); assert(requests.size() == 2 && requests.back().latency == 4);
    Complete(blufi);
    blufi.NotifyLinkActivity(); Poll(blufi, 70000);
    assert(requests.size() == 3 && requests.back().latency == 0); Complete(blufi);
    blufi.session_.Activate(BlufiRole::kWifiProvisioning);
    Poll(blufi, 120000); assert(requests.size() == 3);
    blufi.session_.Activate(BlufiRole::kControlOnly);
#if CONFIG_AUDREAM_WATCH_ANCS
    WatchAncsClient::Get().busy = true;
    Poll(blufi, 140000); assert(requests.size() == 3);
    WatchAncsClient::Get().busy = false;
#endif
    Poll(blufi, 155000); assert(requests.size() == 4 && requests.back().latency == 4); Complete(blufi);
    blufi.HandleLinkPowerEvent(ESP_BLUFI_EVENT_BLE_DISCONNECT, nullptr);
    blufi.NotifyLinkActivity(); Poll(blufi, 200000); assert(requests.size() == 4);
    blufi.HandleLinkPowerEvent(ESP_BLUFI_EVENT_BLE_CONNECT, &event);
    Poll(blufi, 205000); assert(requests.size() == 5 && requests.back().latency == 0);
    blufi.session_.Deactivate(); Poll(blufi, 220000);
    blufi.session_.Activate(BlufiRole::kControlOnly); Poll(blufi, 250000); assert(requests.size() == 5);
    blufi.HandleLinkPowerEvent(ESP_BLUFI_EVENT_BLE_CONNECT, &event);
    blufi.shutdown_failed_ = true; Poll(blufi, 260000);
    blufi.shutdown_failed_ = false; Poll(blufi, 280000); assert(requests.size() == 5);
    std::cout << "PASS: actual BLUFI GAP routing and link adapter, SDK parameters, busy leases, shutdown and reconnect (ANCS="
        << CONFIG_AUDREAM_WATCH_ANCS << ")\n";
}
