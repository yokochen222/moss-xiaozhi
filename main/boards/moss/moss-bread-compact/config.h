#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/uart.h>

/*
 * DIY bread-compact MOSS (INMP441 + MAX98357A simplex I2S).
 * No ES8311/ES7210, so no device AEC and no realtime barge-in.
 * Do not include moss_shared_audio.h (that gain is for ES7210).
 */
#define AUDIO_INPUT_SAMPLE_RATE 16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_I2S_METHOD_SIMPLEX 1

#define AUDIO_I2S_MIC_GPIO_WS GPIO_NUM_6
#define AUDIO_I2S_MIC_GPIO_SCK GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN GPIO_NUM_4

#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#define BUILTIN_LED_GPIO GPIO_NUM_48
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define TOUCH_BUTTON_GPIO GPIO_NUM_47

/*
 * SSD1306 128x64 OLED on I2C port 0.
 * Source bread-compact-wifi: SDA=1 SCL=2 (codec I2C pins on other moss boards).
 */
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true
#define DISPLAY_SWAP_XY false
#define DISPLAY_SDA_PIN GPIO_NUM_1
#define DISPLAY_SCL_PIN GPIO_NUM_2

#define MOSS_MCP_PERIPHERALS_ENABLE 1

#define MOSS_LAMP_BAR_PIN0 GPIO_NUM_8
#define MOSS_LAMP_BAR_PIN1 GPIO_NUM_3
#define MOSS_LAMP_BAR_PIN2 GPIO_NUM_9
#define MOSS_LAMP_BAR_PIN3 GPIO_NUM_10
#define MOSS_LAMP_BAR_PIN4 GPIO_NUM_11
#define MOSS_LAMP_EYE_PIN GPIO_NUM_12
#define MOSS_IR_UART_TX_PIN GPIO_NUM_17
#define MOSS_IR_UART_RX_PIN GPIO_NUM_18
#define MOSS_IR_UART_PORT UART_NUM_2

#endif  // _BOARD_CONFIG_H_
