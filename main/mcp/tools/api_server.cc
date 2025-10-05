#include "mcp_tools.h"
#include "api/api.h"
#include <esp_log.h>
#include <cJSON.h>

#define TAG "ApiServerTool"

namespace mcp_tools {

class ApiServerTool : public McpTool {
private:
    ApiServer& api_server_;

public:
    static ApiServerTool& GetInstance() {
        static ApiServerTool instance;
        return instance;
    }
    
    ApiServerTool() : McpTool("self.api_server.control", "控制HTTP API服务器的启停和状态查询"), 
                      api_server_(ApiServer::GetInstance()) {
    }

    void Register() override {
        ESP_LOGI(TAG, "注册API服务器控制工具");
        
        McpServer::GetInstance().AddTool(
            name(),
            "HTTP API服务器控制工具\n"
            "功能说明：\n"
            "1. 启动API服务器：action='start'，可选port参数指定端口（默认5500）\n"
            "2. 停止API服务器：action='stop'\n"
            "3. 查询服务器状态：action='status'\n"
            "API服务器提供以下接口：\n"
            "- POST /ir/send：发送红外指令\n"
            "- GET /ir/read：读取接收到的红外数据\n",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("port", kPropertyTypeInteger, 5500)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto action = properties["action"].value<std::string>();
                auto port = properties["port"].value<int>();
                
                if (action == "start") {
                    bool success = api_server_.Start(port);
                    
                    if (success) {
                        return "API服务器启动成功，端口: " + std::to_string(port);
                    } else {
                        return "API服务器启动失败";
                    }
                    
                } else if (action == "stop") {
                    api_server_.Stop();
                    return "API服务器已停止";
                    
                } else if (action == "status") {
                    bool is_running = api_server_.IsRunning();
                    int current_port = api_server_.GetPort();
                    
                    if (is_running) {
                        return "API服务器正在运行，端口: " + std::to_string(current_port);
                    } else {
                        return "API服务器未运行";
                    }
                    
                } else {
                    return "无效的操作: " + action + "。有效操作: start, stop, status";
                }
            }
        );
    }
};

} // namespace mcp_tools

static auto& g_api_server_tool_instance = mcp_tools::ApiServerTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_api_server_tool_instance);