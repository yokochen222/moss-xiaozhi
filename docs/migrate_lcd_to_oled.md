# 从LCD迁移到OLED显示指南

## 概述
本指南说明了如何将lichuang-dev板子从LCD显示(ST7789)迁移到OLED显示(SSD1306/SH1106)。

## 修改内容

### 1. 配置文件修改
**文件**: `main/boards/lichuang-dev/config.h`

#### 移除LCD相关定义：
```c
// 移除这些定义
#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY true
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_42
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true
```

#### 添加OLED相关定义：
```c
// OLED Display Configuration
#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64  // 或32，取决于你的OLED模块
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true
#define DISPLAY_SWAP_XY false

// OLED使用I2C，不需要SPI引脚
#define DISPLAY_SDA_PIN  GPIO_NUM_1
#define DISPLAY_SCL_PIN  GPIO_NUM_2
```

### 2. 板子实现文件修改
**文件**: `main/boards/lichuang-dev/lichuang_dev_board.cc`

#### 头文件修改：
```c
// 替换
#include "display/lcd_display.h"
// 为
#include "display/oled_display.h"
#include "display/no_display.h"

// 移除
#include <driver/spi_common.h>
```

#### 类成员变量修改：
```c
// 移除
LcdDisplay* display_;

// 添加
Display* display_;
i2c_master_bus_handle_t display_i2c_bus_;
esp_lcd_panel_io_handle_t panel_io_ = nullptr;
esp_lcd_panel_handle_t panel_ = nullptr;
```

#### 方法修改：

##### 移除LCD相关方法：
- `InitializeSpi()` - SPI总线初始化
- `InitializeSt7789Display()` - ST7789 LCD初始化

##### 添加OLED相关方法：
- `InitializeDisplayI2c()` - I2C总线初始化
- `InitializeOledDisplay()` - OLED显示初始化

#### 构造函数修改：
```c
// 替换
InitializeI2c();
InitializeSpi();
InitializeSt7789Display();
InitializeButtons();
InitializeIot();
GetBacklight()->RestoreBrightness();

// 为
InitializeI2c();
InitializeDisplayI2c();
InitializeOledDisplay();
InitializeButtons();
InitializeIot();
```

#### Backlight方法修改：
```c
// OLED不需要背光控制
virtual Backlight* GetBacklight() override {
    return nullptr; // OLED doesn't need backlight control
}
```

## 硬件连接

### OLED I2C连接：
- **SDA**: GPIO_NUM_1
- **SCL**: GPIO_NUM_2
- **VCC**: 3.3V
- **GND**: GND

### 注意事项：
1. **I2C地址**: 默认使用0x3C，如果你的OLED使用0x3D，需要修改代码
2. **显示尺寸**: 根据你的OLED模块选择DISPLAY_HEIGHT (64或32)
3. **引脚冲突**: 确保DISPLAY_SDA_PIN和DISPLAY_SCL_PIN不与音频codec引脚冲突

## 编译配置

在menuconfig中，确保选择正确的OLED类型：
```
Component config → MOSS → Board Type → lichuang-dev
Component config → MOSS → Display → OLED Type → SSD1306 128x64
```

## 验证步骤

1. 运行配置检查脚本：
   ```bash
   python3 scripts/test_oled_config.py
   ```

2. 编译项目：
   ```bash
   idf.py build
   ```

3. 烧录并测试显示功能

## 故障排除

### 常见问题：
1. **I2C地址错误**: 使用I2C扫描器确认地址
2. **显示不亮**: 检查电源连接和I2C线路
3. **编译错误**: 确保所有头文件路径正确

### 调试工具：
- 使用`scripts/i2c_scanner.py`扫描I2C设备
- 检查串口日志确认OLED初始化状态