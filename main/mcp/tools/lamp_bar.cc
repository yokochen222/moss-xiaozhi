#include "mcp_tools.h"
#include "board.h"
#include "audio_codec.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "LampBarTool"
#define LED_COUNT 5
const gpio_num_t led_pins[LED_COUNT] = {GPIO_NUM_8, GPIO_NUM_3, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11};

namespace mcp_tools {

class LampBarTool : public McpTool {
private:
    bool power_ = false;
    bool flowing_ = false;
    TaskHandle_t flow_task_ = nullptr;
    LampBarTool();
    ~LampBarTool() = default;
    LampBarTool(const LampBarTool&) = delete;
    LampBarTool& operator=(const LampBarTool&) = delete;

    void InitializeGpio();
    static void FlowTask(void* arg);

public:
    static LampBarTool& GetInstance() {
        static LampBarTool instance;
        return instance;
    }
    void Register() override;
};

LampBarTool::LampBarTool() : McpTool("self.lamp_bar.control", "控制MOSS的流水灯效果") {
    InitializeGpio();
}

void LampBarTool::InitializeGpio() {
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

void LampBarTool::FlowTask(void* arg) {
    LampBarTool* instance = static_cast<LampBarTool*>(arg);
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

void LampBarTool::Register() {
    ESP_LOGI(TAG, "注册流水灯控制工具");
    McpServer::GetInstance().AddTool(
        name(),
        "MOSS设备流水灯控制工具\n"
        "使用说明：\n"
        "- action='start_flow'：开启流水灯效果\n"
        "- action='stop_flow'：关闭流水灯效果\n"
        "- action='get_status'：获取流水灯当前状态信息\n"
        ,
        PropertyList({
            Property("action", kPropertyTypeString)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            if (action == "start_flow") {
                if(!flowing_) {
                    flowing_ = true;
                    power_ = true;
                    xTaskCreate(FlowTask, "FlowTask", 2048, this, 5, &flow_task_);
                }
                return "流水灯效果已开启";
            } else if (action == "stop_flow") {
                for(int i = 0; i < LED_COUNT; i++) {
                    gpio_set_level(led_pins[i], 0);
                }
                flowing_ = false;
                power_ = false;
                return "流水灯效果已关闭";
            } else if (action == "get_status") {
                std::string status = "流水灯状态:\n";
                status += "电源: " + std::string(power_ ? "开启" : "关闭") + "\n";
                status += "流水效果: " + std::string(flowing_ ? "运行中" : "停止") + "\n";
                status += "LED数量: " + std::to_string(LED_COUNT) + "\n";
                return status;
            } else {
                return "未知动作: " + action + "\n支持的动作: start_flow, stop_flow, get_status";
            }
        }
    );
}

} // namespace mcp_tools

static auto& g_lamp_bar_tool_instance = mcp_tools::LampBarTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_lamp_bar_tool_instance); 