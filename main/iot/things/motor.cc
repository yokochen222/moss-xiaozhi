#include "iot/thing.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>      // for fabs

#define TAG "Motor"

// 标准 1.8° 步进电机，全步模式下每圈 200 步
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

namespace iot {

class Motor : public Thing {
private:
    struct TaskArgs {
        Motor* self;
        int direction;
        int steps;
    };

    // 相序表（四相八拍）
    static const uint8_t phasecw[8];
    static const uint8_t phaseccw[8];

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

public:
    Motor() : Thing("Motor", "步进电机控制器") {
        initialize_gpio();
        initialize_yaw_gpio();

        methods_.AddMethod("RotatePitch", "控制MOSS上下视角角度，旋转角度(正数为向下转动,负数为向上转动，注意angle参数的类型必须为int",
            ParameterList({
                Parameter("angle", "旋转角度(正数为向下转动,负数为向上转动，注意angle参数的类型必须为int)", kValueTypeNumber, true)
            }),
            [this](const ParameterList& parameters) {
                float angle = parameters["angle"].number();
                int direction = (angle > 0) ? 1 : -1;
                int steps = (int)(fabs(angle) / 360.0 * STEPS_PER_REVOLUTION + 0.5);
                ESP_LOGI(TAG, "RotatePitch: %.2f° -> %d Steps", angle, steps);

                auto args = new TaskArgs{this, direction, steps};
                xTaskCreate(pitchRotationTask, "PitchRot", 2048, args, 1, NULL);
            }
        );

        methods_.AddMethod("RotateYaw", "控制MOSS左右视角角度，旋转角度(正数为向右转动,负数为向左转动，注意angle参数的类型必须为int",
            ParameterList({
                Parameter("angle", "旋转角度(正数为向右转动,负数为向左转动，注意angle参数的类型必须为int)", kValueTypeNumber, true)
            }),
            [this](const ParameterList& parameters) {
                float angle = parameters["angle"].number();
                int direction = (angle > 0) ? 1 : -1;
                int steps = (int)(fabs(angle) / 360.0 * STEPS_PER_REVOLUTION + 0.5);
                ESP_LOGI(TAG, "RotateYaw: %.2f° -> %d Steps", angle, steps);

                auto args = new TaskArgs{this, direction, steps};
                xTaskCreate(yawRotationTask, "YawRot", 2048, args, 1, NULL);
            }
        );
    }

    static void pitchRotationTask(void *pvParameters) {
        auto args = static_cast<TaskArgs *>(pvParameters);
        Motor* motor = args->self;
        int direction = args->direction;
        int steps = args->steps;

        delete args;

        const uint8_t *phase_seq = (direction == -1) ? phaseccw : phasecw;

        for (int i = 0; i < steps; ++i) {
            for (int j = 0; j < 8; ++j) {
                motor->set_motor_phase(phase_seq[j]);
                vTaskDelay(STEP_DELAY_MS / portTICK_PERIOD_MS);
            }
        }

        motor->set_motor_phase(0x00); // 停止输出
        vTaskDelete(NULL);
    }

    static void yawRotationTask(void *pvParameters) {
        auto args = static_cast<TaskArgs *>(pvParameters);
        Motor* motor = args->self;
        int direction = args->direction;
        int steps = args->steps;

        delete args;

        const uint8_t *phase_seq = (direction == -1) ? phaseccw : phasecw;

        for (int i = 0; i < steps; ++i) {
            for (int j = 0; j < 8; ++j) {
                motor->set_motor2_phase(phase_seq[j]);
                vTaskDelay(STEP_DELAY_MS / portTICK_PERIOD_MS);
            }
        }

        motor->set_motor2_phase(0x00); // 停止输出
        vTaskDelete(NULL);
    }
};

// 初始化静态成员
const uint8_t iot::Motor::phasecw[8] = {0x08, 0x0C, 0x04, 0x06, 0x02, 0x03, 0x01, 0x09}; // 正转
const uint8_t iot::Motor::phaseccw[8] = {0x09, 0x01, 0x03, 0x02, 0x06, 0x04, 0x0C, 0x08}; // 反转

} // namespace iot

DECLARE_THING(Motor);