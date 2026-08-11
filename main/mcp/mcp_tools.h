#ifndef MCP_TOOLS_H
#define MCP_TOOLS_H

#include "mcp_server.h"
#include <string>
#include <vector>
#include <functional>
#include <map>

// 所有单独的工具实现都放在tools/子文件夹

namespace mcp_tools {

// MCP工具基类
class McpTool {
public:
    McpTool(const std::string& name, const std::string& description) 
        : name_(name), description_(description) {}
    virtual ~McpTool() = default;

    virtual void Register() = 0;
    
    const std::string& name() const { return name_; }
    const std::string& description() const { return description_; }

protected:
    std::string name_;
    std::string description_;
};

// 工具注册函数
void RegisterTool(const std::string& name, std::function<McpTool*()> creator);
McpTool* CreateTool(const std::string& name);

// 注册所有工具
void RegisterAllTools();

// 工具注册宏
#define DECLARE_MCP_TOOL(TypeName) \
    static mcp_tools::McpTool* Create##TypeName() { \
        return new mcp_tools::TypeName(); \
    } \
    static bool Register##TypeNameHelper = []() { \
        RegisterTool(#TypeName, Create##TypeName); \
        return true; \
    }();

// 单例对象注册宏
#define DECLARE_MCP_TOOL_INSTANCE(instance) \
    static bool Register##instance##Helper = []() { \
        (instance).Register(); \
        return true; \
    }();

} // namespace mcp_tools

#endif // MCP_TOOLS_H 