#include "wifi_board.h"
#include "display/lcd_display.h"
#include "esp_lcd_sh8601.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "config.h"
#include "axp2101.h"
#include "i2c_device.h"
#include "audio/local_recording_service.h"
#include "storage/sd_card_storage.h"
#include "system/boot_diagnostics.h"
#include "system/audream_power_manager.h"
#include "blufi.h"

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
#include <freertos/task.h>
#include "settings.h"

#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include <algorithm>

#define TAG "WaveshareEsp32s3TouchAMOLED2inch06"

namespace {

constexpr uint16_t kRecordingButtonHoldMs = 1500;
constexpr int64_t kDeepSleepWakeRecordHoldUs = static_cast<int64_t>(kRecordingButtonHoldMs) * 1000LL;
constexpr uint64_t kRecordingFeedbackScreenOnUs = 3000ULL * 1000ULL;
constexpr uint64_t kRecordingHoldRetryUs = 200ULL * 1000ULL;
constexpr size_t kRecordingFeedbackDrawRows = 4;
constexpr int kAudreamLogoGlyphWidth = 5;
constexpr int kAudreamLogoGlyphHeight = 7;
constexpr int kAudreamLogoGlyphScale = 8;
constexpr int kAudreamLogoGlyphSpacing = kAudreamLogoGlyphScale;
constexpr int kAudreamLogoGlyphCount = 7;
constexpr int kAudreamLogoWidth =
    kAudreamLogoGlyphCount * kAudreamLogoGlyphWidth * kAudreamLogoGlyphScale +
    (kAudreamLogoGlyphCount - 1) * kAudreamLogoGlyphSpacing;
constexpr int kAudreamLogoHeight = kAudreamLogoGlyphHeight * kAudreamLogoGlyphScale;
constexpr uint16_t kAudreamLogoBackgroundColor = 0x0000;
constexpr uint16_t kAudreamLogoForegroundColor = 0xFFFF;
static_assert(kAudreamLogoWidth <= DISPLAY_WIDTH);
static_assert(kAudreamLogoHeight <= DISPLAY_HEIGHT);

void DrivePinLowForDeepSleep(gpio_num_t pin) {
    if (pin == GPIO_NUM_NC) {
        return;
    }
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    gpio_pullup_dis(pin);
    gpio_pulldown_en(pin);
}

constexpr uint8_t kAudreamLogoGlyphs[kAudreamLogoGlyphCount][kAudreamLogoGlyphHeight] = {
    {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},  // A
    {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},  // U
    {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110},  // D
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001},  // R
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111},  // E
    {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},  // A
    {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001},  // M
};

}  // namespace

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);  // hold 4s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);
        WriteReg(0x93, (3300 - 500) / 100);

        // Enable ALDO1 and ALDO2.
        WriteReg(0x90, 0x03);

        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x0A); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
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
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES*  0.1, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES*  0.1, 0);
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }
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

    void PrepareForDeepSleep() {
        SetImmediateBrightness(0);
        SendCommand(0x28, nullptr, 0);
        SendCommand(0x10, nullptr, 0);
        esp_rom_delay_us(120000);
        panel_awake_ = false;
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
    };

    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    CustomLcdDisplay* display_ = nullptr;
    CustomBacklight* backlight_ = nullptr;
    Button boot_button_;
    esp_timer_handle_t power_status_timer_ = nullptr;
    esp_timer_handle_t deep_sleep_record_hold_timer_ = nullptr;
    esp_timer_handle_t recording_feedback_screen_timer_ = nullptr;
    esp_timer_handle_t recording_hold_retry_timer_ = nullptr;
    bool power_source_initialized_ = false;
    bool usb_powered_ = false;
    bool recording_button_pressed_ = false;
    bool recording_hold_start_pending_ = false;
    RecordingHoldStartSource recording_hold_start_source_ = RecordingHoldStartSource::kButton;
    bool recording_feedback_display_task_started_ = false;
    bool recording_feedback_display_initialized_ = false;
    bool recording_feedback_spi_initialized_ = false;
    bool recording_feedback_pulse_pending_ = false;
    esp_lcd_panel_handle_t recording_feedback_panel_ = nullptr;
    uint16_t* recording_feedback_draw_buffer_ = nullptr;
    uint16_t* recording_feedback_logo_tile_buffer_ = nullptr;
    bool pending_deep_sleep_recording_ = false;
    bool suppress_next_wake_long_press_ = false;
    int64_t deep_sleep_wake_button_down_us_ = 0;

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
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
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void UpdateUsbPowerState(bool force_log = false) {
        if (pmic_ == nullptr) {
            return;
        }
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
        power_manager.Start(BOOT_BUTTON_GPIO);
        UpdateUsbPowerState(true);

        const esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg);
                if (board == nullptr) {
                    return;
                }
                board->UpdateUsbPowerState();
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

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeSH8601Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // Initialize LCD panel IO.
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
            EXAMPLE_PIN_NUM_LCD_CS,
            nullptr,
            nullptr);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // Initialize LCD panel driver.
        ESP_LOGD(TAG, "Install LCD driver");
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
        panel_config.vendor_config = (void* )&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(panel_io, &panel_config, &panel));
        esp_lcd_panel_set_gap(panel, 0x16, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    void InitializeRecordingFeedbackDisplay() {
        if (recording_feedback_display_task_started_) {
            return;
        }
        recording_feedback_display_task_started_ = true;
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
            kAudreamLogoBackgroundColor
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
            kAudreamLogoGlyphScale * kAudreamLogoGlyphScale;
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
            kAudreamLogoForegroundColor
        );

        const int logo_x = ((DISPLAY_WIDTH - kAudreamLogoWidth) / 2) & ~1;
        const int logo_y = ((DISPLAY_HEIGHT - kAudreamLogoHeight) / 2) & ~1;
        const int glyph_advance =
            kAudreamLogoGlyphWidth * kAudreamLogoGlyphScale + kAudreamLogoGlyphSpacing;
        for (int glyph = 0; glyph < kAudreamLogoGlyphCount; ++glyph) {
            for (int row = 0; row < kAudreamLogoGlyphHeight; ++row) {
                const uint8_t row_bits = kAudreamLogoGlyphs[glyph][row];
                for (int column = 0; column < kAudreamLogoGlyphWidth; ++column) {
                    const uint8_t mask = 1U << (kAudreamLogoGlyphWidth - column - 1);
                    if ((row_bits & mask) == 0) {
                        continue;
                    }
                    const int x = logo_x + glyph * glyph_advance +
                        column * kAudreamLogoGlyphScale;
                    const int y = logo_y + row * kAudreamLogoGlyphScale;
                    const esp_err_t draw_result = esp_lcd_panel_draw_bitmap(
                        recording_feedback_panel_,
                        x,
                        y,
                        x + kAudreamLogoGlyphScale,
                        y + kAudreamLogoGlyphScale,
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
        if (!recording_feedback_spi_initialized_) {
            InitializeSpi();
            recording_feedback_spi_initialized_ = true;
        }

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
            EXAMPLE_PIN_NUM_LCD_CS,
            nullptr,
            nullptr);
        const esp_err_t io_result = esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io);
        if (io_result != ESP_OK) {
            recording_feedback_display_task_started_ = false;
            ESP_LOGE(TAG, "Failed to create recording feedback panel IO: %s", esp_err_to_name(io_result));
            return;
        }
        const sh8601_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(sh8601_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = const_cast<sh8601_vendor_config_t*>(&vendor_config);

        esp_lcd_panel_handle_t panel = nullptr;
        const esp_err_t panel_create_result =
            esp_lcd_new_panel_sh8601(panel_io, &panel_config, &panel);
        if (panel_create_result != ESP_OK) {
            recording_feedback_display_task_started_ = false;
            ESP_LOGE(
                TAG,
                "Failed to create recording feedback SH8601 panel: %s",
                esp_err_to_name(panel_create_result)
            );
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_io_del(panel_io));
            return;
        }

        const auto run_panel_step = [](const char* name, esp_err_t result) {
            if (result == ESP_OK) {
                return true;
            }
            ESP_LOGE(
                TAG,
                "Recording feedback panel step failed: step=%s result=%s",
                name,
                esp_err_to_name(result)
            );
            return false;
        };
        const bool panel_initialized =
            run_panel_step("set_gap", esp_lcd_panel_set_gap(panel, 0x16, 0)) &&
            run_panel_step("reset", esp_lcd_panel_reset(panel)) &&
            run_panel_step("initialize", esp_lcd_panel_init(panel)) &&
            run_panel_step("invert", esp_lcd_panel_invert_color(panel, false)) &&
            run_panel_step("mirror", esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y)) &&
            run_panel_step("display_on", esp_lcd_panel_disp_on_off(panel, true));
        if (!panel_initialized) {
            recording_feedback_display_task_started_ = false;
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_del(panel));
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_io_del(panel_io));
            return;
        }

        recording_feedback_panel_ = panel;
        backlight_ = new CustomBacklight(panel_io, true);
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

    void PulseRecordingFeedbackScreen() {
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

    void PrepareAudioForDeepSleep() {
        auto* codec = GetAudioCodec();
        if (codec == nullptr) {
            return;
        }
        if (codec->output_enabled()) {
            codec->SetOutputVolume(0);
        }
        codec->EnableInput(false);
        codec->EnableOutput(false);
    }

    void PrepareBoardForDeepSleep() {
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
        if (backlight_ != nullptr) {
            backlight_->PrepareForDeepSleep();
        }
        PrepareAudioForDeepSleep();
        SdCardStorage::GetInstance().PrepareForDeepSleep();
        ConfigureSdPinsForDeepSleep();
        ConfigureDisplayPinsForDeepSleep();
        ConfigureAudioPinsForDeepSleep();
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
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
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    void InitializeRecordingButton() {
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
    }

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

    void HandleRecordingLongPressTrigger() {
        auto& recording = LocalRecordingService::GetInstance();
        auto& audio_service = Application::GetInstance().GetAudioService();
        if (recording.IsRecording()) {
            recording_hold_start_pending_ = false;
            const auto metadata = recording.Stop(audio_service);
            PulseRecordingFeedbackScreen();
            if (metadata.valid) {
                ESP_LOGI(
                    TAG,
                    "Button stopped local recording: id=%s duration=%lu ms size=%lu bytes",
                    metadata.recording_id.c_str(),
                    static_cast<unsigned long>(metadata.duration_ms),
                    static_cast<unsigned long>(metadata.size_bytes)
                );
            } else {
                ESP_LOGW(TAG, "Button stopped local recording without valid metadata");
            }
            return;
        }

        BeginRecordingHoldStart(RecordingHoldStartSource::kButton);
    }

    void BeginRecordingHoldStart(RecordingHoldStartSource source) {
        recording_hold_start_source_ = source;
        recording_hold_start_pending_ = true;
        TryStartRecordingFromHold();
    }

    const char* RecordingHoldStartSourceName() const {
        switch (recording_hold_start_source_) {
            case RecordingHoldStartSource::kDeepSleepWake:
                return "Deep sleep wake";
            case RecordingHoldStartSource::kButton:
            default:
                return "Button";
        }
    }

    void TryStartRecordingFromHold() {
        if (!recording_hold_start_pending_) {
            return;
        }
        if (!recording_button_pressed_ && gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
            recording_hold_start_pending_ = false;
            ESP_LOGI(TAG, "Recording hold start canceled because the button was released");
            return;
        }
        const auto storage_status = SdCardStorage::GetInstance().GetStatus();
        if (!storage_status.mounted) {
            ScheduleRecordingHoldStartRetry();
            return;
        }

        auto& recording = LocalRecordingService::GetInstance();
        if (recording.IsRecording()) {
            recording_hold_start_pending_ = false;
            ESP_LOGI(TAG, "%s recording hold start skipped because recording is already active", RecordingHoldStartSourceName());
            return;
        }
        auto& audio_service = Application::GetInstance().GetAudioService();
        if (recording.Start(audio_service)) {
            recording_hold_start_pending_ = false;
            const auto metadata = recording.CurrentMetadata();
            PulseRecordingFeedbackScreen();
            ESP_LOGI(
                TAG,
                "%s started local recording: id=%s",
                RecordingHoldStartSourceName(),
                metadata.recording_id.c_str()
            );
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
                ESP_LOGE(TAG, "Failed to create recording hold retry timer: %s", esp_err_to_name(result));
                return;
            }
        }
        const esp_err_t result = esp_timer_start_once(recording_hold_retry_timer_, kRecordingHoldRetryUs);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            recording_hold_start_pending_ = false;
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
        LogWakeContext();
        InitializeCodecI2c();
        InitializeAxp2101();
        InitializeRecordingPowerManagement();
        InitializeRecordingButton();
        DetectDeepSleepRecordingIntent();
        ESP_LOGI(TAG, "Touch and WiFi application features are disabled");
        auto& storage = SdCardStorage::GetInstance();
        storage.SetInitializationCompleteCallback([this](const StorageStatus& status) {
            HandleStorageInitializationComplete(status);
        });
        storage.StartInitializationTask();
    }

    virtual bool IsRecordingOnlyMode() const override {
        return true;
    }

    virtual void StartNetwork() override {
        const esp_err_t result = Blufi::GetInstance().init(false);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Audream BLE control: %s", esp_err_to_name(result));
            return;
        }
        AudreamPowerManager::GetInstance().SetBeforeDeepSleepCallback([this]() {
            auto& blufi = Blufi::GetInstance();
            blufi.SendDeviceWillSleepEvent();
            vTaskDelay(pdMS_TO_TICKS(200));
            blufi.deinit();
            PrepareBoardForDeepSleep();
        });
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
            AUDIO_INPUT_REFERENCE);
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
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        (void)level;
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED2inch06);
