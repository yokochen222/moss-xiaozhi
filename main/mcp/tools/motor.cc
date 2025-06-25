#include "mcp_tools.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>

#define TAG "MotorTool"

#define STEPS_PER_REVOLUTION 512
#define STEP_DELAY_MS 10

// 第一个电机 (Pitch / 俯仰)
#define MOTOR_PIN_A ((gpio_num_t)13)
#define MOTOR_PIN_B ((gpio_num_t)14)
#define MOTOR_PIN_C ((gpio_num_t)21)
#define MOTOR_PIN_D ((gpio_num_t)46)

// 第二个电机 (Yaw / 左右)
#define MOTOR2_PIN_A ((gpio_num_t)42)
#define MOTOR2_PIN_B ((gpio_num_t)41)
#define MOTOR2_PIN_C ((gpio_num_t)40)
#define MOTOR2_PIN_D ((gpio_num_t)39)

namespace mcp_tools {

class MotorTool : public McpTool {
private:
    struct TaskArgs {
        MotorTool* self;
        int direction;
        int steps;
        bool is_pitch;
    };

    static const uint8_t phasecw[8];
    static const uint8_t phaseccw[8];

    MotorTool();
    ~MotorTool() = default;
    MotorTool(const MotorTool&) = delete;
    MotorTool& operator=(const MotorTool&) = delete;

    void set_motor_phase(uint8_t phase) {
        gpio_set_level(MOTOR_PIN_A, (phase & 0x08) >> 3);
        gpio_set_level(MOTOR_PIN_B, (phase & 0x04) >> 2);
        gpio_set_level(MOTOR_PIN_C, (phase & 0x02) >> 1);
        gpio_set_level(MOTOR_PIN_D, (phase & 0x01));
    }

    void set_motor2_phase(uint8_t phase) {
        gpio_set_level(MOTOR2_PIN_A, (phase & 0x08) >> 3);
        gpio_set_level(MOTOR2_PIN_B, (phase & 0x04) >> 2);
        gpio_set_level(MOTOR2_PIN_C, (phase & 0x02) >> 1);
        gpio_set_level(MOTOR2_PIN_D, (phase & 0x01));
    }

    void initialize_gpio() {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask =
            (1ULL << MOTOR_PIN_A) |
            (1ULL << MOTOR_PIN_B) |
            (1ULL << MOTOR_PIN_C) |
            (1ULL << MOTOR_PIN_D);
        gpio_config(&io_conf);
        set_motor_phase(0x00);
    }

    void initialize_yaw_gpio() {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask =
            (1ULL << MOTOR2_PIN_A) |
            (1ULL << MOTOR2_PIN_B) |
            (1ULL << MOTOR2_PIN_C) |
            (1ULL << MOTOR2_PIN_D);
        gpio_config(&io_conf);
        set_motor2_phase(0x00);
    }

    static void RotationTask(void *pvParameters) {
        auto args = static_cast<TaskArgs *>(pvParameters);
        MotorTool* motor = args->self;
        int direction = args->direction;
        int steps = args->steps;
        bool is_pitch = args->is_pitch;
        delete args;

        const uint8_t *phase_seq = (direction == -1) ? phaseccw : phasecw;

        for (int i = 0; i < steps; ++i) {
            for (int j = 0; j < 8; ++j) {
                if (is_pitch)
                    motor->set_motor_phase(phase_seq[j]);
                else
                    motor->set_motor2_phase(phase_seq[j]);
                vTaskDelay(STEP_DELAY_MS / portTICK_PERIOD_MS);
            }
        }
        if (is_pitch)
            motor->set_motor_phase(0x00);
        else
            motor->set_motor2_phase(0x00);
        vTaskDelete(NULL);
    }

public:
    static MotorTool& GetInstance() {
        static MotorTool instance;
        return instance;
    }

    void Register() override {
        ESP_LOGI(TAG, "注册步进电机控制工具");

        McpServer::GetInstance().AddTool(
            name(),
            "MOSS视角电机控制工具\n"
            "使用说明：\n"
            "- motor='pitch'：控制上下视角，angle为旋转角度（int，正数向下，负数向上）\n"
            "- motor='yaw'：控制左右视角，angle为旋转角度（int，正数向右，负数向左）\n"
            "参数说明：\n"
            "- motor：必填，取值为'pitch'或'yaw'\n"
            "- angle：必填，整型，表示旋转角度\n",
            PropertyList({
                Property("motor", kPropertyTypeString), // "pitch" or "yaw"
                Property("angle", kPropertyTypeInteger) // 角度，正负
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto motor_type = properties["motor"].value<std::string>();
                int angle = properties["angle"].value<int>();
                int direction = (angle > 0) ? 1 : -1;
                int steps = (int)(fabs(angle) / 360.0 * STEPS_PER_REVOLUTION + 0.5);

                if (motor_type == "pitch") {
                    ESP_LOGI(TAG, "RotatePitch: %d° -> %d Steps", angle, steps);
                    auto args = new TaskArgs{this, direction, steps, true};
                    xTaskCreate(RotationTask, "PitchRot", 2048, args, 1, NULL);
                    return "俯仰电机已执行";
                } else if (motor_type == "yaw") {
                    ESP_LOGI(TAG, "RotateYaw: %d° -> %d Steps", angle, steps);
                    auto args = new TaskArgs{this, direction, steps, false};
                    xTaskCreate(RotationTask, "YawRot", 2048, args, 1, NULL);
                    return "左右电机已执行";
                } else {
                    return "未知电机类型: " + motor_type + "，请用 pitch 或 yaw";
                }
            }
        );
    }
};

// 静态成员初始化
const uint8_t MotorTool::phasecw[8]  = {0x08, 0x0C, 0x04, 0x06, 0x02, 0x03, 0x01, 0x09};
const uint8_t MotorTool::phaseccw[8] = {0x09, 0x01, 0x03, 0x02, 0x06, 0x04, 0x0C, 0x08};

MotorTool::MotorTool() : McpTool("self.motor.control", "步进电机控制器") {
    initialize_gpio();
    initialize_yaw_gpio();
}

} // namespace mcp_tools

static auto& g_motor_tool_instance = mcp_tools::MotorTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_motor_tool_instance);