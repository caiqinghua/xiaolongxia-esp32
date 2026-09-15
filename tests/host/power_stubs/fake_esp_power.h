#pragma once

#include <cstdint>
#include <functional>
#include <vector>

using esp_err_t = int;
using gpio_num_t = int;
using TaskHandle_t = void*;
using BaseType_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_ERR_INVALID_ARG = 1;
constexpr int ESP_ERR_NOT_SUPPORTED = 2;
constexpr int GPIO_NUM_NC = -1;
constexpr int GPIO_MODE_INPUT = 0;
constexpr int ESP_SLEEP_WAKEUP_ALL = 0;
constexpr int pdPASS = 1;
constexpr int pdMS_TO_TICKS(int milliseconds) { return milliseconds; }

namespace fake_power {
inline int64_t now_us = 1000000;
inline int wake_level = 1;
inline int timer_failure_at = 0;
inline int wake_error = ESP_OK;
inline int clear_error = ESP_OK;
inline int timer_calls = 0;
inline int wifi_stops = 0;
inline bool slept = false;
inline std::vector<uint64_t> wake_timers;
inline std::function<void()> on_delay;
struct DeepSleepEntered {};
inline void Reset() {
    now_us = 1000000;
    wake_level = 1;
    timer_failure_at = wake_error = clear_error = timer_calls = wifi_stops = 0;
    slept = false;
    wake_timers.clear();
    on_delay = nullptr;
}
}

inline int64_t esp_timer_get_time() { return fake_power::now_us; }
inline void vTaskDelay(int milliseconds) {
    fake_power::now_us += static_cast<int64_t>(milliseconds) * 1000;
    if (fake_power::on_delay) { fake_power::on_delay(); }
}
inline void vTaskDelete(void*) {}
inline BaseType_t xTaskCreate(void (*)(void*), const char*, int, void*, int, TaskHandle_t* handle) {
    *handle = reinterpret_cast<void*>(1);
    return pdPASS;
}
inline esp_err_t gpio_set_direction(gpio_num_t, int) { return ESP_OK; }
inline esp_err_t gpio_pullup_en(gpio_num_t) { return ESP_OK; }
inline esp_err_t gpio_pulldown_dis(gpio_num_t) { return ESP_OK; }
inline int gpio_get_level(gpio_num_t) { return fake_power::wake_level; }
inline esp_err_t esp_sleep_disable_wakeup_source(int) { return fake_power::clear_error; }
inline esp_err_t esp_sleep_enable_ext0_wakeup(gpio_num_t, int) { return fake_power::wake_error; }
inline esp_err_t esp_sleep_enable_timer_wakeup(uint64_t delay_us) {
    ++fake_power::timer_calls;
    if (fake_power::timer_calls == fake_power::timer_failure_at) { return ESP_ERR_INVALID_ARG; }
    fake_power::wake_timers.push_back(delay_us);
    return ESP_OK;
}
inline void esp_deep_sleep_start() {
    fake_power::slept = true;
    throw fake_power::DeepSleepEntered{};
}
inline esp_err_t esp_wifi_stop() { ++fake_power::wifi_stops; return ESP_OK; }
inline const char* esp_err_to_name(esp_err_t) { return "fake_error"; }
struct esp_pm_config_t { int max_freq_mhz; int min_freq_mhz; bool light_sleep_enable; };
inline esp_err_t esp_pm_configure(const esp_pm_config_t*) { return ESP_OK; }
