#include "device/eye_motor.h"
#include <esp_log.h>
#include "mcp_tools.h"

#define TAG "OpticDriveTool"

namespace mcp_tools {

class EyeMotorTool : public McpTool {
private:
    EyeMotorDevice& eye_motor_device_;

public:
    static EyeMotorTool& GetInstance() {
        static EyeMotorTool instance;
        return instance;
    }
    EyeMotorTool()
        : McpTool("self.optic_drive.control",
                  "控制MOSS视界驱动器，用于眼部电机正转、反转、调速和停止"),
          eye_motor_device_(EyeMotorDevice::GetInstance()) {}
    void Register() override;
};

void EyeMotorTool::Register() {
    McpServer::GetInstance().AddTool(
        name(),
        "MOSS 视界驱动器控制工具\n"
        "硬件：PCA9685 AIN2=ch3, AIN1=ch4, PWMA=ch5；驱动芯片 TB6612FNG\n"
        "使用说明：\n"
        "- action='start'：默认预设，正转 8 秒再反转 8 秒，循环\n"
        "- action='start_forward'：视界驱动器持续正转，默认全速\n"
        "- action='start_backward'：视界驱动器持续反转，默认全速\n"
        "- action='stop'：停止视界驱动器\n"
        "- action='get_status'：获取视界驱动器状态\n"
        "- action='set_speed'：调整当前转速（需先启动）\n",
        std::vector<Property>{Property("action", kPropertyTypeString),
                              Property("speed", kPropertyTypeInteger, 100, 1, 100)},
        [this](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            auto speed = properties["speed"].value<int>();

            if (action == "start" || action == "oscillate") {
                if (eye_motor_device_.StartOscillate(speed)) {
                    return "视界驱动器已按预设运行: 正转8秒 / 反转8秒, 速度: " +
                           std::to_string(speed) + "%";
                } else {
                    return "启动视界驱动器预设失败";
                }
            } else if (action == "start_forward") {
                if (eye_motor_device_.StartForward(speed)) {
                    return "视界驱动器已正转, 速度: " + std::to_string(speed) + "%";
                } else {
                    return "启动视界驱动器正转失败";
                }
            } else if (action == "start_backward") {
                if (eye_motor_device_.StartBackward(speed)) {
                    return "视界驱动器已反转, 速度: " + std::to_string(speed) + "%";
                } else {
                    return "启动视界驱动器反转失败";
                }
            } else if (action == "stop") {
                if (eye_motor_device_.Stop()) {
                    return "视界驱动器已停止";
                } else {
                    return "停止视界驱动器失败";
                }
            } else if (action == "get_status") {
                std::string status = "视界驱动器状态:\n";
                status += "硬件: PCA9685 AIN2=ch3, AIN1=ch4, PWMA=ch5\n";
                status += "驱动芯片: TB6612FNG\n";
                status += "运行状态: " +
                          std::string(eye_motor_device_.IsRunning() ? "运行中" : "已停止") + "\n";
                status +=
                    "当前速度: " + std::to_string(eye_motor_device_.GetCurrentSpeedPercent()) +
                    "%\n";
                auto state = eye_motor_device_.GetState();
                if (state == EYE_MOTOR_STATE_FORWARD) {
                    status += "转动方向: 正转\n";
                } else if (state == EYE_MOTOR_STATE_BACKWARD) {
                    status += "转动方向: 反转\n";
                } else {
                    status += "转动方向: 静止\n";
                }
                return status;
            } else if (action == "set_speed") {
                if (eye_motor_device_.SetSpeed(speed)) {
                    return "视界驱动器速度已调整为: " + std::to_string(speed) + "%";
                } else {
                    return "调整速度失败，视界驱动器可能未启动";
                }
            } else {
                return "未知动作: " + action +
                       "\n支持的动作: start, start_forward, start_backward, stop, get_status, "
                       "set_speed";
            }
        });
}

}  // namespace mcp_tools

static auto& g_eye_motor_tool_instance = mcp_tools::EyeMotorTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_eye_motor_tool_instance);
