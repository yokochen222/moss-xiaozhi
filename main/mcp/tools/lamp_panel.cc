#include "device/lamp_panel.h"
#include <esp_log.h>
#include "board.h"
#include "mcp_tools.h"

#define TAG "HullTool"

namespace mcp_tools {

class LampPanelTool : public McpTool {
private:
    LampPanelDevice& lamp_panel_device_;

public:
    static LampPanelTool& GetInstance() {
        static LampPanelTool instance;
        return instance;
    }
    LampPanelTool()
        : McpTool("self.hull.control", "舷灯/锚灯。备注：不需要播报内容，直接执行。"),
          lamp_panel_device_(LampPanelDevice::GetInstance()) {}
    void Register() override;
};

void LampPanelTool::Register() {
    ESP_LOGI(TAG, "注册舷灯/锚灯控制工具");
    McpServer::GetInstance().AddTool(
        name(),
        "舷灯 / 锚灯\n"
        "备注：不需要播报内容，直接执行。\n"
        "硬件：PCA9685 ch13/ch14=舷灯 x2，ch15=锚灯。PWM 调光。\n"
        "使用说明：\n"
        "- action='turn_on_panel'：点亮两盏舷灯\n"
        "- action='turn_on_bottom'：点亮锚灯（固定 10%）\n"
        "- action='turn_on_all'：同时点亮舷灯与锚灯\n"
        "- action='turn_off_panel'：熄灭舷灯\n"
        "- action='turn_off_bottom'：熄灭锚灯\n"
        "- action='turn_off_all'：全部熄灭\n"
        "- action='set_brightness'：设置舷灯亮度 1-40（默认 20，锚灯不可调）\n"
        "- action='get_status'：查询状态\n"
        "\n"
        "舷灯默认 20%，上限 40%；锚灯点亮固定 10%。\n"
        "与流光 (ch8-ch12) 通道独立、互不影响。\n",
        PropertyList({Property("action", kPropertyTypeString),
                      Property("brightness", kPropertyTypeInteger, 20, 1, 40)}),
        [this](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();

            if (action == "turn_on_panel") {
                if (lamp_panel_device_.TurnOnPanelLeds()) {
                    return "舷灯已点亮，亮度 " +
                           std::to_string(lamp_panel_device_.GetBrightness()) + "%";
                }
                return "点亮舷灯失败";
            } else if (action == "turn_on_bottom") {
                if (lamp_panel_device_.TurnOnBottomLed()) {
                    return "锚灯已点亮（固定 10%）";
                }
                return "点亮锚灯失败";
            } else if (action == "turn_on_all") {
                if (lamp_panel_device_.TurnOnAll()) {
                    return "舷灯与锚灯已全部点亮，舷灯 " +
                           std::to_string(lamp_panel_device_.GetBrightness()) + "%，锚灯 10%";
                }
                return "点亮舷灯/锚灯失败";
            } else if (action == "turn_off_panel") {
                if (lamp_panel_device_.TurnOffPanelLeds()) {
                    return "舷灯已熄灭";
                }
                return "熄灭舷灯失败";
            } else if (action == "turn_off_bottom") {
                if (lamp_panel_device_.TurnOffBottomLed()) {
                    return "锚灯已熄灭";
                }
                return "熄灭锚灯失败";
            } else if (action == "turn_off_all") {
                if (lamp_panel_device_.TurnOffAll()) {
                    return "舷灯与锚灯已全部熄灭";
                }
                return "熄灭舷灯/锚灯失败";
            } else if (action == "set_brightness") {
                auto brightness = properties["brightness"].value<int>();
                if (lamp_panel_device_.SetBrightness(brightness)) {
                    return "舷灯亮度已设为 " + std::to_string(lamp_panel_device_.GetBrightness()) +
                           "%（上限 40%）";
                }
                return "设置舷灯亮度失败";
            } else if (action == "get_status") {
                std::string status = "舷灯 / 锚灯状态:\n";
                status += "  舷灯1: " +
                          std::string(lamp_panel_device_.IsPanelLed1On() ? "开启" : "关闭") + "\n";
                status += "  舷灯2: " +
                          std::string(lamp_panel_device_.IsPanelLed2On() ? "开启" : "关闭") + "\n";
                status += "  舷灯亮度: " + std::to_string(lamp_panel_device_.GetBrightness()) +
                          "% (上限 40%)\n";
                status +=
                    "  锚灯: " + std::string(lamp_panel_device_.IsBottomLedOn() ? "开启" : "关闭") +
                    " (固定 10%)\n";
                status += "硬件: PCA9685 (ch13/ch14=舷灯, ch15=锚灯)；与流光 (ch8-ch12) 通道独立。";
                return status;
            } else {
                return "未知动作: " + action +
                       "\n支持的动作: turn_on_panel, turn_on_bottom, turn_on_all,"
                       "\n              turn_off_panel, turn_off_bottom, turn_off_all, "
                       "set_brightness, get_status";
            }
        });
}

}  // namespace mcp_tools

static auto& g_lamp_panel_tool_instance = mcp_tools::LampPanelTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_lamp_panel_tool_instance);
