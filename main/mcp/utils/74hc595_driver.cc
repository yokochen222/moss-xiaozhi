#include "74hc595_driver.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include <rom/ets_sys.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "74HC595"

// FM 74HC595D / long traces / ULN load: keep edges well above datasheet minimums.
static constexpr uint32_t kEdgeDelayUs = 5;

ShiftRegister74HC595::ShiftRegister74HC595(gpio_num_t ser_pin, gpio_num_t rck_pin,
                                           gpio_num_t sck_pin)
    : ser_pin_(ser_pin), rck_pin_(rck_pin), sck_pin_(sck_pin), current_data_(0) {}

ShiftRegister74HC595::~ShiftRegister74HC595() {
    // 先关断输出再释放 GPIO；否则脚变高阻后 595 仍保持上次锁存，线圈可能常通。
    ClearAll();
    gpio_reset_pin(ser_pin_);
    gpio_reset_pin(rck_pin_);
    gpio_reset_pin(sck_pin_);
}

void ShiftRegister74HC595::Initialize() {
    ESP_LOGI(TAG, "初始化74HC595，引脚: SER=%d, RCK=%d, SCK=%d", ser_pin_, rck_pin_, sck_pin_);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << ser_pin_) | (1ULL << rck_pin_) | (1ULL << sck_pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(ser_pin_, 0);
    gpio_set_level(rck_pin_, 0);
    gpio_set_level(sck_pin_, 0);

    current_data_ = 0;
    ClearAll();

    ESP_LOGI(TAG, "74HC595初始化完成，当前状态: 0x%02X", current_data_);
}

void ShiftRegister74HC595::PulseClock() {
    // Idle-low, rising-edge shift (手册: SCK 上升沿移位)
    gpio_set_level(sck_pin_, 0);
    ets_delay_us(kEdgeDelayUs);
    gpio_set_level(sck_pin_, 1);
    ets_delay_us(kEdgeDelayUs);
    gpio_set_level(sck_pin_, 0);
}

void ShiftRegister74HC595::PulseLatch() {
    // 手册: RCK 高电平存储。完成锁存后必须拉回低电平关闭透明窗，
    // 否则空闲时 SCK/SER 噪声可能改写输出，造成线圈意外常通发烫。
    gpio_set_level(rck_pin_, 0);
    ets_delay_us(kEdgeDelayUs);
    gpio_set_level(rck_pin_, 1);
    ets_delay_us(kEdgeDelayUs);
    gpio_set_level(rck_pin_, 0);
    ets_delay_us(kEdgeDelayUs);
}

void ShiftRegister74HC595::SetOutputs(uint8_t data) {
    // Shift with latch closed (RCK low); then open/store on RCK high.
    gpio_set_level(rck_pin_, 0);
    ets_delay_us(kEdgeDelayUs);
    for (int i = 0; i < 8; i++) {
        gpio_set_level(ser_pin_, (data << i) & 0x80 ? 1 : 0);
        ets_delay_us(kEdgeDelayUs);
        PulseClock();
    }
    PulseLatch();
    current_data_ = data;
    ESP_LOGD(TAG, "SetOutputs 0x%02X", data);
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

void ShiftRegister74HC595::ClearAll() { SetOutputs(0x00); }

void ShiftRegister74HC595::Reset() {
    current_data_ = 0;

    gpio_set_level(ser_pin_, 0);
    gpio_set_level(rck_pin_, 0);
    gpio_set_level(sck_pin_, 0);

    ClearAll();
}
