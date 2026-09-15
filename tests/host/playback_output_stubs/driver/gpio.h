#pragma once
#include <cstdint>
using gpio_num_t = int;
constexpr gpio_num_t GPIO_NUM_NC = -1;
constexpr int GPIO_MODE_INPUT_OUTPUT = 3;
constexpr int GPIO_PULLUP_DISABLE = 0, GPIO_PULLDOWN_DISABLE = 0, GPIO_INTR_DISABLE = 0;
struct gpio_config_t {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
};
int gpio_config(const gpio_config_t*);
int gpio_set_level(gpio_num_t, uint32_t);
int gpio_get_level(gpio_num_t);
