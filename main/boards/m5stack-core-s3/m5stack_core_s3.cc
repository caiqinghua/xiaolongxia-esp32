#include "wifi_board.h"
#include "cores3_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "config.h"
#include "power_save_timer.h"
#include "i2c_device.h"
#include "axp2101.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <freertos/semphr.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <driver/gpio.h>
#if CONFIG_BT_ENABLED
#include <esp_bt.h>
#if CONFIG_BT_BLUEDROID_ENABLED
#include <esp_bt_main.h>
#endif
#endif
#include "esp_video.h"

#define TAG "M5StackCoreS3Board"

namespace {

constexpr uint64_t kPowerOnDeepSleepDelayUs = 15ULL * 1000ULL * 1000ULL;
constexpr uint8_t kAxp2101DcdcOnOffReg = 0x80;
constexpr uint8_t kAxp2101Dcdc1EnableMask = 0x01;
constexpr uint8_t kAxp2101SleepControlReg = 0x26;
constexpr uint8_t kAxp2101SleepControlClearMask = 0x02;
constexpr uint8_t kAxp2101SleepControlSetMask = 0x05;
constexpr uint8_t kAxp2101LdoOnOffReg0 = 0x90;
constexpr uint8_t kAxp2101LdoOnOffReg1 = 0x91;
constexpr uint8_t kAxp2101ChargeLedControlReg = 0x69;
constexpr uint8_t kAxp2101ChargeLedOff = 0x00;
constexpr uint8_t kAxp2101AdcChannelEnableReg = 0x30;
constexpr uint8_t kAxp2101AdcChannelsOff = 0x00;
constexpr uint8_t kBmi270PowerControlReg = 0x7D;
constexpr uint8_t kBmi270PowerControlSuspend = 0x00;
constexpr uint8_t kBmm150PowerControlReg = 0x4B;
constexpr uint8_t kBmm150PowerControlSuspend = 0x00;
constexpr uint8_t kFt6336PowerModeReg = 0xA5;
constexpr uint8_t kFt6336PowerModeHibernate = 0x03;

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

bool TryWriteOneShotI2cReg(
    i2c_master_bus_handle_t i2c_bus,
    uint8_t addr,
    uint8_t reg,
    uint8_t value,
    const char* device_name
) {
    const esp_err_t probe_result = i2c_master_probe(i2c_bus, addr, pdMS_TO_TICKS(20));
    if (probe_result != ESP_OK) {
        ESP_LOGD(TAG, "%s not found at 0x%02x before deep sleep: %s", device_name, addr, esp_err_to_name(probe_result));
        return false;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };

    i2c_master_dev_handle_t device = nullptr;
    esp_err_t result = i2c_master_bus_add_device(i2c_bus, &device_config, &device);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add %s I2C device at 0x%02x: %s", device_name, addr, esp_err_to_name(result));
        return false;
    }

    uint8_t buffer[2] = {reg, value};
    result = i2c_master_transmit(device, buffer, sizeof(buffer), 100);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "%s deep sleep register write ok: addr=0x%02x reg=0x%02x value=0x%02x", device_name, addr, reg, value);
    } else {
        ESP_LOGW(
            TAG,
            "%s deep sleep register write failed: addr=0x%02x reg=0x%02x value=0x%02x error=%s",
            device_name,
            addr,
            reg,
            value,
            esp_err_to_name(result)
        );
    }

    const esp_err_t remove_result = i2c_master_bus_rm_device(device);
    if (remove_result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to remove %s I2C device at 0x%02x: %s", device_name, addr, esp_err_to_name(remove_result));
    }
    return result == ESP_OK;
}

}  // namespace

class Pmic : public Axp2101 {
public:
    // Power Init
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        initialized_ = Configure();
    }

    bool initialized() const {
        return initialized_;
    }

    void SetBrightness(uint8_t brightness) {
        if (!initialized_) {
            return;
        }
        brightness = ((brightness + 641) >> 5);
        const esp_err_t result = TryWriteReg(0x99, brightness);
        if (result != ESP_OK) {
            initialized_ = false;
            ESP_LOGW(TAG, "Failed to set PMIC brightness; PMIC control disabled: %s", esp_err_to_name(result));
        }
    }

    bool IsAvailable() const {
        return initialized_;
    }

    bool ReadPowerSnapshot(int& level, bool& charging, bool& discharging) {
        uint8_t status1 = 0;
        uint8_t status2 = 0;
        uint8_t battery = 0;
        const esp_err_t status1_result = TryReadReg(0x00, &status1);
        const esp_err_t status2_result = TryReadReg(0x01, &status2);
        const esp_err_t battery_result = TryReadReg(0xA4, &battery);
        if (status1_result != ESP_OK || status2_result != ESP_OK || battery_result != ESP_OK) {
            initialized_ = false;
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
        return true;
    }

    void PowerOffSafe() {
        uint8_t value = 0;
        esp_err_t result = TryReadReg(0x10, &value);
        if (result != ESP_OK) {
            initialized_ = false;
            ESP_LOGW(TAG, "PMIC power-off read failed: %s", esp_err_to_name(result));
            return;
        }

        result = TryWriteReg(0x10, value | 0x01);
        if (result != ESP_OK) {
            initialized_ = false;
            ESP_LOGW(TAG, "PMIC power-off write failed: %s", esp_err_to_name(result));
        }
    }

    void PrepareForDeepSleep() {
        if (!initialized_) {
            ESP_LOGW(TAG, "PMIC unavailable before deep sleep; rail update skipped");
            return;
        }

        uint8_t ldo0 = 0;
        uint8_t dcdc = 0;
        uint8_t ldo1 = 0;
        uint8_t sleep_control = 0;
        uint8_t charge_led = 0;
        uint8_t adc_channels = 0;
        uint8_t aldo1_voltage = 0;
        uint8_t aldo2_voltage = 0;
        uint8_t aldo3_voltage = 0;
        uint8_t aldo4_voltage = 0;
        uint8_t bldo1_voltage = 0;
        uint8_t bldo3_voltage = 0;
        esp_err_t result = ESP_OK;
        result |= TryReadReg(kAxp2101SleepControlReg, &sleep_control);
        result |= TryReadReg(kAxp2101LdoOnOffReg0, &ldo0);
        result |= TryReadReg(kAxp2101DcdcOnOffReg, &dcdc);
        result |= TryReadReg(kAxp2101LdoOnOffReg1, &ldo1);
        result |= TryReadReg(kAxp2101ChargeLedControlReg, &charge_led);
        result |= TryReadReg(kAxp2101AdcChannelEnableReg, &adc_channels);
        result |= TryReadReg(0x92, &aldo1_voltage);
        result |= TryReadReg(0x93, &aldo2_voltage);
        result |= TryReadReg(0x94, &aldo3_voltage);
        result |= TryReadReg(0x95, &aldo4_voltage);
        result |= TryReadReg(0x97, &bldo1_voltage);
        result |= TryReadReg(0x99, &bldo3_voltage);
        if (result != ESP_OK) {
            initialized_ = false;
            ESP_LOGW(TAG, "PMIC rail snapshot failed; rail update skipped: %s", esp_err_to_name(result));
            return;
        }

        ESP_LOGI(
            TAG,
            "PMIC rail snapshot before deep sleep: reg0x26=0x%02x reg0x30=0x%02x reg0x69=0x%02x reg0x80=0x%02x reg0x90=0x%02x reg0x91=0x%02x "
            "reg0x92=0x%02x reg0x93=0x%02x reg0x94=0x%02x reg0x95=0x%02x reg0x97=0x%02x reg0x99=0x%02x",
            sleep_control,
            adc_channels,
            charge_led,
            dcdc,
            ldo0,
            ldo1,
            aldo1_voltage,
            aldo2_voltage,
            aldo3_voltage,
            aldo4_voltage,
            bldo1_voltage,
            bldo3_voltage
        );

        if (!UpdateRegWithRetry(kAxp2101SleepControlReg, kAxp2101SleepControlClearMask, kAxp2101SleepControlSetMask)
            || !WriteRegWithRetry(kAxp2101AdcChannelEnableReg, kAxp2101AdcChannelsOff)
            || !WriteRegWithRetry(kAxp2101ChargeLedControlReg, kAxp2101ChargeLedOff)
            || !WriteRegWithRetry(kAxp2101DcdcOnOffReg, kAxp2101Dcdc1EnableMask)
            || !WriteRegWithRetry(kAxp2101LdoOnOffReg1, 0x00)
            || !WriteRegWithRetry(kAxp2101LdoOnOffReg0, 0x00)) {
            initialized_ = false;
            ESP_LOGW(TAG, "PMIC deep sleep rail update failed");
            return;
        }
        ESP_LOGI(TAG, "PMIC AXP sleep configured: ADC off, CHGLED off, only DCDC1 kept on, REG90/REG91 LDO rails off");
    }

private:
    static constexpr int kWriteAttempts = 3;
    bool initialized_ = false;

    bool Configure() {
        uint8_t data = 0;
        esp_err_t result = TryReadReg(0x90, &data);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "AXP2101 initial read failed: %s", esp_err_to_name(result));
            return false;
        }

        data |= 0b10110100;
        return WriteRegWithRetry(0x90, data)
            && WriteRegWithRetry(0x99, (0b11110 - 5))
            && WriteRegWithRetry(0x97, (0b11110 - 2))
            && WriteRegWithRetry(0x69, 0b00110101)
            && WriteRegWithRetry(0x30, 0b111111)
            && WriteRegWithRetry(0x90, 0xBF)
            && WriteRegWithRetry(0x94, 33 - 5)
            && WriteRegWithRetry(0x95, 33 - 5);
    }

    bool WriteRegWithRetry(uint8_t reg, uint8_t value) {
        esp_err_t result = ESP_FAIL;
        for (int attempt = 1; attempt <= kWriteAttempts; ++attempt) {
            result = TryWriteReg(reg, value);
            if (result == ESP_OK) {
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

class CustomBacklight : public Backlight {
public:
    CustomBacklight(Pmic** pmic) : pmic_(pmic) {}

    void SetBrightnessImpl(uint8_t brightness) override {
        Pmic* pmic = pmic_ == nullptr ? nullptr : *pmic_;
        if (pmic != nullptr && pmic->IsAvailable()) {
            pmic->SetBrightness(target_brightness_);
        }
        brightness_ = target_brightness_;
    }

private:
    Pmic** pmic_;
};

class Aw9523 : public I2cDevice {
public:
    // Exanpd IO Init
    Aw9523(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x02, 0b00000111);  // P0
        WriteReg(0x03, 0b10001111);  // P1
        WriteReg(0x04, 0b00011000);  // CONFIG_P0
        WriteReg(0x05, 0b00001100);  // CONFIG_P1
        WriteReg(0x11, 0b00010000);  // GCR P0 port is Push-Pull mode.
        WriteReg(0x12, 0b11111111);  // LEDMODE_P0
        WriteReg(0x13, 0b11111111);  // LEDMODE_P1
    }

    void ResetAw88298() {
        ESP_LOGI(TAG, "Reset AW88298");
        WriteReg(0x02, 0b00000011);
        vTaskDelay(pdMS_TO_TICKS(10));
        WriteReg(0x02, 0b00000111);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void ResetIli9342() {
        ESP_LOGI(TAG, "Reset IlI9342");
        WriteReg(0x03, 0b10000001);
        vTaskDelay(pdMS_TO_TICKS(20));
        WriteReg(0x03, 0b10000011);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    void PrepareForDeepSleep() {
        WriteReg(0x12, 0x00);
        WriteReg(0x13, 0x00);
        WriteReg(0x02, 0x00);
        WriteReg(0x03, 0x00);
        ESP_LOGI(TAG, "AW9523 outputs prepared for deep sleep: LED modes off, P0/P1 output latches low");
    }
};

class Ft6336 : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };
    
    Ft6336(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        read_buffer_ = new uint8_t[6];
    }

    ~Ft6336() {
        delete[] read_buffer_;
    }

    void UpdateTouchPoint() {
        ReadRegs(0x02, read_buffer_, 6);
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    inline const TouchPoint_t& GetTouchPoint() {
        return tp_;
    }

    void PrepareForDeepSleep() {
        const esp_err_t result = TryWriteReg(kFt6336PowerModeReg, kFt6336PowerModeHibernate);
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "FT6336 hibernate command sent before deep sleep");
        } else {
            ESP_LOGW(TAG, "Failed to send FT6336 hibernate command before deep sleep: %s", esp_err_to_name(result));
        }
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;
};

class M5StackCoreS3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_;
    Aw9523* aw9523_;
    Ft6336* ft6336_;
    LcdDisplay* display_;
    EspVideo* camera_;
    esp_timer_handle_t touchpad_timer_;
    esp_timer_handle_t power_on_deep_sleep_timer_ = nullptr;
    PowerSaveTimer* power_save_timer_;
    SemaphoreHandle_t pmic_mutex_ = nullptr;
    bool pmic_recovery_task_started_ = false;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(10);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            if (pmic_ != nullptr && pmic_->IsAvailable()) {
                pmic_->PowerOffSafe();
            } else {
                ESP_LOGW(TAG, "Shutdown request ignored because PMIC is unavailable");
            }
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
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
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void I2cDetect() {
        uint8_t address;
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        if (!TryInitializePmic()) {
            ESP_LOGW(TAG, "PMIC recovery will be retried after audio codec initialization");
        }
    }

    bool TryInitializePmic() {
        if (pmic_ != nullptr && pmic_->IsAvailable()) {
            return true;
        }

        if (pmic_mutex_ != nullptr) {
            xSemaphoreTake(pmic_mutex_, portMAX_DELAY);
        }
        if (pmic_ != nullptr && pmic_->IsAvailable()) {
            if (pmic_mutex_ != nullptr) {
                xSemaphoreGive(pmic_mutex_);
            }
            return true;
        }

        if (pmic_ != nullptr) {
            delete pmic_;
            pmic_ = nullptr;
        }

        auto* pmic = new Pmic(i2c_bus_, 0x34);
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
        if ((pmic_ != nullptr && pmic_->IsAvailable()) || pmic_recovery_task_started_) {
            return;
        }
        pmic_recovery_task_started_ = true;
        ESP_LOGW(TAG, "Scheduling PMIC recovery task after %s", reason);
        const BaseType_t result = xTaskCreate(
            [](void* arg) {
                auto* board = static_cast<M5StackCoreS3Board*>(arg);
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
            if (pmic_ != nullptr && pmic_->IsAvailable()) {
                ESP_LOGI(TAG, "PMIC recovery task stopped because PMIC is already available");
                pmic_recovery_task_started_ = false;
                return;
            }
            if (TryInitializePmic()) {
                ESP_LOGI(
                    TAG,
                    "PMIC recovery task completed: attempt=%d/%d",
                    attempt,
                    kRetryAttempts
                );
                pmic_recovery_task_started_ = false;
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

    void InitializeAw9523() {
        ESP_LOGI(TAG, "Init AW9523");
        aw9523_ = new Aw9523(i2c_bus_, 0x58);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void PollTouchpad() {
        static bool was_touched = false;
        static int64_t touch_start_time = 0;
        const int64_t TOUCH_THRESHOLD_MS = 500;  // 触摸时长阈值，超过500ms视为长按
        
        ft6336_->UpdateTouchPoint();
        auto& touch_point = ft6336_->GetTouchPoint();
        
        // 检测触摸开始
        if (touch_point.num > 0 && !was_touched) {
            was_touched = true;
            touch_start_time = esp_timer_get_time() / 1000; // 转换为毫秒
        } 
        // 检测触摸释放
        else if (touch_point.num == 0 && was_touched) {
            was_touched = false;
            int64_t touch_duration = (esp_timer_get_time() / 1000) - touch_start_time;
            
            // 只有短触才触发
            if (touch_duration < TOUCH_THRESHOLD_MS) {
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    EnterWifiConfigMode();
                    return;
                }
                app.ToggleChatState();
            }
        }
    }

    void InitializeFt6336TouchPad() {
        ESP_LOGI(TAG, "Init FT6336");
        ft6336_ = new Ft6336(i2c_bus_, 0x38);
        
        // 创建定时器，20ms 间隔
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                M5StackCoreS3Board* board = (M5StackCoreS3Board*)arg;
                board->PollTouchpad();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "touchpad_timer",
            .skip_unhandled_events = true,
        };
        
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &touchpad_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(touchpad_timer_, 20 * 1000));
    }

    void InitializePowerOnDeepSleepTimer() {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                M5StackCoreS3Board* board = static_cast<M5StackCoreS3Board*>(arg);
                if (board != nullptr) {
                    board->EnterPowerOnDeepSleep();
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "cores3_deep_sleep",
            .skip_unhandled_events = true,
        };

        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &power_on_deep_sleep_timer_));
        ESP_ERROR_CHECK(esp_timer_start_once(power_on_deep_sleep_timer_, kPowerOnDeepSleepDelayUs));
        ESP_LOGI(TAG, "Power-on deep sleep timer started: delay=15s");
    }

    void StopWirelessForDeepSleep() {
        esp_err_t result = esp_wifi_stop();
        if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "Failed to stop Wi-Fi before deep sleep: %s", esp_err_to_name(result));
        }
        result = esp_wifi_deinit();
        if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "Failed to deinit Wi-Fi before deep sleep: %s", esp_err_to_name(result));
        }

#if CONFIG_BT_ENABLED
#if CONFIG_BT_BLUEDROID_ENABLED
        esp_bluedroid_status_t bluedroid_status = esp_bluedroid_get_status();
        if (bluedroid_status == ESP_BLUEDROID_STATUS_ENABLED) {
            result = esp_bluedroid_disable();
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Failed to disable Bluetooth stack before deep sleep: %s", esp_err_to_name(result));
            }
            bluedroid_status = esp_bluedroid_get_status();
        }
        if (bluedroid_status == ESP_BLUEDROID_STATUS_INITIALIZED) {
            result = esp_bluedroid_deinit();
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Failed to deinit Bluetooth stack before deep sleep: %s", esp_err_to_name(result));
            }
        }
#endif
        esp_bt_controller_status_t bt_status = esp_bt_controller_get_status();
        if (bt_status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
            result = esp_bt_controller_disable();
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Failed to disable Bluetooth controller before deep sleep: %s", esp_err_to_name(result));
            }
            bt_status = esp_bt_controller_get_status();
        }
        if (bt_status == ESP_BT_CONTROLLER_STATUS_INITED) {
            result = esp_bt_controller_deinit();
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Failed to deinit Bluetooth controller before deep sleep: %s", esp_err_to_name(result));
            }
        }
#endif
    }

    void PrepareAudioForDeepSleep() {
        auto* codec = GetAudioCodec();
        if (codec == nullptr) {
            return;
        }
        codec->PrepareForDeepSleep();
    }

    void PrepareMotionSensorsForDeepSleep() {
        static constexpr uint8_t kBmi270Addresses[] = {0x68, 0x69};
        static constexpr uint8_t kBmm150Addresses[] = {0x10, 0x11, 0x12, 0x13};

        bool bmi270_suspended = false;
        for (const uint8_t addr : kBmi270Addresses) {
            if (TryWriteOneShotI2cReg(i2c_bus_, addr, kBmi270PowerControlReg, kBmi270PowerControlSuspend, "BMI270")) {
                bmi270_suspended = true;
                break;
            }
        }
        if (!bmi270_suspended) {
            ESP_LOGW(TAG, "BMI270 suspend command was not acknowledged before deep sleep");
        }

        bool bmm150_suspended = false;
        for (const uint8_t addr : kBmm150Addresses) {
            if (TryWriteOneShotI2cReg(i2c_bus_, addr, kBmm150PowerControlReg, kBmm150PowerControlSuspend, "BMM150")) {
                bmm150_suspended = true;
                break;
            }
        }
        if (!bmm150_suspended) {
            ESP_LOGW(TAG, "BMM150 suspend command was not acknowledged before deep sleep");
        }
    }

    void ConfigureDisplayPinsForDeepSleep() {
        static constexpr gpio_num_t kDisplayPins[] = {
            GPIO_NUM_3,
            GPIO_NUM_35,
            GPIO_NUM_36,
            GPIO_NUM_37,
        };
        for (const gpio_num_t pin : kDisplayPins) {
            DrivePinLowForDeepSleep(pin);
        }
    }

    void ConfigureAudioPinsForDeepSleep() {
        static constexpr gpio_num_t kAudioPins[] = {
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_I2S_GPIO_DOUT,
        };
        for (const gpio_num_t pin : kAudioPins) {
            DrivePinLowForDeepSleep(pin);
        }
        ReleasePinForDeepSleep(AUDIO_I2S_GPIO_MCLK);
    }

    void ConfigureCameraPinsForDeepSleep() {
        static constexpr gpio_num_t kCameraPins[] = {
            CAMERA_PIN_D0,
            CAMERA_PIN_D1,
            CAMERA_PIN_D2,
            CAMERA_PIN_D3,
            CAMERA_PIN_D4,
            CAMERA_PIN_D5,
            CAMERA_PIN_D6,
            CAMERA_PIN_D7,
            CAMERA_PIN_VSYNC,
            CAMERA_PIN_HREF,
            CAMERA_PIN_PCLK,
        };
        for (const gpio_num_t pin : kCameraPins) {
            ReleasePinForDeepSleep(pin);
        }
    }

    void EnterPowerOnDeepSleep() {
        ESP_LOGI(TAG, "Entering power-on timed deep sleep");
        if (touchpad_timer_ != nullptr) {
            const esp_err_t result = esp_timer_stop(touchpad_timer_);
            if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Failed to stop touchpad timer before deep sleep: %s", esp_err_to_name(result));
            }
        }
        if (display_ != nullptr) {
            display_->SetPowerSaveMode(true);
        }
        if (pmic_ != nullptr) {
            pmic_->SetBrightness(0);
        }
        PrepareAudioForDeepSleep();
        if (ft6336_ != nullptr) {
            ft6336_->PrepareForDeepSleep();
        }
        PrepareMotionSensorsForDeepSleep();
        if (aw9523_ != nullptr) {
            aw9523_->PrepareForDeepSleep();
        }
        ConfigureDisplayPinsForDeepSleep();
        ConfigureAudioPinsForDeepSleep();
        ConfigureCameraPinsForDeepSleep();
        StopWirelessForDeepSleep();
        if (pmic_ != nullptr) {
            pmic_->PrepareForDeepSleep();
        }
        esp_deep_sleep_start();
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_37;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_36;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeIli9342Display() {
        ESP_LOGI(TAG, "Init IlI9342");

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_3;
        io_config.dc_gpio_num = GPIO_NUM_35;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        aw9523_->ResetIli9342();

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

     void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAMERA_PIN_D0,
                [1] = CAMERA_PIN_D1,
                [2] = CAMERA_PIN_D2,
                [3] = CAMERA_PIN_D3,
                [4] = CAMERA_PIN_D4,
                [5] = CAMERA_PIN_D5,
                [6] = CAMERA_PIN_D6,
                [7] = CAMERA_PIN_D7,
            },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus_,
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);
        camera_->SetHMirror(false);
    }

public:
    M5StackCoreS3Board() {
        pmic_mutex_ = xSemaphoreCreateMutex();
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeAxp2101();
        InitializeAw9523();
        I2cDetect();
        InitializeSpi();
        InitializeIli9342Display();
        InitializeCamera();
        InitializeFt6336TouchPad();
        GetBacklight()->RestoreBrightness();
        InitializePowerOnDeepSleepTimer();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CoreS3AudioCodec audio_codec(i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_AW88298_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        SchedulePmicRecoveryTask("audio codec initialization");
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        if (pmic_ == nullptr || !pmic_->IsAvailable()) {
            ESP_LOGW(TAG, "Battery level requested while PMIC is unavailable; retrying PMIC initialization");
            if (!TryInitializePmic()) {
                return false;
            }
        }
        static bool last_discharging = false;
        if (!pmic_->ReadPowerSnapshot(level, charging, discharging)) {
            return false;
        }
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual Backlight *GetBacklight() override {
        static CustomBacklight backlight(&pmic_);
        return &backlight;
    }
};

DECLARE_BOARD(M5StackCoreS3Board);
