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
                "Web端代码内容展示工具\n"
                "使用说明：\n"
                "1. 在调用本工具前，必须先调用'打开浏览器聊天窗口'MCP工具，确保窗口已成功启动，否则无法展示内容。\n"
                "2. 如果浏览器聊天窗口未成功启动，则无需调用本工具，且代码内容无需朗读。\n"
                "参数说明：\n"
                "- content：要展示的代码内容，必须为markdown格式，才能正常渲染。\n"
                ,
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