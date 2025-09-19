#include "mcp_tools.h"
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "LampEyeTool"
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_DUTY_RES LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY 5000

namespace mcp_tools {

class LampEyeTool : public McpTool {
private:
#ifdef CONFIG_IDF_TARGET_ESP32
    gpio_num_t gpio_num_ = GPIO_NUM_21;
#else
    gpio_num_t gpio_num_ = GPIO_NUM_21;
#endif
    bool power_ = false;
    bool breathing_ = false;
    bool pause_ = false;
    TaskHandle_t breathing_task_handle_ = nullptr; // 新增：用于管理任务句柄
    SemaphoreHandle_t pwm_mutex_; // 新增：防止PWM并发访问的互斥锁

    LampEyeTool();
    ~LampEyeTool() = default;
    LampEyeTool(const LampEyeTool&) = delete;
    LampEyeTool& operator=(const LampEyeTool&) = delete;

    void InitializeGpio();
    void SetDuty(int duty); // 封装设置LED亮度的方法
    static void BreathingTask(void* arg);

public:
    static LampEyeTool& GetInstance() {
        static LampEyeTool instance;
        return instance;
    }
    void Register() override;
};

LampEyeTool::LampEyeTool() : McpTool("self.lamp_eye.control", "控制MOSS的眼部灯光") {
    pwm_mutex_ = xSemaphoreCreateMutex(); // 初始化互斥锁
    InitializeGpio();
}

void LampEyeTool::InitializeGpio() {
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

// 封装的设置 duty 方法，使用互斥锁保护
void LampEyeTool::SetDuty(int duty) {
    if (xSemaphoreTake(pwm_mutex_, portMAX_DELAY)) {
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
        xSemaphoreGive(pwm_mutex_);
    }
}

void LampEyeTool::BreathingTask(void* arg) {
    LampEyeTool* instance = static_cast<LampEyeTool*>(arg);
    int direction = 1;
    int duty = 0;

    while (instance->breathing_) {
        if (instance->pause_) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        duty += direction * 100;
        if (duty >= ((1 << LEDC_DUTY_RES) - 1)) {
            duty = (1 << LEDC_DUTY_RES) - 1;
            direction = -1;
        } else if (duty <= 0) {
            duty = 0;
            direction = 1;
        }

        instance->SetDuty(duty);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    vTaskDelete(nullptr);
}

void LampEyeTool::Register() {
    ESP_LOGI(TAG, "注册眼部灯光控制工具");
    McpServer::GetInstance().AddTool(
        name(),
        "MOSS设备眼部灯光控制工具\n"
        "使用说明：\n"
        "- action='turn_on'：开启眼部灯光常亮模式\n"
        "- action='turn_off'：关闭眼部灯光常亮模式\n"
        "- action='start_breathing'：开启呼吸灯光模式\n"
        "- action='pause_breathing'：暂停呼吸灯光效果\n"
        "- action='resume_breathing'：恢复呼吸灯光效果\n"
        "- action='stop_breathing'：关闭呼吸灯光效果\n"
        "- action='get_status'：获取呼吸灯当前状态信息\n",
        PropertyList({
            Property("action", kPropertyTypeString),
            Property("power", kPropertyTypeBoolean, false)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            if (action == "turn_on") {
                power_ = true;
                SetDuty((1 << LEDC_DUTY_RES) - 1);
                return "眼部灯光已开启";
            } else if (action == "turn_off") {
                power_ = false;
                SetDuty(0);
                return "眼部灯光已关闭";
            } else if (action == "start_breathing") {
                if (!breathing_) {
                    breathing_ = true;
                    pause_ = false;
                    xTaskCreate(BreathingTask, "BreathingTask", 2048, this, 5, &breathing_task_handle_);
                }
                return "开始呼吸灯光效果";
            } else if (action == "pause_breathing") {
                pause_ = true;
                return "暂停呼吸灯光效果";
            } else if (action == "resume_breathing") {
                pause_ = false;
                return "恢复呼吸灯光效果";
            } else if (action == "stop_breathing") {
                if (breathing_) {
                    breathing_ = false;

                    if (breathing_task_handle_) {
                        vTaskDelete(breathing_task_handle_);
                        breathing_task_handle_ = nullptr;
                    }

                    SetDuty(0);
                    power_ = false;
                    pause_ = false;
                }
                return "关闭呼吸灯光效果";
            } else if (action == "get_status") {
                std::string status = "眼部灯光状态:\n";
                status += "电源: " + std::string(power_ ? "开启" : "关闭") + "\n";
                status += "呼吸效果: " + std::string(breathing_ ? "运行中" : "停止") + "\n";
                if (breathing_) {
                    status += "呼吸状态: " + std::string(pause_ ? "暂停" : "运行") + "\n";
                }
                return status;
            } else {
                return "未知动作: " + action + "\n支持的动作: turn_on, turn_off, start_breathing, pause_breathing, resume_breathing, stop_breathing, get_status";
            }
        }
    );
}

} // namespace mcp_tools

static auto& g_lamp_eye_tool_instance = mcp_tools::LampEyeTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_lamp_eye_tool_instance);