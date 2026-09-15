#include "sh8601_panel_factory.h"
#include <esp_lcd_sh8601.h>
#include <esp_lcd_panel_ops.h>

esp_err_t CreateSh8601Panel(esp_lcd_spi_bus_handle_t bus,
    const esp_lcd_panel_io_spi_config_t& io_config,
    const esp_lcd_panel_dev_config_t& panel_config,
    int gap_x, int gap_y, bool mirror_x, bool mirror_y,
    esp_lcd_panel_io_handle_t& io, esp_lcd_panel_handle_t& panel) {
    if (io || panel) { return ESP_ERR_INVALID_STATE; }
    esp_lcd_panel_io_handle_t new_io = nullptr;
    esp_lcd_panel_handle_t new_panel = nullptr;
    esp_err_t result = esp_lcd_new_panel_io_spi(bus, &io_config, &new_io);
    if (result != ESP_OK) { return result; }
    result = esp_lcd_new_panel_sh8601(new_io, &panel_config, &new_panel);
    if (result == ESP_OK) { result = esp_lcd_panel_set_gap(new_panel, gap_x, gap_y); }
    if (result == ESP_OK) { result = esp_lcd_panel_reset(new_panel); }
    if (result == ESP_OK) { result = esp_lcd_panel_init(new_panel); }
    if (result == ESP_OK) { result = esp_lcd_panel_invert_color(new_panel, false); }
    if (result == ESP_OK) { result = esp_lcd_panel_mirror(new_panel, mirror_x, mirror_y); }
    if (result == ESP_OK) { result = esp_lcd_panel_disp_on_off(new_panel, true); }
    if (result != ESP_OK) {
        if (new_panel) { ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_del(new_panel)); }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_io_del(new_io));
        return result;
    }
    io = new_io;
    panel = new_panel;
    return ESP_OK;
}
