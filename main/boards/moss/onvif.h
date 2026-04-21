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
    bool Move(float x, float y);
    bool Stop();

private:
    OnvifCamera() = default;
    ~OnvifCamera() = default;
    OnvifCamera(const OnvifCamera&) = delete;
    OnvifCamera& operator=(const OnvifCamera&) = delete;

    bool SendSoapRequest(const std::string& endpoint, const std::string& soap_action, 
                         const std::string& body, std::string& response);
    bool GetSnapshotUri(std::string& uri);
    bool GetProfiles(std::string& profile_token);
    std::string BuildSoapEnvelope(const std::string& body);

    std::string ip_;
    int port_ = 80;
    std::string username_;
    std::string password_;
    std::string profile_token_ = "protoken_ch0001";
    bool connected_ = false;
};

#endif // ONVIF_H
