# MCP Utils 目录

这个目录包含了MCP工具使用的通用驱动和工具类，这些组件可以被多个MCP工具共享使用。

## 当前包含的组件

### 74HC595 移位寄存器驱动
- **文件**: `74hc595_driver.h` / `74hc595_driver.cc`
- **功能**: 提供74HC595移位寄存器的GPIO控制和数据发送功能
- **用途**: 流水灯控制、LED矩阵控制、GPIO扩展等
- **特性**: 
  - 支持8位数据输出
  - 自动GPIO配置
  - 时序优化
  - 错误处理

## 使用方式

### 在其他MCP工具中使用
```cpp
#include "../utils/74hc595_driver.h"

// 创建驱动实例
ShiftRegister74HC595 shift_reg(GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11);
shift_reg.Initialize();

// 使用驱动功能
shift_reg.SetOutputs(0b10101010);
```

### 在板级代码中使用
```cpp
#include "mcp/utils/74hc595_driver.h"

// 创建驱动实例
ShiftRegister74HC595 shift_reg(GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11);
shift_reg.Initialize();
```

## 目录结构
```
mcp/utils/
├── README.md              # 本说明文件
├── 74hc595_driver.h      # 74HC595驱动头文件
└── 74hc595_driver.cc     # 74HC595驱动实现文件
```

## 添加新组件

当需要添加新的通用驱动或工具类时：

1. 将头文件(.h)和实现文件(.cc)放在此目录
2. 更新本README文件，说明新组件的功能和使用方法
3. 确保新组件遵循项目的编码规范
4. 添加适当的错误处理和日志记录

## 注意事项

- 此目录下的组件应该具有良好的封装性
- 避免在此目录下放置特定于某个工具的代码
- 所有组件都应该有适当的错误处理
- 遵循项目的命名规范和代码风格
