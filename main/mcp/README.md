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

- **LED控制工具** (`self.led.set_status`) - 控制LED开关
- **系统信息工具** (`self.system.get_info`) - 获取系统信息
- **测试工具** (`self.test.echo`) - 回显测试消息
- **网络工具** (`self.network.get_status`) - 获取网络状态
- **眼部灯光工具** (`self.lamp_eye.control`) - 控制MOSS眼部灯光

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