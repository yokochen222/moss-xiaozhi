#include "device/lamp_bar.h"
#include <esp_log.h>
#include "board.h"
#include "mcp_tools.h"

#define TAG "LampBarTool"

namespace mcp_tools {

class LampBarTool : public McpTool {
private:
    LampBarDevice& lamp_bar_device_;

public:
    static LampBarTool& GetInstance() {
        static LampBarTool instance;
        return instance;
    }
    LampBarTool()
        : McpTool("self.lamp_bar.control", "控制MOSS的流水灯效果"),
          lamp_bar_device_(LampBarDevice::GetInstance()) {}
    void Register() override;
};

void LampBarTool::Register() {
    McpServer::GetInstance().AddTool(
        name(),
        "MOSS设备灯光控制工具（流水灯）\n"
        "使用说明：\n"
        "流水灯控制：\n"
        "- action='start_flow'：开启流水灯效果\n"
        "- action='stop_flow'：关闭流水灯效果\n"
        "- action='set_brightness'：设置亮度 1-40（默认 20）\n"
        "系统控制：\n"
        "- action='get_status'：获取流水灯当前状态信息\n"
        "- action='reset_driver'：关闭流水灯通道（PCA9685 ch8-12）\n"
        "- action='force_restart'：强制重启流水灯系统\n",
        PropertyList({Property("action", kPropertyTypeString),
                      Property("brightness", kPropertyTypeInteger, 20, 1, 40)}),
        [this](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            if (action == "start_flow") {
                if (lamp_bar_device_.StartFlow()) {
                    return "流水灯效果已开启，亮度 " +
                           std::to_string(lamp_bar_device_.GetBrightness()) + "%";
                } else {
                    return "启动流水灯效果失败";
                }
            } else if (action == "stop_flow") {
                if (lamp_bar_device_.StopFlow()) {
                    return "流水灯效果已关闭";
                } else {
                    return "停止流水灯效果失败";
                }
            } else if (action == "set_brightness") {
                auto brightness = properties["brightness"].value<int>();
                if (lamp_bar_device_.SetBrightness(brightness)) {
                    return "流水灯亮度已设为 " + std::to_string(lamp_bar_device_.GetBrightness()) +
                           "%（上限 40%）";
                }
                return "设置流水灯亮度失败";
            } else if (action == "get_status") {
                std::string status = "MOSS灯光系统状态:\n";
                status += "流水灯:\n";
                status +=
                    "  电源: " + std::string(lamp_bar_device_.IsPowered() ? "开启" : "关闭") + "\n";
                status +=
                    "  流水效果: " + std::string(lamp_bar_device_.IsFlowing() ? "运行中" : "停止") +
                    "\n";
                status += "  亮度: " + std::to_string(lamp_bar_device_.GetBrightness()) +
                          "% (上限 40%)\n";
                status += "硬件: 使用PCA9685控制 (ch8-ch12: 流水灯)";
                return status;
            } else if (action == "reset_driver") {
                if (lamp_bar_device_.ResetDriver()) {
                    return "PCA9685流水灯通道已重置";
                } else {
                    return "重置PCA9685流水灯通道失败";
                }
            } else if (action == "force_restart") {
                if (lamp_bar_device_.ForceRestart()) {
                    return "所有灯光系统已强制重启，所有状态已重置";
                } else {
                    return "强制重启灯光系统失败";
                }
            } else {
                return "未知动作: " + action +
                       "\n支持的动作: start_flow, stop_flow, set_brightness, get_status, "
                       "reset_driver, force_restart";
            }
        });
}

}  // namespace mcp_tools

static auto& g_lamp_bar_tool_instance = mcp_tools::LampBarTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_lamp_bar_tool_instance);
