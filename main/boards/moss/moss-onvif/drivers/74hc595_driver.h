#ifndef _74HC595_DRIVER_H_
#define _74HC595_DRIVER_H_

#include <driver/gpio.h>
#include <esp_log.h>

// onvif 灯条/面板：低 5 位流水灯，高 3 位面板/底灯，SetOutputs 会合并两者。
class ShiftRegister74HC595 {
private:
    gpio_num_t ser_pin_;
    gpio_num_t rck_pin_;
    gpio_num_t sck_pin_;
    uint8_t current_data_;
    static uint8_t panel_state_;

    void PulseClock();
    void PulseLatch();

public:
    ShiftRegister74HC595(gpio_num_t ser_pin, gpio_num_t rck_pin, gpio_num_t sck_pin);
    ~ShiftRegister74HC595();

    void Initialize();
    void SetOutputs(uint8_t data);
    void SetOutput(uint8_t bit, bool level);
    void SetPanelBit(uint8_t bit, bool level);
    static uint8_t GetPanelState() { return panel_state_; }

    void ClearAll();
    uint8_t GetCurrentData() const { return current_data_; }
    void Reset();
};

#endif
