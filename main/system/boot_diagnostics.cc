#include "system/boot_diagnostics.h"

#include <cstdio>

#include "esp_log.h"

namespace {

constexpr char kFallbackTag[] = "BootDiagnostics";

}  // namespace

BootDiagnostics& BootDiagnostics::GetInstance() {
    static BootDiagnostics instance;
    return instance;
}

void BootDiagnostics::CaptureEarlyBoot() {
    std::lock_guard<std::mutex> lock(mutex_);
    reset_reason_ = esp_reset_reason();
    wake_cause_ = esp_sleep_get_wakeup_cause();
    captured_ = true;
}

void BootDiagnostics::SetBootGpioLevel(int level) {
    std::lock_guard<std::mutex> lock(mutex_);
    boot_gpio_level_ = level;
}

void BootDiagnostics::Log(const char* tag) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ESP_LOGI(
        tag == nullptr ? kFallbackTag : tag,
        "Boot diagnostics: captured=%d reset=%s(%d) wake=%s(%d) boot_gpio=%d flushed=%d",
        captured_ ? 1 : 0,
        ResetReasonName(reset_reason_),
        static_cast<int>(reset_reason_),
        WakeCauseName(wake_cause_),
        static_cast<int>(wake_cause_),
        boot_gpio_level_,
        flushed_ ? 1 : 0
    );
}

bool BootDiagnostics::AppendToFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (flushed_) {
        return true;
    }
    FILE* file = fopen(path.c_str(), "a");
    if (file == nullptr) {
        ESP_LOGE(kFallbackTag, "Failed to open boot diagnostics file: %s", path.c_str());
        return false;
    }
    std::fprintf(
        file,
        "{"
        "\"captured\":%s,"
        "\"reset\":\"%s\","
        "\"reset_code\":%d,"
        "\"wake\":\"%s\","
        "\"wake_code\":%d,"
        "\"boot_gpio\":%d"
        "}\n",
        captured_ ? "true" : "false",
        ResetReasonName(reset_reason_),
        static_cast<int>(reset_reason_),
        WakeCauseName(wake_cause_),
        static_cast<int>(wake_cause_),
        boot_gpio_level_
    );
    fclose(file);
    flushed_ = true;
    ESP_LOGI(kFallbackTag, "Boot diagnostics appended to %s", path.c_str());
    return true;
}

const char* BootDiagnostics::ResetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN:
            return "unknown";
        case ESP_RST_POWERON:
            return "poweron";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:
            return "task_watchdog";
        case ESP_RST_WDT:
            return "watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep_sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_USB:
            return "usb";
        case ESP_RST_JTAG:
            return "jtag";
        case ESP_RST_EFUSE:
            return "efuse";
        case ESP_RST_PWR_GLITCH:
            return "power_glitch";
        case ESP_RST_CPU_LOCKUP:
            return "cpu_lockup";
        default:
            return "unknown";
    }
}

const char* BootDiagnostics::WakeCauseName(esp_sleep_wakeup_cause_t cause) {
    switch (cause) {
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            return "undefined";
        case ESP_SLEEP_WAKEUP_EXT0:
            return "ext0";
        case ESP_SLEEP_WAKEUP_EXT1:
            return "ext1";
        case ESP_SLEEP_WAKEUP_TIMER:
            return "timer";
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            return "touchpad";
        case ESP_SLEEP_WAKEUP_ULP:
            return "ulp";
        case ESP_SLEEP_WAKEUP_GPIO:
            return "gpio";
        case ESP_SLEEP_WAKEUP_UART:
            return "uart";
        case ESP_SLEEP_WAKEUP_WIFI:
            return "wifi";
        case ESP_SLEEP_WAKEUP_COCPU:
            return "cocpu";
        case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG:
            return "cocpu_trap";
        case ESP_SLEEP_WAKEUP_BT:
            return "bt";
        default:
            return "unknown";
    }
}
