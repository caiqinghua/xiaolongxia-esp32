#include "watch/watch_rtc_runtime.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <thread>

namespace {
std::array<uint8_t, 18> registers{};
int adds = 0, removes = 0;
int io_calls = 0;
bool fail_add = false, fail_io = false;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t* config, i2c_master_dev_handle_t* device) {
    assert(bus && config->device_address == 0x51 && config->dev_addr_length == 7 && config->scl_speed_hz == 100000);
    ++adds;
    if (fail_add) return -1;
    *device = registers.data();
    return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device) {
    assert(device == registers.data()); ++removes; return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t* reg, size_t tx_size, uint8_t* data, size_t size, int timeout) {
    ++io_calls;
    assert(device == registers.data() && tx_size == 1 && *reg + size <= registers.size() && timeout == 100);
    if (fail_io) return -1;
    std::copy_n(registers.data() + *reg, size, data);
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t* data, size_t size, int timeout) {
    ++io_calls;
    assert(device == registers.data() && size >= 2 && size <= 8 && data[0] + size - 1 <= registers.size() && timeout == 100);
    if (fail_io) return -1;
    std::copy_n(data + 1, size - 1, registers.data() + data[0]);
    return ESP_OK;
}
int main() {
    {
        WatchRtcRuntime runtime;
        uint64_t output = 0;
        assert(!runtime.ReadTime(output) && !runtime.SaveTime(1709164800000ULL));
        assert(!runtime.Begin(nullptr) && adds == 0);
        fail_add = true;
        assert(!runtime.Begin(&adds));
        fail_add = false;
        assert(runtime.Begin(&adds) && runtime.Begin(&adds) && adds == 2);
        assert(!runtime.ReadTime(output));
        assert(runtime.SaveTime(1709164800000ULL));
        assert(runtime.ReadTime(output) && output == 1709164800000ULL);
        fail_io = true;
        assert(!runtime.SaveTime(output) && !runtime.ReadTime(output));
        fail_io = false;
        const auto update = [&runtime](uint64_t value) {
            for (int index = 0; index < 200; ++index) assert(runtime.SaveTime(value + index * 1000));
        };
        std::thread first(update, 1709164800000ULL), second(update, 1709251200000ULL);
        first.join(); second.join();
        assert(runtime.ReadTime(output));
        runtime.Suspend();
        const int calls_before = io_calls;
        const uint64_t previous = output;
        assert(!runtime.ReadTime(output) && output == previous);
        assert(!runtime.SaveTime(1709164800000ULL));
        assert(!runtime.Begin(&adds));
        assert(io_calls == calls_before && adds == 2);
        runtime.Suspend();
        runtime.Resume();
        runtime.Resume();
        assert(runtime.SaveTime(1709164800000ULL));
        assert(runtime.ReadTime(output) && output == 1709164800000ULL);
    }
    assert(removes == 1);
    std::cout << "PASS: RTC runtime bus configuration, bounded I/O, failure, serialized writes and sleep fence\n";
}
