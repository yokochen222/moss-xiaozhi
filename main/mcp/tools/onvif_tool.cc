#include "mcp_tools.h"
#include "boards/moss/onvif.h"
#include "settings.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <cJSON.h>

#define TAG "OnvifTool"
#define ONVIF_NS "onvif_camera"

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
            "- 保存摄像头配置到设备\n"
            "- 获取摄像头截图并上传到AI服务器识别内容\n"
            "参数说明：\n"
            "- action: 操作类型\n"
            "  * 'save_camera_config' - 保存摄像头配置到设备存储\n"
            "  * 'snapshot' - 获取摄像头截图并上传到AI服务器识别内容\n"
            "- ip: 摄像头IP地址（可选）\n"
            "- port: 摄像头端口号（可选，默认80）\n"
            "- username: 摄像头用户名（可选）\n"
            "- password: 摄像头密码（可选）\n"
            "- question: 向AI提出的问题（可选，默认：请描述这张图片的内容）\n",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("ip", kPropertyTypeString, ""),
                Property("port", kPropertyTypeInteger, 80),
                Property("username", kPropertyTypeString, ""),
                Property("password", kPropertyTypeString, ""),
                Property("question", kPropertyTypeString, "请描述这张图片的内容")
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto question = properties["question"].value<std::string>();
                auto action = properties["action"].value<std::string>();
                auto ip = properties["ip"].value<std::string>();
                auto port = properties["port"].value<int>();
                auto username = properties["username"].value<std::string>();
                auto password = properties["password"].value<std::string>();
                return OnvifAction(question, action, ip, port, username, password);
            }
        );
    }

private:
    ReturnValue OnvifAction(const std::string& question, const std::string& action,
                           const std::string& ip, int port, const std::string& username,
                           const std::string& password) {

        if (action.empty()) {
            return "操作类型不能为空";
        }

        auto& onvif = OnvifCamera::GetInstance();

        // 存储摄像头 ip 端口 账号 和密码到 nvs
        if (action == "save_camera_config") {
            if (ip.empty() || username.empty() || password.empty()) {
                return "保存摄像头配置失败：ip、用户名和密码不能为空";
            }
            if (port <= 0 || port > 65535) {
                return "保存摄像头配置失败：端口号无效";
            }

            Settings settings(ONVIF_NS, true);
            settings.SetString("ip", ip);
            settings.SetInt("port", port);
            settings.SetString("username", username);
            settings.SetString("password", password);

            ESP_LOGI(TAG, "ONVIF摄像头配置已保存: %s:%d", ip.c_str(), port);

            // 重新初始化摄像头连接
            onvif.Initialize(ip, port, username, password);
            if (onvif.IsConnected()) {
                return "摄像头配置保存成功并已连接";
            } else {
                return "摄像头配置保存成功，但连接失败，请检查网络和配置";
            }
        }

        // 从nvs读取摄像头配置信息并校验完整性
        Settings onvif_settings(ONVIF_NS, false);
        std::string saved_ip = onvif_settings.GetString("ip");
        int saved_port = onvif_settings.GetInt("port", 0);
        std::string saved_username = onvif_settings.GetString("username");
        std::string saved_password = onvif_settings.GetString("password");

        // 如果有传入参数且已连接，使用传入参数
        if (!ip.empty() && !username.empty() && !password.empty()) {
            if (!onvif.IsConnected()) {
                onvif.Initialize(ip, port, username, password);
            }
        } else if (!saved_ip.empty() && !saved_username.empty() && !saved_password.empty()) {
            // 使用保存的配置
            if (!onvif.IsConnected()) {
                onvif.Initialize(saved_ip, saved_port, saved_username, saved_password);
            }
        } else {
            // 没有配置信息
            return "摄像头未配置，请先调用 save_camera_config 保存摄像头配置";
        }

        if (!onvif.IsConnected()) {
            return "ONVIF摄像头连接失败，请检查网络和配置";
        }
      
        if (action == "snapshot") {
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
        } 
        return "未知操作: 支持的操作: save_camera_config, snapshot";
    }
};

} // namespace mcp_tools

static auto& g_onvif_tool_instance = mcp_tools::OnvifTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_onvif_tool_instance);
