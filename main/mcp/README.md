# MCP工具模块

本文件夹包含了模块化的MCP工具实现，结构参考iot模块。

## 文件结构

```
mcp/
├── mcp_tools.h              # 工具基类和注册接口
├── mcp_tools.cc             # 工具注册实现
├── README.md                # 本文件
└── tools/                   # 所有单独的tool实现
    ├── led_control_tool.cc  # LED控制工具
    ├── system_info_tool.cc  # 系统信息工具
    ├── test_tool.cc         # 测试工具
    ├── network_tool.cc      # 网络工具
    └── lamp_eye.cc          # 眼部灯光控制工具
    ├── lamp_bar.cc           # 流水灯控制工具
    ├── lamp_panel.cc         # 面板灯/底灯控制工具
```

## 设计特点

- ✅ **自动注册**：使用`DECLARE_MCP_TOOL`宏自动注册工具
- ✅ **自动编译**：CMake自动包含`tools/`下所有`.cc`文件
- ✅ **零配置**：新增工具无需修改CMakeLists.txt
- ✅ **结构一致**：与iot模块保持相同的设计模式

## 如何添加新工具

### 只需两步：

1. **创建工具文件**：在`tools/`下新建`your_tool.cc`
2. **完成！** 工具会自动注册和编译，无需其他配置

### 示例代码：

```cpp
#include "mcp_tools.h"
#include <esp_log.h>

#define TAG "YourTool"

namespace mcp_tools {

class YourTool : public McpTool {
public:
    YourTool() : McpTool("self.your.tool_name", "你的工具描述") {}

    void Register() override {
        ESP_LOGI(TAG, "注册你的工具");
        
        McpServer::GetInstance().AddTool(
            name(),
            description(),
            PropertyList({
                Property("param1", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto param1 = properties["param1"].value<std::string>();
                return "处理结果: " + param1;
            }
        );
    }
};

} // namespace mcp_tools

DECLARE_MCP_TOOL(YourTool);
```

## 现有工具

- **眼部灯光工具** (`self.lamp_eye.control`) - PCA9685 ch6 眼部灯光
- **流水灯工具** (`self.lamp_bar.control`) - PCA9685 ch8–12 流水灯
- **面板灯/底灯工具** (`self.lamp_panel.control`) - PCA9685 ch13–15 面板/底灯
- **眼电机工具** (`self.eye_motor.control`) - PCA9685 ch3–5 + TB6612 眼电机
- **云台控制工具** (`self.gimbal.control`) - 74HC595 + ULN2003 双轴步进云台
- **红外工具** (`self.infrared.*`) - 红外收发

## PCA9685 与 74HC595

| 通道 / 总线 | 功能 |
|-------------|------|
| PCA LED0 | LCD 背光 |
| PCA LED1 | NS4150B 功放使能 |
| PCA LED3–5 | 眼电机 TB6612（AIN2/AIN1/PWMA） |
| PCA LED6 | 眼部呼吸灯 |
| PCA LED8–12 | 流水灯 |
| PCA LED13–15 | 面板灯 / 底灯 |
| 74HC595 SER/RCK/SCK = 21/47/48 | 双轴步进云台 |

## 优势对比

| 特性 | 传统方式 | 当前方式 |
|------|----------|----------|
| 新增工具 | 需要修改CMakeLists.txt | ✅ 零配置 |
| 文件管理 | 手动维护文件列表 | ✅ 自动发现 |
| 编译速度 | 需要重新配置 | ✅ 增量编译 |
| 维护成本 | 高 | ✅ 低 |

## 技术实现

- **自动包含**：`file(GLOB MCP_TOOL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/mcp/tools/*.cc)`
- **自动注册**：`DECLARE_MCP_TOOL`宏在编译时自动执行
- **命名空间**：`mcp_tools`命名空间隔离
- **基类设计**：统一的`McpTool`基类

## 参数类型

- `kPropertyTypeBoolean`