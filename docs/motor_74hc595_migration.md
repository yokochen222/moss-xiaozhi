# 电机控制迁移到74HC595文档

## 概述
为了节省GPIO引脚，将原来的直接GPIO控制步进电机的方式改为通过74HC595移位寄存器控制。

## 硬件连接

### ESP32S3 到 74HC595
- GPIO9 → SER (数据引脚)
- GPIO10 → RCLK (锁存引脚)  
- GPIO11 → SRCLK (时钟引脚)

### 74HC595 到 第一个ULN2003 (俯仰电机)
- Q0 → L1
- Q1 → L2
- Q2 → L3
- Q3 → L4

### 74HC595 到 第二个ULN2003 (左右电机)
- Q4 → L1
- Q5 → L2
- Q6 → L3
- Q7 → L4

## 代码修改内容

### 1. 引脚定义更改
**原来：**
```cpp
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
```

**现在：**
```cpp
// 74HC595引脚定义
#define SER_PIN GPIO_NUM_9   // 数据引脚
#define RCK_PIN GPIO_NUM_10  // 锁存引脚
#define SCK_PIN GPIO_NUM_11  // 时钟引脚
```

### 2. 类成员变量更改
**新增：**
```cpp
ShiftRegister74HC595* shift_register_;
```

### 3. 电机控制函数更改
**原来：**
```cpp
void set_motor_phase(uint8_t phase) {
    gpio_set_level(MOTOR_PIN_A, (phase & 0x08) >> 3);
    gpio_set_level(MOTOR_PIN_B, (phase & 0x04) >> 2);
    gpio_set_level(MOTOR_PIN_C, (phase & 0x02) >> 1);
    gpio_set_level(MOTOR_PIN_D, (phase & 0x01));
}
```

**现在：**
```cpp
void set_motor_phase(uint8_t phase) {
    // 第一个电机使用Q0-Q3 (低4位)
    uint8_t current_data = shift_register_->GetCurrentData();
    // 清除Q0-Q3位，保持Q4-Q7不变
    current_data &= 0xF0;
    // 设置Q0-Q3为电机相位
    current_data |= (phase & 0x0F);
    shift_register_->SetOutputs(current_data);
}
```

### 4. 初始化函数更改
**原来：**
```cpp
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
```

**现在：**
```cpp
void initialize_shift_register() {
    // 创建74HC595驱动实例
    shift_register_ = new ShiftRegister74HC595(SER_PIN, RCK_PIN, SCK_PIN);
    shift_register_->Initialize();
    
    // 初始化时关闭所有电机
    shift_register_->ClearAll();
}
```

### 5. 析构函数更改
**新增：**
```cpp
MotorTool::~MotorTool() {
    if (shift_register_) {
        delete shift_register_;
    }
}
```

## 工作原理

### 74HC595数据位分配
- **Q0-Q3 (低4位)**: 控制第一个电机(俯仰电机)
- **Q4-Q7 (高4位)**: 控制第二个电机(左右电机)

### 电机相位控制
每个电机使用4位控制，通过位操作保持另一个电机的状态不变：
- `set_motor_phase()`: 只修改Q0-Q3位，保持Q4-Q7位不变
- `set_motor2_phase()`: 只修改Q4-Q7位，保持Q0-Q3位不变

### 通信时序
使用现有的74HC595驱动，通信时序为：
1. 数据通过SER引脚串行输入
2. 时钟SCK上升沿移入数据
3. 锁存RCK上升沿将数据输出到Q0-Q7引脚

## 优势

1. **节省GPIO**: 从8个GPIO减少到3个GPIO
2. **保持功能**: 完全保持原有的电机控制功能
3. **代码复用**: 使用现有的74HC595驱动代码
4. **扩展性**: 可以轻松添加更多设备到74HC595的输出引脚

## 注意事项

1. 确保74HC595的电源和地连接正确
2. 电机控制时序保持不变，只是控制方式从直接GPIO改为74HC595
3. 两个电机可以同时工作，互不影响
4. 原有的API接口完全保持不变，用户无需修改调用代码

## 测试建议

1. 验证74HC595通信是否正常
2. 测试单个电机控制功能
3. 测试两个电机同时工作
4. 验证电机旋转角度和方向是否正确
