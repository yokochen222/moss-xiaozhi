#include "device/lamp_eye.h"
#include <esp_log.h>
#include "mcp_tools.h"

#define TAG "IrisResonatorTool"

namespace mcp_tools {

class LampEyeTool : public McpTool {
private:
    LampEyeDevice& lamp_eye_device_;

public:
    static LampEyeTool& GetInstance() {
        static LampEyeTool instance;
        return instance;
    }
    LampEyeTool()
        : McpTool("self.iris_resonator.control", "控制MOSS虹膜谐振灯（眼部脉动/常亮）"),
          lamp_eye_device_(LampEyeDevice::GetInstance()) {}
    void Register() override;
};

void LampEyeTool::Register() {
    ESP_LOGI(TAG, "注册虹膜谐振灯控制工具");
    McpServer::GetInstance().AddTool(
        name(),
        "MOSS 虹膜谐振灯控制工具\n"
        "硬件：PCA9685 通道 ch6（PWM 谐振/常亮）\n"
        "使用说明：\n"
        "- action='turn_on'：虹膜谐振灯常亮\n"
        "- action='turn_off'：关闭虹膜谐振灯\n"
        "- action='start_breathing'：启动谐振脉动\n"
        "- action='pause_breathing'：暂停谐振脉动\n"
        "- action='resume_breathing'：恢复谐振脉动\n"
        "- action='stop_breathing'：停止谐振脉动\n"
        "- action='set_brightness'：设置亮度 1-40（默认 20，脉动峰值随亮度缩放）\n"
        "- action='get_status'：查询虹膜谐振灯状态\n",
        PropertyList({Property("action", kPropertyTypeString),
                      Property("power", kPropertyTypeBoolean, false),
                      Property("brightness", kPropertyTypeInteger, 20, 1, 40)}),
        [this](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            if (action == "turn_on") {
                if (lamp_eye_device_.TurnOn()) {
                    return "虹膜谐振灯已常亮，亮度 " +
                           std::to_string(lamp_eye_device_.GetBrightness()) + "%";
                } else {
                    return "开启虹膜谐振灯失败";
                }
            } else if (action == "turn_off") {
                if (lamp_eye_device_.TurnOff()) {
                    return "虹膜谐振灯已关闭";
                } else {
                    return "关闭虹膜谐振灯失败";
                }
            } else if (action == "start_breathing") {
                if (lamp_eye_device_.StartBreathing()) {
                    return "虹膜谐振脉动已启动，峰值亮度 " +
                           std::to_string(lamp_eye_device_.GetBrightness()) + "%";
                } else {
                    return "启动虹膜谐振脉动失败";
                }
            } else if (action == "pause_breathing") {
                if (lamp_eye_device_.PauseBreathing()) {
                    return "虹膜谐振脉动已暂停";
                } else {
                    return "暂停虹膜谐振脉动失败";
                }
            } else if (action == "resume_breathing") {
                if (lamp_eye_device_.ResumeBreathing()) {
                    return "虹膜谐振脉动已恢复";
                } else {
                    return "恢复虹膜谐振脉动失败";
                }
            } else if (action == "stop_breathing") {
                if (lamp_eye_device_.StopBreathing()) {
                    return "虹膜谐振脉动已停止";
                } else {
                    return "停止虹膜谐振脉动失败";
                }
            } else if (action == "set_brightness") {
                auto brightness = properties["brightness"].value<int>();
                if (lamp_eye_device_.SetBrightness(brightness)) {
                    return "虹膜谐振灯亮度已设为 " +
                           std::to_string(lamp_eye_device_.GetBrightness()) + "%（上限 40%）";
                }
                return "设置虹膜谐振灯亮度失败";
            } else if (action == "get_status") {
                std::string status = "虹膜谐振灯状态:\n";
                status += "硬件: PCA9685 ch6 (PWM)\n";
                status +=
                    "电源: " + std::string(lamp_eye_device_.IsPowered() ? "开启" : "关闭") + "\n";
                status +=
                    "谐振脉动: " + std::string(lamp_eye_device_.IsBreathing() ? "运行中" : "停止") +
                    "\n";
                status +=
                    "亮度: " + std::to_string(lamp_eye_device_.GetBrightness()) + "% (上限 40%)\n";
                if (lamp_eye_device_.IsBreathing()) {
                    status +=
                        "脉动状态: " + std::string(lamp_eye_device_.IsPaused() ? "暂停" : "运行") +
                        "\n";
                }
                return status;
            } else {
                return "未知动作: " + action +
                       "\n支持的动作: turn_on, turn_off, start_breathing, pause_breathing, "
                       "resume_breathing, stop_breathing, set_brightness, get_status";
            }
        });
}

}  // namespace mcp_tools

static auto& g_lamp_eye_tool_instance = mcp_tools::LampEyeTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_lamp_eye_tool_instance);
