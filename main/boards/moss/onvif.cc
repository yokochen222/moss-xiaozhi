#include "onvif.h"
#include "board.h"
#include "system_info.h"
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <cstdlib>
#include <sstream>
#include <iomanip>

#define TAG "OnvifCamera"

static std::string Base64Encode(const std::string& data) {
    size_t dlen = 0, olen = 0;
    mbedtls_base64_encode((unsigned char*)nullptr, 0, &dlen, (const unsigned char*)data.data(), data.size());
    std::string result(dlen, 0);
    mbedtls_base64_encode((unsigned char*)result.data(), result.size(), &olen, (const unsigned char*)data.data(), data.size());
    return result;
}

static std::string Md5Hash(const std::string& input) {
    unsigned char hash[16];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    mbedtls_md_setup(&ctx, info, 0);
    mbedtls_md_update(&ctx, (const unsigned char*)input.data(), input.size());
    mbedtls_md_finish(&ctx, hash);
    mbedtls_md_free(&ctx);
    char hex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    hex[32] = '\0';
    return std::string(hex);
}

// Digest认证数据
struct DigestAuth {
    std::string realm;
    std::string nonce;
    std::string qop;
    std::string opaque;
};

static std::string ExtractAuthHeader(const std::string& header_value) {
    // 解析 WWW-Authenticate: Digest ...
    std::string auth_header = header_value;
    if (auth_header.find("Digest") == std::string::npos) {
        return "";
    }
    return auth_header.substr(7); // 去掉 "Digest " 前缀
}

static bool ParseDigestParams(const std::string& params, DigestAuth& auth) {
    // 解析 realm, nonce, qop, opaque 等参数
    size_t pos = 0;
    std::string remaining = params;
    
    while (pos < remaining.size()) {
        // 跳过空格和逗号
        while (pos < remaining.size() && (remaining[pos] == ' ' || remaining[pos] == ',')) pos++;
        if (pos >= remaining.size()) break;
        
        // 查找等号
        size_t eq_pos = remaining.find('=', pos);
        if (eq_pos == std::string::npos) break;
        
        std::string key = remaining.substr(pos, eq_pos - pos);
        // 去掉可能的引号
        size_t value_start = eq_pos + 1;
        while (value_start < remaining.size() && remaining[value_start] == ' ') value_start++;
        
        char quote = remaining[value_start];
        size_t value_end;
        if (quote == '"') {
            value_start++;
            value_end = remaining.find('"', value_start);
            if (value_end == std::string::npos) break;
        } else {
            value_end = remaining.find_first_of(", \r\n", value_start);
            if (value_end == std::string::npos) value_end = remaining.size();
        }
        
        std::string value = remaining.substr(value_start, value_end - value_start);
        
        if (key == "realm") auth.realm = value;
        else if (key == "nonce") auth.nonce = value;
        else if (key == "qop") auth.qop = value;
        else if (key == "opaque") auth.opaque = value;
        
        pos = value_end + 1;
    }
    
    return !auth.realm.empty() && !auth.nonce.empty();
}

static std::string BuildDigestAuthHeader(const std::string& username, const std::string& password,
                                         const std::string& method, const std::string& uri,
                                         const DigestAuth& auth) {
    std::string nc = "00000001";
    
    // 生成cnonce
    char cnonce[33];
    uint32_t rand1 = esp_random();
    uint32_t rand2 = esp_random();
    snprintf(cnonce, sizeof(cnonce), "%08lx%08lx", (unsigned long)rand1, (unsigned long)rand2);
    
    std::string A1 = username + ":" + auth.realm + ":" + password;
    std::string A2 = method + ":" + uri;
    
    std::string HA1 = Md5Hash(A1);
    std::string HA2 = Md5Hash(A2);
    
    std::string response = Md5Hash(HA1 + ":" + auth.nonce + ":" + nc + ":" + cnonce + ":" + auth.qop + ":" + HA2);
    
    std::ostringstream header;
    header << "Digest ";
    header << "username=\"" << username << "\", ";
    header << "realm=\"" << auth.realm << "\", ";
    header << "nonce=\"" << auth.nonce << "\", ";
    header << "uri=\"" << uri << "\", ";
    header << "cnonce=\"" << cnonce << "\", ";
    header << "nc=" << nc << ", ";
    header << "qop=" << auth.qop << ", ";
    header << "response=\"" << response << "\"";
    if (!auth.opaque.empty()) {
        header << ", opaque=\"" << auth.opaque << "\"";
    }
    
    return header.str();
}

static const char* SOAP_ENVELOPE_TEMPLATE = R"(<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope" xmlns:ptz="http://www.onvif.org/ver20/ptz/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <soap:Header/>
  <soap:Body>
%s
  </soap:Body>
</soap:Envelope>)";

static const char* DEVICE_SERVICE_ENDPOINT = "/onvif/device_service";
static const char* PTZ_SERVICE_ENDPOINT = "/onvif/ptz_service";
static const char* SNAPSHOT_ENDPOINT = "/onvif/getsnapshot/?channel=2";

OnvifCamera& OnvifCamera::GetInstance() {
    static OnvifCamera instance;
    return instance;
}

std::string OnvifCamera::BuildSoapEnvelope(const std::string& body) {
    char envelope[4096];
    snprintf(envelope, sizeof(envelope), SOAP_ENVELOPE_TEMPLATE, body.c_str());
    return std::string(envelope);
}

bool OnvifCamera::Initialize(const std::string& ip, int port, const std::string& username, const std::string& password) {
    ip_ = ip;
    port_ = port;
    username_ = username;
    password_ = password;
    
    ESP_LOGI(TAG, "ONVIF camera initializing: %s:%d, user: %s", ip_.c_str(), port_, username_.c_str());
    
    std::string response;
    std::string body = R"(<GetDeviceInformation xmlns="http://www.onvif.org/ver10/device/wsdl"/>)";
    
    if (SendSoapRequest(DEVICE_SERVICE_ENDPOINT, 
                        "http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation",
                        BuildSoapEnvelope(body), response)) {
        if (response.find("GetDeviceInformationResponse") != std::string::npos) {
            connected_ = true;
            ESP_LOGI(TAG, "ONVIF camera connected successfully");
            return true;
        }
    }
    
    ESP_LOGE(TAG, "Failed to connect to ONVIF camera");
    connected_ = false;
    return false;
}

bool OnvifCamera::SendSoapRequest(const std::string& endpoint, const std::string& soap_action,
                                   const std::string& body, std::string& response) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network not available");
        return false;
    }

    auto http = network->CreateHttp(3);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }

    // 设置超时为10秒
    http->SetTimeout(10000);

    std::string url = "http://" + ip_ + ":" + std::to_string(port_) + endpoint;
    ESP_LOGI(TAG, "Sending SOAP request to: %s", url.c_str());
    
    http->SetHeader("Content-Type", "application/soap+xml; charset=utf-8");
    http->SetHeader("SOAPAction", soap_action);

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open connection to %s", url.c_str());
        return false;
    }

    http->Write(body.c_str(), body.size());
    http->Write("", 0);

    int status = http->GetStatusCode();
    
    if (status == 401) {
        // 需要Digest认证，先获取WWW-Authenticate头
        std::string auth_header = http->GetResponseHeader("WWW-Authenticate");
        ESP_LOGI(TAG, "Got 401, WWW-Authenticate: %s", auth_header.c_str());
        http->Close();
        
        // 等待连接完全关闭
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // 解析认证参数
        std::string auth_params = ExtractAuthHeader(auth_header);
        DigestAuth digest_auth;
        if (ParseDigestParams(auth_params, digest_auth)) {
            ESP_LOGI(TAG, "Parsed digest auth: realm=%s, nonce=%s, qop=%s", 
                     digest_auth.realm.c_str(), digest_auth.nonce.c_str(), digest_auth.qop.c_str());
            
            // 重新建立连接并添加Digest认证
            http = network->CreateHttp(3);
            http->SetTimeout(10000);
            
            std::string digest_header = BuildDigestAuthHeader(username_, password_, "POST", endpoint, digest_auth);
            http->SetHeader("Content-Type", "application/soap+xml; charset=utf-8");
            http->SetHeader("SOAPAction", soap_action);
            http->SetHeader("Authorization", digest_header);
            
            if (!http->Open("POST", url)) {
                ESP_LOGE(TAG, "Failed to open connection for digest auth");
                return false;
            }
            
            http->Write(body.c_str(), body.size());
            http->Write("", 0);
            
            status = http->GetStatusCode();
            ESP_LOGI(TAG, "Digest auth HTTP status: %d", status);
        }
    }
    
    response = http->ReadAll();
    http->Close();

    return status == 200;
}

bool OnvifCamera::GetProfiles(std::string& profile_token) {
    std::string response;
    std::string body = R"(<GetProfiles xmlns="http://www.onvif.org/ver10/media/wsdl"/>)";
    
    if (SendSoapRequest(DEVICE_SERVICE_ENDPOINT,
                        "http://www.onvif.org/ver10/media/wsdl/GetProfiles",
                        BuildSoapEnvelope(body), response)) {
        auto pos = response.find("protoken_ch0001");
        if (pos != std::string::npos) {
            profile_token = "protoken_ch0001";
            return true;
        }
    }
    
    return false;
}

bool OnvifCamera::GetSnapshotUri(std::string& uri) {
    std::string response;
    char body[512];
    snprintf(body, sizeof(body), 
        R"(<trt:GetSnapshotUri xmlns:trt="http://www.onvif.org/ver10/media/wsdl">
          <trt:ProfileToken>%s</trt:ProfileToken>
        </trt:GetSnapshotUri>)", profile_token_.c_str());
    
    if (SendSoapRequest(DEVICE_SERVICE_ENDPOINT,
                        "http://www.onvif.org/ver10/media/wsdl/GetSnapshotUri",
                        BuildSoapEnvelope(body), response)) {
        auto start = response.find("<tt:Uri>");
        auto end = response.find("</tt:Uri>");
        if (start != std::string::npos && end != std::string::npos) {
            uri = response.substr(start + 8, end - start - 8);
            return true;
        }
    }
    
    uri = "http://" + ip_ + ":" + std::to_string(port_) + SNAPSHOT_ENDPOINT;
    return true;
}

bool OnvifCamera::GetSnapshot(std::string& image_data) {
    if (ip_.empty()) {
        ESP_LOGE(TAG, "Camera IP is empty, please call Initialize first");
        return false;
    }
    
    if (!connected_) {
        ESP_LOGE(TAG, "Camera not connected, please call Initialize first");
        return false;
    }
    
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network not available");
        return false;
    }

    std::string snapshot_url = "/onvif/getsnapshot/?channel=2";
    std::string url = "http://" + ip_ + ":" + std::to_string(port_) + snapshot_url;
    ESP_LOGI(TAG, "Getting snapshot from: %s", url.c_str());

    int status = 0;
    std::string auth_header;
    
    // 直接使用Digest认证（跳过Basic尝试）
    {
        auto http = network->CreateHttp(3);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            return false;
        }
        
        http->SetTimeout(30000);
        
        if (!http->Open("GET", url)) {
            ESP_LOGE(TAG, "Failed to open connection for snapshot");
            return false;
        }
        
        status = http->GetStatusCode();
        ESP_LOGI(TAG, "Snapshot first attempt status: %d", status);
        
        if (status == 200) {
            image_data = http->ReadAll();
            http->Close();
            
            if (!image_data.empty()) {
                ESP_LOGI(TAG, "Snapshot retrieved: %d bytes", image_data.size());
                return true;
            }
        }
        
        // 获取认证头信息
        auth_header = http->GetResponseHeader("WWW-Authenticate");
        ESP_LOGI(TAG, "First attempt got %d, WWW-Authenticate: %s", status, auth_header.c_str());
        http->Close();
    }
    
    // 如果需要Digest认证，进行第二次尝试
    if (status == 401 && !auth_header.empty()) {
        std::string auth_params = ExtractAuthHeader(auth_header);
        DigestAuth digest_auth;
        if (ParseDigestParams(auth_params, digest_auth)) {
            ESP_LOGI(TAG, "Need digest auth: realm=%s, nonce=%s, qop=%s", 
                     digest_auth.realm.c_str(), digest_auth.nonce.c_str(), digest_auth.qop.c_str());
            
            // 创建新的HTTP客户端用于Digest认证
            auto http = network->CreateHttp(3);
            if (!http) {
                ESP_LOGE(TAG, "Failed to create HTTP client for digest auth");
                return false;
            }
            
            http->SetTimeout(30000);
            
            // 构建Digest认证头
            std::string digest_header = BuildDigestAuthHeader(
                username_, password_, "GET", snapshot_url, digest_auth);
            http->SetHeader("Authorization", digest_header);
            
            ESP_LOGI(TAG, "Opening digest auth connection for snapshot...");
            if (!http->Open("GET", url)) {
                ESP_LOGE(TAG, "Failed to open digest auth connection");
                return false;
            }
            
            // 获取状态码
            status = http->GetStatusCode();
            ESP_LOGI(TAG, "Snapshot digest auth status: %d", status);
            
            if (status == 200) {
                // 检查响应头
                std::string content_length = http->GetResponseHeader("Content-Length");
                std::string transfer_encoding = http->GetResponseHeader("Transfer-Encoding");
                ESP_LOGI(TAG, "Response headers - Content-Length: %s, Transfer-Encoding: %s", 
                         content_length.c_str(), transfer_encoding.c_str());
                
                if (!content_length.empty()) {
                    size_t len = std::stoi(content_length);
                    ESP_LOGI(TAG, "Content-Length: %d bytes", (int)len);
                }
                
                image_data = http->ReadAll();
                
                if (image_data.empty()) {
                    ESP_LOGW(TAG, "ReadAll returned empty data");
                } else {
                    ESP_LOGI(TAG, "ReadAll completed, size: %d bytes", (int)image_data.size());
                    http->Close();
                    ESP_LOGI(TAG, "Snapshot retrieved with Digest auth: %d bytes", image_data.size());
                    return true;
                }
                http->Close();
            }
            
            http->Close();
        } else {
            ESP_LOGE(TAG, "Failed to parse digest auth params");
        }
    }
    
    ESP_LOGE(TAG, "Snapshot request failed after all attempts, final status: %d", status);
    return false;
}

bool OnvifCamera::Move(float x, float y) {
    std::string response;
    char body[512];
    snprintf(body, sizeof(body),
        R"(<ptz:ContinuousMove xmlns:ptz="http://www.onvif.org/ver20/ptz/wsdl">
          <ptz:ProfileToken>%s</ptz:ProfileToken>
          <ptz:Velocity>
            <tt:PanTilt x="%.2f" y="%.2f"/>
          </ptz:Velocity>
        </ptz:ContinuousMove>)", profile_token_.c_str(), x, y);
    
    bool success = SendSoapRequest(PTZ_SERVICE_ENDPOINT,
                                   "http://www.onvif.org/ver20/ptz/wsdl/ContinuousMove",
                                   BuildSoapEnvelope(body), response);
    
    if (success) {
        ESP_LOGI(TAG, "PTZ move command sent: x=%.2f, y=%.2f", x, y);
    } else {
        ESP_LOGE(TAG, "Failed to send PTZ move command");
    }
    
    return success;
}

bool OnvifCamera::Stop() {
    std::string response;
    char body[512];
    snprintf(body, sizeof(body),
        R"(<ptz:Stop xmlns:ptz="http://www.onvif.org/ver20/ptz/wsdl">
          <ptz:ProfileToken>%s</ptz:ProfileToken>
          <ptz:PanTilt>true</ptz:PanTilt>
        </ptz:Stop>)", profile_token_.c_str());
    
    bool success = SendSoapRequest(PTZ_SERVICE_ENDPOINT,
                                   "http://www.onvif.org/ver20/ptz/wsdl/Stop",
                                   BuildSoapEnvelope(body), response);
    
    if (success) {
        ESP_LOGI(TAG, "PTZ stop command sent");
    } else {
        ESP_LOGE(TAG, "Failed to send PTZ stop command");
    }
    
    return success;
}

std::string OnvifCamera::ExplainSnapshot(const std::string& question,
                                        const std::string& explain_url,
                                        const std::string& auth_token) {
    if (ip_.empty() || !connected_) {
        ESP_LOGE(TAG, "Camera not connected, please call Initialize first");
        return R"({"success": false, "message": "摄像头未连接"})";
    }
    
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network not available");
        return R"({"success": false, "message": "网络不可用"})";
    }
    
    // 使用低分辨率参数 resolution=2
    std::string snapshot_url = "/onvif/getsnapshot/?channel=2&resolution=2";
    std::string url = "http://" + ip_ + ":" + std::to_string(port_) + snapshot_url;
    ESP_LOGI(TAG, "Getting snapshot from: %s", url.c_str());
    
    int status = 0;
    std::string auth_header;
    
    // 直接使用Digest认证（摄像头只支持Digest，跳过Basic尝试）
    // 先获取nonce
    {
        auto http = network->CreateHttp(3);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            return R"({"success": false, "message": "创建HTTP客户端失败"})";
        }
        
        http->SetTimeout(10000);
        
        if (!http->Open("GET", url)) {
            ESP_LOGE(TAG, "Failed to open connection for nonce");
            return R"({"success": false, "message": "连接摄像头失败"})";
        }
        
        status = http->GetStatusCode();
        
        if (status == 401) {
            auth_header = http->GetResponseHeader("WWW-Authenticate");
            ESP_LOGI(TAG, "Got nonce header: %s", auth_header.c_str());
        }
        http->Close();
    }
    
    // 如果获取到nonce，进行Digest认证
    if (!auth_header.empty()) {
        std::string auth_params = ExtractAuthHeader(auth_header);
        DigestAuth digest_auth;
        if (ParseDigestParams(auth_params, digest_auth)) {
            ESP_LOGI(TAG, "Using digest auth for snapshot");
            
            auto http = network->CreateHttp(3);
            if (!http) {
                ESP_LOGE(TAG, "Failed to create HTTP client for digest auth");
                return R"({"success": false, "message": "创建HTTP客户端失败"})";
            }
            
            http->SetTimeout(10000);
            
            std::string digest_header = BuildDigestAuthHeader(
                username_, password_, "GET", snapshot_url, digest_auth);
            http->SetHeader("Authorization", digest_header);
            
            if (!http->Open("GET", url)) {
                ESP_LOGE(TAG, "Failed to open digest auth connection");
                return R"({"success": false, "message": "连接摄像头失败"})";
            }
            
            status = http->GetStatusCode();
            ESP_LOGI(TAG, "Snapshot digest auth status: %d", status);
            
            if (status != 200) {
                http->Close();
                ESP_LOGE(TAG, "Snapshot digest auth failed, status: %d", status);
                return R"({"success": false, "message": "摄像头认证失败"})";
            }
            
            // 获取截图数据
            ESP_LOGI(TAG, "Reading snapshot data...");
            std::string image_data = http->ReadAll();
            http->Close();
            
            if (image_data.empty()) {
                ESP_LOGE(TAG, "Snapshot data is empty");
                return R"({"success": false, "message": "截图数据为空"})";
            }
            
            ESP_LOGI(TAG, "Snapshot retrieved: %d bytes, uploading to AI server", (int)image_data.size());
            
            // 流式上传到AI服务器
            return UploadToExplainServer(question, explain_url, auth_token, image_data);
        }
    }
    
    if (status != 200) {
        ESP_LOGE(TAG, "Snapshot request failed, status: %d", status);
        return R"({"success": false, "message": "获取截图失败"})";
    }
    
    return R"({"success": false, "message": "未知错误"})";
}

std::string OnvifCamera::UploadToExplainServer(const std::string& question,
                                               const std::string& explain_url,
                                               const std::string& auth_token,
                                               const std::string& image_data) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network not available for upload");
        return R"({"success": false, "message": "网络不可用"})";
    }
    
    auto http = network->CreateHttp(3);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client for upload");
        return R"({"success": false, "message": "创建HTTP客户端失败"})";
    }
    
    std::string boundary = "----ONVIF_CAMERA_BOUNDARY";
    
    // 设置HTTP头
    http->SetTimeout(30000);
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!auth_token.empty()) {
        http->SetHeader("Authorization", "Bearer " + auth_token);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    
    if (!http->Open("POST", explain_url)) {
        ESP_LOGE(TAG, "Failed to connect to explain URL");
        return R"({"success": false, "message": "连接AI服务器失败"})";
    }
    
    // 构造multipart/form-data请求体
    // 第一块：question字段
    std::string question_field;
    question_field += "--" + boundary + "\r\n";
    question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
    question_field += "\r\n";
    question_field += question + "\r\n";
    http->Write(question_field.c_str(), question_field.size());
    
    // 第二块：文件字段头部
    std::string file_header;
    file_header += "--" + boundary + "\r\n";
    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"onvif_snapshot.jpg\"\r\n";
    file_header += "Content-Type: image/jpeg\r\n";
    file_header += "\r\n";
    http->Write(file_header.c_str(), file_header.size());
    
    // 第三块：图片数据（分块发送以节省内存）
    const size_t chunk_size = 4096;
    size_t offset = 0;
    size_t total_sent = 0;
    
    while (offset < image_data.size()) {
        size_t len = std::min(chunk_size, image_data.size() - offset);
        http->Write(image_data.data() + offset, len);
        offset += len;
        total_sent += len;
    }
    
    ESP_LOGI(TAG, "Uploaded %d bytes to AI server", (int)total_sent);
    
    // 第四块：multipart尾部
    std::string multipart_footer;
    multipart_footer += "\r\n--" + boundary + "--\r\n";
    http->Write(multipart_footer.c_str(), multipart_footer.size());
    
    // 结束块
    http->Write("", 0);
    
    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "Upload failed, status code: %d", status);
        http->Close();
        return R"({"success": false, "message": "上传图片失败"})";
    }
    
    std::string result = http->ReadAll();
    http->Close();
    
    ESP_LOGI(TAG, "AI server response: %s", result.c_str());
    return result;
}
