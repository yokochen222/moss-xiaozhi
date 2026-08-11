#include "mcp_tools.h"
#include "board.h"
#include "device/lamp_panel.h"
#include <esp_log.h>

#define TAG "LampPanelTool"

namespace mcp_tools {

class LampPanelTool : public McpTool {
private:
    LampPanelDevice& lamp_panel_device_;

public:
    static LampPanelTool& GetInstance() {
        static LampPanelTool instance;
        return instance;
    }
    LampPanelTool() : McpTool("self.lamp_panel.control", "控制MOSS的面板灯与底灯"),
                      lamp_panel_device_(LampPanelDevice::GetInstance()) {
    }
    void Register() override;
};

void LampPanelTool::Register() {
    ESP_LOGI(TAG, "注册面板灯/底灯控制工具");
    McpServer::GetInstance().AddTool(
        name(),
        "MOSS设备面板灯/底灯控制工具\n"
        "使用说明：\n"
        "点亮：\n"
        "- action='turn_on_panel'：仅点亮两盏前面板灯\n"
        "- action='turn_on_bottom'：仅点亮底灯\n"
        "- action='turn_on_all'：同时点亮面板灯(两盏)和底灯\n"
        "熄灭：\n"
        "- action='turn_off_panel'：熄灭两盏前面板灯\n"
        "- action='turn_off_bottom'：熄灭底灯\n"
        "- action='turn_off_all'：熄灭全部三盏灯\n"
        "- action='get_status'：查询面板灯/底灯当前状态\n"
        "\n"
        "注意：本工具只控制 Q5/Q6/Q7 三位输出位（前面板灯 x2 + 底灯）。\n"
        "      与流水灯 (Q0-Q4) 在硬件层面完全独立、互不影响：\n"
        "      无论流水灯是否在运行，本工具对面板/底灯的开关都会原样生效；\n"
        "      反之，本工具也不会打断流水灯正在进行的动画。\n",
        PropertyList({
            Property("action", kPropertyTypeString)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();

            if (action == "turn_on_panel") {
                if (lamp_panel_device_.TurnOnPanelLeds()) {
                    return "前面板灯(两盏)已点亮";
                }
                return "点亮前面板灯失败";
            } else if (action == "turn_on_bottom") {
                if (lamp_panel_device_.TurnOnBottomLed()) {
                    return "底灯已点亮";
                }
                return "点亮底灯失败";
            } else if (action == "turn_on_all") {
                if (lamp_panel_device_.TurnOnAll()) {
                    return "前面板灯(两盏)与底灯已全部点亮";
                }
                return "点亮全部面板灯/底灯失败";
            } else if (action == "turn_off_panel") {
                if (lamp_panel_device_.TurnOffPanelLeds()) {
                    return "前面板灯(两盏)已熄灭";
                }
                return "熄灭前面板灯失败";
            } else if (action == "turn_off_bottom") {
                if (lamp_panel_device_.TurnOffBottomLed()) {
                    return "底灯已熄灭";
                }
                return "熄灭底灯失败";
            } else if (action == "turn_off_all") {
                if (lamp_panel_device_.TurnOffAll()) {
                    return "前面板灯(两盏)与底灯已全部熄灭";
                }
                return "熄灭全部面板灯/底灯失败";
            } else if (action == "get_status") {
                std::string status = "MOSS 面板灯/底灯状态:\n";
                status += "  面板灯1: " + std::string(lamp_panel_device_.IsPanelLed1On() ? "开启" : "关闭") + "\n";
                status += "  面板灯2: " + std::string(lamp_panel_device_.IsPanelLed2On() ? "开启" : "关闭") + "\n";
                status += "  底灯:    " + std::string(lamp_panel_device_.IsBottomLedOn() ? "开启" : "关闭") + "\n";
                status += "硬件: 74HC595 (Q5=面板灯1, Q6=面板灯2, Q7=底灯)；与流水灯共用同一组 SER/RCK/SCK 引脚但状态完全隔离。";
                return status;
            } else {
                return "未知动作: " + action +
                       "\n支持的动作: turn_on_panel, turn_on_bottom, turn_on_all,"
                       "\n              turn_off_panel, turn_off_bottom, turn_off_all, get_status";
            }
        }
    );
}

} // namespace mcp_tools

static auto& g_lamp_panel_tool_instance = mcp_tools::LampPanelTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_lamp_panel_tool_instance);
