#ifndef ONVIF_H
#define ONVIF_H

#include <string>
#include <memory>
#include <functional>

class NetworkInterface;
class HttpClient;

class OnvifCamera {
public:
    static OnvifCamera& GetInstance();
    
    bool Initialize(const std::string& ip, int port, const std::string& username, const std::string& password);
    bool IsConnected() const { return connected_; }
    
    bool GetSnapshot(std::string& image_data);
    
    // 流式上传截图到AI服务器进行识别
    // question: 向AI提出的问题
    // explain_url: AI识别服务器URL
    // auth_token: 认证令牌（可选）
    // 返回AI服务器的分析结果
    std::string ExplainSnapshot(const std::string& question,
                               const std::string& explain_url,
                               const std::string& auth_token = "");
    
    // 流式移动（速度模式）
    bool Move(float x, float y);
    bool Stop();
    
    // 精确定位到指定角度（使用速度+时间控制）
    // pan: 水平角度（-1.0 ~ 1.0），tilt: 垂直角度（-1.0 ~ 1.0）
    // speed: 移动速度（0.0 ~ 1.0），返回是否成功
    bool MoveToAngle(float pan, float tilt, float speed);
    
    // 获取当前 PTZ 状态（位置）
    struct PTZStatus {
        float pan = 0.0f;   // 水平位置
        float tilt = 0.0f;  // 垂直位置
        float zoom = 0.0f;  // 缩放
    };
    bool GetStatus(PTZStatus& status);
    bool GetStatus();

    bool GetSnapshotUri(std::string& uri);
    bool GetSnapshotUri(std::string& uri, const std::string& profile_token);

private:
    std::string UploadToExplainServer(const std::string& question,
                                     const std::string& explain_url,
                                     const std::string& auth_token,
                                     const std::string& image_data);
    
    OnvifCamera() = default;
    ~OnvifCamera() = default;
    OnvifCamera(const OnvifCamera&) = delete;
    OnvifCamera& operator=(const OnvifCamera&) = delete;

    bool SendSoapRequest(const std::string& endpoint, const std::string& soap_action, 
                         const std::string& body, std::string& response);
    bool GetProfiles(std::string& profile_token);
    std::string BuildSoapEnvelope(const std::string& body);
    bool GetSnapshotWithDigestAuth(std::string& image_data);

    std::string ip_;
    int port_ = 80;
    std::string username_;
    std::string password_;
    std::string profile_token_ = "protoken_ch0001";
    bool connected_ = false;
    
    // 缓存的 PTZ 状态
    PTZStatus cached_ptz_status_;
    int64_t status_cache_time_ = 0;
    static constexpr int64_t STATUS_CACHE_MS = 500;  // 状态缓存有效期
};

#endif // ONVIF_H
