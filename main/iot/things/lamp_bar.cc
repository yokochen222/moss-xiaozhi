#include "iot/thing.h"
#include "board.h"
#include "audio_codec.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "LampBar"
#define LED_COUNT 5
const gpio_num_t led_pins[LED_COUNT] = {GPIO_NUM_8, GPIO_NUM_3, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11};

namespace iot {

class LampBar : public Thing {
private:
    bool power_ = false;
    bool flowing_ = false;
    TaskHandle_t flow_task_ = nullptr;

    void InitializeGpio() {
        for(int i = 0; i < LED_COUNT; i++) {
            gpio_config_t config = {
                .pin_bit_mask = (1ULL << led_pins[i]),
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            ESP_ERROR_CHECK(gpio_config(&config));
            gpio_set_level(led_pins[i], 0);
        }
    }

    static void FlowTask(void* arg) {
        LampBar* instance = static_cast<LampBar*>(arg);
        while(instance->flowing_) {
            gpio_set_level(led_pins[0], 1);
            gpio_set_level(led_pins[1], 0);
            gpio_set_level(led_pins[2], 0);
            gpio_set_level(led_pins[3], 0);
            gpio_set_level(led_pins[4], 1);
            vTaskDelay(pdMS_TO_TICKS(110));
            gpio_set_level(led_pins[0], 0);
            gpio_set_level(led_pins[1], 1);
            gpio_set_level(led_pins[2], 0);
            gpio_set_level(led_pins[3], 1);
            gpio_set_level(led_pins[4], 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(led_pins[0], 0);
            gpio_set_level(led_pins[1], 1);
            gpio_set_level(led_pins[2], 1);
            gpio_set_level(led_pins[3], 0);
            gpio_set_level(led_pins[4], 0);
            vTaskDelay(pdMS_TO_TICKS(90));
            gpio_set_level(led_pins[0], 1);
            gpio_set_level(led_pins[1], 0);
            gpio_set_level(led_pins[2], 1);
            gpio_set_level(led_pins[3], 1);
            gpio_set_level(led_pins[4], 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(led_pins[0], 0);
            gpio_set_level(led_pins[1], 0);
            gpio_set_level(led_pins[2], 1);
            gpio_set_level(led_pins[3], 0);
            gpio_set_level(led_pins[4], 1);
            vTaskDelay(pdMS_TO_TICKS(130));
        }
        for(int i = 0; i < LED_COUNT; i++) {
            gpio_set_level(led_pins[i], 0);
        }
        vTaskDelete(NULL);
    }

public:
    LampBar() : Thing("LampBar", "流水灯"), power_(false), flowing_(false) {
        InitializeGpio();

        properties_.AddBooleanProperty("power", "流水灯是否打开", [this]() -> bool {
            return power_;
        });

        methods_.AddMethod("TurnOff", "关闭流水灯效果", ParameterList(), [this](const ParameterList& parameters) {
            for(int i = 0; i < LED_COUNT; i++) {
                gpio_set_level(led_pins[i], 0);
            }
            flowing_ = false;
            power_ = false;
        });

        methods_.AddMethod("StartFlow", "打开流水灯效果", ParameterList(), [this](const ParameterList& parameters) {
            if(!flowing_) {
                flowing_ = true;
                power_ = true;
                xTaskCreate(FlowTask, "FlowTask", 2048, this, 5, &flow_task_);
            }
        });
    }

    ~LampBar() {
        flowing_ = false;
    }
};

} // namespace iot

DECLARE_THING(LampBar);
 