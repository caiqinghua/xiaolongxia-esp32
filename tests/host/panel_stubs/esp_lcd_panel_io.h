#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int ESP_ERR_INVALID_STATE = 0x103;
using esp_lcd_spi_bus_handle_t = int;
struct PanelIo {};
struct Panel {};
using esp_lcd_panel_io_handle_t = PanelIo*;
using esp_lcd_panel_handle_t = Panel*;
struct esp_lcd_panel_io_spi_config_t { int cs_gpio_num; };
esp_err_t esp_lcd_new_panel_io_spi(int, const esp_lcd_panel_io_spi_config_t*, PanelIo**);
esp_err_t esp_lcd_panel_io_del(PanelIo*);
#define ESP_ERROR_CHECK_WITHOUT_ABORT(expression) static_cast<void>(expression)
