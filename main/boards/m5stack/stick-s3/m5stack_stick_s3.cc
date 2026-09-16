#include "wifi_board.h"

#include <algorithm>
#include <atomic>

#include "M5PM1.h"
#include "application.h"
#include "audio/codecs/es8311_audio_codec.h"
#include "button.h"
#include "config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stick_s3_endurance_recorder.h"
#include "storage/sd_card_storage.h"
#include "system/audream_power_manager.h"

namespace {

constexpr char kTag[] = "M5StackStickS3";
constexpr uint32_t kBatteryPollIntervalUs = 5 * 1000 * 1000;
constexpr uint8_t kLowBatteryPercent = 5;
constexpr uint8_t kLowBatteryRequiredSamples = 3;
constexpr uint16_t kRecordingButtonHoldMs = 1500;

int BatteryPercentFromVoltage(uint16_t voltage_mv) {
    constexpr int kBatteryMinVoltageMv = 3400;
    constexpr int kBatteryMaxVoltageMv = 4200;
    if (voltage_mv <= kBatteryMinVoltageMv) {
        return 0;
    }
    if (voltage_mv >= kBatteryMaxVoltageMv) {
        return 100;
    }
    return static_cast<int>(
        (voltage_mv - kBatteryMinVoltageMv) * 100 /
        (kBatteryMaxVoltageMv - kBatteryMinVoltageMv)
    );
}

}  // namespace

class M5StackStickS3EnduranceBoard : public WifiBoard {
private:
    Button boot_button_;
    Button user_button_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    M5PM1 pmic_;
    esp_timer_handle_t battery_timer_ = nullptr;
    uint8_t low_battery_sample_count_ = 0;
    std::atomic<bool> low_battery_stop_pending_{false};

    void SetRecordingIndicator(bool enabled) {
        const m5pm1_err_t result = pmic_.setLedEnLevel(enabled);
        if (result != M5PM1_OK) {
            ESP_LOGW(
                kTag,
                "Failed to set the recording indicator: enabled=%d error=%d",
                enabled ? 1 : 0,
                static_cast<int>(result)
            );
            return;
        }
        ESP_LOGI(kTag, "Recording indicator: %s", enabled ? "on" : "off");
    }

    void InitializeI2c() {
        const i2c_master_bus_config_t config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&config, &i2c_bus_));
    }

    void InitializePower() {
        if (pmic_.begin(i2c_bus_, M5PM1_DEFAULT_ADDR) != M5PM1_OK) {
            ESP_LOGE(kTag, "M5PM1 initialization failed");
            return;
        }
        pmic_.setChargeEnable(true);
        pmic_.setBoostEnable(false);
        pmic_.pinMode(0, INPUT);
        pmic_.pinMode(2, OUTPUT);
        pmic_.gpioSetDrive(M5PM1_GPIO_NUM_2, M5PM1_GPIO_DRIVE_PUSHPULL);
        pmic_.digitalWrite(2, HIGH);
        pmic_.pinMode(3, OUTPUT);
        pmic_.gpioSetDrive(M5PM1_GPIO_NUM_3, M5PM1_GPIO_DRIVE_PUSHPULL);
        pmic_.digitalWrite(3, LOW);
        pmic_.setDoubleOffDisable(false);
        SetRecordingIndicator(false);

        gpio_config_t backlight_config = {};
        backlight_config.pin_bit_mask = 1ULL << DISPLAY_BACKLIGHT_PIN;
        backlight_config.mode = GPIO_MODE_OUTPUT;
        ESP_ERROR_CHECK(gpio_config(&backlight_config));
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
        ESP_LOGI(kTag, "Display backlight and speaker amplifier are disabled");
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            Application::GetInstance().Schedule([this]() {
                auto& recorder = StickS3EnduranceRecorder::GetInstance();
                if (recorder.IsRecording()) {
                    ESP_LOGI(
                        kTag,
                        "Recording elapsed_ms=%llu",
                        static_cast<unsigned long long>(recorder.DurationMs())
                    );
                    return;
                }
                if (!recorder.Start(
                        Application::GetInstance().GetAudioService(),
                        ReadBatteryPercentForMetadata("start")
                    )) {
                    ESP_LOGE(kTag, "Boot button could not start endurance recording");
                    return;
                }
                SetRecordingIndicator(true);
            });
        });
        boot_button_.OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() {
                auto& recorder = StickS3EnduranceRecorder::GetInstance();
                if (recorder.IsRecording()) {
                    recorder.Stop(
                        Application::GetInstance().GetAudioService(),
                        "user_button",
                        ReadBatteryPercentForMetadata("stop")
                    );
                    SetRecordingIndicator(false);
                }
            });
        });
    }

    bool ReadBatteryStatus(
        int& level,
        bool& charging,
        bool& discharging,
        uint16_t& voltage_mv
    ) {
        if (pmic_.readVbat(&voltage_mv) != M5PM1_OK) {
            return false;
        }
        const int charging_pin = pmic_.digitalRead(0);
        if (charging_pin < 0) {
            return false;
        }
        charging = charging_pin == 0;
        discharging = !charging;
        level = BatteryPercentFromVoltage(voltage_mv);
        return true;
    }

    int ReadBatteryPercentForMetadata(const char* phase) {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        uint16_t voltage_mv = 0;
        if (!ReadBatteryStatus(level, charging, discharging, voltage_mv)) {
            ESP_LOGW(kTag, "Recording %s battery status is unavailable", phase);
            return -1;
        }
        ESP_LOGI(
            kTag,
            "Recording %s battery status: level=%d%% voltage_mv=%u charging=%d discharging=%d",
            phase,
            level,
            static_cast<unsigned>(voltage_mv),
            charging ? 1 : 0,
            discharging ? 1 : 0
        );
        return level;
    }

    void LogBootBatteryStatus() {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        uint16_t voltage_mv = 0;
        if (!ReadBatteryStatus(level, charging, discharging, voltage_mv)) {
            ESP_LOGW(kTag, "Boot battery status is unavailable");
            return;
        }
        ESP_LOGI(
            kTag,
            "Boot battery status: level=%d%% voltage_mv=%u charging=%d discharging=%d",
            level,
            static_cast<unsigned>(voltage_mv),
            charging ? 1 : 0,
            discharging ? 1 : 0
        );
    }

    void InitializeBatteryTimer() {
        const esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                static_cast<M5StackStickS3EnduranceBoard*>(arg)->CheckLowBattery();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "stick_s3_battery",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &battery_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(battery_timer_, kBatteryPollIntervalUs));
    }

    void PrepareBoardForDeepSleep() {
        if (battery_timer_ != nullptr) {
            const esp_err_t result = esp_timer_stop(battery_timer_);
            if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(
                    kTag,
                    "Failed to stop the battery timer before deep sleep: %s",
                    esp_err_to_name(result)
                );
            }
        }
        GetAudioCodec()->PrepareForDeepSleep();
        SdCardStorage::GetInstance().PrepareForDeepSleep();
        SetRecordingIndicator(false);
        pmic_.digitalWrite(3, LOW);
        pmic_.digitalWrite(2, LOW);
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
    }

    void CheckLowBattery() {
        auto& recorder = StickS3EnduranceRecorder::GetInstance();
        if (!recorder.IsRecording()) {
            low_battery_sample_count_ = 0;
            return;
        }

        int level = 0;
        bool charging = false;
        bool discharging = false;
        if (!GetBatteryLevel(level, charging, discharging) || charging || !discharging) {
            low_battery_sample_count_ = 0;
            return;
        }
        if (level <= kLowBatteryPercent) {
            low_battery_sample_count_ = std::min<uint8_t>(
                static_cast<uint8_t>(low_battery_sample_count_ + 1),
                kLowBatteryRequiredSamples
            );
        } else {
            low_battery_sample_count_ = 0;
        }
        if (low_battery_sample_count_ < kLowBatteryRequiredSamples) {
            return;
        }

        bool expected = false;
        if (!low_battery_stop_pending_.compare_exchange_strong(expected, true)) {
            return;
        }
        ESP_LOGW(kTag, "Low battery stop scheduled: level=%d%%", level);
        Application::GetInstance().Schedule([this]() {
            StopForLowBattery();
        });
    }

    void StopForLowBattery() {
        auto& recorder = StickS3EnduranceRecorder::GetInstance();
        bool metadata_written = true;
        if (recorder.IsRecording()) {
            metadata_written = recorder.Stop(
                Application::GetInstance().GetAudioService(),
                "low_battery",
                ReadBatteryPercentForMetadata("low-battery stop")
            );
        }
        if (!metadata_written) {
            ESP_LOGE(kTag, "Low battery stop could not finalize session metadata");
        }
        PrepareBoardForDeepSleep();
        ESP_LOGI(
            kTag,
            "Low battery stop completed; entering deep sleep metadata=%d",
            metadata_written ? 1 : 0
        );
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        esp_deep_sleep_start();
    }

    void HandleStorageInitializationComplete(const StorageStatus& status) {
        if (!status.mounted) {
            ESP_LOGE(
                kTag,
                "Endurance recording storage is unavailable, storage_error=%lu",
                static_cast<unsigned long>(status.error_code)
            );
            return;
        }
        StickS3EnduranceRecorder::GetInstance().LogRecentSessionMetadata();
        ESP_LOGI(kTag, "Endurance recorder is ready; press the Boot button to start");
    }

public:
    M5StackStickS3EnduranceBoard()
        : boot_button_(BOOT_BUTTON_GPIO, false, kRecordingButtonHoldMs),
          user_button_(USER_BUTTON_GPIO) {
        InitializeI2c();
        InitializePower();
        LogBootBatteryStatus();
        InitializeButtons();
        InitializeBatteryTimer();
        auto& power_manager = AudreamPowerManager::GetInstance();
        power_manager.SetBeforeDeepSleepCallback([this]() {
            PrepareBoardForDeepSleep();
        });
        power_manager.Start(BOOT_BUTTON_GPIO);

        auto& storage = SdCardStorage::GetInstance();
        storage.SetInitializationCompleteCallback([this](const StorageStatus& status) {
            HandleStorageInitializationComplete(status);
        });
        storage.StartInitializationTask();
    }

    bool IsRecordingOnlyMode() const override {
        return true;
    }

    void StartNetwork() override {
        ESP_LOGI(kTag, "Wi-Fi, Bluetooth, display, and speaker remain disabled during endurance recording");
    }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec codec(
            i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_GPIO_PA,
            AUDIO_CODEC_ES8311_ADDR,
            false
        );
        return &codec;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        uint16_t voltage_mv = 0;
        if (!ReadBatteryStatus(level, charging, discharging, voltage_mv)) {
            return false;
        }
        ESP_LOGI(
            kTag,
            "Battery endurance status: level=%d%% voltage_mv=%u charging=%d elapsed_ms=%llu",
            level,
            static_cast<unsigned>(voltage_mv),
            charging ? 1 : 0,
            static_cast<unsigned long long>(
                StickS3EnduranceRecorder::GetInstance().DurationMs()
            )
        );
        return true;
    }
};

DECLARE_BOARD(M5StackStickS3EnduranceBoard);
