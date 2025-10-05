# API Methods 模块化架构

## 概述

API Methods 模块提供了可扩展的API接口架构，将不同类型的API接口按功能模块化分离，便于维护和扩展。

## 目录结构

```
api/
├── api.h                    # API服务器主头文件
├── api.cc                   # API服务器主实现文件
├── README.md               # API模块说明文档
└── methods/                # API方法模块目录
    ├── README.md           # 本文件
    └── ir/                 # 红外相关API模块
        ├── ir_handlers.h   # 红外处理器头文件
        ├── ir_handlers.cc  # 红外处理器实现
        ├── ir_data_manager.h  # 红外数据管理器头文件
        └── ir_data_manager.cc # 红外数据管理器实现
```

## 设计原则

### 1. 模块化分离
- 每个功能模块独立目录
- 处理器和数据管理器分离
- 清晰的命名空间隔离

### 2. 可扩展性
- 新增模块只需创建新目录
- 统一的接口设计模式
- 自动化的CMake构建支持

### 3. 代码复用
- 共享的辅助函数
- 统一的数据管理接口
- 一致的错误处理机制

## 模块规范

### 命名规范
- 模块目录：小写字母，如 `ir`, `audio`, `display`
- 头文件：`{module}_handlers.h`, `{module}_data_manager.h`
- 实现文件：`{module}_handlers.cc`, `{module}_data_manager.cc`
- 命名空间：`api_methods::{module}`

### 文件结构规范

#### 处理器文件 (`{module}_handlers.h/cc`)
- 实现HTTP请求处理逻辑
- 包含JSON解析和响应构造
- 调用相应的数据管理器

#### 数据管理器文件 (`{module}_data_manager.h/cc`)
- 实现数据存储和管理逻辑
- 提供线程安全的数据访问
- 单例模式确保唯一实例

### 接口规范

#### 处理器接口
```cpp
namespace api_methods {
namespace {module} {

esp_err_t Handle{Action}(httpd_req_t *req);

} // namespace {module}
} // namespace api_methods
```

#### 数据管理器接口
```cpp
namespace api_methods {
namespace {module} {

class {Module}DataManager {
public:
    static {Module}DataManager& GetInstance();
    void Add{Type}Data(const std::string& data);
    std::vector<std::string> Get{Type}Data();
    void Clear{Type}Data();
};

} // namespace {module}
} // namespace api_methods
```

## 添加新模块

### 1. 创建模块目录
```bash
mkdir -p api/methods/{new_module}
```

### 2. 创建处理器文件
```cpp
// api/methods/{new_module}/{new_module}_handlers.h
#pragma once
#include <esp_err.h>
#include <esp_http_server.h>

namespace api_methods {
namespace {new_module} {
    esp_err_t Handle{Action}(httpd_req_t *req);
} // namespace {new_module}
} // namespace api_methods
```

### 3. 创建数据管理器文件
```cpp
// api/methods/{new_module}/{new_module}_data_manager.h
#pragma once
#include <string>
#include <vector>
#include <mutex>

namespace api_methods {
namespace {new_module} {
    class {NewModule}DataManager {
    public:
        static {NewModule}DataManager& GetInstance();
        // 添加必要的数据管理方法
    };
} // namespace {new_module}
} // namespace api_methods
```

### 4. 更新CMakeLists.txt
```cmake
# 添加新的源文件
"api/methods/{new_module}/{new_module}_handlers.cc"
"api/methods/{new_module}/{new_module}_data_manager.cc"
```

### 5. 在主API服务器中注册路由
```cpp
// 在 api.cc 的 Start() 函数中添加
httpd_uri_t {new_module}_uri = {
    .uri = "/{new_module}/{action}",
    .method = HTTP_POST, // 或 GET
    .handler = api_methods::{new_module}::Handle{Action},
    .user_ctx = this
};
httpd_register_uri_handler((httpd_handle_t)server_, &{new_module}_uri);
```

## 现有模块

### 红外模块 (`ir`)
- **功能**：红外指令发送和接收
- **接口**：
  - `POST /ir/send` - 发送红外指令
  - `GET /ir/read` - 读取接收到的红外数据
- **数据管理**：单条数据存储，自动清理换行符

## 最佳实践

1. **错误处理**：使用统一的错误响应格式
2. **日志记录**：每个模块使用独立的TAG
3. **内存管理**：及时释放cJSON对象
4. **线程安全**：使用互斥锁保护共享数据
5. **性能优化**：避免不必要的字符串拷贝

## 示例：添加音频模块

```cpp
// api/methods/audio/audio_handlers.cc
namespace api_methods {
namespace audio {

esp_err_t HandlePlayAudio(httpd_req_t *req) {
    // 解析请求
    std::string body = ParseRequestBody(req);
    cJSON* json = cJSON_Parse(body.c_str());
    
    // 获取音频文件路径
    cJSON* audio_path = cJSON_GetObjectItem(json, "audio_path");
    std::string path = audio_path->valuestring;
    
    // 调用音频设备播放
    bool success = AudioDevice::GetInstance().PlayAudio(path);
    
    // 构造响应
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", success ? "success" : "error");
    cJSON_AddStringToObject(response, "message", success ? "Audio playing" : "Failed to play audio");
    
    // 发送响应...
}

} // namespace audio
} // namespace api_methods
```

这种模块化架构使得API服务器易于维护和扩展，每个模块职责清晰，代码复用性高。
