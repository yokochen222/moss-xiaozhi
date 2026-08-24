#include "device/eye_motor.h"
#include <esp_log.h>
#include "mcp_tools.h"

#define TAG "EyeMotorTool"

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
        : McpTool("self.eye_motor.control",
                  "控制eye_motor电机驱动模块，用于驱动电机正转、反转、调速和停止"),
          eye_motor_device_(EyeMotorDevice::GetInstance()) {}
    void Register() override;
};

void EyeMotorTool::Register() {
    McpServer::GetInstance().AddTool(
        name(),
        "EyeMotor TB6612FNG电机驱动控制工具\n"
        "硬件说明：PCA9685 通道 AIN2=ch3, AIN1=ch4, PWMA=ch5\n"
        "使用说明：\n"
        "- action='start'：默认预设，正转 8 秒再反转 8 秒，循环\n"
        "- action='start_forward'：电机持续正转，默认全速\n"
        "- action='start_backward'：电机持续反转，默认全速\n"
        "- action='stop'：停止电机\n"
        "- action='get_status'：获取电机状态\n"
        "- action='set_speed'：调整当前转速(需先启动电机)\n",
        std::vector<Property>{Property("action", kPropertyTypeString),
                              Property("speed", kPropertyTypeInteger, 100, 1, 100)},
        [this](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            auto speed = properties["speed"].value<int>();

            if (action == "start" || action == "oscillate") {
                if (eye_motor_device_.StartOscillate(speed)) {
                    return "电机已按预设运行: 正转8秒 / 反转8秒, 速度: " + std::to_string(speed) +
                           "%";
                } else {
                    return "启动电机预设失败";
                }
            } else if (action == "start_forward") {
                if (eye_motor_device_.StartForward(speed)) {
                    return "电机已正转, 速度: " + std::to_string(speed) + "%";
                } else {
                    return "启动电机正转失败";
                }
            } else if (action == "start_backward") {
                if (eye_motor_device_.StartBackward(speed)) {
                    return "电机已反转, 速度: " + std::to_string(speed) + "%";
                } else {
                    return "启动电机反转失败";
                }
            } else if (action == "stop") {
                if (eye_motor_device_.Stop()) {
                    return "电机已停止";
                } else {
                    return "停止电机失败";
                }
            } else if (action == "get_status") {
                std::string status = "EyeMotor电机状态:\n";
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
                    return "速度已调整为: " + std::to_string(speed) + "%";
                } else {
                    return "调整速度失败，电机可能未启动";
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
