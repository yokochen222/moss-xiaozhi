#pragma once
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <cstdint>

class Pca9685 {
public:
    static Pca9685& GetInstance();
    bool Init(i2c_master_bus_handle_t bus, uint8_t addr = 0x40);
    bool IsReady() const { return ready_.load(std::memory_order_acquire); }

    bool SetPwm(uint8_t channel, uint16_t on, uint16_t off);
    bool SetDuty(uint8_t channel, uint16_t duty);
    bool SetDigital(uint8_t channel, bool level);
    // OV2640 PWDN on LED2：true=掉电，false=工作。绕过普通通道写保护。
    bool SetDvpPowerDown(bool power_down);
    bool SetAllOff();  // skips channel 2

    Pca9685(const Pca9685&) = delete;
    Pca9685& operator=(const Pca9685&) = delete;

private:
    Pca9685() = default;
    bool WriteReg(uint8_t reg, uint8_t value);
    bool WriteRegs(uint8_t reg, const uint8_t* data, size_t len);
    bool GuardChannel(uint8_t channel) const;  // false if ch==2 or ch>15
    bool SetDigitalUnlocked(uint8_t channel, bool level);

    i2c_master_dev_handle_t dev_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    std::atomic<bool> ready_{false};
};
