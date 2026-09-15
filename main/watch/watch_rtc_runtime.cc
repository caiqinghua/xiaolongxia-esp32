#include "watch_rtc_runtime.h"
#include <array>
#include <algorithm>
#include "esp_log.h"

WatchRtcRuntime::~WatchRtcRuntime() {
    if (device_) {
        const auto result = i2c_master_bus_rm_device(device_);
        if (result != ESP_OK) ESP_LOGE("WatchRTC", "RTC I2C device removal failed: %d", static_cast<int>(result));
    }
}

bool WatchRtcRuntime::Begin(i2c_master_bus_handle_t bus) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (suspended_) return false;
    if (device_) return true;
    if (!bus) return false;
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = WatchPcf85063::kAddress;
    config.scl_speed_hz = 100000;
    return i2c_master_bus_add_device(bus, &config, &device_) == ESP_OK;
}

bool WatchRtcRuntime::ReadTime(uint64_t& unix_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_ && !suspended_ && rtc_.ReadUnixMs(unix_ms);
}

bool WatchRtcRuntime::SaveTime(uint64_t unix_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_ && !suspended_ && rtc_.WriteUnixMs(unix_ms);
}

void WatchRtcRuntime::Suspend() {
    // Acquiring the same lock drains any admitted calendar transaction.
    std::lock_guard<std::mutex> lock(mutex_);
    suspended_ = true;
}

void WatchRtcRuntime::Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    suspended_ = false;
}

bool WatchRtcRuntime::Read(uint8_t reg, uint8_t* data, size_t size) {
    return i2c_master_transmit_receive(device_, &reg, 1, data, size, 100) == ESP_OK;
}

bool WatchRtcRuntime::Write(uint8_t reg, const uint8_t* data, size_t size) {
    std::array<uint8_t, 8> packet{};
    if (size > packet.size() - 1) return false;
    packet[0] = reg;
    std::copy_n(data, size, packet.data() + 1);
    return i2c_master_transmit(device_, packet.data(), size + 1, 100) == ESP_OK;
}
