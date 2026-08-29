#pragma once

#include "sdkconfig.h"

#if CONFIG_BOARD_TYPE_MOSS_OV2640

#include <cstdint>

class LampPanelDevice {
private:
    static constexpr uint8_t CH_PANEL1 = 13;
    static constexpr uint8_t CH_PANEL2 = 14;
    static constexpr uint8_t CH_BOTTOM = 15;
    static constexpr uint16_t MAX_DUTY = 4095;
    static constexpr uint8_t kPwmCeilingPercent = 40;
    static constexpr uint8_t kDefaultBrightnessPercent = 20;
    static constexpr uint8_t kBottomFixedPwmPercent = 10;

    bool panel_led_1_on_;
    bool panel_led_2_on_;
    bool bottom_led_on_;
    uint8_t panel_brightness_;

    static uint8_t ClampBrightness(int percent);
    static uint16_t PercentToDuty(uint8_t percent);
    static uint16_t BottomDuty();
    uint16_t PanelDuty() const;
    bool ApplyOutputs();

public:
    LampPanelDevice();
    ~LampPanelDevice();

    LampPanelDevice(const LampPanelDevice&) = delete;
    LampPanelDevice& operator=(const LampPanelDevice&) = delete;

    bool TurnOffAll();
    bool TurnOnPanelLeds();
    bool TurnOffPanelLeds();
    bool TurnOnBottomLed();
    bool TurnOffBottomLed();
    bool TurnOnAll();

    bool SetBrightness(int percent);
    uint8_t GetBrightness() const { return panel_brightness_; }

    bool IsPanelLed1On() const { return panel_led_1_on_; }
    bool IsPanelLed2On() const { return panel_led_2_on_; }
    bool IsBottomLedOn() const { return bottom_led_on_; }
    bool IsAnyLedOn() const { return panel_led_1_on_ || panel_led_2_on_ || bottom_led_on_; }

    static LampPanelDevice& GetInstance();
};

#else

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>

#include "config.h"

class ShiftRegister74HC595;

class LampPanelDevice {
private:
    static constexpr gpio_num_t SER_PIN = MOSS_LAMP_74HC595_SER_PIN;
    static constexpr gpio_num_t RCK_PIN = MOSS_LAMP_74HC595_RCK_PIN;
    static constexpr gpio_num_t SCK_PIN = MOSS_LAMP_74HC595_SCK_PIN;

    static constexpr uint8_t PANEL_LED_1_BIT = 5;
    static constexpr uint8_t PANEL_LED_2_BIT = 6;
    static constexpr uint8_t BOTTOM_LED_BIT = 7;

    bool panel_led_1_on_;
    bool panel_led_2_on_;
    bool bottom_led_on_;
    ShiftRegister74HC595* shift_register_;

    void InitializeShiftRegister();
    void ApplyOutputs();

public:
    LampPanelDevice();
    ~LampPanelDevice();

    LampPanelDevice(const LampPanelDevice&) = delete;
    LampPanelDevice& operator=(const LampPanelDevice&) = delete;

    bool TurnOffAll();
    bool TurnOnPanelLeds();
    bool TurnOffPanelLeds();
    bool TurnOnBottomLed();
    bool TurnOffBottomLed();
    bool TurnOnAll();

    bool IsPanelLed1On() const { return panel_led_1_on_; }
    bool IsPanelLed2On() const { return panel_led_2_on_; }
    bool IsBottomLedOn() const { return bottom_led_on_; }
    bool IsAnyLedOn() const { return panel_led_1_on_ || panel_led_2_on_ || bottom_led_on_; }

    static LampPanelDevice& GetInstance();
};

#endif
