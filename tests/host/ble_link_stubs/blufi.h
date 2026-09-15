#pragma once
#include "boards/common/blufi_session_state.h"
#include "watch/watch_ble_link_policy.h"
#include "esp_blufi_api.h"
#include "esp_gap_ble_api.h"
#include "esp_timer.h"
#include <mutex>

class Blufi {
public:
    Blufi() { current_ = this; }
    static Blufi& GetInstance() { return *current_; }
    static esp_err_t _gap_register_callback();
    void PollLinkPower();
    void NotifyLinkActivity();
    void HandleLinkPowerEvent(esp_blufi_cb_event_t, esp_blufi_cb_param_t*);
    void HandleLinkParameterUpdate(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, bool);
    std::mutex lifecycle_mutex_;
    BlufiSessionState session_;
    WatchBleLinkPolicy link_power_;
    bool shutdown_failed_ = false;
    uint64_t control_lease_ = 0;
private:
    inline static Blufi* current_ = nullptr;
};
void esp_blufi_gap_event_handler(esp_gap_ble_cb_event_t, esp_ble_gap_cb_param_t*);
