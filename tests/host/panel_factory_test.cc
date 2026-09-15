#include "display/sh8601_panel_factory.h"
#include "esp_lcd_panel_ops.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
int fail_at = -1;
int step = 0;
int io_live = 0;
int panel_live = 0;
std::vector<std::string> calls;
void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
int Step(const char* name) { calls.emplace_back(name); return step++ == fail_at ? ESP_FAIL : ESP_OK; }
void Reset(int failure = -1) {
    Check(!io_live && !panel_live, "previous panel resources released");
    fail_at = failure; step = 0; calls.clear();
}
int Create(PanelIo*& io, Panel*& panel) {
    return CreateSh8601Panel(2, {7}, {8}, 22, 0, true, false, io, panel);
}
}

esp_err_t esp_lcd_new_panel_io_spi(int bus, const esp_lcd_panel_io_spi_config_t* config, PanelIo** out) {
    Check(bus == 2 && config->cs_gpio_num == 7, "SPI configuration forwarded");
    if (Step("io") != ESP_OK) { return ESP_FAIL; }
    *out = new PanelIo; ++io_live; return ESP_OK;
}
esp_err_t esp_lcd_new_panel_sh8601(PanelIo* io, const esp_lcd_panel_dev_config_t* config, Panel** out) {
    Check(io && io_live == 1 && config->reset_gpio_num == 8, "driver configuration forwarded");
    if (Step("driver") != ESP_OK) { return ESP_FAIL; }
    *out = new Panel; ++panel_live; return ESP_OK;
}
esp_err_t esp_lcd_panel_set_gap(Panel* panel, int x, int y) {
    Check(panel && x == 22 && y == 0, "panel offsets forwarded"); return Step("gap");
}
esp_err_t esp_lcd_panel_reset(Panel*) { return Step("reset"); }
esp_err_t esp_lcd_panel_init(Panel*) { return Step("init"); }
esp_err_t esp_lcd_panel_invert_color(Panel*, bool invert) {
    Check(!invert, "non-inverted panel colors retained"); return Step("invert");
}
esp_err_t esp_lcd_panel_mirror(Panel*, bool x, bool y) {
    Check(x && !y, "panel orientation forwarded"); return Step("mirror");
}
esp_err_t esp_lcd_panel_disp_on_off(Panel*, bool on) { Check(on, "panel enabled"); return Step("on"); }
esp_err_t esp_lcd_panel_del(Panel* panel) {
    Check(io_live == 1 && panel_live == 1, "driver deleted before its IO");
    calls.emplace_back("delete_driver"); delete panel; --panel_live; return ESP_OK;
}
esp_err_t esp_lcd_panel_io_del(PanelIo* io) {
    Check(io_live == 1 && panel_live == 0, "IO has no live driver");
    calls.emplace_back("delete_io"); delete io; --io_live; return ESP_OK;
}

int main() {
    for (int failure = 0; failure < 8; ++failure) {
        Reset(failure);
        PanelIo* io = nullptr; Panel* panel = nullptr;
        Check(Create(io, panel) == ESP_FAIL, "initialization failure propagated");
        Check(!io && !panel && !io_live && !panel_live, "failed initialization publishes no handles or leaks");
        Check(step == failure + 1, "no later initialization step runs after failure");
        Reset();
        Check(Create(io, panel) == ESP_OK, "initialization can retry after each failed step");
        Check(io && panel && io_live == 1 && panel_live == 1, "successful handles published once");
        const auto count = calls.size();
        Check(Create(io, panel) == ESP_ERR_INVALID_STATE && calls.size() == count, "live handles cannot be overwritten");
        esp_lcd_panel_del(panel); esp_lcd_panel_io_del(io);
    }
    Reset();
    PanelIo* io = nullptr; Panel* panel = nullptr;
    Check(Create(io, panel) == ESP_OK, "panel initializes");
    Check(calls == std::vector<std::string>{"io", "driver", "gap", "reset", "init", "invert", "mirror", "on"},
        "original panel initialization sequence preserved");
    esp_lcd_panel_del(panel); esp_lcd_panel_io_del(io);
    std::cout << "PASS: panel initialization, eight failure points, cleanup, and retry\n";
}
