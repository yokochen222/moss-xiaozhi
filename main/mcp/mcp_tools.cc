#include "mcp_tools.h"
#include <esp_log.h>

// 注意：所有tools/*.cc文件已通过CMake的file(GLOB)自动包含
// 无需手动包含每个工具文件，工具会自动注册

#define TAG "McpTools"

namespace mcp_tools {

// 工具注册表 - 使用函数内静态变量确保安全初始化
static std::map<std::string, std::function<McpTool*()>>& GetToolRegistry() {
    static std::map<std::string, std::function<McpTool*()>> tool_registry;
    return tool_registry;
}

void RegisterTool(const std::string& name, std::function<McpTool*()> creator) {
    auto& registry = GetToolRegistry();
    registry[name] = creator;
    ESP_LOGI(TAG, "注册MCP工具: %s", name.c_str());
}

McpTool* CreateTool(const std::string& name) {
    auto& registry = GetToolRegistry();
    auto it = registry.find(name);
    if (it != registry.end()) {
        return it->second();
    }
    ESP_LOGE(TAG, "未找到工具: %s", name.c_str());
    return nullptr;
}

void RegisterAllTools() {
    ESP_LOGI(TAG, "开始注册所有MCP工具");
    
    auto& registry = GetToolRegistry();
    // 遍历注册表，创建并注册所有工具
    for (auto& pair : registry) {
        auto tool = pair.second();
        if (tool) {
            tool->Register();
            delete tool;
        }
    }
    
    ESP_LOGI(TAG, "所有MCP工具注册完成");
}

} // namespace mcp_tools 