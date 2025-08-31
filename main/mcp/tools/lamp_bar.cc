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
#define SER_PIN GPIO_NUM_9   // 数据引脚
#define RCK_PIN GPIO_NUM_10  // 锁存引脚
#define SCK_PIN GPIO_NUM_11  // 时钟引脚

namespace mcp_tools {

class LampBarTool : public McpTool {
private:
    bool power_ = false;
    bool flowing_ = false;
    bool eye_power_ = false;
    bool eye_breathing_ = false;
    bool eye_pause_ = false;
    TaskHandle_t flow_task_ = nullptr;
    TaskHandle_t eye_task_ = nullptr;
    ShiftRegister74HC595* shift_register_;
    
    LampBarTool();
    ~LampBarTool();
    LampBarTool(const LampBarTool&) = delete;
    LampBarTool& operator=(const LampBarTool&) = delete;

    void InitializeShiftRegister();
    static void FlowTask(void* arg);
    static void EyeBreathingTask(void* arg);
    void UpdateEyeLED(bool state);

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
    if (eye_task_ != nullptr) {
        eye_breathing_ = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        eye_task_ = nullptr;
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

void LampBarTool::UpdateEyeLED(bool state) {
    // 获取当前74HC595的输出状态
    uint8_t current = shift_register_->GetCurrentData();
    
    if (state) {
        // 点亮眼部LED (Q5，位5)，保持其他位不变
        current |= (1 << 5);
    } else {
        // 关闭眼部LED (Q5，位5)，保持其他位不变
        current &= ~(1 << 5);
    }
    
    // 更新输出，保持流水灯状态
    shift_register_->SetOutputs(current);
}

void LampBarTool::EyeBreathingTask(void* arg) {
    LampBarTool* instance = static_cast<LampBarTool*>(arg);
    float duty = 0.0f;  // 使用浮点数实现更平滑的渐变
    float direction = 1.0f;
    const float max_duty = 100.0f;
    const float step = 0.8f;  // 大幅减小步长，让呼吸更慢
    
    ESP_LOGI(TAG, "眼部呼吸灯光任务启动");
    
    while (instance->eye_breathing_) {
        if (instance->eye_pause_) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        duty += direction * step;
        if (duty >= max_duty) {
            duty = max_duty;
            direction = -1.0f;
        } else if (duty <= 0.0f) {
            duty = 0.0f;
            direction = 1.0f;
        }
        
        // 添加日志，帮助调试
        if (static_cast<int>(duty) % 30 == 0) {  // 每30步记录一次
            ESP_LOGD(TAG, "呼吸亮度: %.1f, 方向: %.1f", duty, direction);
        }
        
        // 使用简化的算法实现缓慢的呼吸效果
        if (duty < 3.0f) {  // 当亮度很低时，完全关闭LED
            instance->UpdateEyeLED(false);
            vTaskDelay(pdMS_TO_TICKS(80));  // 保持关闭状态更长时间
        } else if (duty > 97.0f) {  // 当亮度很高时，完全点亮LED
            instance->UpdateEyeLED(true);
            vTaskDelay(pdMS_TO_TICKS(80));  // 保持点亮状态更长时间
        } else {
            // 中间区域使用简单的开关控制，但变化很慢
            if (duty > 50.0f) {
                instance->UpdateEyeLED(true);
            } else {
                instance->UpdateEyeLED(false);
            }
            vTaskDelay(pdMS_TO_TICKS(60));  // 大幅增加延时，让呼吸更慢
        }
        
        // 给其他任务一些运行时间
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    
    ESP_LOGI(TAG, "眼部呼吸灯光任务停止");
    // 停止时关闭眼部LED
    instance->UpdateEyeLED(false);
    vTaskDelete(NULL);
}

void LampBarTool::Register() {
    McpServer::GetInstance().AddTool(
        name(),
        "MOSS设备灯光控制工具（流水灯 + 眼部灯光）\n"
        "使用说明：\n"
        "流水灯控制：\n"
        "- action='start_flow'：开启流水灯效果\n"
        "- action='stop_flow'：关闭流水灯效果\n"
        "眼部灯光控制：\n"
        "- action='eye_on'：开启眼部灯光常亮模式\n"
        "- action='eye_off'：关闭眼部灯光常亮模式\n"
        "- action='eye_breathing_start'：开启眼部呼吸灯光模式\n"
        "- action='eye_breathing_smooth'：开启平滑眼部呼吸灯光模式\n"
        "- action='eye_breathing_pause'：暂停眼部呼吸灯光效果\n"
        "- action='eye_breathing_resume'：恢复眼部呼吸灯光效果\n"
        "- action='eye_breathing_stop'：关闭眼部呼吸灯光效果\n"
        "调试功能：\n"
        "- action='test_eye'：测试眼部LED是否工作\n"
        "系统控制：\n"
        "- action='get_status'：获取所有灯光当前状态信息\n"
        "- action='reset_driver'：重置74HC595驱动\n"
        "- action='force_restart'：强制重启所有灯光系统\n"
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
                        } else if (action == "eye_on") {
                eye_power_ = true;
                eye_breathing_ = false;
                if (eye_task_ != nullptr) {
                    eye_breathing_ = false;
                    vTaskDelay(pdMS_TO_TICKS(100));
                    eye_task_ = nullptr;
                }
                UpdateEyeLED(true);
                return "眼部灯光已开启";
            } else if (action == "eye_off") {
                eye_power_ = false;
                UpdateEyeLED(false);
                return "眼部灯光已关闭";
            } else if (action == "eye_breathing_start") {
                if (!eye_breathing_) {
                    eye_breathing_ = true;
                    eye_pause_ = false;
                    eye_power_ = true;
                    ESP_LOGI(TAG, "创建眼部呼吸灯光任务");
                    xTaskCreate(EyeBreathingTask, "EyeBreathingTask", 2048, this, 3, &eye_task_);
                    ESP_LOGI(TAG, "眼部呼吸灯光任务创建成功，任务句柄: %p", eye_task_);
                } else {
                    ESP_LOGW(TAG, "眼部呼吸灯光任务已在运行中");
                }
                return "开始眼部呼吸灯光效果";
            } else if (action == "eye_breathing_smooth") {
                if (!eye_breathing_) {
                    eye_breathing_ = true;
                    eye_pause_ = false;
                    eye_power_ = true;
                    ESP_LOGI(TAG, "创建平滑眼部呼吸灯光任务");
                    xTaskCreate(EyeBreathingTask, "EyeBreathingTask", 2048, this, 3, &eye_task_);
                    ESP_LOGI(TAG, "平滑眼部呼吸灯光任务创建成功，任务句柄: %p", eye_task_);
                } else {
                    ESP_LOGW(TAG, "眼部呼吸灯光任务已在运行中");
                }
                return "开始平滑眼部呼吸灯光效果";
            } else if (action == "eye_breathing_pause") {
                eye_pause_ = true;
                return "暂停眼部呼吸灯光效果";
            } else if (action == "eye_breathing_resume") {
                eye_pause_ = false;
                return "恢复眼部呼吸灯光效果";
            } else if (action == "eye_breathing_stop") {
                eye_breathing_ = false;
                if (eye_task_ != nullptr) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    eye_task_ = nullptr;
                }
                UpdateEyeLED(false);
                eye_power_ = false;
                return "关闭眼部呼吸灯光效果";
            } else if (action == "test_eye") {
                ESP_LOGI(TAG, "测试眼部LED功能");
                // 测试眼部LED开关
                ESP_LOGI(TAG, "开启眼部LED (Q5)");
                UpdateEyeLED(true);
                vTaskDelay(pdMS_TO_TICKS(1000));
                
                ESP_LOGI(TAG, "关闭眼部LED (Q5)");
                UpdateEyeLED(false);
                vTaskDelay(pdMS_TO_TICKS(1000));
                
                ESP_LOGI(TAG, "再次开启眼部LED (Q5)");
                UpdateEyeLED(true);
                vTaskDelay(pdMS_TO_TICKS(1000));
                
                ESP_LOGI(TAG, "最终关闭眼部LED (Q5)");
                UpdateEyeLED(false);
                
                return "眼部LED测试完成，请观察Q5引脚是否正常开关";
            } else if (action == "get_status") {
                std::string status = "MOSS灯光系统状态:\n";
                status += "流水灯:\n";
                status += "  电源: " + std::string(power_ ? "开启" : "关闭") + "\n";
                status += "  流水效果: " + std::string(flowing_ ? "运行中" : "停止") + "\n";
                status += "  任务句柄: " + std::string(flow_task_ != nullptr ? "有效" : "无效") + "\n";
                status += "眼部灯光:\n";
                status += "  电源: " + std::string(eye_power_ ? "开启" : "关闭") + "\n";
                status += "  呼吸效果: " + std::string(eye_breathing_ ? "运行中" : "停止") + "\n";
                if (eye_breathing_) {
                    status += "  呼吸状态: " + std::string(eye_pause_ ? "暂停" : "运行") + "\n";
                }
                status += "  任务句柄: " + std::string(eye_task_ != nullptr ? "有效" : "无效") + "\n";
                status += "硬件: 使用74HC595移位寄存器控制 (Q0-Q4: 流水灯, Q5: 眼部灯光)";
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
                
                if (eye_breathing_) {
                    eye_breathing_ = false;
                    eye_power_ = false;
                    
                    if (eye_task_ != nullptr) {
                        eye_task_ = nullptr;
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
                return "未知动作: " + action + "\n支持的动作: start_flow, stop_flow, eye_on, eye_off, eye_breathing_start, eye_breathing_smooth, eye_breathing_pause, eye_breathing_resume, eye_breathing_stop, test_eye, get_status, reset_driver, force_restart";
            }
        }
    );
}

} // namespace mcp_tools

static auto& g_lamp_bar_tool_instance = mcp_tools::LampBarTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_lamp_bar_tool_instance);
