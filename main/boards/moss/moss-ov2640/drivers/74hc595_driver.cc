#include "74hc595_driver.h"
#include <driver/gpio.h>
#include <esp_log.h>

#define TAG "74HC595Gimbal"

ShiftRegister74HC595::ShiftRegister74HC595(gpio_num_t ser_pin, gpio_num_t rck_pin, gpio_num_t sck_pin)
    : ser_pin_(ser_pin), rck_pin_(rck_pin), sck_pin_(sck_pin), current_data_(0) {
}

ShiftRegister74HC595::~ShiftRegister74HC595() {
    gpio_reset_pin(ser_pin_);
    gpio_reset_pin(rck_pin_);
    gpio_reset_pin(sck_pin_);
}

void ShiftRegister74HC595::Initialize() {
    ESP_LOGI(TAG, "gimbal 595 SER=%d RCK=%d SCK=%d", ser_pin_, rck_pin_, sck_pin_);

    gpio_config_t io = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    io.pin_bit_mask = (1ULL << ser_pin_);
    ESP_ERROR_CHECK(gpio_config(&io));
    io.pin_bit_mask = (1ULL << rck_pin_);
    ESP_ERROR_CHECK(gpio_config(&io));
    io.pin_bit_mask = (1ULL << sck_pin_);
    ESP_ERROR_CHECK(gpio_config(&io));

    gpio_set_level(ser_pin_, 0);
    gpio_set_level(rck_pin_, 0);
    gpio_set_level(sck_pin_, 0);
    ClearAll();
}

void ShiftRegister74HC595::SetOutputs(uint8_t data) {
    for (int i = 0; i < 8; i++) {
        gpio_set_level(ser_pin_, ((data << i) & 0x80) ? 1 : 0);
        gpio_set_level(sck_pin_, 0);
        gpio_set_level(sck_pin_, 1);
    }
    gpio_set_level(rck_pin_, 0);
    gpio_set_level(rck_pin_, 1);
    current_data_ = data;
}

void ShiftRegister74HC595::ClearAll() {
    SetOutputs(0);
}
