#ifndef _74HC595_DRIVER_H_
#define _74HC595_DRIVER_H_

#include <driver/gpio.h>
#include <esp_log.h>

class ShiftRegister74HC595 {
private:
    gpio_num_t ser_pin_;  // 数据引脚 (GPIO9)
    gpio_num_t rck_pin_;  // 锁存引脚 (GPIO10) 
    gpio_num_t sck_pin_;  // 时钟引脚 (GPIO11)
    uint8_t current_data_; // 当前输出状态缓存
    
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
    void Reset(); // 重置驱动状态
};

#endif // _74HC595_DRIVER_H_
