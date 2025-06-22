#include "mcp_tools.h"
#include "extend/chat_web_server/web_server.h"
#include <esp_log.h>
#include <string>

namespace mcp_tools
{

    class CodeDisplay : public McpTool
    {

    public:
        CodeDisplay() : McpTool("code_display", "代码展示工具") {}
        static CodeDisplay &GetInstance()
        {
            static CodeDisplay instance;
            return instance;
        }

        void Register() override
        {
            ESP_LOGI("CodeDisplay", "注册代码展示工具");
            McpServer::GetInstance().AddTool(
                name(),
                "用于在Web端展示代码内容\n"
                "在调用此工具前必须先调用 打开浏览器聊天窗口 MCP工具启动窗口才能展示内容出来\n"
                "注意：如果打开浏览器聊天窗口 MCP工具启动失败，则不能调用此工具，如果调用了此工具代码内容无需朗读出来\n",
                PropertyList({
                    Property("content", kPropertyTypeString),
                }),
                [this](const PropertyList& properties) -> ReturnValue {
                    std::string content = properties["content"].value<std::string>();
                    if (!content.empty()) {
                        forward_chat_message("moss", content.c_str(), "markdown");
                        return "已推送到Web端";
                    } else {
                        return "内容为空";
                    }
                }
            );
        }
    };

} // namespace mcp_tools

static auto& g_code_display = mcp_tools::CodeDisplay::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_code_display);