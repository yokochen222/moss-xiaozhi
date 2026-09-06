#include "74hc595_driver.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "74HC595"

ShiftRegister74HC595::ShiftRegister74HC595(gpio_num_t ser_pin, gpio_num_t rck_pin, gpio_num_t sck_pin)
    : ser_pin_(ser_pin), rck_pin_(rck_pin), sck_pin_(sck_pin), current_data_(0) {
}

ShiftRegister74HC595::~ShiftRegister74HC595() {
    gpio_reset_pin(ser_pin_);
    gpio_reset_pin(rck_pin_);
    gpio_reset_pin(sck_pin_);
}

void ShiftRegister74HC595::Initialize() {
    ESP_LOGI(TAG, "初始化74HC595，引脚: SER=%d, RCK=%d, SCK=%d", ser_pin_, rck_pin_, sck_pin_);

    gpio_hold_dis(ser_pin_);
    gpio_hold_dis(rck_pin_);
    gpio_hold_dis(sck_pin_);
    gpio_reset_pin(ser_pin_);
    gpio_reset_pin(rck_pin_);
    gpio_reset_pin(sck_pin_);

    gpio_config_t ser_config = {
        .pin_bit_mask = (1ULL << ser_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&ser_config));

    gpio_config_t rck_config = {
        .pin_bit_mask = (1ULL << rck_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rck_config));

    gpio_config_t sck_config = {
        .pin_bit_mask = (1ULL << sck_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sck_config));

    gpio_set_level(ser_pin_, 0);
    gpio_set_level(rck_pin_, 0);
    gpio_set_level(sck_pin_, 0);

    current_data_ = 0;
    ClearAll();

    ESP_LOGI(TAG, "74HC595初始化完成，当前状态: 0x%02X", current_data_);
}

void ShiftRegister74HC595::PulseClock() {
    gpio_set_level(sck_pin_, 0);
    gpio_set_level(sck_pin_, 1);
}

void ShiftRegister74HC595::PulseLatch() {
    gpio_set_level(rck_pin_, 0);
    gpio_set_level(rck_pin_, 1);
}

void ShiftRegister74HC595::SetOutputs(uint8_t data) {
    // lichuang-dev: 从最高位开始移入，8 位原样输出，不做 Q5-Q7 合并。
    for (int i = 0; i < 8; i++) {
        if ((data << i) & 0x80) {
            gpio_set_level(ser_pin_, 1);
        } else {
            gpio_set_level(ser_pin_, 0);
        }
        gpio_set_level(sck_pin_, 0);
        gpio_set_level(sck_pin_, 1);
    }
    gpio_set_level(rck_pin_, 0);
    gpio_set_level(rck_pin_, 1);
    current_data_ = data;
}

void ShiftRegister74HC595::SetOutput(uint8_t bit, bool level) {
    if (bit >= 8) {
        ESP_LOGW(TAG, "位索引超出范围: %d", bit);
        return;
    }
    if (level) {
        current_data_ |= (1 << bit);
    } else {
        current_data_ &= ~(1 << bit);
    }
    SetOutputs(current_data_);
}

void ShiftRegister74HC595::ClearAll() {
    current_data_ = 0x00;
    SetOutputs(current_data_);
}

void ShiftRegister74HC595::Reset() {
    current_data_ = 0x00;
    gpio_set_level(ser_pin_, 0);
    gpio_set_level(rck_pin_, 0);
    gpio_set_level(sck_pin_, 0);
    ClearAll();
}
