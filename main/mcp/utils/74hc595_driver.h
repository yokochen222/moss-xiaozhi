#ifndef _74HC595_DRIVER_H_
#define _74HC595_DRIVER_H_

#include <driver/gpio.h>
#include <esp_log.h>

class ShiftRegister74HC595 {
private:
    gpio_num_t ser_pin_;  // 数据引脚
    gpio_num_t rck_pin_;  // 锁存引脚
    gpio_num_t sck_pin_;  // 时钟引脚
    uint8_t current_data_; // 当前输出状态缓存

    void PulseClock();
    void PulseLatch();

    // 高 3 位 (Q5/Q6/Q7) 的"跨调用方共享" 缓存：
    // 与流水灯 (Q0-Q4) 在软件层面互不感知，物理上由本类保证 SetOutputs
    // 写入时不抹除对方写下的高 3 位。
    // 即任何调用方在调用 SetOutputs 时，其低 5 位会被原样发送；
    // 高 3 位会被本字段覆盖为最近一次 SetPanelBit 设置的值。
    static uint8_t panel_state_;

public:
    ShiftRegister74HC595(gpio_num_t ser_pin, gpio_num_t rck_pin, gpio_num_t sck_pin);
    ~ShiftRegister74HC595();

    void Initialize();
    void SetOutputs(uint8_t data);
    void SetOutput(uint8_t bit, bool level);

    // 只更新 Q5/Q6/Q7 区域的状态位（仅修改内部 panel_state_，
    // 不主动触发 SetOutputs；本类保证在后续任意 SetOutputs 调用中
    // 该状态都会被原样保留并锁存到硬件）。
    // bit 范围限定为 5~7；越界则忽略。
    void SetPanelBit(uint8_t bit, bool level);

    // 获取当前高 3 位 (Q5/Q6/Q7) 的状态（调试/状态查询用）
    static uint8_t GetPanelState() { return panel_state_; }

    void ClearAll();
    uint8_t GetCurrentData() const { return current_data_; }
    void Reset(); // 重置驱动状态
};

#endif // _74HC595_DRIVER_H_
