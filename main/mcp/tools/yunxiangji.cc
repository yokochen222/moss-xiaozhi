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
                  "查询待播报的云享记提醒。若返回 none 表示没有待办或已播报，不要播报。") {}
    void Register() override;
};

void YunxiangjiTool::Register() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    ESP_LOGI(TAG, "register self.yunxiangji.take / ack");
    McpServer::GetInstance().AddTool(
        "self.yunxiangji.take",
        "查询待播报的云享记提醒内容。每次对话开始若消息是「请调用工具查询提醒内容」，必须先调用本工"
        "具，且整轮只调用一次。"
        "返回 none 表示没有待播报或已经播报过，不要向用户播报、不要寒暄、不要重复查询、不要再说话。"
        "否则把返回内容完整口头播报给用户恰好一次，不要改写、不要再说第二遍，然后立刻调用 "
        "self.yunxiangji.ack。ack 之后禁止再播报同一句话。",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            std::string text = Application::GetInstance().TakePendingAnnounce();
            if (text.empty()) {
                return "none";
            }
            return text;
        });

    McpServer::GetInstance().AddTool(
        "self.yunxiangji.ack",
        "标记待播报云享记为已播报。口头播报一次后必须立刻调用。"
        "返回「已播报」。调用后不要再向用户说话、不要重复同一句、不要寒暄，直接结束本轮。",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            return Application::GetInstance().AckPendingAnnounce();
        });
}

void RegisterYunxiangjiTools() { YunxiangjiTool::GetInstance().Register(); }

}  // namespace mcp_tools

static auto& g_yunxiangji_tool_instance __attribute__((used)) =
    mcp_tools::YunxiangjiTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_yunxiangji_tool_instance);
