#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>

// The caller owns the SPI bus and serializes panel creation. Outputs are only
// published after every initialization step succeeds; existing handles are never
// overwritten. Successful handles must be owned by either feedback or LVGL.
esp_err_t CreateSh8601Panel(esp_lcd_spi_bus_handle_t bus,
    const esp_lcd_panel_io_spi_config_t& io_config,
    const esp_lcd_panel_dev_config_t& panel_config,
    int gap_x, int gap_y, bool mirror_x, bool mirror_y,
    esp_lcd_panel_io_handle_t& io, esp_lcd_panel_handle_t& panel);
