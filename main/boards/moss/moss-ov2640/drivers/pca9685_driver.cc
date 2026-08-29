#include "pca9685_driver.h"

#include <esp_log.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>
#include <atomic>

#define TAG "Pca9685"

namespace {

portMUX_TYPE init_spinlock = portMUX_INITIALIZER_UNLOCKED;

constexpr uint8_t kRegMode1 = 0x00;
constexpr uint8_t kRegLed0OnL = 0x06;
constexpr uint8_t kRegPrescale = 0xFE;

constexpr uint8_t kMode1Sleep = 0x10;
constexpr uint8_t kMode1Ai = 0x20;
constexpr uint8_t kMode1Allcall = 0x01;

constexpr uint8_t kFullOnOffBit = 0x10;

constexpr uint8_t kPrescale1000Hz = 5;

}  // namespace

Pca9685& Pca9685::GetInstance() {
    static Pca9685 instance;
    return instance;
}

bool Pca9685::Init(i2c_master_bus_handle_t bus, uint8_t addr) {
    if (ready_) {
        return true;
    }

    if (mutex_ == nullptr) {
        SemaphoreHandle_t created = xSemaphoreCreateMutex();
        if (created == nullptr) {
            ESP_LOGW(TAG, "Failed to create mutex");
            return false;
        }
        portENTER_CRITICAL(&init_spinlock);
        if (mutex_ == nullptr) {
            mutex_ = created;
            created = nullptr;
        }
        portEXIT_CRITICAL(&init_spinlock);
        if (created != nullptr) {
            vSemaphoreDelete(created);
        }
    }

    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    if (ready_) {
        xSemaphoreGive(mutex_);
        return true;
    }

    if (dev_ == nullptr) {
        i2c_device_config_t i2c_device_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 100 * 1000,
            .scl_wait_us = 0,
            .flags =
                {
                    .disable_ack_check = 0,
                },
        };
        esp_err_t err = i2c_master_bus_add_device(bus, &i2c_device_cfg, &dev_);
        if (err != ESP_OK) {
            xSemaphoreGive(mutex_);
            ESP_LOGW(TAG, "Failed to add I2C device: %s", esp_err_to_name(err));
            return false;
        }
    }

    bool ok = WriteReg(kRegMode1, kMode1Sleep | kMode1Ai);
    if (ok) {
        ok = WriteReg(kRegPrescale, kPrescale1000Hz);
    }
    if (ok) {
        ok = WriteReg(kRegMode1, kMode1Ai | kMode1Allcall);
    }

    if (ok) {
        xSemaphoreGive(mutex_);
        vTaskDelay(pdMS_TO_TICKS(1));
        if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
            return false;
        }
        if (ready_) {
            xSemaphoreGive(mutex_);
            return true;
        }
    }

    if (ok) {
        const uint8_t full_off[4] = {0, 0, 0, kFullOnOffBit};
        for (uint8_t ch = 0; ch <= 15; ++ch) {
            if (ch == 2) {
                continue;
            }
            if (!WriteRegs(kRegLed0OnL + 4 * ch, full_off, 4)) {
                ok = false;
            }
        }
    }

    ready_.store(ok, std::memory_order_release);
    if (!ok) {
        ESP_LOGW(TAG, "PCA9685 init failed");
    }
    xSemaphoreGive(mutex_);
    return ok;
}

bool Pca9685::WriteReg(uint8_t reg, uint8_t value) {
    uint8_t buffer[2] = {reg, value};
    esp_err_t err = i2c_master_transmit(dev_, buffer, 2, 100);
    if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2));
        err = i2c_master_transmit(dev_, buffer, 2, 100);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WriteReg 0x%02X failed: %s", reg, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool Pca9685::WriteRegs(uint8_t reg, const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return false;
    }

    uint8_t buffer[1 + 4];
    if (len > 4) {
        ESP_LOGW(TAG, "WriteRegs length %u exceeds buffer", static_cast<unsigned>(len));
        return false;
    }

    buffer[0] = reg;
    for (size_t i = 0; i < len; ++i) {
        buffer[1 + i] = data[i];
    }

    esp_err_t err = i2c_master_transmit(dev_, buffer, 1 + len, 100);
    if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2));
        err = i2c_master_transmit(dev_, buffer, 1 + len, 100);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WriteRegs 0x%02X failed: %s", reg, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool Pca9685::GuardChannel(uint8_t channel) const {
    if (channel == 2) {
        ESP_LOGW(TAG, "channel 2 (DVP) is protected, skip write");
        return false;
    }
    if (channel > 15) {
        ESP_LOGW(TAG, "channel out of range: %u", channel);
        return false;
    }
    return true;
}

bool Pca9685::SetPwm(uint8_t channel, uint16_t on, uint16_t off) {
    if (!ready_) {
        return false;
    }
    if (!GuardChannel(channel)) {
        return false;
    }

    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    const uint8_t data[4] = {
        static_cast<uint8_t>(on & 0xFF),
        static_cast<uint8_t>((on >> 8) & 0x0F),
        static_cast<uint8_t>(off & 0xFF),
        static_cast<uint8_t>((off >> 8) & 0x0F),
    };
    const bool ok = WriteRegs(kRegLed0OnL + 4 * channel, data, 4);
    xSemaphoreGive(mutex_);
    return ok;
}

bool Pca9685::SetDuty(uint8_t channel, uint16_t duty) {
    if (!ready_) {
        return false;
    }
    if (!GuardChannel(channel)) {
        return false;
    }

    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    bool ok;
    if (duty == 0) {
        const uint8_t data[4] = {0, 0, 0, kFullOnOffBit};
        ok = WriteRegs(kRegLed0OnL + 4 * channel, data, 4);
    } else if (duty >= 4095) {
        const uint8_t data[4] = {0, kFullOnOffBit, 0, 0};
        ok = WriteRegs(kRegLed0OnL + 4 * channel, data, 4);
    } else {
        const uint8_t data[4] = {
            0,
            0,
            static_cast<uint8_t>(duty & 0xFF),
            static_cast<uint8_t>((duty >> 8) & 0x0F),
        };
        ok = WriteRegs(kRegLed0OnL + 4 * channel, data, 4);
    }

    xSemaphoreGive(mutex_);
    return ok;
}

bool Pca9685::SetDigitalUnlocked(uint8_t channel, bool level) {
    uint8_t data[4];
    if (level) {
        data[0] = 0;
        data[1] = kFullOnOffBit;
        data[2] = 0;
        data[3] = 0;
    } else {
        data[0] = 0;
        data[1] = 0;
        data[2] = 0;
        data[3] = kFullOnOffBit;
    }
    return WriteRegs(kRegLed0OnL + 4 * channel, data, 4);
}

bool Pca9685::SetDigital(uint8_t channel, bool level) {
    if (!ready_) {
        return false;
    }
    if (!GuardChannel(channel)) {
        return false;
    }

    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    const bool ok = SetDigitalUnlocked(channel, level);
    xSemaphoreGive(mutex_);
    return ok;
}

bool Pca9685::SetDvpPowerDown(bool power_down) {
    if (!ready_) {
        return false;
    }
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    // Channel 2 is intentionally addressable only here (OV2640 PWDN).
    const bool ok = SetDigitalUnlocked(2, power_down);
    xSemaphoreGive(mutex_);
    return ok;
}

bool Pca9685::SetAllOff() {
    if (!ready_) {
        return false;
    }

    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    bool all_ok = true;
    const uint8_t full_off[4] = {0, 0, 0, kFullOnOffBit};
    for (uint8_t ch = 0; ch <= 15; ++ch) {
        if (ch == 2) {
            continue;
        }
        if (!WriteRegs(kRegLed0OnL + 4 * ch, full_off, 4)) {
            all_ok = false;
        }
    }

    xSemaphoreGive(mutex_);
    return all_ok;
}
