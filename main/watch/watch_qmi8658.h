#pragma once

#if defined(ESP_PLATFORM) && !defined(AUDREAM_SENSORLIB_LIFECYCLE_PATCHED)
#error "The watch motion driver requires the reviewed SensorLib lifecycle overlay"
#endif

// Use the current SensorLib implementation, not its legacy root-level header.
#include <sensor/imu/qmi8658/SensorQMI8658.hpp>
#include <cstdint>
#include "watch_activity.h"

// One motion worker owns all calls, including Begin/Start/Stop. This adapter
// does not select board axes, create a polling task, or enable interrupt pins.
class WatchQmi8658 final : private SensorQMI8658 {
public:
#if defined(ESP_PLATFORM)
    bool Begin(i2c_master_bus_handle_t bus, uint8_t address = QMI8658_L_SLAVE_ADDRESS);
#endif
    bool Begin(SensorCommCustom::CustomCallback io, SensorCommCustomHal::CustomHalCallback hal,
        uint8_t address = QMI8658_L_SLAVE_ADDRESS);
    bool Start();
    bool Stop();
    bool Read(WatchImuReading& reading);
    uint64_t Generation() const { return generation_; }
    bool Running() const { return running_; }
    WatchQmi8658() = default;
    WatchQmi8658(const WatchQmi8658&) = delete;
    WatchQmi8658& operator=(const WatchQmi8658&) = delete;

private:
    void afterCommReady() override;
    bool FinishBegin(bool succeeded);
    bool ReadStepCount(uint32_t& count);
    bool io_failed_ = false;
    bool ready_ = false;
    bool running_ = false;
    uint64_t generation_ = 0;
};
