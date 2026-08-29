#include <esp_log.h>
#include <string>

#include "device/face_tracker.h"
#include "mcp_tools.h"

#define TAG "FaceTrackTool"

namespace mcp_tools {

class FaceTrackTool : public McpTool {
public:
    static FaceTrackTool& GetInstance() {
        static FaceTrackTool instance;
        return instance;
    }

    FaceTrackTool()
        : McpTool("self.face_track.control",
                  "板内人脸居中跟踪（esp-dl MSR+MNP + 双轴步进云台）。默认关闭，不影响其它功能。") {
    }

    void Register() override;
};

void FaceTrackTool::Register() {
    McpServer::GetInstance().AddTool(
        name(),
        "人脸锁定跟踪控制（板内 esp-dl，不上传图像）\n"
        "- action='start'：开启跟踪，云台把最大人脸移到画面中心\n"
        "- action='stop'：关闭跟踪并释放相机/检测器\n"
        "- action='get_status'：返回 JSON（running/has_face/err_x/err_y 等）\n"
        "可与语音并行；拍照时会自动短暂暂停跟踪。",
        std::vector<Property>{
            Property("action", kPropertyTypeString),
        },
        [](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            auto& tracker = FaceTracker::GetInstance();

            if (action == "start") {
                if (tracker.Start()) {
                    return std::string("face tracking started");
                }
                return std::string("face tracking start failed");
            }
            if (action == "stop") {
                if (tracker.Stop()) {
                    return std::string("face tracking stopped");
                }
                return std::string("face tracking stop failed");
            }
            if (action == "get_status") {
                return tracker.GetStatusString();
            }
            return std::string("未知动作: ") + action + "\n支持的动作: start, stop, get_status";
        });
    ESP_LOGI(TAG, "Registered %s", name().c_str());
}

}  // namespace mcp_tools

static auto& g_face_track_tool_instance = mcp_tools::FaceTrackTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_face_track_tool_instance);
