#include "iot/thing.h"
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"  // 添加FreeRTOS头文件
#include "freertos/task.h"     // 添加任务管理头文件

#define TAG "Lamp_EYE"
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_DUTY_RES LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY 5000

namespace iot {

class LampEye : public Thing {
private:
#ifdef CONFIG_IDF_TARGET_ESP32
    gpio_num_t gpio_num_ = GPIO_NUM_12;
#else
    gpio_num_t gpio_num_ = GPIO_NUM_12;
#endif
    bool power_ = false;
    bool breathing_ = false;
    bool pause_ = false;

    void InitializeGpio() {
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_MODE,
            .duty_resolution = LEDC_DUTY_RES,
            .timer_num = LEDC_TIMER,
            .freq_hz = LEDC_FREQUENCY,
            .clk_cfg = LEDC_AUTO_CLK
        };
        ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

        ledc_channel_config_t ledc_channel = {
            .gpio_num = gpio_num_,
            .speed_mode = LEDC_MODE,
            .channel = LEDC_CHANNEL,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER,
            .duty = 0,
            .hpoint = 0
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    }

    static void BreathingTask(void* arg) {
        LampEye* instance = static_cast<LampEye*>(arg);
        int direction = 1;
        int duty = 0;

        while (instance->breathing_) {
            if (instance->pause_) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            duty += direction * 100;
            if (duty >= (1 << LEDC_DUTY_RES) - 1) {
                duty = (1 << LEDC_DUTY_RES) - 1;
                direction = -1;
            } else if (duty <= 0) {
                duty = 0;
                direction = 1;
            }

            ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
            ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelete(NULL);
    }

public:
    LampEye() : Thing("LampEye", "MOSS的眼部灯光"), power_(false), breathing_(false), pause_(false) {
        InitializeGpio();

        properties_.AddBooleanProperty("powerEye", "灯是否打开", [this]() -> bool {
            return power_;
        });

        methods_.AddMethod("TurnLampEyeOn", "打开MOSS的眼部灯光", ParameterList(), [this](const ParameterList& parameters) {
            power_ = true;
            ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, (1 << LEDC_DUTY_RES) - 1);
            ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
        });

        methods_.AddMethod("TurnLampEyeOff", "关闭MOSS的眼部灯光", ParameterList(), [this](const ParameterList& parameters) {
            power_ = false;
            ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
            ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
        });

        methods_.AddMethod("StartBreathing", "开始MOSS的眼部呼吸灯光效果", ParameterList(), [this](const ParameterList& parameters) {
            if (!breathing_) {
                breathing_ = true;
                pause_ = false;
                xTaskCreate(BreathingTask, "BreathingTask", 2048, this, 5, NULL);
            }
        });

        methods_.AddMethod("PauseBreathing", "暂停MOSS的眼部呼吸灯光效果", ParameterList(), [this](const ParameterList& parameters) {
            pause_ = true;
        });

        methods_.AddMethod("ResumeBreathing", "恢复MOSS的眼部呼吸灯光效果", ParameterList(), [this](const ParameterList& parameters) {
            pause_ = false;
        });

        methods_.AddMethod("StopBreathing", "停止MOSS的眼部呼吸灯光效果", ParameterList(), [this](const ParameterList& parameters) {
            breathing_ = false;
            // 添加延时确保任务已退出
            vTaskDelay(pdMS_TO_TICKS(50));
            // 将PWM输出设置为0
            ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
            ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
            power_ = false;
        });
    }
};

} // namespace iot

DECLARE_THING(LampEye);
