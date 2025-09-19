#include "mcp_tools.h"
#include "board.h"
#include "audio_codec.h"
#include "../utils/74hc595_driver.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "LampBarTool"
#define LED_COUNT 5

// 74HC595引脚定义
#define SER_PIN GPIO_NUM_3   // 数据引脚
#define RCK_PIN GPIO_NUM_4  // 锁存引脚
#define SCK_PIN GPIO_NUM_5  // 时钟引脚

namespace mcp_tools {

class LampBarTool : public McpTool {
private:
    bool power_ = false;
    bool flowing_ = false;
    TaskHandle_t flow_task_ = nullptr;
    ShiftRegister74HC595* shift_register_;
    
    LampBarTool();
    ~LampBarTool();
    LampBarTool(const LampBarTool&) = delete;
    LampBarTool& operator=(const LampBarTool&) = delete;

    void InitializeShiftRegister();
    static void FlowTask(void* arg);

public:
    static LampBarTool& GetInstance() {
        static LampBarTool instance;
        return instance;
    }
    void Register() override;
};

LampBarTool::LampBarTool() : McpTool("self.lamp_bar.control", "控制MOSS的流水灯效果") {
    InitializeShiftRegister();
}

LampBarTool::~LampBarTool() {
    // 停止所有任务
    if (flow_task_ != nullptr) {
        flowing_ = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        flow_task_ = nullptr;
    }
    
    if (shift_register_) {
        delete shift_register_;
    }
}

void LampBarTool::InitializeShiftRegister() {
    // 创建74HC595驱动实例
    shift_register_ = new ShiftRegister74HC595(SER_PIN, RCK_PIN, SCK_PIN);
    shift_register_->Initialize();
    
    // 初始化时关闭所有LED
    shift_register_->ClearAll();
}

void LampBarTool::FlowTask(void* arg) {
    LampBarTool* instance = static_cast<LampBarTool*>(arg);
    
    while(instance->flowing_) {
        // 流水灯效果1: 两端亮，中间暗
        uint8_t pattern1 = 0b10001;  // LED0和LED4亮
        instance->shift_register_->SetOutputs(pattern1);
        vTaskDelay(pdMS_TO_TICKS(110));
        
        // 流水灯效果2: 交替亮
        uint8_t pattern2 = 0b01010;  // LED1和LED3亮
        instance->shift_register_->SetOutputs(pattern2);
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // 流水灯效果3: 中间亮
        uint8_t pattern3 = 0b00110;  // LED2和LED3亮
        instance->shift_register_->SetOutputs(pattern3);
        vTaskDelay(pdMS_TO_TICKS(90));
        
        // 流水灯效果4: 交叉亮
        uint8_t pattern4 = 0b10110;  // LED0, LED2, LED3亮
        instance->shift_register_->SetOutputs(pattern4);
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // 流水灯效果5: 两端亮
        uint8_t pattern5 = 0b10001;  // LED0和LED4亮
        instance->shift_register_->SetOutputs(pattern5);
        vTaskDelay(pdMS_TO_TICKS(130));
    }
    
    // 停止时关闭所有LED
    instance->shift_register_->ClearAll();
    vTaskDelete(NULL);
}



void LampBarTool::Register() {
    McpServer::GetInstance().AddTool(
        name(),
        "MOSS设备灯光控制工具（流水灯）\n"
        "使用说明：\n"
        "流水灯控制：\n"
        "- action='start_flow'：开启流水灯效果\n"
        "- action='stop_flow'：关闭流水灯效果\n"
        "系统控制：\n"
        "- action='get_status'：获取流水灯当前状态信息\n"
        "- action='reset_driver'：重置74HC595驱动\n"
        "- action='force_restart'：强制重启流水灯系统\n"
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
                if (flowing_) {
                    flowing_ = false;
                    power_ = false;
                    
                    // 等待任务结束
                    if (flow_task_ != nullptr) {
                        vTaskDelay(pdMS_TO_TICKS(100)); // 给任务一点时间结束
                        flow_task_ = nullptr;
                    }
                    
                    // 关闭所有LED
                    shift_register_->ClearAll();
                }
                return "流水灯效果已关闭";
            } else if (action == "get_status") {
                std::string status = "MOSS灯光系统状态:\n";
                status += "流水灯:\n";
                status += "  电源: " + std::string(power_ ? "开启" : "关闭") + "\n";
                status += "  流水效果: " + std::string(flowing_ ? "运行中" : "停止") + "\n";
                status += "  任务句柄: " + std::string(flow_task_ != nullptr ? "有效" : "无效") + "\n";
                status += "硬件: 使用74HC595移位寄存器控制 (Q0-Q4: 流水灯)";
                return status;
            } else if (action == "reset_driver") {
                shift_register_->Reset();
                return "74HC595驱动已重置";
            } else if (action == "force_restart") {
                // 强制停止当前任务
                if (flowing_) {
                    flowing_ = false;
                    power_ = false;
                    
                    if (flow_task_ != nullptr) {
                        flow_task_ = nullptr;
                    }
                }
                
                
                // 重置驱动
                shift_register_->Reset();
                
                // 关闭所有LED
                shift_register_->ClearAll();
                
                // 等待一下
                vTaskDelay(pdMS_TO_TICKS(500));
                
                return "所有灯光系统已强制重启，所有状态已重置";
            } else {    
                return "未知动作: " + action + "\n支持的动作: start_flow, stop_flow, get_status, reset_driver, force_restart";
            }
        }
    );
}

} // namespace mcp_tools

static auto& g_lamp_bar_tool_instance = mcp_tools::LampBarTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_lamp_bar_tool_instance);
