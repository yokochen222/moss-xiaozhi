#include "device/lamp_panel.h"
#include <esp_log.h>
#include "board.h"
#include "mcp_tools.h"

#define TAG "BowBeaconTool"

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
        : McpTool("self.bow_beacon.control", "控制MOSS前舷信标与暗舷锚灯"),
          lamp_panel_device_(LampPanelDevice::GetInstance()) {}
    void Register() override;
};

void LampPanelTool::Register() {
    ESP_LOGI(TAG, "注册前舷信标/暗舷锚灯控制工具");
    McpServer::GetInstance().AddTool(
        name(),
        "MOSS 前舷信标 / 暗舷锚灯控制工具\n"
        "硬件：PCA9685 ch13/ch14=前舷信标 x2，ch15=暗舷锚灯。PWM 调光。\n"
        "使用说明：\n"
        "- action='turn_on_panel'：点亮两盏前舷信标\n"
        "- action='turn_on_bottom'：点亮暗舷锚灯（固定 10%）\n"
        "- action='turn_on_all'：同时点亮前舷信标与暗舷锚灯\n"
        "- action='turn_off_panel'：熄灭前舷信标\n"
        "- action='turn_off_bottom'：熄灭暗舷锚灯\n"
        "- action='turn_off_all'：全部熄灭\n"
        "- action='set_brightness'：设置前舷信标亮度 1-40（默认 20，锚灯不可调）\n"
        "- action='get_status'：查询状态\n"
        "\n"
        "前舷信标默认 20%，上限 40%；暗舷锚灯点亮固定 10%。\n"
        "与光子流环 (ch8-ch12) 通道独立、互不影响。\n",
        PropertyList({Property("action", kPropertyTypeString),
                      Property("brightness", kPropertyTypeInteger, 20, 1, 40)}),
        [this](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();

            if (action == "turn_on_panel") {
                if (lamp_panel_device_.TurnOnPanelLeds()) {
                    return "前舷信标已点亮，亮度 " +
                           std::to_string(lamp_panel_device_.GetBrightness()) + "%";
                }
                return "点亮前舷信标失败";
            } else if (action == "turn_on_bottom") {
                if (lamp_panel_device_.TurnOnBottomLed()) {
                    return "暗舷锚灯已点亮（固定 10%）";
                }
                return "点亮暗舷锚灯失败";
            } else if (action == "turn_on_all") {
                if (lamp_panel_device_.TurnOnAll()) {
                    return "前舷信标与暗舷锚灯已全部点亮，信标 " +
                           std::to_string(lamp_panel_device_.GetBrightness()) + "%，锚灯 10%";
                }
                return "点亮前舷信标/暗舷锚灯失败";
            } else if (action == "turn_off_panel") {
                if (lamp_panel_device_.TurnOffPanelLeds()) {
                    return "前舷信标已熄灭";
                }
                return "熄灭前舷信标失败";
            } else if (action == "turn_off_bottom") {
                if (lamp_panel_device_.TurnOffBottomLed()) {
                    return "暗舷锚灯已熄灭";
                }
                return "熄灭暗舷锚灯失败";
            } else if (action == "turn_off_all") {
                if (lamp_panel_device_.TurnOffAll()) {
                    return "前舷信标与暗舷锚灯已全部熄灭";
                }
                return "熄灭前舷信标/暗舷锚灯失败";
            } else if (action == "set_brightness") {
                auto brightness = properties["brightness"].value<int>();
                if (lamp_panel_device_.SetBrightness(brightness)) {
                    return "前舷信标亮度已设为 " +
                           std::to_string(lamp_panel_device_.GetBrightness()) + "%（上限 40%）";
                }
                return "设置前舷信标亮度失败";
            } else if (action == "get_status") {
                std::string status = "前舷信标 / 暗舷锚灯状态:\n";
                status += "  前舷信标1: " +
                          std::string(lamp_panel_device_.IsPanelLed1On() ? "开启" : "关闭") + "\n";
                status += "  前舷信标2: " +
                          std::string(lamp_panel_device_.IsPanelLed2On() ? "开启" : "关闭") + "\n";
                status += "  信标亮度: " + std::to_string(lamp_panel_device_.GetBrightness()) +
                          "% (上限 40%)\n";
                status += "  暗舷锚灯: " +
                          std::string(lamp_panel_device_.IsBottomLedOn() ? "开启" : "关闭") +
                          " (固定 10%)\n";
                status +=
                    "硬件: PCA9685 (ch13/ch14=前舷信标, ch15=暗舷锚灯)；与光子流环 (ch8-ch12) "
                    "通道独立。";
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
