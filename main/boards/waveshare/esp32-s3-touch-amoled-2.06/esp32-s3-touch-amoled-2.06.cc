#include "wifi_board.h"
#include "display/lcd_display.h"
#include "display/sh8601_panel_factory.h"
#include "esp_lcd_sh8601.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "config.h"
#include "axp2101.h"
#include "i2c_device.h"
#include "audio/local_recording_service.h"
#include "audream_device_control.h"
#include "storage/sd_card_storage.h"
#include "storage/temporary_ap_transfer_service.h"
#include "system/boot_diagnostics.h"
#include "system/battery_telemetry_service.h"
#include "system/audream_power_manager.h"
#include "blufi.h"
#if CONFIG_AUDREAM_WATCH_UI
#include "recording_button_intent.h"
#include "watch/watch_view.h"
#include "watch/watch_motion_runtime.h"
#include "watch/watch_rtc_runtime.h"
#include "system/device_time_service.h"
#include "display/lvgl_display/lvgl_theme.h"
#endif

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pm.h>
#include <esp_rom_sys.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "settings.h"

#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <memory>

#define TAG "WaveshareEsp32s3TouchAMOLED2inch06"

#if CONFIG_AUDREAM_WATCH_UI
LV_FONT_DECLARE(font_puhui_30_4);
#endif

namespace {

constexpr uint16_t kRecordingButtonHoldMs = 1500;
constexpr int64_t kDeepSleepWakeRecordHoldUs = static_cast<int64_t>(kRecordingButtonHoldMs) * 1000LL;
constexpr uint64_t kRecordingFeedbackScreenOnUs = 3000ULL * 1000ULL;
constexpr uint64_t kRecordingHoldRetryUs = 200ULL * 1000ULL;
#if CONFIG_AUDREAM_WATCH_UI
constexpr int64_t kWatchRecordingIntentTimeoutUs = 10LL * 1000LL * 1000LL;
#endif
constexpr bool kRecordingBlePeriodicStatusReportsEnabled = false;
#if CONFIG_AUDREAM_WATCH_UI
constexpr bool kRecordingAudioOutputPathEnabled = true;
#else
constexpr bool kRecordingAudioOutputPathEnabled = false;
#endif
constexpr uint8_t kRecordingAudioInputChannelCount = 3;
constexpr uint64_t kRecordingBleStartGraceUs = 2ULL * 1000ULL * 1000ULL;
constexpr uint64_t kRecordingBleReportIntervalUs = 5ULL * 60ULL * 1000ULL * 1000ULL;
constexpr uint64_t kRecordingBleReportWindowUs = 30ULL * 1000ULL * 1000ULL;
constexpr uint64_t kRecordingBleStatusRetryUs = 1ULL * 1000ULL * 1000ULL;
constexpr size_t kRecordingFeedbackDrawRows = 4;
constexpr int kFeedbackGlyphWidth = 5;
constexpr int kFeedbackGlyphHeight = 7;
constexpr int kFeedbackGlyphScale = 8;
constexpr int kFeedbackGlyphSpacing = kFeedbackGlyphScale;
constexpr int kFeedbackMaxGlyphCount = 7;
constexpr int kFeedbackMaxTextWidth =
    kFeedbackMaxGlyphCount * kFeedbackGlyphWidth * kFeedbackGlyphScale +
    (kFeedbackMaxGlyphCount - 1) * kFeedbackGlyphSpacing;
constexpr int kFeedbackTextHeight = kFeedbackGlyphHeight * kFeedbackGlyphScale;
constexpr uint16_t kFeedbackBackgroundColor = 0x0000;
constexpr uint16_t kFeedbackForegroundColor = 0xFFFF;
constexpr uint8_t kAxp2101I2cAddress = 0x34;
constexpr uint8_t kAxp2101LdoOnOffReg0 = 0x90;
constexpr uint8_t kAxp2101Aldo1VoltageReg = 0x92;
constexpr uint8_t kAxp2101Aldo1Aldo2Aldo3Aldo4EnableMask = 0x0F;
constexpr uint8_t kAxp2101Aldo1Voltage3300mv = (3300 - 500) / 100;
constexpr uint8_t kAxp2101AdcChannelEnableReg = 0x30;
constexpr uint8_t kAxp2101BatteryDetectionReg = 0x68;
constexpr uint8_t kAxp2101BatteryVoltageHighReg = 0x34;
constexpr uint8_t kAxp2101BatteryVoltageLowReg = 0x35;
constexpr uint16_t kRecordingLowBatteryStopVoltageMv = 3450;
constexpr uint16_t kRecordingEmergencyStopVoltageMv = 3350;
constexpr uint8_t kRecordingLowBatteryRequiredSamples = 3;
static_assert(kFeedbackMaxTextWidth <= DISPLAY_WIDTH);
static_assert(kFeedbackTextHeight <= DISPLAY_HEIGHT);

void RecoverI2cPinsBeforeMasterInit() {
    gpio_config_t io_config = {};
    io_config.pin_bit_mask = (1ULL << AUDIO_CODEC_I2C_SDA_PIN) | (1ULL << AUDIO_CODEC_I2C_SCL_PIN);
    io_config.mode = GPIO_MODE_OUTPUT_OD;
    io_config.pull_up_en = GPIO_PULLUP_ENABLE;
    io_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_config.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_config);

    gpio_set_level(AUDIO_CODEC_I2C_SDA_PIN, 1);
    gpio_set_level(AUDIO_CODEC_I2C_SCL_PIN, 1);
    esp_rom_delay_us(1000);

    for (int i = 0; i < 16; ++i) {
        gpio_set_level(AUDIO_CODEC_I2C_SCL_PIN, 0);
        esp_rom_delay_us(5);
        gpio_set_level(AUDIO_CODEC_I2C_SCL_PIN, 1);
        esp_rom_delay_us(5);
    }

    gpio_set_level(AUDIO_CODEC_I2C_SDA_PIN, 0);
    esp_rom_delay_us(5);
    gpio_set_level(AUDIO_CODEC_I2C_SCL_PIN, 1);
    esp_rom_delay_us(5);
    gpio_set_level(AUDIO_CODEC_I2C_SDA_PIN, 1);
    esp_rom_delay_us(1000);
}

void DrivePinLowForDeepSleep(gpio_num_t pin) {
    if (pin == GPIO_NUM_NC) {
        return;
    }
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    gpio_pullup_dis(pin);
    gpio_pulldown_en(pin);
}

void ReleasePinForDeepSleep(gpio_num_t pin) {
    if (pin == GPIO_NUM_NC) {
        return;
    }
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_pullup_dis(pin);
    gpio_pulldown_dis(pin);
}

using FeedbackGlyph = uint8_t[kFeedbackGlyphHeight];

const FeedbackGlyph* GetFeedbackGlyph(char character) {
    static constexpr FeedbackGlyph kGlyphA = {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001};
    static constexpr FeedbackGlyph kGlyphD = {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110};
    static constexpr FeedbackGlyph kGlyphE = {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111};
    static constexpr FeedbackGlyph kGlyphM = {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001};
    static constexpr FeedbackGlyph kGlyphO = {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110};
    static constexpr FeedbackGlyph kGlyphP = {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000};
    static constexpr FeedbackGlyph kGlyphR = {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001};
    static constexpr FeedbackGlyph kGlyphS = {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110};
    static constexpr FeedbackGlyph kGlyphT = {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100};
    static constexpr FeedbackGlyph kGlyphU = {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110};
    static constexpr FeedbackGlyph kGlyph0 = {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110};
    static constexpr FeedbackGlyph kGlyph1 = {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110};
    static constexpr FeedbackGlyph kGlyph2 = {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111};
    static constexpr FeedbackGlyph kGlyph3 = {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110};
    static constexpr FeedbackGlyph kGlyph4 = {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010};
    static constexpr FeedbackGlyph kGlyph5 = {0b11111, 0b10000, 0b10000, 0b11110, 0b00001, 0b00001, 0b11110};
    static constexpr FeedbackGlyph kGlyph6 = {0b01110, 0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110};
    static constexpr FeedbackGlyph kGlyph7 = {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000};
    static constexpr FeedbackGlyph kGlyph8 = {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110};
    static constexpr FeedbackGlyph kGlyph9 = {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001, 0b01110};
    static constexpr FeedbackGlyph kGlyphPercent = {0b11001, 0b11010, 0b00010, 0b00100, 0b01000, 0b01011, 0b10011};
    switch (character) {
        case 'A': return &kGlyphA;
        case 'D': return &kGlyphD;
        case 'E': return &kGlyphE;
        case 'M': return &kGlyphM;
        case 'O': return &kGlyphO;
        case 'P': return &kGlyphP;
        case 'R': return &kGlyphR;
        case 'S': return &kGlyphS;
        case 'T': return &kGlyphT;
        case 'U': return &kGlyphU;
        case '0': return &kGlyph0;
        case '1': return &kGlyph1;
        case '2': return &kGlyph2;
        case '3': return &kGlyph3;
        case '4': return &kGlyph4;
        case '5': return &kGlyph5;
        case '6': return &kGlyph6;
        case '7': return &kGlyph7;
        case '8': return &kGlyph8;
        case '9': return &kGlyph9;
        case '%': return &kGlyphPercent;
        default: return nullptr;
    }
}

}  // namespace

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        Probe("before configuration");
        initialized_ = Configure();
        Probe(initialized_ ? "after configuration" : "after failed configuration");
    }

    bool initialized() const {
        return initialized_;
    }

    bool PrepareForDeepSleep() {
        Probe("before deep sleep rail update");
        if (!UpdateRegWithRetry(kAxp2101LdoOnOffReg0, kAxp2101Aldo1Aldo2Aldo3Aldo4EnableMask, 0x00)) {
            ESP_LOGE(TAG, "Failed to configure PMIC rails for deep sleep");
            return false;
        }
        Probe("after deep sleep rail update");
        ESP_LOGI(TAG, "PMIC deep sleep rails configured: ALDO1/ALDO2/ALDO3/ALDO4 off");
        return true;
    }

    void PowerOffForIdleSleep() {
        ESP_LOGI(TAG, "PMIC software power-off requested for idle sleep");
        PowerOff();
    }

    bool ReadPowerSnapshot(int& level, bool& charging, bool& discharging) {
        uint8_t status1 = 0;
        uint8_t status2 = 0;
        uint8_t battery = 0;
        const esp_err_t status1_result = TryReadReg(0x00, &status1);
        const esp_err_t status2_result = TryReadReg(0x01, &status2);
        const esp_err_t battery_result = TryReadReg(0xA4, &battery);
        if (status1_result != ESP_OK || status2_result != ESP_OK || battery_result != ESP_OK) {
            ESP_LOGW(
                TAG,
                "PMIC battery snapshot read failed: status1=%s status2=%s battery=%s",
                esp_err_to_name(status1_result),
                esp_err_to_name(status2_result),
                esp_err_to_name(battery_result)
            );
            return false;
        }
        const int battery_direction = (status2 & 0b01100000) >> 5;
        charging = battery_direction == 1;
        discharging = battery_direction == 2;
        level = battery;
        ESP_LOGI(
            TAG,
            "PMIC battery snapshot: level=%d status1=0x%02x status2=0x%02x charging=%d discharging=%d",
            level,
            status1,
            status2,
            charging ? 1 : 0,
            discharging ? 1 : 0
        );
        return true;
    }

    bool ReadBatteryTelemetrySnapshot(BatteryTelemetrySnapshot& snapshot) {
        uint8_t status1 = 0;
        uint8_t status2 = 0;
        uint8_t battery = 0;
        uint8_t voltage_high = 0;
        uint8_t voltage_low = 0;
        const esp_err_t status1_result = TryReadReg(0x00, &status1);
        const esp_err_t status2_result = TryReadReg(0x01, &status2);
        const esp_err_t battery_result = TryReadReg(0xA4, &battery);
        const esp_err_t voltage_high_result = TryReadReg(
            kAxp2101BatteryVoltageHighReg,
            &voltage_high
        );
        const esp_err_t voltage_low_result = TryReadReg(
            kAxp2101BatteryVoltageLowReg,
            &voltage_low
        );
        if (
            status1_result != ESP_OK ||
            status2_result != ESP_OK ||
            battery_result != ESP_OK ||
            voltage_high_result != ESP_OK ||
            voltage_low_result != ESP_OK
        ) {
            ESP_LOGW(
                TAG,
                "PMIC telemetry read failed: status1=%s status2=%s battery=%s voltage_high=%s voltage_low=%s",
                esp_err_to_name(status1_result),
                esp_err_to_name(status2_result),
                esp_err_to_name(battery_result),
                esp_err_to_name(voltage_high_result),
                esp_err_to_name(voltage_low_result)
            );
            return false;
        }

        const int battery_direction = (status2 & 0b01100000) >> 5;
        snapshot.battery_percent = battery;
        snapshot.battery_voltage_mv =
            (static_cast<uint16_t>(voltage_high & 0x1F) << 8) |
            voltage_low;
        snapshot.status1 = status1;
        snapshot.status2 = status2;
        snapshot.flags = 0;
        if (battery_direction == 1) {
            snapshot.flags |= kBatteryTelemetryCharging;
        } else if (battery_direction == 2) {
            snapshot.flags |= kBatteryTelemetryDischarging;
        }
        if ((status1 & 0x20) != 0) {
            snapshot.flags |= kBatteryTelemetryUsbPowered;
        }
        return true;
    }

private:
    static constexpr int kWriteAttempts = 3;
    bool initialized_ = false;

    void Probe(const char* stage) {
        static constexpr uint8_t kProbeRegs[] = {
            0x00,
            0x01,
            0x22,
            kAxp2101LdoOnOffReg0,
            kAxp2101Aldo1VoltageReg,
            0xA4,
        };
        char summary[160] = {};
        size_t offset = 0;
        for (const uint8_t reg : kProbeRegs) {
            uint8_t value = 0;
            const esp_err_t result = TryReadReg(reg, &value);
            if (result == ESP_OK) {
                offset += snprintf(
                    summary + offset,
                    sizeof(summary) - offset,
                    " reg0x%02x=0x%02x",
                    reg,
                    value
                );
            } else {
                offset += snprintf(
                    summary + offset,
                    sizeof(summary) - offset,
                    " reg0x%02x=%s",
                    reg,
                    esp_err_to_name(result)
                );
            }
            if (offset >= sizeof(summary)) {
                break;
            }
        }
        ESP_LOGI(TAG, "PMIC probe %s:%s", stage, summary);
    }

    bool Configure() {
        static constexpr struct {
            uint8_t reg;
            uint8_t value;
        } kConfiguration[] = {
            {0x22, 0b110},
            {0x27, 0x10},
            {0x80, 0x01},
            {0x82, (3300 - 1500) / 100},
            {kAxp2101Aldo1VoltageReg, kAxp2101Aldo1Voltage3300mv},
            {0x93, (3300 - 500) / 100},
            {kAxp2101LdoOnOffReg0, kAxp2101Aldo1Aldo2Aldo3Aldo4EnableMask},
            {0x64, 0x02},
            {0x61, 0x02},
            {0x62, 0x0A},
            {0x63, 0x01},
        };
        for (const auto& entry : kConfiguration) {
            if (!WriteRegWithRetry(entry.reg, entry.value)) {
                return false;
            }
        }
        if (!UpdateRegWithRetry(kAxp2101AdcChannelEnableReg, 0x00, 0x01) ||
            !UpdateRegWithRetry(kAxp2101BatteryDetectionReg, 0x00, 0x01)) {
            return false;
        }
        return true;
    }

    bool WriteRegWithRetry(uint8_t reg, uint8_t value) {
        esp_err_t result = ESP_FAIL;
        for (int attempt = 1; attempt <= kWriteAttempts; ++attempt) {
            result = TryWriteReg(reg, value);
            if (result == ESP_OK) {
                uint8_t readback = 0;
                const esp_err_t readback_result = TryReadReg(reg, &readback);
                if (readback_result == ESP_OK) {
                    ESP_LOGI(
                        TAG,
                        "PMIC register write ok: reg=0x%02x value=0x%02x readback=0x%02x attempt=%d/%d",
                        reg,
                        value,
                        readback,
                        attempt,
                        kWriteAttempts
                    );
                } else {
                    ESP_LOGW(
                        TAG,
                        "PMIC register write ok but readback failed: reg=0x%02x value=0x%02x attempt=%d/%d error=%s",
                        reg,
                        value,
                        attempt,
                        kWriteAttempts,
                        esp_err_to_name(readback_result)
                    );
                }
                return true;
            }
            ESP_LOGW(
                TAG,
                "PMIC register write failed: reg=0x%02x attempt=%d/%d error=%s",
                reg,
                attempt,
                kWriteAttempts,
                esp_err_to_name(result)
            );
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        return false;
    }

    bool UpdateRegWithRetry(uint8_t reg, uint8_t clear_mask, uint8_t set_mask) {
        esp_err_t result = ESP_FAIL;
        for (int attempt = 1; attempt <= kWriteAttempts; ++attempt) {
            uint8_t value = 0;
            result = TryReadReg(reg, &value);
            if (result == ESP_OK) {
                const uint8_t updated = (value & ~clear_mask) | set_mask;
                result = TryWriteReg(reg, updated);
                if (result == ESP_OK) {
                    ESP_LOGI(
                        TAG,
                        "PMIC register update ok: reg=0x%02x old=0x%02x new=0x%02x clear=0x%02x set=0x%02x attempt=%d/%d",
                        reg,
                        value,
                        updated,
                        clear_mask,
                        set_mask,
                        attempt,
                        kWriteAttempts
                    );
                    return true;
                }
            } else {
                ESP_LOGW(
                    TAG,
                    "PMIC register update read failed: reg=0x%02x attempt=%d/%d error=%s",
                    reg,
                    attempt,
                    kWriteAttempts,
                    esp_err_to_name(result)
                );
            }
            ESP_LOGW(
                TAG,
                "PMIC register update failed: reg=0x%02x attempt=%d/%d error=%s",
                reg,
                attempt,
                kWriteAttempts,
                esp_err_to_name(result)
            );
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        return false;
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const sh8601_lcd_init_cmd_t vendor_specific_init[] = {
    // set display to qspi mode
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x63, (uint8_t []){0xFF}, 1, 10},
    {0x51, (uint8_t []){0x00}, 1, 10},
    {0x2A, (uint8_t []){0x00,0x16,0x01,0xAF}, 4, 0},
    {0x2B, (uint8_t []){0x00,0x00,0x01,0xF5}, 4, 0},
    {0x29, (uint8_t []){0x00}, 0, 10},
    {0x51, (uint8_t []){0xFF}, 1, 0},
};

// Display class for the Waveshare AMOLED panel.
class CustomLcdDisplay : public SpiLcdDisplay {
public:
#if CONFIG_AUDREAM_WATCH_UI
private:
    std::unique_ptr<WatchView> watch_view_;
    bool watch_port_resume_required_ = false;
    bool watch_display_idle_ = false;

public:
    ~CustomLcdDisplay() override {
        if (watch_view_ && Lock()) {
            watch_view_.reset();
            Unlock();
        }
    }

    bool SetupWatchUI(std::function<void(const WatchViewRequest&)> handler) override {
        if (watch_view_) { return true; }
        if (setup_ui_called_ || !display_ || !Lock(3000)) { return false; }
        auto* theme = static_cast<LvglTheme*>(current_theme_);
        // The basic font omits characters used by localized watch controls.
        const auto* text_font = &font_puhui_30_4;
        const auto* digits_font = &lv_font_montserrat_48;
        watch_view_ = std::make_unique<WatchView>(lv_display_get_screen_active(display_), text_font,
            digits_font, theme->icon_font()->font(), std::move(handler), []() {
                Application::GetInstance().Schedule([]() {
                    AudreamPowerManager::GetInstance().NotifyUserActivity("watch_touch");
                });
            });
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, nullptr);
        setup_ui_called_ = true;
        Unlock();
        return true;
    }

    void UpdateWatchState(const WatchViewState& state) override {
        const bool idle = AudreamPowerManager::GetInstance().IsDisplayIdle();
        bool visibility_changed = false;
        if (watch_view_ && Lock(1000)) {
            watch_view_->Update(state);
            if (!watch_port_resume_required_ && watch_display_idle_ != idle) {
                watch_view_->SetVisible(!idle);
                watch_display_idle_ = idle;
                visibility_changed = true;
            }
            Unlock();
        }
        if (visibility_changed) {
            if (auto* backlight = Board::GetInstance().GetBacklight()) {
                if (idle) backlight->SetBrightness(0);
                else backlight->RestoreBrightness();
            }
        }
    }

    void CompleteWatchRequest(uint64_t id, bool succeeded) override {
        if (watch_view_ && Lock()) {
            watch_view_->Complete(id, succeeded);
            Unlock();
        }
    }

    void SetPowerSaveMode(bool on) override {
        if (watch_view_) {
            if (Lock(1000)) {
                watch_view_->SetVisible(!on);
                watch_display_idle_ = on;
                Unlock();
            }
        } else {
            SpiLcdDisplay::SetPowerSaveMode(on);
        }
    }

    void UpdateStatusBar(bool update_all = false) override {
        if (!watch_view_) { SpiLcdDisplay::UpdateStatusBar(update_all); }
    }

    bool PauseWatchForSleep() {
        if (!watch_view_ || !Lock(1500)) { return false; }
        watch_view_->SetVisible(false);
        watch_port_resume_required_ = true;
        const esp_err_t result = lvgl_port_stop();
        Unlock();
        return result == ESP_OK;
    }

    bool RestoreWatchAfterSleepCancellation() {
        if (!watch_view_ || !Lock(1500)) { return false; }
        if (watch_port_resume_required_) {
            const esp_err_t result = lvgl_port_resume();
            if (result != ESP_OK) { Unlock(); return false; }
            watch_port_resume_required_ = false;
        }
        watch_view_->SetVisible(true);
        watch_display_idle_ = false;
        Unlock();
        return true;
    }
#endif

    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t* )lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        uint16_t y1 = area->y1;
        uint16_t y2 = area->y2;

        // round the start of coordinate down to the nearest 2M number
        area->x1 = (x1 >> 1) << 1;
        area->y1 = (y1 >> 1) << 1;
        // round the end of coordinate up to the nearest 2N+1 number
        area->x2 = ((x2 >> 1) << 1) + 1;
        area->y2 = ((y2 >> 1) << 1) + 1;
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width,
                     int height,
                     int offset_x,
                     int offset_y,
                     bool mirror_x,
                     bool mirror_y,
                     bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle,
                        width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
#if CONFIG_AUDREAM_WATCH_UI
        if (watch_view_) { return; }
#endif
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES*  0.1, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES*  0.1, 0);
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }

    lv_display_t* LvglHandle() const { return display_; }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io, bool panel_awake = false)
        : Backlight(), panel_io_(panel_io), panel_awake_(panel_awake) {}

    void SetImmediateBrightness(uint8_t brightness) {
        if (brightness > 100) {
            brightness = 100;
        }
        if (transition_timer_ != nullptr) {
            esp_timer_stop(transition_timer_);
        }
        brightness_ = brightness;
        target_brightness_ = brightness;
        SetBrightnessImpl(brightness);
    }

    bool WakeForRecordingFeedback() {
        if (!panel_awake_) {
            const esp_err_t sleep_out_result = SendCommand(0x11, nullptr, 0);
            if (sleep_out_result != ESP_OK) {
                return false;
            }
            esp_rom_delay_us(120000);
            panel_awake_ = true;
        }
        const esp_err_t display_on_result = SendCommand(0x29, nullptr, 0);
        return display_on_result == ESP_OK;
    }

    bool PrepareForDeepSleep() {
        SetImmediateBrightness(0);
        const esp_err_t display_off = SendCommand(0x28, nullptr, 0);
        const esp_err_t sleep_in = SendCommand(0x10, nullptr, 0);
        esp_rom_delay_us(120000);
        if (sleep_in == ESP_OK) { panel_awake_ = false; }
        return display_off == ESP_OK && sleep_in == ESP_OK;
    }

protected:
    esp_lcd_panel_io_handle_t panel_io_;
    bool panel_awake_ = false;

    esp_err_t SendCommand(uint8_t command, const void* data, size_t size) {
        int lcd_cmd = command;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        const esp_err_t result = esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, data, size);
        if (result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Recording feedback panel command failed: command=0x%02x size=%u result=%s",
                command,
                static_cast<unsigned int>(size),
                esp_err_to_name(result)
            );
        }
        return result;
    }

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255*  brightness) / 100))};
        SendCommand(0x51, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED2inch06 : public WifiBoard {
private:
    enum class RecordingHoldStartSource {
        kButton,
        kDeepSleepWake,
        kWatchControl,
    };

    i2c_master_bus_handle_t i2c_bus_;
#if CONFIG_AUDREAM_WATCH_UI
    std::unique_ptr<WatchMotionRuntime> motion_;
    WatchRtcRuntime rtc_;
    std::atomic<bool> rtc_save_pending_{false};
    std::mutex rtc_save_mutex_;

    void InitializeWatchRtc() {
        const bool ready = rtc_.Begin(i2c_bus_);
        uint64_t utc = 0;
        auto& time = DeviceTimeService::GetInstance();
        if (ready && !time.IsSynchronized() && rtc_.ReadTime(utc) && utc >= 1600000000000ULL) {
            time.SyncUnixTimeMs(utc);
            ESP_LOGI(TAG, "Watch UTC restored from external RTC");
        } else {
            ESP_LOGI(TAG, "External RTC initialization: bus_ready=%d system_time_valid=%d", ready, time.IsSynchronized());
        }
        AudreamDeviceControl::GetInstance().SetTimeSynchronizedCallback([this]() {
            rtc_save_pending_.store(true);
            Application::GetInstance().Schedule([this]() {
                if (AudreamPowerManager::GetInstance().State() != AudreamPowerState::kDeepSleepPreparing)
                    FlushWatchRtc();
            });
        });
    }

    void FlushWatchRtc() {
        std::lock_guard<std::mutex> lock(rtc_save_mutex_);
        if (!rtc_save_pending_.exchange(false)) return;
        const auto utc = DeviceTimeService::GetInstance().CurrentUnixTimeMs();
        if (utc < 1600000000000ULL || !rtc_.SaveTime(utc)) {
            rtc_save_pending_.store(true);
            ESP_LOGW(TAG, "External RTC UTC write was not verified; system clock remains available");
        } else {
            ESP_LOGI(TAG, "External RTC UTC write verified");
        }
    }
#endif
    Pmic* pmic_ = nullptr;
    SemaphoreHandle_t pmic_mutex_ = nullptr;
    CustomLcdDisplay* display_ = nullptr;
    CustomBacklight* backlight_ = nullptr;
    std::mutex panel_init_mutex_;
    bool panel_spi_initialized_ = false;
    esp_lcd_touch_handle_t touch_ = nullptr;
    esp_lcd_panel_io_handle_t touch_io_ = nullptr;
    lv_indev_t* touch_input_ = nullptr;
    bool watch_peripherals_prepared_ = false;
    Button boot_button_;
    esp_timer_handle_t power_status_timer_ = nullptr;
    esp_timer_handle_t deep_sleep_record_hold_timer_ = nullptr;
    esp_timer_handle_t recording_feedback_screen_timer_ = nullptr;
    esp_timer_handle_t recording_hold_retry_timer_ = nullptr;
    esp_timer_handle_t recording_ble_report_timer_ = nullptr;
    esp_timer_handle_t recording_ble_close_timer_ = nullptr;
    esp_timer_handle_t recording_ble_status_retry_timer_ = nullptr;
    LocalRecordingMetadata pending_recording_stop_event_;
    bool pending_recording_stop_event_valid_ = false;
    bool power_source_initialized_ = false;
    bool usb_powered_ = false;
    bool recording_button_pressed_ = false;
    bool recording_hold_start_pending_ = false;
    WatchRecordingError watch_recording_error_ = WatchRecordingError::kNone;
#if CONFIG_AUDREAM_WATCH_UI
    RecordingButtonIntent recording_button_intent_{kDeepSleepWakeRecordHoldUs};
    int64_t recording_hold_deadline_us_ = 0;
#endif
    RecordingHoldStartSource recording_hold_start_source_ = RecordingHoldStartSource::kButton;
    std::atomic<bool> recording_feedback_display_task_started_{false};
    std::atomic<bool> recording_feedback_display_initialized_{false};
    std::atomic<bool> recording_feedback_pulse_pending_{false};
    esp_lcd_panel_handle_t recording_feedback_panel_ = nullptr;
    uint16_t* recording_feedback_draw_buffer_ = nullptr;
    uint16_t* recording_feedback_logo_tile_buffer_ = nullptr;
    const char* recording_feedback_text_ = "AUDREAM";
    char boot_battery_feedback_text_[8] = {};
    bool pending_deep_sleep_recording_ = false;
    bool suppress_next_wake_long_press_ = false;
    bool deep_sleep_wake_ = false;
    bool pmic_recovery_task_started_ = false;
    uint8_t recording_low_battery_sample_count_ = 0;
    std::atomic<bool> recording_low_battery_stop_pending_{false};
    int64_t deep_sleep_wake_button_down_us_ = 0;

    void InitializeCodecI2c() {
        if (deep_sleep_wake_) {
            RecoverI2cPinsBeforeMasterInit();
        }
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        if (!TryInitializePmic()) {
            ESP_LOGW(TAG, "PMIC recovery will be retried after audio codec initialization");
        }
    }

    bool TryInitializePmic() {
        if (pmic_ != nullptr) {
            return true;
        }
        if (pmic_mutex_ != nullptr) {
            xSemaphoreTake(pmic_mutex_, portMAX_DELAY);
        }
        if (pmic_ != nullptr) {
            if (pmic_mutex_ != nullptr) {
                xSemaphoreGive(pmic_mutex_);
            }
            return true;
        }
        auto* pmic = new Pmic(i2c_bus_, kAxp2101I2cAddress);
        if (!pmic->initialized()) {
            ESP_LOGE(TAG, "AXP2101 initialization failed; continuing without PMIC control");
            delete pmic;
            if (pmic_mutex_ != nullptr) {
                xSemaphoreGive(pmic_mutex_);
            }
            return false;
        }
        pmic_ = pmic;
        if (pmic_mutex_ != nullptr) {
            xSemaphoreGive(pmic_mutex_);
        }
        return true;
    }

    void SchedulePmicRecoveryTask(const char* reason) {
        if (pmic_ != nullptr || pmic_recovery_task_started_) {
            return;
        }
        pmic_recovery_task_started_ = true;
        ESP_LOGW(TAG, "Scheduling PMIC recovery task after %s", reason);
        const BaseType_t result = xTaskCreate(
            [](void* arg) {
                auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg);
                if (board != nullptr) {
                    board->RunPmicRecoveryTask();
                }
                vTaskDelete(nullptr);
            },
            "pmic_recovery",
            4096,
            this,
            tskIDLE_PRIORITY + 1,
            nullptr
        );
        if (result != pdPASS) {
            pmic_recovery_task_started_ = false;
            ESP_LOGE(TAG, "Failed to create PMIC recovery task");
        }
    }

    void RunPmicRecoveryTask() {
        static constexpr int kRetryAttempts = 90;
        static constexpr TickType_t kRetryDelay = pdMS_TO_TICKS(1000);
        for (int attempt = 1; attempt <= kRetryAttempts; ++attempt) {
            if (pmic_ != nullptr) {
                ESP_LOGI(TAG, "PMIC recovery task stopped because PMIC is already available");
                return;
            }
            if (TryInitializePmic()) {
                ESP_LOGI(
                    TAG,
                    "PMIC recovery task completed: attempt=%d/%d",
                    attempt,
                    kRetryAttempts
                );
                return;
            }
            if (attempt == 1 || attempt % 10 == 0) {
                ESP_LOGW(
                    TAG,
                    "PMIC recovery task still waiting: attempt=%d/%d",
                    attempt,
                    kRetryAttempts
                );
            }
            vTaskDelay(kRetryDelay);
        }
        pmic_recovery_task_started_ = false;
        ESP_LOGE(TAG, "PMIC recovery task gave up after retries");
    }

    bool EnsurePmicReady(const char* reason, int retry_attempts, TickType_t retry_delay) {
        if (pmic_ != nullptr) {
            return true;
        }
        ESP_LOGW(TAG, "PMIC is unavailable before %s; retrying initialization", reason);
        for (int attempt = 1; attempt <= retry_attempts; ++attempt) {
            if (TryInitializePmic()) {
                ESP_LOGI(
                    TAG,
                    "PMIC initialization recovered before %s: attempt=%d/%d",
                    reason,
                    attempt,
                    retry_attempts
                );
                return true;
            }
            ESP_LOGW(
                TAG,
                "PMIC initialization retry before %s failed: attempt=%d/%d",
                reason,
                attempt,
                retry_attempts
            );
            vTaskDelay(retry_delay);
        }
        ESP_LOGE(TAG, "PMIC unavailable before %s after retries", reason);
        return false;
    }

    bool EnsurePmicReadyForDeepSleep() {
        static constexpr int kRetryAttempts = 3;
        static constexpr TickType_t kRetryDelay = pdMS_TO_TICKS(100);
        return EnsurePmicReady("deep sleep", kRetryAttempts, kRetryDelay);
    }

    void UpdateUsbPowerState(bool force_log = false) {
        if (pmic_ == nullptr) {
            return;
        }
        const bool was_initialized = power_source_initialized_;
        const bool was_usb_powered = usb_powered_;
        const uint8_t status1 = pmic_->GetStatus1();
        const uint8_t status2 = pmic_->GetStatus2();
        const bool usb_powered = pmic_->IsVbusIn();
        const bool changed = !power_source_initialized_ || usb_powered_ != usb_powered;
        usb_powered_ = usb_powered;
        power_source_initialized_ = true;
        AudreamPowerManager::GetInstance().SetUsbPowered(usb_powered);
        if (changed || force_log) {
            ESP_LOGI(
                TAG,
                "Power source status: usb_powered=%d status1=0x%02x status2=0x%02x charging=%d discharging=%d",
                usb_powered ? 1 : 0,
                status1,
                status2,
                pmic_->IsCharging() ? 1 : 0,
                pmic_->IsDischarging() ? 1 : 0
            );
        }
        if (was_initialized && was_usb_powered != usb_powered) {
            BatteryTelemetryService::GetInstance().RecordEvent(
                usb_powered
                    ? BatteryTelemetryEvent::kUsbConnected
                    : BatteryTelemetryEvent::kUsbDisconnected
            );
        }
    }

    void CheckRecordingLowBattery() {
        auto& recording = LocalRecordingService::GetInstance();
        if (!recording.IsRecording()) {
            recording_low_battery_sample_count_ = 0;
            recording_low_battery_stop_pending_.store(false);
            return;
        }

        BatteryTelemetrySnapshot snapshot;
        if (!ReadBatteryTelemetrySnapshot(snapshot)) {
            return;
        }
        const bool externally_powered =
            (snapshot.flags & (kBatteryTelemetryUsbPowered | kBatteryTelemetryCharging)) != 0;
        if (externally_powered) {
            recording_low_battery_sample_count_ = 0;
            return;
        }
        if (snapshot.battery_voltage_mv == 0) {
            return;
        }

        const bool emergency =
            snapshot.battery_voltage_mv <= kRecordingEmergencyStopVoltageMv;
        if (snapshot.battery_voltage_mv <= kRecordingLowBatteryStopVoltageMv) {
            recording_low_battery_sample_count_ = std::min<uint8_t>(
                static_cast<uint8_t>(recording_low_battery_sample_count_ + 1),
                kRecordingLowBatteryRequiredSamples
            );
        } else {
            recording_low_battery_sample_count_ = 0;
        }
        if (!emergency &&
            recording_low_battery_sample_count_ < kRecordingLowBatteryRequiredSamples) {
            return;
        }

        bool expected = false;
        if (!recording_low_battery_stop_pending_.compare_exchange_strong(expected, true)) {
            return;
        }
        ESP_LOGW(
            TAG,
            "Stopping local recording for low battery: level=%u voltage=%u mV emergency=%d samples=%u",
            static_cast<unsigned>(snapshot.battery_percent),
            static_cast<unsigned>(snapshot.battery_voltage_mv),
            emergency ? 1 : 0,
            static_cast<unsigned>(recording_low_battery_sample_count_)
        );
        Application::GetInstance().Schedule([this]() {
            StopRecordingForLowBattery();
        });
    }

    void StopRecordingForLowBattery() {
        auto& recording = LocalRecordingService::GetInstance();
        if (!recording.IsRecording()) {
            recording_low_battery_stop_pending_.store(false);
            ESP_LOGI(TAG, "Low-battery recording stop was canceled because recording already stopped");
            return;
        }

        auto& audio_service = Application::GetInstance().GetAudioService();
        const auto metadata = recording.Stop(audio_service);
        if (recording.IsRecording()) {
            recording_low_battery_stop_pending_.store(false);
            ESP_LOGE(TAG, "Low-battery stop did not finish; retaining recording state for retry");
            return;
        }
        BatteryTelemetryService::GetInstance().RecordEvent(
            BatteryTelemetryEvent::kRecordingStopped
        );
        PulseRecordingFeedbackScreen("STOP");
        if (metadata.valid) {
            ESP_LOGW(
                TAG,
                "Low battery stopped local recording: id=%s duration=%lu ms size=%lu bytes",
                metadata.recording_id.c_str(),
                static_cast<unsigned long>(metadata.duration_ms),
                static_cast<unsigned long>(metadata.size_bytes)
            );
        } else {
            ESP_LOGE(TAG, "Low-battery recording stop did not produce valid metadata");
        }
        StopRecordingBleReporting(metadata);
        recording_low_battery_sample_count_ = 0;
        recording_low_battery_stop_pending_.store(false);
    }

    bool ReadBatteryTelemetrySnapshot(BatteryTelemetrySnapshot& snapshot) {
        if (pmic_ == nullptr && !TryInitializePmic()) {
            return false;
        }
        if (pmic_mutex_ != nullptr) {
            xSemaphoreTake(pmic_mutex_, portMAX_DELAY);
        }
        const bool success = pmic_ != nullptr && pmic_->ReadBatteryTelemetrySnapshot(snapshot);
        if (pmic_mutex_ != nullptr) {
            xSemaphoreGive(pmic_mutex_);
        }
        if (!success) {
            return false;
        }

        auto& recording = LocalRecordingService::GetInstance();
        if (recording.IsRecording()) {
            snapshot.flags |= kBatteryTelemetryRecording;
        }
        if (recording.IsPaused()) {
            snapshot.flags |= kBatteryTelemetryRecordingPaused;
        }
        if (TemporaryAPTransferService::GetInstance().IsActive()) {
            snapshot.flags |= kBatteryTelemetryTransferActive;
        }
        return true;
    }

    void InitializeBatteryTelemetry() {
        BatteryTelemetryService::GetInstance().Start(
            [this](BatteryTelemetrySnapshot& snapshot) {
                return ReadBatteryTelemetrySnapshot(snapshot);
            }
        );
    }

    void InitializeRecordingPowerManagement() {
#if CONFIG_PM_ENABLE
        const esp_pm_config_t config = {
            .max_freq_mhz = 240,
            .min_freq_mhz = 40,
            .light_sleep_enable = true,
        };
        ESP_ERROR_CHECK(esp_pm_configure(&config));
        ESP_LOGI(TAG, "Automatic light sleep and dynamic CPU frequency are enabled");
#endif
        auto& power_manager = AudreamPowerManager::GetInstance();
        power_manager.Start(BOOT_BUTTON_GPIO, UsesIdleDeepSleep(GetRuntimeProfile()));
        UpdateUsbPowerState(true);

        const esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg);
                if (board == nullptr) {
                    return;
                }
                board->UpdateUsbPowerState();
                board->CheckRecordingLowBattery();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "audream_power_status",
            .skip_unhandled_events = true,
        };
        const esp_err_t create_result = esp_timer_create(&timer_args, &power_status_timer_);
        if (create_result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Failed to create power status timer: %s",
                esp_err_to_name(create_result)
            );
            return;
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_timer_start_periodic(power_status_timer_, 5000000ULL)
        );
    }

    bool InitializeSpi() {
        if (panel_spi_initialized_) { return true; }
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        const esp_err_t result = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Display SPI initialization failed: %s", esp_err_to_name(result));
            return false;
        }
        panel_spi_initialized_ = true;
        return true;
    }

    bool CreateDisplayPanel(esp_lcd_panel_io_handle_t& panel_io, esp_lcd_panel_handle_t& panel) {
        if (!InitializeSpi()) { return false; }
        const esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
            EXAMPLE_PIN_NUM_LCD_CS, nullptr, nullptr);
        const sh8601_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(sh8601_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = const_cast<sh8601_vendor_config_t*>(&vendor_config);
        const esp_err_t result = CreateSh8601Panel(SPI2_HOST, io_config, panel_config,
            0x16, 0, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, panel_io, panel);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Display panel initialization failed: %s", esp_err_to_name(result));
            return false;
        }
        return true;
    }

    bool InitializeSH8601Display() {
        std::lock_guard<std::mutex> lock(panel_init_mutex_);
        if (display_) { return display_->LvglHandle() != nullptr; }
        if (recording_feedback_panel_ || recording_feedback_display_task_started_.load()) {
            ESP_LOGE(TAG, "LVGL initialization rejected: recording feedback owns the panel");
            return false;
        }
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        if (!CreateDisplayPanel(panel_io, panel)) { return false; }
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io, true);
        backlight_->RestoreBrightness();
        return display_->LvglHandle() != nullptr;
    }

    void InitializeRecordingFeedbackDisplay() {
        if (recording_feedback_display_task_started_.exchange(true)) {
            return;
        }
        const BaseType_t created = xTaskCreate(
            [](void* arg) {
                auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg);
                if (board != nullptr) {
                    board->InitializeRecordingFeedbackDisplayTask();
                }
                vTaskDelete(nullptr);
            },
            "rec_display",
            8192,
            this,
            4,
            nullptr
        );
        if (created != pdPASS) {
            recording_feedback_display_task_started_ = false;
            ESP_LOGE(TAG, "Failed to create recording feedback display task");
            return;
        }
    }

    bool DrawRecordingFeedbackFrame() {
        if (recording_feedback_panel_ == nullptr) {
            ESP_LOGE(TAG, "Cannot draw recording feedback frame because the panel is unavailable");
            return false;
        }
        constexpr size_t pixel_count = DISPLAY_WIDTH * kRecordingFeedbackDrawRows;
        if (recording_feedback_draw_buffer_ == nullptr) {
            recording_feedback_draw_buffer_ = static_cast<uint16_t*>(heap_caps_malloc(
                pixel_count * sizeof(uint16_t),
                MALLOC_CAP_DMA | MALLOC_CAP_8BIT
            ));
            if (recording_feedback_draw_buffer_ == nullptr) {
                ESP_LOGE(
                    TAG,
                    "Failed to allocate recording feedback draw buffer: bytes=%u",
                    static_cast<unsigned int>(pixel_count * sizeof(uint16_t))
                );
                return false;
            }
        }
        std::fill_n(
            recording_feedback_draw_buffer_,
            pixel_count,
            kFeedbackBackgroundColor
        );

        for (int y = 0; y < DISPLAY_HEIGHT; y += kRecordingFeedbackDrawRows) {
            const int y_end = std::min(
                y + static_cast<int>(kRecordingFeedbackDrawRows),
                static_cast<int>(DISPLAY_HEIGHT)
            );
            const esp_err_t draw_result = esp_lcd_panel_draw_bitmap(
                recording_feedback_panel_,
                0,
                y,
                DISPLAY_WIDTH,
                y_end,
                recording_feedback_draw_buffer_
            );
            if (draw_result != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "Failed to draw recording feedback frame: y=%d result=%s",
                    y,
                    esp_err_to_name(draw_result)
                );
                return false;
            }
        }

        constexpr size_t logo_tile_pixel_count =
            kFeedbackGlyphScale * kFeedbackGlyphScale;
        if (recording_feedback_logo_tile_buffer_ == nullptr) {
            recording_feedback_logo_tile_buffer_ = static_cast<uint16_t*>(heap_caps_malloc(
                logo_tile_pixel_count * sizeof(uint16_t),
                MALLOC_CAP_DMA | MALLOC_CAP_8BIT
            ));
            if (recording_feedback_logo_tile_buffer_ == nullptr) {
                ESP_LOGE(
                    TAG,
                    "Failed to allocate recording feedback logo buffer: bytes=%u",
                    static_cast<unsigned int>(logo_tile_pixel_count * sizeof(uint16_t))
                );
                return false;
            }
        }
        std::fill_n(
            recording_feedback_logo_tile_buffer_,
            logo_tile_pixel_count,
            kFeedbackForegroundColor
        );

        const char* text = recording_feedback_text_;
        if (text == nullptr || text[0] == '\0') {
            text = "AUDREAM";
        }
        const int glyph_count = std::min(
            static_cast<int>(std::strlen(text)),
            kFeedbackMaxGlyphCount
        );
        const int text_width =
            glyph_count * kFeedbackGlyphWidth * kFeedbackGlyphScale +
            std::max(0, glyph_count - 1) * kFeedbackGlyphSpacing;
        const int logo_x = ((DISPLAY_WIDTH - text_width) / 2) & ~1;
        const int logo_y = ((DISPLAY_HEIGHT - kFeedbackTextHeight) / 2) & ~1;
        const int glyph_advance =
            kFeedbackGlyphWidth * kFeedbackGlyphScale + kFeedbackGlyphSpacing;
        for (int glyph = 0; glyph < glyph_count; ++glyph) {
            const FeedbackGlyph* glyph_data = GetFeedbackGlyph(text[glyph]);
            if (glyph_data == nullptr) {
                continue;
            }
            for (int row = 0; row < kFeedbackGlyphHeight; ++row) {
                const uint8_t row_bits = (*glyph_data)[row];
                for (int column = 0; column < kFeedbackGlyphWidth; ++column) {
                    const uint8_t mask = 1U << (kFeedbackGlyphWidth - column - 1);
                    if ((row_bits & mask) == 0) {
                        continue;
                    }
                    const int x = logo_x + glyph * glyph_advance +
                        column * kFeedbackGlyphScale;
                    const int y = logo_y + row * kFeedbackGlyphScale;
                    const esp_err_t draw_result = esp_lcd_panel_draw_bitmap(
                        recording_feedback_panel_,
                        x,
                        y,
                        x + kFeedbackGlyphScale,
                        y + kFeedbackGlyphScale,
                        recording_feedback_logo_tile_buffer_
                    );
                    if (draw_result != ESP_OK) {
                        ESP_LOGE(
                            TAG,
                            "Failed to draw recording feedback logo: glyph=%d row=%d column=%d result=%s",
                            glyph,
                            row,
                            column,
                            esp_err_to_name(draw_result)
                        );
                        return false;
                    }
                }
            }
        }
        return true;
    }

    void InitializeRecordingFeedbackDisplayTask() {
        std::lock_guard<std::mutex> lock(panel_init_mutex_);
        if (display_) {
            recording_feedback_display_task_started_ = false;
            PulseRecordingFeedbackScreen();
            return;
        }
        if (!recording_feedback_panel_) {
            esp_lcd_panel_io_handle_t panel_io = nullptr;
            if (!CreateDisplayPanel(panel_io, recording_feedback_panel_)) {
                recording_feedback_display_task_started_ = false;
                return;
            }
            backlight_ = new CustomBacklight(panel_io, true);
        }
        backlight_->SetImmediateBrightness(0);
        if (!DrawRecordingFeedbackFrame()) {
            recording_feedback_display_task_started_ = false;
            return;
        }
        recording_feedback_display_initialized_ = true;
        ESP_LOGI(TAG, "Recording feedback display initialized");
        if (recording_feedback_pulse_pending_) {
            PulseRecordingFeedbackScreen();
        }
    }

    void PulseRecordingFeedbackScreen(const char* text = nullptr) {
        if (display_) {
            // LVGL owns the panel for the entire combined-mode boot. Never
            // initialize or draw the lightweight feedback path over its buffer.
            Application::GetInstance().Schedule([this]() {
                AudreamPowerManager::GetInstance().NotifyUserActivity("watch_recording_feedback");
                if (backlight_ && backlight_->WakeForRecordingFeedback()) {
                    display_->SetPowerSaveMode(false);
                    backlight_->RestoreBrightness();
                }
            });
            return;
        }
#if CONFIG_AUDREAM_WATCH_UI
        ESP_LOGW(TAG, "Recording feedback skipped because the watch display is unavailable");
        return;
#endif
        if (text != nullptr && text[0] != '\0') {
            recording_feedback_text_ = text;
        }
        if (!recording_feedback_display_initialized_) {
            recording_feedback_pulse_pending_ = true;
            InitializeRecordingFeedbackDisplay();
            return;
        }
        if (backlight_ == nullptr) {
            ESP_LOGE(TAG, "Recording feedback screen pulse skipped because backlight is unavailable");
            return;
        }
        if (!backlight_->WakeForRecordingFeedback()) {
            ESP_LOGE(TAG, "Recording feedback screen pulse stopped because panel wake failed");
            return;
        }
        if (!DrawRecordingFeedbackFrame()) {
            ESP_LOGE(TAG, "Recording feedback screen pulse stopped because drawing failed");
            return;
        }
        recording_feedback_pulse_pending_ = false;
        backlight_->RestoreBrightness();
        if (recording_feedback_screen_timer_ == nullptr) {
            const esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg);
                    if (board == nullptr) {
                        return;
                    }
                    board->TurnOffRecordingFeedbackScreen();
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "audream_rec_screen",
                .skip_unhandled_events = true,
            };
            const esp_err_t result = esp_timer_create(&timer_args, &recording_feedback_screen_timer_);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Failed to create recording feedback screen timer: %s", esp_err_to_name(result));
                return;
            }
        }
        const esp_err_t stop_result = esp_timer_stop(recording_feedback_screen_timer_);
        if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Failed to stop recording feedback screen timer: %s", esp_err_to_name(stop_result));
        }
        const esp_err_t start_result =
            esp_timer_start_once(recording_feedback_screen_timer_, kRecordingFeedbackScreenOnUs);
        if (start_result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Failed to start recording feedback screen timer: %s",
                esp_err_to_name(start_result)
            );
            return;
        }
    }

    void TurnOffRecordingFeedbackScreen() {
        if (backlight_ == nullptr) {
            ESP_LOGW(TAG, "Recording feedback screen off skipped because backlight is unavailable");
            return;
        }
        backlight_->SetImmediateBrightness(0);
    }

    void PulseBootBatteryScreen() {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        if (!GetBatteryLevel(level, charging, discharging)) {
            PulseRecordingFeedbackScreen("AUDREAM");
            return;
        }
        level = std::clamp(level, 0, 100);
        std::snprintf(boot_battery_feedback_text_, sizeof(boot_battery_feedback_text_), "%d%%", level);
        PulseRecordingFeedbackScreen(boot_battery_feedback_text_);
    }

    void ConfigureDisplayPinsForDeepSleep() {
        static constexpr gpio_num_t kDisplayPins[] = {
            EXAMPLE_PIN_NUM_LCD_CS,
            EXAMPLE_PIN_NUM_LCD_PCLK,
            EXAMPLE_PIN_NUM_LCD_DATA0,
            EXAMPLE_PIN_NUM_LCD_DATA1,
            EXAMPLE_PIN_NUM_LCD_DATA2,
            EXAMPLE_PIN_NUM_LCD_DATA3,
            EXAMPLE_PIN_NUM_LCD_RST,
        };
        for (const gpio_num_t pin : kDisplayPins) {
            DrivePinLowForDeepSleep(pin);
        }
    }

    void ConfigureSdPinsForDeepSleep() {
        static constexpr gpio_num_t kSdPins[] = {
            AUDREAM_SD_CLK_GPIO,
            AUDREAM_SD_MOSI_GPIO,
            AUDREAM_SD_MISO_GPIO,
            AUDREAM_SD_CS_GPIO,
        };
        for (const gpio_num_t pin : kSdPins) {
            DrivePinLowForDeepSleep(pin);
        }
    }

    void ConfigureAudioPinsForDeepSleep() {
        static constexpr gpio_num_t kAudioPins[] = {
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_CODEC_PA_PIN,
        };
        for (const gpio_num_t pin : kAudioPins) {
            DrivePinLowForDeepSleep(pin);
        }
    }

    void ConfigureI2cPinsForDeepSleep() {
        ReleasePinForDeepSleep(AUDIO_CODEC_I2C_SDA_PIN);
        ReleasePinForDeepSleep(AUDIO_CODEC_I2C_SCL_PIN);
    }

    void PrepareAudioForDeepSleep() {
        auto* codec = GetAudioCodec();
        if (codec == nullptr) {
            return;
        }
        codec->PrepareForDeepSleep();
    }

    void PrepareBoardForDeepSleep() {
        auto& battery_telemetry = BatteryTelemetryService::GetInstance();
        if (!battery_telemetry.FlushBeforeSleep()) {
            ESP_LOGW(TAG, "Battery telemetry could not be flushed before deep sleep");
        }
        battery_telemetry.Stop();
        if (recording_feedback_screen_timer_ != nullptr) {
            const esp_err_t stop_result = esp_timer_stop(recording_feedback_screen_timer_);
            if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(
                    TAG,
                    "Failed to stop recording feedback screen timer before deep sleep: %s",
                    esp_err_to_name(stop_result)
                );
            }
        }
        if (power_status_timer_ != nullptr) {
            const esp_err_t stop_result = esp_timer_stop(power_status_timer_);
            if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(
                    TAG,
                    "Failed to stop power status timer before deep sleep: %s",
                    esp_err_to_name(stop_result)
                );
            }
        }
        if (backlight_ != nullptr && !watch_peripherals_prepared_) {
            backlight_->PrepareForDeepSleep();
        }
        PrepareAudioForDeepSleep();
        SdCardStorage::GetInstance().PrepareForDeepSleep();
        ConfigureSdPinsForDeepSleep();
        ConfigureDisplayPinsForDeepSleep();
        ConfigureAudioPinsForDeepSleep();
        if (EnsurePmicReadyForDeepSleep()) {
            pmic_->PowerOffForIdleSleep();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ConfigureI2cPinsForDeepSleep();
    }

    // Caller holds panel_init_mutex_. Retain failed handles for the next retry.
    bool ReleaseTouch() {
        if (touch_input_) {
            if (lvgl_port_remove_touch(touch_input_) != ESP_OK) { return false; }
            touch_input_ = nullptr;
        }
        if (touch_) {
            if (esp_lcd_touch_del(touch_) != ESP_OK) { return false; }
            touch_ = nullptr;
        }
        if (touch_io_) {
            if (esp_lcd_panel_io_del(touch_io_) != ESP_OK) { return false; }
            touch_io_ = nullptr;
        }
        return true;
    }

#if CONFIG_AUDREAM_WATCH_UI
    bool PrepareWatchPeripheralsForSleep() {
        FlushWatchRtc();
        rtc_.Suspend();
        if (motion_ && !motion_->PrepareForSleep()) return false;
        {
            std::lock_guard<std::mutex> lock(panel_init_mutex_);
            if (!ReleaseTouch()) { return false; }
        }
        if (!display_ || !backlight_ || !display_->PauseWatchForSleep()) { return false; }
        // Parameter commands drain queued SPI color transfers after LVGL stops.
        if (!backlight_->PrepareForDeepSleep()) { return false; }
        watch_peripherals_prepared_ = true;
        return true;
    }

    bool RestoreWatchPeripherals() {
        if (motion_ && !motion_->CancelSleep()) return false;
        if (!display_ || !backlight_ || !backlight_->WakeForRecordingFeedback()) { return false; }
        if (!display_->RestoreWatchAfterSleepCancellation() || !InitializeTouch()) { return false; }
        backlight_->RestoreBrightness();
        watch_peripherals_prepared_ = false;
        rtc_.Resume();
        FlushWatchRtc();
        return true;
    }
#endif

    bool InitializeTouch() {
        std::lock_guard<std::mutex> lock(panel_init_mutex_);
        if (touch_input_) { return true; }
        if (!ReleaseTouch()) { return false; }
        if (!display_ || !display_->LvglHandle()) {
            ESP_LOGE(TAG, "Touch initialization requires an initialized LVGL display");
            return false;
        }
        esp_lcd_touch_handle_t tp = nullptr;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = GPIO_NUM_9,
            .int_gpio_num = GPIO_NUM_38,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
        tp_io_config.scl_speed_hz = 400*  1000;
        esp_err_t result = esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Touch panel IO initialization failed: %s", esp_err_to_name(result));
            return false;
        }
        ESP_LOGI(TAG, "Initialize touch controller");
        result = esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Touch controller initialization failed: %s", esp_err_to_name(result));
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_io_del(tp_io_handle));
            return false;
        }
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = display_->LvglHandle(),
            .handle = tp,
        };
        auto* input = lvgl_port_add_touch(&touch_cfg);
        if (!input) {
            ESP_LOGE(TAG, "LVGL touch registration failed");
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_touch_del(tp));
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_io_del(tp_io_handle));
            return false;
        }
        touch_ = tp;
        touch_io_ = tp_io_handle;
        touch_input_ = input;
#if CONFIG_AUDREAM_WATCH_UI
        // The hidden watch root cannot receive touch events. Keep the input device
        // alive so the first touch wakes the screen without activating a hidden control.
        lv_indev_add_event_cb(input, [](lv_event_t* event) {
            if (AudreamPowerManager::GetInstance().IsDisplayIdle()) {
                lv_indev_wait_release(static_cast<lv_indev_t*>(lv_event_get_user_data(event)));
            }
            Application::GetInstance().Schedule([]() {
                AudreamPowerManager::GetInstance().NotifyUserActivity("watch_touch_input");
            });
        }, LV_EVENT_PRESSED, input);
#endif
        ESP_LOGI(TAG, "Touch panel initialized successfully");
        return true;
    }

    void InitializeRecordingButton() {
#if CONFIG_AUDREAM_WATCH_UI
        boot_button_.OnPressDown([this]() {
            recording_button_intent_.Press(esp_timer_get_time());
        });
        boot_button_.OnPressUp([this]() {
            QueueWatchRecordingHold(recording_button_intent_.Release(esp_timer_get_time()));
        });
        boot_button_.OnLongPress([this]() {
            QueueWatchRecordingHold(recording_button_intent_.ConfirmHold());
        });
        boot_button_.OnClick([this]() {
            if (recording_button_intent_.LastPressWasHold()) return;
            Application::GetInstance().Schedule([this]() {
                auto& recording = LocalRecordingService::GetInstance();
                if (recording.IsRecording() && AddRecordingHighlight("device_button")) {
                    AudreamPowerManager::GetInstance().NotifyUserActivity("recording_highlight");
                } else {
                    AudreamPowerManager::GetInstance().NotifyUserActivity("button_click");
                }
            });
        });
#else
        boot_button_.OnPressDown([this]() {
            recording_button_pressed_ = true;
        });
        boot_button_.OnPressUp([this]() {
            recording_button_pressed_ = false;
            recording_hold_start_pending_ = false;
            if (suppress_next_wake_long_press_) {
                suppress_next_wake_long_press_ = false;
                ESP_LOGI(TAG, "Cleared deep sleep wake long press suppression on button release");
            }
        });
        boot_button_.OnClick([this]() {
            if (suppress_next_wake_long_press_) {
                suppress_next_wake_long_press_ = false;
                ESP_LOGI(TAG, "Cleared deep sleep wake long press suppression on button release");
            }
            auto& recording = LocalRecordingService::GetInstance();
            if (recording.IsRecording()) {
                if (recording.AddMarkerHighlight("device_button")) {
                    PulseRecordingFeedbackScreen();
                    AudreamPowerManager::GetInstance().NotifyUserActivity("recording_highlight");
                    ESP_LOGI(TAG, "Button added local recording highlight");
                }
                return;
            }
            AudreamPowerManager::GetInstance().NotifyUserActivity("button_click");
        });
        boot_button_.OnLongPress([this]() {
            if (suppress_next_wake_long_press_) {
                ESP_LOGI(TAG, "Ignored wake long press because deep sleep recording hold check is pending");
                return;
            }
            recording_button_pressed_ = true;
            AudreamPowerManager::GetInstance().NotifyUserActivity("button_long_press");
            Application::GetInstance().Schedule([this]() {
                HandleRecordingLongPressTrigger();
            });
        });
#endif
    }

#if CONFIG_AUDREAM_WATCH_UI
    void QueueWatchRecordingHold(uint64_t token) {
        if (token == 0) return;
        const int64_t confirmed_at_us = esp_timer_get_time();
        Application::GetInstance().Schedule([this, token, confirmed_at_us]() {
            if (!recording_button_intent_.Consume(token)) return;
            if (esp_timer_get_time() - confirmed_at_us >= kWatchRecordingIntentTimeoutUs) {
                ESP_LOGW(TAG, "Queued recording hold expired before the application was ready");
                return;
            }
            AudreamPowerManager::GetInstance().NotifyUserActivity("button_long_press");
            if (recording_hold_start_pending_) {
                ExecuteWatchRecordingCommand(WatchRecordingCommand::kStop);
                ESP_LOGI(TAG, "Pending recording start canceled by another hold");
                return;
            }
            HandleRecordingLongPressTrigger();
        });
    }

    void DetectWatchBootRecordingIntent() {
        if (gpio_get_level(BOOT_BUTTON_GPIO) != 0) return;
        recording_button_intent_.Press(esp_timer_get_time());
        const esp_timer_create_args_t args = {
            .callback = [](void* arg) {
                auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg);
                if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    board->QueueWatchRecordingHold(
                        board->recording_button_intent_.ConfirmElapsed(esp_timer_get_time()));
                } else {
                    // A high level alone cannot establish when the button was released.
                    board->recording_button_intent_.CancelPress();
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "audream_boot_hold",
            .skip_unhandled_events = true,
        };
        auto result = esp_timer_create(&args, &deep_sleep_record_hold_timer_);
        if (result == ESP_OK) {
            result = esp_timer_start_once(deep_sleep_record_hold_timer_, kDeepSleepWakeRecordHoldUs);
        }
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Boot hold timer unavailable; button callbacks remain active: %s",
                     esp_err_to_name(result));
        }
    }
#endif

    void DetectDeepSleepRecordingIntent() {
        const esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
        if (wakeup_cause != ESP_SLEEP_WAKEUP_EXT0) {
            return;
        }
        const int button_level = gpio_get_level(BOOT_BUTTON_GPIO);
        if (button_level != 0) {
            ESP_LOGI(TAG, "Deep sleep wake by button without held recording intent");
            return;
        }
        pending_deep_sleep_recording_ = true;
        suppress_next_wake_long_press_ = true;
        deep_sleep_wake_button_down_us_ = esp_timer_get_time();
        AudreamPowerManager::GetInstance().NotifyUserActivity("deep_sleep_record_intent", 120);
        ESP_LOGI(TAG, "Detected deep sleep wake recording candidate");
    }

    void HandleStorageInitializationComplete(const StorageStatus& status) {
        if (status.mounted) {
            auto& diagnostics = BootDiagnostics::GetInstance();
            diagnostics.Log(TAG);
            diagnostics.AppendToFile("/sdcard/audream/boot_diagnostics.log");
        }
        if (!pending_deep_sleep_recording_) {
            return;
        }
        if (!status.mounted) {
            pending_deep_sleep_recording_ = false;
            ESP_LOGE(
                TAG,
                "Deep sleep wake recording was canceled because storage is unavailable, error_code=%lu",
                static_cast<unsigned long>(status.error_code)
            );
            return;
        }
        ScheduleDeepSleepRecordingHoldCheck();
    }

    void ScheduleDeepSleepRecordingHoldCheck() {
        const int64_t elapsed_us = esp_timer_get_time() - deep_sleep_wake_button_down_us_;
        const int64_t delay_us = kDeepSleepWakeRecordHoldUs - elapsed_us;
        if (delay_us <= 0) {
            Application::GetInstance().Schedule([this]() {
                EvaluateDeepSleepRecordingHold();
            });
            return;
        }
        if (deep_sleep_record_hold_timer_ == nullptr) {
            const esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg);
                    if (board == nullptr) {
                        return;
                    }
                    Application::GetInstance().Schedule([board]() {
                        board->EvaluateDeepSleepRecordingHold();
                    });
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "audream_wake_record",
                .skip_unhandled_events = true,
            };
            const esp_err_t result = esp_timer_create(&timer_args, &deep_sleep_record_hold_timer_);
            if (result != ESP_OK) {
                pending_deep_sleep_recording_ = false;
                suppress_next_wake_long_press_ = false;
                ESP_LOGE(
                    TAG,
                    "Failed to create deep sleep recording hold timer: %s",
                    esp_err_to_name(result)
                );
                return;
            }
        }
        ESP_LOGI(
            TAG,
            "Deep sleep wake recording hold check scheduled in %lld ms",
            static_cast<long long>(delay_us / 1000)
        );
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_timer_start_once(deep_sleep_record_hold_timer_, static_cast<uint64_t>(delay_us))
        );
    }

    void EvaluateDeepSleepRecordingHold() {
        if (!pending_deep_sleep_recording_) {
            return;
        }
        if (gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
            pending_deep_sleep_recording_ = false;
            suppress_next_wake_long_press_ = false;
            ESP_LOGI(TAG, "Deep sleep wake recording canceled because button was released before hold threshold");
            return;
        }
        pending_deep_sleep_recording_ = false;
        recording_button_pressed_ = true;
        BeginRecordingHoldStart(RecordingHoldStartSource::kDeepSleepWake);
    }

    void StopTimerIfActive(esp_timer_handle_t timer, const char* name) {
        if (timer == nullptr) {
            return;
        }
        const esp_err_t result = esp_timer_stop(timer);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Failed to stop %s timer: %s", name, esp_err_to_name(result));
        }
    }

    void EnsureRecordingBleTimers() {
        if (recording_ble_report_timer_ == nullptr) {
            const esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg);
                    if (board == nullptr) {
                        return;
                    }
                    Application::GetInstance().Schedule([board]() {
                        board->OpenRecordingBleReportWindow();
                    });
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "audream_ble_report",
                .skip_unhandled_events = true,
            };
            const esp_err_t result = esp_timer_create(&timer_args, &recording_ble_report_timer_);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Failed to create recording BLE report timer: %s", esp_err_to_name(result));
            }
        }
        if (recording_ble_close_timer_ == nullptr) {
            const esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg);
                    if (board == nullptr) {
                        return;
                    }
                    Application::GetInstance().Schedule([board]() {
                        board->CloseRecordingBleWindow();
                    });
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "audream_ble_close",
                .skip_unhandled_events = true,
            };
            const esp_err_t result = esp_timer_create(&timer_args, &recording_ble_close_timer_);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Failed to create recording BLE close timer: %s", esp_err_to_name(result));
            }
        }
        if (recording_ble_status_retry_timer_ == nullptr) {
            const esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg);
                    if (board == nullptr) {
                        return;
                    }
                    Application::GetInstance().Schedule([board]() {
                        board->SendRecordingBleEventRetry();
                    });
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "audream_ble_status",
                .skip_unhandled_events = true,
            };
            const esp_err_t result = esp_timer_create(&timer_args, &recording_ble_status_retry_timer_);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Failed to create recording BLE status timer: %s", esp_err_to_name(result));
            }
        }
    }

    bool EnsureRecordingBleRunning(const char* reason) {
        auto& blufi = Blufi::GetInstance();
        if (blufi.IsActive()) {
            return true;
        }
        const esp_err_t result = blufi.init(false);
        if (result != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to start BLE for %s: %s",
                reason == nullptr ? "recording_event" : reason,
                esp_err_to_name(result)
            );
            return false;
        }
        ESP_LOGI(TAG, "BLE started for %s", reason == nullptr ? "recording_event" : reason);
        return true;
    }

    void ScheduleRecordingBleClose(uint64_t delay_us) {
        EnsureRecordingBleTimers();
        if (recording_ble_close_timer_ == nullptr) {
            return;
        }
        StopTimerIfActive(recording_ble_close_timer_, "recording_ble_close");
        const esp_err_t result = esp_timer_start_once(recording_ble_close_timer_, delay_us);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Failed to schedule BLE close timer: %s", esp_err_to_name(result));
        }
    }

    void StartRecordingBleStatusRetries() {
        EnsureRecordingBleTimers();
        if (recording_ble_status_retry_timer_ == nullptr) {
            return;
        }
        StopTimerIfActive(recording_ble_status_retry_timer_, "recording_ble_status");
        const esp_err_t result = esp_timer_start_periodic(
            recording_ble_status_retry_timer_,
            kRecordingBleStatusRetryUs
        );
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Failed to schedule BLE status retry timer: %s", esp_err_to_name(result));
        }
    }

    uint32_t CurrentRecordingDurationMs(const LocalRecordingMetadata& metadata) const {
        if (metadata.recording_id.empty()) {
            return 0;
        }
        return LocalRecordingService::GetInstance().CurrentDurationMs();
    }

    void SendRecordingBleEventRetry() {
        auto& blufi = Blufi::GetInstance();
        if (pending_recording_stop_event_valid_) {
            if (!blufi.IsActive() && !EnsureRecordingBleRunning("recording_stopped_retry")) {
                return;
            }
            blufi.SendRecordingStoppedEvent(
                pending_recording_stop_event_.recording_id,
                pending_recording_stop_event_.file_name,
                pending_recording_stop_event_.duration_ms,
                pending_recording_stop_event_.size_bytes
            );
            return;
        }

        auto& recording = LocalRecordingService::GetInstance();
        if (!recording.IsRecording()) {
            return;
        }
        const auto metadata = recording.CurrentMetadata();
        if (metadata.recording_id.empty()) {
            return;
        }
        if (!blufi.IsActive() && !EnsureRecordingBleRunning("recording_status_retry")) {
            return;
        }
        blufi.SendRecordingStatusEvent(
            metadata.recording_id,
            metadata.file_name,
            CurrentRecordingDurationMs(metadata)
        );
    }

    void OpenRecordingBleReportWindow() {
        if (!LocalRecordingService::GetInstance().IsRecording()) {
            return;
        }
        pending_recording_stop_event_valid_ = false;
        EnsureRecordingBleRunning("recording_status");
        SendRecordingBleEventRetry();
        StartRecordingBleStatusRetries();
        ScheduleRecordingBleClose(kRecordingBleReportWindowUs);
    }

    void CloseRecordingBleWindow() {
        StopTimerIfActive(recording_ble_status_retry_timer_, "recording_ble_status");
        if (pending_recording_stop_event_valid_) {
            pending_recording_stop_event_valid_ = false;
            ESP_LOGI(TAG, "Recording stopped event retry window ended; keeping BLE active");
            return;
        }
        auto& blufi = Blufi::GetInstance();
        if (!blufi.IsActive()) {
            return;
        }
        const esp_err_t result = blufi.deinit();
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop recording BLE window: %s", esp_err_to_name(result));
        } else {
            ESP_LOGI(TAG, "Recording BLE window closed");
        }
    }

    void HandleRecordingStatusAcknowledged() {
        Application::GetInstance().Schedule([this]() {
            if (pending_recording_stop_event_valid_) {
                StopTimerIfActive(recording_ble_status_retry_timer_, "recording_ble_status");
                StopTimerIfActive(recording_ble_close_timer_, "recording_ble_close");
                pending_recording_stop_event_valid_ = false;
                ESP_LOGI(TAG, "Recording stopped event acknowledged; keeping BLE active");
                return;
            }
            if (!LocalRecordingService::GetInstance().IsRecording()) {
                return;
            }
            ESP_LOGI(TAG, "Recording status acknowledged; closing BLE window");
            CloseRecordingBleWindow();
        });
    }

    void StartRecordingBleReporting(const LocalRecordingMetadata& metadata) {
        EnsureRecordingBleTimers();
        pending_recording_stop_event_valid_ = false;
        EnsureRecordingBleRunning("recording_started");
        Blufi::GetInstance().SendRecordingStartedEvent(
            metadata.recording_id,
            metadata.file_name,
            metadata.started_at_unix_ms
        );
        StopTimerIfActive(recording_ble_report_timer_, "recording_ble_report");
        if (kRecordingBlePeriodicStatusReportsEnabled && recording_ble_report_timer_ != nullptr) {
            const esp_err_t result = esp_timer_start_periodic(
                recording_ble_report_timer_,
                kRecordingBleReportIntervalUs
            );
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Failed to schedule BLE report timer: %s", esp_err_to_name(result));
            }
        } else if (!kRecordingBlePeriodicStatusReportsEnabled) {
            ESP_LOGI(TAG, "Recording BLE periodic status reports are disabled");
        }
        ScheduleRecordingBleClose(kRecordingBleStartGraceUs);
    }

    void StopRecordingBleReporting(const LocalRecordingMetadata& metadata) {
        EnsureRecordingBleTimers();
        StopTimerIfActive(recording_ble_report_timer_, "recording_ble_report");
        StopTimerIfActive(recording_ble_close_timer_, "recording_ble_close");
        pending_recording_stop_event_ = metadata;
        pending_recording_stop_event_valid_ = true;
        EnsureRecordingBleRunning("recording_stopped");
        SendRecordingBleEventRetry();
        StartRecordingBleStatusRetries();
        ScheduleRecordingBleClose(kRecordingBleReportWindowUs);
    }

    void HandleRecordingLongPressTrigger() {
#if CONFIG_AUDREAM_WATCH_UI
        ExecuteWatchRecordingCommand(LocalRecordingService::GetInstance().IsRecording()
            ? WatchRecordingCommand::kStop : WatchRecordingCommand::kStart);
#else
        if (LocalRecordingService::GetInstance().IsRecording()) {
            StopLocalRecordingWithFeedback();
        } else {
            BeginRecordingHoldStart(RecordingHoldStartSource::kButton);
        }
#endif
    }

    bool StopLocalRecordingWithFeedback() {
        auto& recording = LocalRecordingService::GetInstance();
        auto& audio_service = Application::GetInstance().GetAudioService();
        if (recording.IsRecording()) {
            recording_hold_start_pending_ = false;
            const auto metadata = recording.Stop(audio_service);
            if (recording.IsRecording()) {
                ESP_LOGE(TAG, "Local recording stop did not finish; recording resources remain reserved");
                return false;
            }
            BatteryTelemetryService::GetInstance().RecordEvent(
                BatteryTelemetryEvent::kRecordingStopped
            );
            PulseRecordingFeedbackScreen("STOP");
            if (metadata.valid) {
                ESP_LOGI(
                    TAG,
                    "Local recording stopped: id=%s duration=%lu ms size=%lu bytes",
                    metadata.recording_id.c_str(),
                    static_cast<unsigned long>(metadata.duration_ms),
                    static_cast<unsigned long>(metadata.size_bytes)
                );
            } else {
                ESP_LOGW(TAG, "Local recording stopped without valid metadata");
            }
            StopRecordingBleReporting(metadata);
            return metadata.valid;
        }
        return false;
    }

    bool SetLocalRecordingPaused(bool paused) {
        auto& recording = LocalRecordingService::GetInstance();
        auto& audio = Application::GetInstance().GetAudioService();
        const bool succeeded = paused ? recording.Pause(audio) : recording.Resume(audio);
        if (succeeded) {
            BatteryTelemetryService::GetInstance().RecordEvent(paused
                ? BatteryTelemetryEvent::kRecordingPaused : BatteryTelemetryEvent::kRecordingResumed);
            PulseRecordingFeedbackScreen(paused ? "PAUSE" : "RESUME");
        }
        return succeeded;
    }

    bool AddRecordingHighlight(const char* source) {
        if (!LocalRecordingService::GetInstance().AddMarkerHighlight(source)) return false;
        PulseRecordingFeedbackScreen();
        return true;
    }

    void BeginRecordingHoldStart(RecordingHoldStartSource source) {
        watch_recording_error_ = WatchRecordingError::kNone;
        recording_hold_start_source_ = source;
        recording_hold_start_pending_ = true;
#if CONFIG_AUDREAM_WATCH_UI
        recording_hold_deadline_us_ = esp_timer_get_time() + kWatchRecordingIntentTimeoutUs;
#endif
        TryStartRecordingFromHold();
    }

    const char* RecordingHoldStartSourceName() const {
        switch (recording_hold_start_source_) {
            case RecordingHoldStartSource::kDeepSleepWake:
                return "Deep sleep wake";
            case RecordingHoldStartSource::kWatchControl:
                return "Watch control";
            case RecordingHoldStartSource::kButton:
            default:
                return "Button";
        }
    }

    void TryStartRecordingFromHold() {
        if (!recording_hold_start_pending_) {
            return;
        }
#if CONFIG_AUDREAM_WATCH_UI
        if (esp_timer_get_time() >= recording_hold_deadline_us_) {
            recording_hold_start_pending_ = false;
            watch_recording_error_ = WatchRecordingError::kStartFailed;
            ESP_LOGW(TAG, "Recording start expired while waiting for storage or audio resources");
            return;
        }
#else
        if (!recording_button_pressed_ && gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
            recording_hold_start_pending_ = false;
            ESP_LOGI(TAG, "Recording hold start canceled because the button was released");
            return;
        }
#endif
        const auto storage_status = SdCardStorage::GetInstance().GetStatus();
        if (!storage_status.mounted) {
            ScheduleRecordingHoldStartRetry();
            return;
        }

        auto& recording = LocalRecordingService::GetInstance();
#if CONFIG_AUDREAM_WATCH_UI
        if (recording.GetSnapshot().state == LocalRecordingService::State::kOpening) {
            // Failed-start cleanup can retain the opening state until audio drains.
            ScheduleRecordingHoldStartRetry();
            return;
        }
#endif
        if (recording.IsRecording()) {
            recording_hold_start_pending_ = false;
            ESP_LOGI(TAG, "%s recording hold start skipped because recording is already active", RecordingHoldStartSourceName());
            return;
        }
        auto& audio_service = Application::GetInstance().GetAudioService();
        if (recording.Start(audio_service)) {
            recording_hold_start_pending_ = false;
            BatteryTelemetryService::GetInstance().RecordEvent(
                BatteryTelemetryEvent::kRecordingStarted
            );
            const auto metadata = recording.CurrentMetadata();
            PulseRecordingFeedbackScreen("START");
            ESP_LOGI(
                TAG,
                "%s started local recording: id=%s",
                RecordingHoldStartSourceName(),
                metadata.recording_id.c_str()
            );
            StartRecordingBleReporting(metadata);
        } else {
            ScheduleRecordingHoldStartRetry();
        }
    }

    void ScheduleRecordingHoldStartRetry() {
        if (recording_hold_retry_timer_ == nullptr) {
            const esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg);
                    if (board == nullptr) {
                        return;
                    }
                    Application::GetInstance().Schedule([board]() {
                        board->TryStartRecordingFromHold();
                    });
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "audream_rec_hold",
                .skip_unhandled_events = true,
            };
            const esp_err_t result = esp_timer_create(&timer_args, &recording_hold_retry_timer_);
            if (result != ESP_OK) {
                recording_hold_start_pending_ = false;
                watch_recording_error_ = WatchRecordingError::kStartFailed;
                ESP_LOGE(TAG, "Failed to create recording hold retry timer: %s", esp_err_to_name(result));
                return;
            }
        }
        const esp_err_t result = esp_timer_start_once(recording_hold_retry_timer_, kRecordingHoldRetryUs);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            recording_hold_start_pending_ = false;
            watch_recording_error_ = WatchRecordingError::kStartFailed;
            ESP_LOGE(TAG, "Failed to schedule recording hold retry: %s", esp_err_to_name(result));
        }
    }

    void LogWakeContext() {
        gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);
        gpio_pullup_en(BOOT_BUTTON_GPIO);
        gpio_pulldown_dis(BOOT_BUTTON_GPIO);
        auto& diagnostics = BootDiagnostics::GetInstance();
        diagnostics.SetBootGpioLevel(gpio_get_level(BOOT_BUTTON_GPIO));
        diagnostics.Log(TAG);
    }

public:
    WaveshareEsp32s3TouchAMOLED2inch06() : boot_button_(BOOT_BUTTON_GPIO, false, kRecordingButtonHoldMs) {
        deep_sleep_wake_ = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
        LogWakeContext();
#if CONFIG_AUDREAM_WATCH_UI
        InitializeRecordingButton();
        DetectWatchBootRecordingIntent();
#endif
        InitializeCodecI2c();
        pmic_mutex_ = xSemaphoreCreateMutex();
        if (pmic_mutex_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create PMIC mutex");
        }
        InitializeAxp2101();
        InitializeRecordingPowerManagement();
        InitializeBatteryTelemetry();
        AudreamDeviceControl::GetInstance().SetRecordingStatusAcknowledgedCallback([this]() {
            HandleRecordingStatusAcknowledged();
        });
        AudreamDeviceControl::GetInstance().SetRecordingPauseCallbacks(
            [this]() { return SetLocalRecordingPaused(true); },
            [this]() { return SetLocalRecordingPaused(false); }
        );
#if CONFIG_AUDREAM_WATCH_UI
        InitializeWatchRtc();
        if (!InitializeSH8601Display() || !InitializeTouch()) {
            ESP_LOGE(TAG, "Watch display or touch initialization failed; recording remains available");
        }
        motion_ = std::make_unique<WatchMotionRuntime>(i2c_bus_);
        if (!motion_->Start()) ESP_LOGE(TAG, "Motion worker could not be started");
#else
        InitializeRecordingButton();
        DetectDeepSleepRecordingIntent();
        PulseBootBatteryScreen();
        ESP_LOGI(TAG, "Touch and WiFi application features are disabled");
#endif
        auto& storage = SdCardStorage::GetInstance();
        storage.SetInitializationCompleteCallback([this](const StorageStatus& status) {
            HandleStorageInitializationComplete(status);
        });
        storage.StartInitializationTask();
    }

    WatchRecordingControlStatus GetWatchRecordingControlStatus() const override {
        return {recording_hold_start_pending_, watch_recording_error_};
    }

    WatchMotionSnapshot GetWatchMotionSnapshot() const override {
#if CONFIG_AUDREAM_WATCH_UI
        return motion_ ? motion_->Snapshot() : WatchMotionSnapshot{};
#else
        return {};
#endif
    }

    WatchWristSnapshot GetWatchWristSnapshot() const override {
#if CONFIG_AUDREAM_WATCH_UI
        return motion_ ? motion_->GetWristSnapshot() : WatchWristSnapshot{};
#else
        return {};
#endif
    }

    bool RequestWatchWrist(uint64_t id, WatchWristAction action, bool enabled) override {
#if CONFIG_AUDREAM_WATCH_UI
        return motion_ && motion_->RequestWrist(id, action, enabled);
#else
        (void)id; (void)action; (void)enabled;
        return false;
#endif
    }

    bool RetryWatchActivityStorage(uint64_t request_id) override {
#if CONFIG_AUDREAM_WATCH_UI
        return motion_ && motion_->RetryStorage(request_id);
#else
        (void)request_id;
        return false;
#endif
    }

    bool ResetCorruptWatchActivityStorage(uint64_t request_id, uint64_t token) override {
#if CONFIG_AUDREAM_WATCH_UI
        return motion_ && motion_->ResetCorruptStorage(request_id, token);
#else
        (void)request_id;
        (void)token;
        return false;
#endif
    }

    bool ExecuteWatchRecordingCommand(WatchRecordingCommand command) override {
#if CONFIG_AUDREAM_WATCH_UI
        auto& recording = LocalRecordingService::GetInstance();
        watch_recording_error_ = WatchRecordingError::kNone;
        bool succeeded = false;
        switch (command) {
            case WatchRecordingCommand::kStart:
                if (recording_hold_start_pending_ || recording.IsRecording()) return false;
                BeginRecordingHoldStart(RecordingHoldStartSource::kWatchControl);
                return recording_hold_start_pending_ || recording.IsRecording();
            case WatchRecordingCommand::kStop: {
                const bool was_pending = recording_hold_start_pending_;
                recording_hold_start_pending_ = false;
                StopTimerIfActive(recording_hold_retry_timer_, "audream_rec_hold");
                succeeded = recording.IsRecording() ? StopLocalRecordingWithFeedback() : was_pending;
                if (!succeeded) watch_recording_error_ = WatchRecordingError::kSaveFailed;
                return succeeded;
            }
            case WatchRecordingCommand::kPause: succeeded = SetLocalRecordingPaused(true); break;
            case WatchRecordingCommand::kResume: succeeded = SetLocalRecordingPaused(false); break;
            case WatchRecordingCommand::kHighlight: succeeded = AddRecordingHighlight("watch_ui"); break;
        }
        if (!succeeded) watch_recording_error_ = WatchRecordingError::kControlFailed;
        return succeeded;
#else
        (void)command;
        return false;
#endif
    }

    virtual bool IsRecordingOnlyMode() const override {
        return GetRuntimeProfile() == BoardRuntimeProfile::kRecordingOnly;
    }

    BoardRuntimeProfile GetRuntimeProfile() const override {
#if CONFIG_AUDREAM_WATCH_UI
        return WaveshareWatchRuntimeProfile(true);
#else
        return WaveshareWatchRuntimeProfile(false);
#endif
    }

    virtual void StartNetwork() override {
        const esp_err_t result = Blufi::GetInstance().init(false);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Audream BLE control: %s", esp_err_to_name(result));
#if !CONFIG_AUDREAM_WATCH_UI
            return;
#endif
        }
#if CONFIG_AUDREAM_WATCH_UI
        if (GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder) {
            AudreamPowerManager::GetInstance().SetPeripheralSleepCallbacks(
                [this]() { return PrepareWatchPeripheralsForSleep(); },
                [this]() { return RestoreWatchPeripherals(); });
        }
#endif
        AudreamPowerManager::GetInstance().SetBeforeDeepSleepCallback([this]() {
            auto& blufi = Blufi::GetInstance();
            blufi.SendDeviceWillSleepEvent();
            vTaskDelay(pdMS_TO_TICKS(200));
            blufi.deinit();
            PrepareBoardForDeepSleep();
        });
#if CONFIG_AUDREAM_WATCH_UI
        WifiBoard::StartNetwork();
#endif
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE,
            ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3,
            kRecordingAudioOutputPathEnabled,
            kRecordingAudioInputChannelCount);
        SchedulePmicRecoveryTask("audio codec initialization");
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        if (display_ == nullptr) {
            return Board::GetDisplay();
        }
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        if (pmic_ == nullptr) {
            ESP_LOGW(TAG, "Battery level requested while PMIC is unavailable; retrying PMIC initialization");
            if (!TryInitializePmic()) {
                ESP_LOGW(TAG, "Battery level unavailable because PMIC initialization retry failed");
                return false;
            }
        }
        return pmic_->ReadPowerSnapshot(level, charging, discharging);
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
#if CONFIG_AUDREAM_WATCH_UI
        WifiBoard::SetPowerSaveLevel(level);
#else
        (void)level;
#endif
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED2inch06);
