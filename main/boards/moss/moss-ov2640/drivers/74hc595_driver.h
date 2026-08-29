#ifndef _74HC595_DRIVER_H_
#define _74HC595_DRIVER_H_

#include <driver/gpio.h>
#include <cstdint>

// ov2640 云台独占 595：原样写出 8 位，不做灯条高 3 位合并。
class ShiftRegister74HC595 {
private:
    gpio_num_t ser_pin_;
    gpio_num_t rck_pin_;
    gpio_num_t sck_pin_;
    uint8_t current_data_;

public:
    ShiftRegister74HC595(gpio_num_t ser_pin, gpio_num_t rck_pin, gpio_num_t sck_pin);
    ~ShiftRegister74HC595();

    void Initialize();
    void SetOutputs(uint8_t data);
    void ClearAll();
    uint8_t GetCurrentData() const { return current_data_; }
};

#endif
