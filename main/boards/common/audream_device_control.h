#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class AudreamDeviceControl {
public:
    static AudreamDeviceControl& GetInstance();

    std::vector<uint8_t> HandleRequest(const uint8_t* data, size_t length);

    AudreamDeviceControl(const AudreamDeviceControl&) = delete;
    AudreamDeviceControl& operator=(const AudreamDeviceControl&) = delete;

private:
    AudreamDeviceControl() = default;

    struct DeviceState {
        int32_t auto_power_off_seconds;
        uint8_t microphone_gain_db;
        uint8_t call_gain;
        uint8_t battery_level_percent;
        bool charging;
        bool battery_level_simulated;
        uint8_t recording_mode;
        uint8_t firmware_major;
        uint8_t firmware_minor;
        uint8_t firmware_patch;
    };

    DeviceState LoadState() const;
    void ApplyMicrophoneGain(uint8_t gain_db) const;
    void ResetSettings() const;
    std::vector<uint8_t> BuildResponse(
        uint8_t opcode,
        uint16_t request_id,
        uint8_t status,
        const DeviceState& state
    ) const;
    std::vector<uint8_t> BuildPayloadResponse(
        uint8_t opcode,
        uint16_t request_id,
        uint8_t status,
        const std::vector<uint8_t>& payload
    ) const;
};
