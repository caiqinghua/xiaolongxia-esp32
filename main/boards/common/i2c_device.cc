#include "i2c_device.h"

#include <esp_log.h>

#define TAG "I2cDevice"


I2cDevice::I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr) {
    i2c_device_config_t i2c_device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &i2c_device_cfg, &i2c_device_));
    assert(i2c_device_ != NULL);
}

I2cDevice::~I2cDevice() {
    if (i2c_device_ == nullptr) {
        return;
    }
    const esp_err_t result = i2c_master_bus_rm_device(i2c_device_);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to remove I2C device: %s", esp_err_to_name(result));
    }
    i2c_device_ = nullptr;
}

void I2cDevice::WriteReg(uint8_t reg, uint8_t value) {
    ESP_ERROR_CHECK(TryWriteReg(reg, value));
}

esp_err_t I2cDevice::TryWriteReg(uint8_t reg, uint8_t value) {
    uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(i2c_device_, buffer, 2, 100);
}

esp_err_t I2cDevice::TryReadReg(uint8_t reg, uint8_t* value) {
    return i2c_master_transmit_receive(i2c_device_, &reg, 1, value, 1, 100);
}

uint8_t I2cDevice::ReadReg(uint8_t reg) {
    uint8_t buffer[1];
    ESP_ERROR_CHECK(TryReadReg(reg, buffer));
    return buffer[0];
}

void I2cDevice::ReadRegs(uint8_t reg, uint8_t* buffer, size_t length) {
    ESP_ERROR_CHECK(i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, length, 100));
}
