#ifndef _74HC595_DRIVER_H_
#define _74HC595_DRIVER_H_

#include <driver/gpio.h>
#include <esp_log.h>

// Source moss-pcb-board / lichuang-dev: Q0-Q7 原样移位，不做面板灯合并。
class ShiftRegister74HC595 {
private:
    gpio_num_t ser_pin_;
    gpio_num_t rck_pin_;
    gpio_num_t sck_pin_;
    uint8_t current_data_;

    void PulseClock();
    void PulseLatch();

public:
    ShiftRegister74HC595(gpio_num_t ser_pin, gpio_num_t rck_pin, gpio_num_t sck_pin);
    ~ShiftRegister74HC595();

    void Initialize();
    void SetOutputs(uint8_t data);
    void SetOutput(uint8_t bit, bool level);
    void ClearAll();
    uint8_t GetCurrentData() const { return current_data_; }
    void Reset();
};

#endif
