#include <esp_log.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include "device/stepper_gimbal.h"
#include "mcp_tools.h"

#define TAG "GimbalTool"

namespace mcp_tools {

namespace {

// 24BYJ-48 减速输出轴：半步约 4096 步/圈，整步约 2048 步/圈。
constexpr int kHalfStepsPerRev = 4096;
constexpr int kFullStepsPerRev = 2048;

std::optional<GimbalDir> ParseDirection(const std::string& direction) {
    if (direction == "up")
        return GimbalDir::Up;
    if (direction == "down")
        return GimbalDir::Down;
    if (direction == "left")
        return GimbalDir::Left;
    if (direction == "right")
        return GimbalDir::Right;
    if (direction == "up_left")
        return GimbalDir::UpLeft;
    if (direction == "up_right")
        return GimbalDir::UpRight;
    if (direction == "down_left")
        return GimbalDir::DownLeft;
    if (direction == "down_right")
        return GimbalDir::DownRight;
    return std::nullopt;
}

std::optional<StepMode> ParseMode(const std::string& mode) {
    if (mode == "half")
        return StepMode::Half;
    if (mode == "full")
        return StepMode::Full;
    return std::nullopt;
}

const char* ModeToString(StepMode mode) { return mode == StepMode::Full ? "full" : "half"; }

int StepsPerRevolution(StepMode mode) {
    return mode == StepMode::Full ? kFullStepsPerRev : kHalfStepsPerRev;
}

// 角度 → 步数（四舍五入）；结果钳位到 ±一圈，与驱动层 MAX_STEPS 一致。
int DegreesToSteps(int degrees, StepMode mode) {
    const int spr = StepsPerRevolution(mode);
    const int64_t raw =
        (static_cast<int64_t>(degrees) * spr + (degrees >= 0 ? 180 : -180)) / 360;
    const int64_t lim = spr;
    return static_cast<int>(std::clamp(raw, -lim, lim));
}

}  // namespace

class GimbalTool : public McpTool {
private:
    StepperGimbalDevice& gimbal_device_;
    bool has_last_mode_;
    StepMode last_mode_;

public:
    static GimbalTool& GetInstance() {
        static GimbalTool instance;
        return instance;
    }

    GimbalTool()
        : McpTool("self.gimbal.control", "控制双轴步进云台（上下左右及斜向移动）"),
          gimbal_device_(StepperGimbalDevice::GetInstance()),
          has_last_mode_(false),
          last_mode_(StepMode::Half) {}

    void Register() override;
};

void GimbalTool::Register() {
    McpServer::GetInstance().AddTool(
        name(),
        "双轴步进云台控制工具（24BYJ-48 / ULN2003）\n"
        "硬件说明：74HC595 (SER=21, RCK=47, SCK=48) → 2×ULN2003 驱动水平/垂直 24BYJ-48\n"
        "换算（重要）：半步 4096 步=360°，整步 2048 步=360°；90°半步=1024，180°半步=2048。\n"
        "禁止把角度数字直接当作 steps/h_steps/v_steps（例如用户说180度绝不能传 h_steps=180）。\n"
        "使用说明：\n"
        "- action='move'：移动云台。优先用角度参数：\n"
        "  1) 双轴角度：h_deg / v_deg（可同时非零）。+H=右 -H=左，+V=上 -V=下\n"
        "     例：用户要左右和上下各转180度 → h_deg=180, v_deg=180\n"
        "  2) 双轴步数：h_steps / v_steps（仅当明确知道步数时用）\n"
        "  3) 方向字：direction + degrees（优先）或 steps\n"
        "     例：向右转90度 → direction=right, degrees=90\n"
        "- 默认 half、delay_ms=2；再快可试 delay_ms=1（易丢步）\n"
        "- 注意：连续两次 move 会取消上一次；要两轴一起转请一次调用里同时给 h/v 或用斜向\n"
        "- action='hold'：保持某一相导通便于万用表测量（默认 2s，可被 stop 立刻取消）\n"
        "  pattern=0x02 → M1/上下 A相(Q1)；pattern=0x20 → M2/左右 A相(Q5)\n"
        "- action='stop'：立即停止并断电线圈（含取消 hold）\n"
        "- action='get_status'：返回 moving|idle 及上次 mode\n"
        "停转后线圈断电，电机座 Pin2-5 对地约等于 VBUS(5V) 是正常的。\n"
        "导通时对应脚应降到约 0~1V。本板用 FM 74HC595D：Pin10/13 为 NC（无 /OE、/MR）。"
        "若 hold 期间仍全是 5V，查 ULN2003 COM/供电、595 的 Qx 是否有电平变化、电机座接线。\n",
        std::vector<Property>{
            Property("action", kPropertyTypeString),
            Property("direction", kPropertyTypeString, ""),
            Property("degrees", kPropertyTypeInteger, 0, -360, 360),
            Property("steps", kPropertyTypeInteger, 1024, 1, 4096),
            Property("h_deg", kPropertyTypeInteger, 0, -360, 360),
            Property("v_deg", kPropertyTypeInteger, 0, -360, 360),
            Property("h_steps", kPropertyTypeInteger, 0, -4096, 4096),
            Property("v_steps", kPropertyTypeInteger, 0, -4096, 4096),
            Property("mode", kPropertyTypeString, "half"),
            Property("delay_ms", kPropertyTypeInteger, 2, 1, 50),
            Property("pattern", kPropertyTypeInteger, 255, 0, 255),
            Property("hold_ms", kPropertyTypeInteger, 2000, 100, 30000),
        },
        [this](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();

            if (action == "hold") {
                auto pattern = static_cast<uint8_t>(properties["pattern"].value<int>());
                auto hold_ms = static_cast<uint16_t>(properties["hold_ms"].value<int>());
                if (gimbal_device_.HoldPattern(pattern, hold_ms)) {
                    return "已保持输出 0x" + ([](uint8_t p) {
                               const char* hex = "0123456789ABCDEF";
                               return std::string{hex[p >> 4], hex[p & 0xF]};
                           })(pattern) +
                           " 共 " + std::to_string(hold_ms) +
                           "ms。测量时：导通相应脚应对地约 0~1V；全是 5V 则 595/ULN 未驱动。";
                }
                return "hold 失败";
            }

            if (action == "move") {
                auto mode_str = properties["mode"].value<std::string>();
                auto mode = ParseMode(mode_str);
                if (!mode) {
                    return "无效步进模式: " + mode_str + "\n支持的模式: half, full";
                }

                auto delay_ms = static_cast<uint16_t>(properties["delay_ms"].value<int>());
                const int h_deg = properties["h_deg"].value<int>();
                const int v_deg = properties["v_deg"].value<int>();
                auto h_steps = static_cast<int16_t>(properties["h_steps"].value<int>());
                auto v_steps = static_cast<int16_t>(properties["v_steps"].value<int>());

                // 角度优先于步数，避免把“180度”误当成 180 步。
                if (h_deg != 0 || v_deg != 0) {
                    h_steps = static_cast<int16_t>(DegreesToSteps(h_deg, *mode));
                    v_steps = static_cast<int16_t>(DegreesToSteps(v_deg, *mode));
                    if (h_steps == 0 && v_steps == 0) {
                        return "角度过小，换算后步数为 0";
                    }
                    if (gimbal_device_.MoveAxes(h_steps, v_steps, *mode, delay_ms)) {
                        has_last_mode_ = true;
                        last_mode_ = *mode;
                        return "云台开始双轴移动: h_deg=" + std::to_string(h_deg) +
                               "→" + std::to_string(h_steps) + "步, v_deg=" +
                               std::to_string(v_deg) + "→" + std::to_string(v_steps) +
                               "步, 模式=" + mode_str +
                               ", 延时=" + std::to_string(delay_ms) + "ms";
                    }
                    return "启动云台双轴移动失败";
                }

                if (h_steps != 0 || v_steps != 0) {
                    if (gimbal_device_.MoveAxes(h_steps, v_steps, *mode, delay_ms)) {
                        has_last_mode_ = true;
                        last_mode_ = *mode;
                        return "云台开始双轴移动: h_steps=" + std::to_string(h_steps) +
                               ", v_steps=" + std::to_string(v_steps) + ", 模式=" + mode_str +
                               ", 延时=" + std::to_string(delay_ms) + "ms";
                    }
                    return "启动云台双轴移动失败";
                }

                auto direction_str = properties["direction"].value<std::string>();
                if (direction_str.empty()) {
                    return "缺少 direction，或设置 h_deg/v_deg（推荐）或 h_steps/v_steps";
                }

                auto dir = ParseDirection(direction_str);
                if (!dir) {
                    return "无效方向: " + direction_str +
                           "\n支持的方向: up, down, left, right, up_left, up_right, down_left, "
                           "down_right";
                }

                const int degrees = properties["degrees"].value<int>();
                uint16_t steps = static_cast<uint16_t>(properties["steps"].value<int>());
                std::string amount_desc;
                if (degrees != 0) {
                    const int converted = DegreesToSteps(std::abs(degrees), *mode);
                    if (converted <= 0) {
                        return "角度过小，换算后步数为 0";
                    }
                    steps = static_cast<uint16_t>(converted);
                    amount_desc = "角度=" + std::to_string(degrees) + "°→" +
                                  std::to_string(steps) + "步";
                } else {
                    amount_desc = "步数=" + std::to_string(steps);
                }

                if (gimbal_device_.Move(*dir, steps, *mode, delay_ms)) {
                    has_last_mode_ = true;
                    last_mode_ = *mode;
                    return "云台开始移动: 方向=" + direction_str + ", " + amount_desc +
                           ", 模式=" + mode_str + ", 延时=" + std::to_string(delay_ms) + "ms";
                }
                return "启动云台移动失败";
            }

            if (action == "stop") {
                if (gimbal_device_.Stop()) {
                    return "云台已停止，全部线圈已关断";
                }
                return "停止云台失败";
            }

            if (action == "get_status") {
                std::string status = gimbal_device_.IsMoving() ? "moving" : "idle";
                if (has_last_mode_) {
                    status += ", mode=" + std::string(ModeToString(last_mode_));
                }
                return status;
            }

            return "未知动作: " + action + "\n支持的动作: move, stop, get_status, hold";
        });
}

}  // namespace mcp_tools

static auto& g_gimbal_tool_instance = mcp_tools::GimbalTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_gimbal_tool_instance);
