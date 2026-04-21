#include "mcp_tools.h"
#include "boards/moss/onvif.h"
#include <esp_log.h>
#include <cJSON.h>

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
            "- 获取摄像头截图并上传到AI服务器识别内容\n"
            "- 控制摄像头云台左右上下移动\n"
            "- 精确定位到指定角度\n"
            "- 获取当前云台位置\n"
            "\n"
            "参数说明：\n"
            "- action: 操作类型\n"
            "  * 'init' - 初始化摄像头连接\n"
            "  * 'snapshot' - 获取截图并上传到AI服务器识别\n"
            "  * 'move' - 云台持续移动（需配合 stop 停止）\n"
            "  * 'stop' - 停止云台移动\n"
            "  * 'status' - 获取当前云台位置\n"
            "  * 'move_to' - 精确定位到指定角度\n"
            "\n"
            "- x: 水平移动值 (-1.0 到 1.0)，move 时有效\n"
            "  * 正值 = 向右移动\n"
            "  * 负值 = 向左移动\n"
            "  * 0 = 不移动\n"
            "\n"
            "- y: 垂直移动值 (-1.0 到 1.0)，move 时有效\n"
            "  * 正值 = 向下移动\n"
            "  * 负值 = 向上移动\n"
            "  * 0 = 不移动\n"
            "\n"
            "- pan: 目标水平角度 (-1.0 到 1.0)，move_to 时有效\n"
            "- tilt: 目标垂直角度 (-1.0 到 1.0)，move_to 时有效\n"
            "- speed: 移动速度 (0.1 到 1.0)，move_to 时有效，默认 0.5\n"
            "\n"
            "- question: 向AI提出的问题（可选，默认：请描述这张图片的内容）\n"
            "\n"
            "摄像头配置：\n"
            "- IP: 192.168.31.24\n"
            "- 端口: 80\n"
            "- 账号: admin\n"
            "- 密码: admin123",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("x", kPropertyTypeFloat, 0.0f, -1.0f, 1.0f),
                Property("y", kPropertyTypeFloat, 0.0f, -1.0f, 1.0f),
                Property("pan", kPropertyTypeFloat, 0.0f, -1.0f, 1.0f),
                Property("tilt", kPropertyTypeFloat, 0.0f, -1.0f, 1.0f),
                Property("speed", kPropertyTypeFloat, 0.5f, 0.1f, 1.0f),
                Property("question", kPropertyTypeString, "请描述这张图片的内容")
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto action = properties["action"].value<std::string>();
                auto x = properties["x"].value<float>();
                auto y = properties["y"].value<float>();
                auto pan = properties["pan"].value<float>();
                auto tilt = properties["tilt"].value<float>();
                auto speed = properties["speed"].value<float>();
                auto question = properties["question"].value<std::string>();

                return OnvifAction(action, x, y, pan, tilt, speed, question);
            }
        );
    }

private:
    ReturnValue OnvifAction(const std::string& action, float x, float y,
                           float pan, float tilt, float speed, const std::string& question) {
        auto& onvif = OnvifCamera::GetInstance();

        if (action == "init") {
            if (onvif.Initialize("192.168.31.24", 80, "admin", "admin123")) {
                return "ONVIF摄像头连接成功";
            } else {
                return "ONVIF摄像头连接失败";
            }
        } else if (action == "snapshot") {
            // 获取AI服务器URL和token
            auto& mcp_server = McpServer::GetInstance();
            auto explain_url = mcp_server.GetExplainUrl();
            auto explain_token = mcp_server.GetExplainToken();
            
            if (explain_url.empty()) {
                // 如果没有配置AI服务器，只返回截图信息
                std::string image_data;
                if (onvif.GetSnapshot(image_data)) {
                    return "截图获取成功，大小: " + std::to_string(image_data.size()) + " 字节\n注意: 需要配置AI服务器才能识别图片内容";
                } else {
                    return "截图获取失败，请检查摄像头连接";
                }
            }
            
            // 调用流式上传并识别
            std::string result = onvif.ExplainSnapshot(question, explain_url, explain_token);
            
            // 解析JSON结果
            cJSON* json = cJSON_Parse(result.c_str());
            if (json) {
                cJSON* success = cJSON_GetObjectItem(json, "success");
                if (success && cJSON_IsTrue(success)) {
                    cJSON* res = cJSON_GetObjectItem(json, "result");
                    if (res && cJSON_IsString(res)) {
                        std::string answer = std::string(res->valuestring);
                        cJSON_Delete(json);
                        return "图片识别结果: " + answer;
                    }
                }
                cJSON* message = cJSON_GetObjectItem(json, "message");
                if (message && cJSON_IsString(message)) {
                    std::string error_msg = std::string(message->valuestring);
                    cJSON_Delete(json);
                    return "图片识别失败: " + error_msg;
                }
                cJSON_Delete(json);
            }
            
            return "图片识别结果解析失败: " + result;
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
        } else if (action == "status") {
            OnvifCamera::PTZStatus status;
            if (onvif.GetStatus(status)) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "当前云台位置:\n"
                    "- 水平角度 (pan): %.2f\n"
                    "- 垂直角度 (tilt): %.2f\n"
                    "- 缩放 (zoom): %.2f",
                    status.pan, status.tilt, status.zoom);
                return std::string(buf);
            } else {
                return "获取云台状态失败";
            }
        } else if (action == "move_to") {
            // 限制速度范围
            float move_speed = std::max(0.1f, std::min(1.0f, speed));
            if (onvif.MoveToAngle(pan, tilt, move_speed)) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "云台已移动到指定位置:\n"
                    "- 目标水平角度 (pan): %.2f\n"
                    "- 目标垂直角度 (tilt): %.2f\n"
                    "- 移动速度: %.2f",
                    pan, tilt, move_speed);
                return std::string(buf);
            } else {
                return "精确定位失败";
            }
        } else {
            return "未知操作: " + action + "\n支持的操作: init, snapshot, move, stop, status, move_to";
        }
    }
};

} // namespace mcp_tools

static auto& g_onvif_tool_instance = mcp_tools::OnvifTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_onvif_tool_instance);
