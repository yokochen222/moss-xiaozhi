#include "mcp_tools.h"
#include <esp_log.h>
#include "extend/chat_web_server/web_server.h"
#include <cstring>
#include <string>

#define TAG "OpenBrowserChatWinTool"

namespace mcp_tools
{

    class OpenBrowserChatWin : public McpTool
    {

    public:
        OpenBrowserChatWin() : McpTool("open_browser_chat_win", "打开浏览器聊天窗口") {}
        static OpenBrowserChatWin &GetInstance()
        {
            static OpenBrowserChatWin instance;
            return instance;
        }
        void Register() override;
    };

    void OpenBrowserChatWin::Register()
    {
        ESP_LOGI(TAG, "注册用于展示用户与MOSS的对话框工具");
        McpServer::GetInstance().AddTool(
            name(),
            "当用户需要在PC电脑浏览器上展示聊天对话框时或者要现实代码时可以调用此工具来展示\n"
            "注意：调用此工具时应该判断 已经注册的MCP工具列表中是否拥有打开浏览器并访问指定网址的能力\n"
            "如果有打开浏览器并且能访问指定网址时 调用了此工具将会返回聊天内容的网址\n"
            "返回网址后你应当调用浏览器打开改网址页面\n",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                const char* ip = get_local_ip();
                std::string url;
                if (ip && std::strlen(ip) > 0 && std::strcmp(ip, "0.0.0.0") != 0) {
                    url = std::string("http://") + ip + ":9000/";
                    return url;
                } else {
                    return std::string("服务器地址获取失败");
                }
            }
        );
    }

} // namespace mcp_tools

static auto& g_open_browser_chat_win = mcp_tools::OpenBrowserChatWin::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_open_browser_chat_win);