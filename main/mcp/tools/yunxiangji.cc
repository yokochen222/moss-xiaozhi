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
    ESP_LOGI(TAG, "register self.yunxiangji.take / ack");
    McpServer::GetInstance().AddTool(
        "self.yunxiangji.take",
        "查询待播报的云享记提醒内容。每次对话开始若消息是「请调用工具查询提醒内容」，必须先调用本工"
        "具。"
        "返回 none 表示没有待播报或已经播报过，不要向用户播报、不要寒暄。"
        "否则把返回内容完整口头播报给用户，不要改写，然后立刻调用 self.yunxiangji.ack。",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            std::string text = Application::GetInstance().PeekPendingAnnounce();
            if (text.empty()) {
                return "none";
            }
            return text;
        });

    McpServer::GetInstance().AddTool(
        "self.yunxiangji.ack",
        "标记待播报云享记为已播报。口头播报完成后必须调用。"
        "统一返回「已播报」。若本来就已播报，同样返回「已播报」，无需再向用户播报。",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            return Application::GetInstance().AckPendingAnnounce();
        });
}

void RegisterYunxiangjiTools() { YunxiangjiTool::GetInstance().Register(); }

}  // namespace mcp_tools

static auto& g_yunxiangji_tool_instance __attribute__((used)) =
    mcp_tools::YunxiangjiTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_yunxiangji_tool_instance);
