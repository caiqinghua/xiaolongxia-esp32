#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

class AudreamDeviceControl {
public:
    static AudreamDeviceControl& GetInstance();

    void SetRecordingStatusAcknowledgedCallback(std::function<void()> callback);
    // Register during board initialization; the callback only enqueues work.
    void SetTimeSynchronizedCallback(std::function<void()> callback);
    void SetRecordingPauseCallbacks(
        std::function<bool()> pause_callback,
        std::function<bool()> resume_callback
    );
    std::vector<uint8_t> HandleRequest(const uint8_t* data, size_t length);

    AudreamDeviceControl(const AudreamDeviceControl&) = delete;
    AudreamDeviceControl& operator=(const AudreamDeviceControl&) = delete;

private:
    AudreamDeviceControl() = default;
    std::function<void()> recording_status_acknowledged_callback_;
    std::function<void()> time_synchronized_callback_;
    std::function<bool()> recording_pause_callback_;
    std::function<bool()> recording_resume_callback_;

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
    uint16_t Capabilities() const;
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
