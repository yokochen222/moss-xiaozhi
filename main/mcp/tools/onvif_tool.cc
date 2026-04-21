#include "mcp_tools.h"
#include "boards/moss/onvif.h"
#include <esp_log.h>

#define TAG "OnvifTool"

namespace mcp_tools {

class OnvifTool : public McpTool {
public:
    static OnvifTool& GetInstance() {
        static OnvifTool instance;
        return instance;
    }

    OnvifTool() : McpTool("self.onvif.camera", "控制局域网内的ONVIF网络摄像头") {
    }

    void Register() override {
        ESP_LOGI(TAG, "注册ONVIF摄像头控制工具");

        McpServer::GetInstance().AddTool(
            name(),
            "ONVIF网络摄像头控制工具\n"
            "功能说明：\n"
            "- 获取摄像头截图并识别内容\n"
            "- 控制摄像头云台左右上下移动\n"
            "\n"
            "参数说明：\n"
            "- action: 操作类型\n"
            "  * 'init' - 初始化摄像头连接\n"
            "  * 'snapshot' - 获取截图并识别内容\n"
            "  * 'move' - 云台移动\n"
            "  * 'stop' - 停止云台移动\n"
            "\n"
            "- x: 水平移动值 (-1.0 到 1.0)\n"
            "  * 正值 = 向右移动\n"
            "  * 负值 = 向左移动\n"
            "  * 0 = 不移动\n"
            "\n"
            "- y: 垂直移动值 (-1.0 到 1.0)\n"
            "  * 正值 = 向下移动\n"
            "  * 负值 = 向上移动\n"
            "  * 0 = 不移动\n"
            "\n"
            "摄像头配置：\n"
            "- IP: 192.168.31.24\n"
            "- 端口: 80\n"
            "- 账号: admin\n"
            "- 密码: admin123",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("x", kPropertyTypeFloat, 0.0f, -1.0f, 1.0f),
                Property("y", kPropertyTypeFloat, 0.0f, -1.0f, 1.0f)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto action = properties["action"].value<std::string>();
                auto x = properties["x"].value<float>();
                auto y = properties["y"].value<float>();

                return OnvifAction(action, x, y);
            }
        );
    }

private:
    ReturnValue OnvifAction(const std::string& action, float x, float y) {
        auto& onvif = OnvifCamera::GetInstance();

        if (action == "init") {
            if (onvif.Initialize("192.168.31.24", 80, "admin", "admin123")) {
                return "ONVIF摄像头连接成功";
            } else {
                return "ONVIF摄像头连接失败";
            }
        } else if (action == "snapshot") {
            std::string image_data;
            if (onvif.GetSnapshot(image_data)) {
                std::string result = "截图获取成功，大小: " + std::to_string(image_data.size()) + " 字节";
                result += "\n注意: 图片内容识别需要上传到AI服务器";
                return result;
            } else {
                return "截图获取失败，请检查摄像头连接";
            }
        } else if (action == "move") {
            if (onvif.Move(x, y)) {
                std::string direction;
                if (x > 0) direction += "向右";
                else if (x < 0) direction += "向左";
                if (y > 0) {
                    if (!direction.empty()) direction += ", ";
                    direction += "向下";
                } else if (y < 0) {
                    if (!direction.empty()) direction += ", ";
                    direction += "向上";
                }
                if (direction.empty()) direction = "保持";
                return "云台移动命令已发送: " + direction;
            } else {
                return "云台移动失败";
            }
        } else if (action == "stop") {
            if (onvif.Stop()) {
                return "云台已停止";
            } else {
                return "云台停止命令失败";
            }
        } else {
            return "未知操作: " + action + "\n支持的操作: init, snapshot, move, stop";
        }
    }
};

} // namespace mcp_tools

static auto& g_onvif_tool_instance = mcp_tools::OnvifTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_onvif_tool_instance);
