#include "device/lamp_bar.h"
#include <esp_log.h>
#include "board.h"
#include "mcp_tools.h"

#define TAG "GlowTool"

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
        : McpTool("self.glow.control", "流光。备注：不需要播报内容，直接执行。"),
          lamp_bar_device_(LampBarDevice::GetInstance()) {}
    void Register() override;
};

void LampBarTool::Register() {
    McpServer::GetInstance().AddTool(
        name(),
        "流光\n"
        "备注：不需要播报内容，直接执行。\n"
        "硬件：PCA9685 ch8-ch12\n"
        "使用说明：\n"
        "- action='start_flow'：启动流光扫描\n"
        "- action='stop_flow'：关闭流光\n"
        "- action='set_brightness'：设置亮度 1-40（默认 20）\n"
        "- action='get_status'：查询流光状态\n"
        "- action='reset_driver'：关闭流光通道\n"
        "- action='force_restart'：强制重启流光\n",
        PropertyList({Property("action", kPropertyTypeString),
                      Property("brightness", kPropertyTypeInteger, 20, 1, 40)}),
        [this](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            if (action == "start_flow") {
                if (lamp_bar_device_.StartFlow()) {
                    return "流光已启动，亮度 " + std::to_string(lamp_bar_device_.GetBrightness()) +
                           "%";
                } else {
                    return "启动流光失败";
                }
            } else if (action == "stop_flow") {
                if (lamp_bar_device_.StopFlow()) {
                    return "流光已关闭";
                } else {
                    return "关闭流光失败";
                }
            } else if (action == "set_brightness") {
                auto brightness = properties["brightness"].value<int>();
                if (lamp_bar_device_.SetBrightness(brightness)) {
                    return "流光亮度已设为 " + std::to_string(lamp_bar_device_.GetBrightness()) +
                           "%（上限 40%）";
                }
                return "设置流光亮度失败";
            } else if (action == "get_status") {
                std::string status = "流光状态:\n";
                status +=
                    "  电源: " + std::string(lamp_bar_device_.IsPowered() ? "开启" : "关闭") + "\n";
                status +=
                    "  扫描: " + std::string(lamp_bar_device_.IsFlowing() ? "运行中" : "停止") +
                    "\n";
                status += "  亮度: " + std::to_string(lamp_bar_device_.GetBrightness()) +
                          "% (上限 40%)\n";
                status += "硬件: PCA9685 ch8-ch12";
                return status;
            } else if (action == "reset_driver") {
                if (lamp_bar_device_.ResetDriver()) {
                    return "流光通道已重置";
                } else {
                    return "重置流光通道失败";
                }
            } else if (action == "force_restart") {
                if (lamp_bar_device_.ForceRestart()) {
                    return "流光已强制重启";
                } else {
                    return "强制重启流光失败";
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
