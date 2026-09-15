#pragma once
#include <cstdint>
class AudreamPowerManager {
public:
    static AudreamPowerManager& GetInstance() { static AudreamPowerManager value; return value; }
    bool recording_active = false;
    bool capture_active = false;
    void SetRecordingActive(bool value) { recording_active = value; capture_active = value; }
    void SetRecordingCaptureActive(bool value) { capture_active = value; }
    void NotifyUserActivity(const char*, uint32_t) {}
};
