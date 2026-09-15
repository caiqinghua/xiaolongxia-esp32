#pragma once
#include "esp_lcd_panel_io.h"
esp_err_t esp_lcd_panel_set_gap(Panel*, int, int);
esp_err_t esp_lcd_panel_reset(Panel*);
esp_err_t esp_lcd_panel_init(Panel*);
esp_err_t esp_lcd_panel_invert_color(Panel*, bool);
esp_err_t esp_lcd_panel_mirror(Panel*, bool, bool);
esp_err_t esp_lcd_panel_disp_on_off(Panel*, bool);
esp_err_t esp_lcd_panel_del(Panel*);
