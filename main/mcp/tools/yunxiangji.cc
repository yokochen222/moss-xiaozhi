#include "application.h"
#include "mcp_tools.h"

#include <esp_log.h>

#define TAG "YunxiangjiTool"

namespace mcp_tools {

class YunxiangjiTool : public McpTool {
public:
    static YunxiangjiTool& GetInstance() {
        static YunxiangjiTool instance;
        return instance;
    }
    YunxiangjiTool()
        : McpTool("self.yunxiangji.take",
                  "云享记待播报。若返回有 content，必须立刻向用户原样口头播报，不要寒暄。") {}
    void Register() override;
};

void YunxiangjiTool::Register() {
    ESP_LOGI(TAG, "register self.yunxiangji.take");
    McpServer::GetInstance().AddTool(
        name(),
        "云享记待播报提醒。每次对话开始若可能有定时提醒，先调用本工具。"
        "返回 none 表示没有待办；否则把 content 完整播报给用户。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            std::string text = Application::GetInstance().TakePendingAnnounce();
            if (text.empty()) {
                return "none";
            }
            return text;
        });
}

}  // namespace mcp_tools

static auto& g_yunxiangji_tool_instance = mcp_tools::YunxiangjiTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_yunxiangji_tool_instance);
