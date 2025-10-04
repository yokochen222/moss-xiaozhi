#ifndef SDCARD_CONFIG_H
#define SDCARD_CONFIG_H

#include <driver/gpio.h>

/**
 * @brief SD卡引脚配置
 * 
 * 根据您的硬件连接选择合适的配置：
 * 
 * 配置1: 您当前的连接 (CS=48, SCK=39, MOSI=42, MISO=46)
 * 配置2: ESP32-S3标准SPI引脚 (推荐，兼容性更好)
 * 配置3: 备用引脚配置
 */

// 选择配置 (取消注释您要使用的配置)
#define SDCARD_CONFIG_CUSTOM    // 您当前的引脚配置
// #define SDCARD_CONFIG_STANDARD  // ESP32-S3标准SPI引脚
// #define SDCARD_CONFIG_ALTERNATE // 备用引脚配置

#ifdef SDCARD_CONFIG_CUSTOM
// 配置1: 您当前的连接
#define SDCARD_CS_PIN   GPIO_NUM_48
#define SDCARD_SCK_PIN  GPIO_NUM_39
#define SDCARD_MOSI_PIN GPIO_NUM_42
#define SDCARD_MISO_PIN GPIO_NUM_46
#define SDCARD_CONFIG_NAME "Custom (您的配置)"

#elif defined(SDCARD_CONFIG_STANDARD)
// 配置2: ESP32-S3标准SPI引脚 (推荐)
#define SDCARD_CS_PIN   GPIO_NUM_10
#define SDCARD_SCK_PIN  GPIO_NUM_12
#define SDCARD_MOSI_PIN GPIO_NUM_11
#define SDCARD_MISO_PIN GPIO_NUM_13
#define SDCARD_CONFIG_NAME "Standard SPI"

#elif defined(SDCARD_CONFIG_ALTERNATE)
// 配置3: 备用引脚配置
#define SDCARD_CS_PIN   GPIO_NUM_21
#define SDCARD_SCK_PIN  GPIO_NUM_20
#define SDCARD_MOSI_PIN GPIO_NUM_19
#define SDCARD_MISO_PIN GPIO_NUM_18
#define SDCARD_CONFIG_NAME "Alternate"

#endif

// 电压兼容性设置
#define SDCARD_USE_3V3_LOGIC 1  // 如果您的模块支持3.3V逻辑电平，设置为1
#define SDCARD_USE_5V_LOGIC  0  // 如果您的模块需要5V逻辑电平，设置为1

// SPI频率设置 (降低频率可能提高兼容性)
#define SDCARD_SPI_FREQUENCY_1MHZ   1000000   // 1MHz (最稳定)
#define SDCARD_SPI_FREQUENCY_5MHZ   5000000   // 5MHz
#define SDCARD_SPI_FREQUENCY_10MHZ  10000000  // 10MHz (默认)

// 选择SPI频率 - 模拟Arduino UNO的SPI_HALF_SPEED (约250kHz)
#define SDCARD_SPI_FREQUENCY 250000

#endif // SDCARD_CONFIG_H
