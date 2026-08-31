#ifndef MCP_TOOLS_H
#define MCP_TOOLS_H

#include <functional>
#include <map>
#include <string>
#include <vector>
#include "mcp_server.h"

// 通用 MCP 在 mcp/tools/；板级 MCP 在 boards/<board>/mcp/

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
void RegisterYunxiangjiTools();

// 工具注册宏
#define DECLARE_MCP_TOOL(TypeName)                                                      \
    static mcp_tools::McpTool* Create##TypeName() { return new mcp_tools::TypeName(); } \
    static bool Register##TypeNameHelper = []() {                                       \
        RegisterTool(#TypeName, Create##TypeName);                                      \
        return true;                                                                    \
    }();

// 单例对象注册宏
#define DECLARE_MCP_TOOL_INSTANCE(instance)         \
    static bool Register##instance##Helper = []() { \
        (instance).Register();                      \
        return true;                                \
    }();

}  // namespace mcp_tools

#endif  // MCP_TOOLS_H