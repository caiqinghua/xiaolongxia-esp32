#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "esp_sleep.h"
#include "esp_system.h"

class BootDiagnostics {
public:
    static BootDiagnostics& GetInstance();

    void CaptureEarlyBoot();
    void SetBootGpioLevel(int level);
    void Log(const char* tag) const;
    bool AppendToFile(const std::string& path);

    static const char* ResetReasonName(esp_reset_reason_t reason);
    static const char* WakeCauseName(esp_sleep_wakeup_cause_t cause);

private:
    BootDiagnostics() = default;

    mutable std::mutex mutex_;
    bool captured_ = false;
    bool flushed_ = false;
    esp_reset_reason_t reset_reason_ = ESP_RST_UNKNOWN;
    esp_sleep_wakeup_cause_t wake_cause_ = ESP_SLEEP_WAKEUP_UNDEFINED;
    int boot_gpio_level_ = -1;
};
