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

// MD5 哈希器类 - 复用上下文避免重复初始化开销
class Md5Hasher {
public:
    Md5Hasher() {
        mbedtls_md_init(&ctx_);
        info_ = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
        mbedtls_md_setup(&ctx_, info_, 0);
    }
    ~Md5Hasher() { mbedtls_md_free(&ctx_); }
    Md5Hasher(const Md5Hasher&) = delete;
    Md5Hasher& operator=(const Md5Hasher&) = delete;
    
    void update(const std::string& data) {
        mbedtls_md_update(&ctx_, (const unsigned char*)data.data(), data.size());
    }
    
    std::string final() {
        unsigned char hash[16];
        mbedtls_md_finish(&ctx_, hash);
        char hex[33];
        for (int i = 0; i < 16; i++) {
            snprintf(hex + i * 2, 3, "%02x", hash[i]);
        }
        hex[32] = '\0';
        return std::string(hex);
    }
    
    static std::string hash(const std::string& input) {
        Md5Hasher hasher;
        hasher.update(input);
        return hasher.final();
    }
    
private:
    mbedtls_md_context_t ctx_;
    const mbedtls_md_info_t* info_;
};

// 简化的 MD5 哈希函数
static std::string Md5Hash(const std::string& input) {
    return Md5Hasher::hash(input);
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
    static const char* nc = "00000001";
    
    // 生成cnonce
    char cnonce[33];
    uint32_t rand1 = esp_random();
    uint32_t rand2 = esp_random();
    snprintf(cnonce, sizeof(cnonce), "%08lx%08lx", (unsigned long)rand1, (unsigned long)rand2);
    
    // 计算 HA1 和 HA2
    std::string A1 = username + ":" + auth.realm + ":" + password;
    std::string A2 = method + ":" + uri;
    std::string HA1 = Md5Hash(A1);
    std::string HA2 = Md5Hash(A2);
    std::string response = Md5Hash(HA1 + ":" + auth.nonce + ":" + nc + ":" + cnonce + ":" + auth.qop + ":" + HA2);
    
    // 使用 snprintf 替代 ostringstream，避免动态内存分配
    char header_buf[1024];
    int len;
    if (!auth.opaque.empty()) {
        len = snprintf(header_buf, sizeof(header_buf),
            "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
            "cnonce=\"%s\", nc=%s, qop=%s, response=\"%s\", opaque=\"%s\"",
            username.c_str(), auth.realm.c_str(), auth.nonce.c_str(), uri.c_str(),
            cnonce, nc, auth.qop.c_str(), response.c_str(), auth.opaque.c_str());
    } else {
        len = snprintf(header_buf, sizeof(header_buf),
            "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
            "cnonce=\"%s\", nc=%s, qop=%s, response=\"%s\"",
            username.c_str(), auth.realm.c_str(), auth.nonce.c_str(), uri.c_str(),
            cnonce, nc, auth.qop.c_str(), response.c_str());
    }
    
    return std::string(header_buf, len);
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
            
            // 获取子码流profile
            if (GetProfiles(profile_token_)) {
                ESP_LOGI(TAG, "Got profile token: %s (sub stream if available)", profile_token_.c_str());
            }
            
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
        ESP_LOGI(TAG, "GetProfiles response: %s", response.c_str());
        
        // 先尝试找第二个profile（通常是子码流）
        auto pos = response.find("protoken_ch0002");
        if (pos != std::string::npos) {
            profile_token = "protoken_ch0002";
            ESP_LOGI(TAG, "Using sub stream profile: %s", profile_token.c_str());
            return true;
        }
        
        // 如果没有第二个，找第一个
        pos = response.find("protoken_ch0001");
        if (pos != std::string::npos) {
            profile_token = "protoken_ch0001";
            ESP_LOGI(TAG, "Using main stream profile: %s", profile_token.c_str());
            return true;
        }
        
        // 尝试从token属性中提取
        pos = response.find("token=\"");
        if (pos != std::string::npos) {
            auto start = pos + 7;
            auto end = response.find("\"", start);
            if (end != std::string::npos) {
                profile_token = response.substr(start, end - start);
                ESP_LOGI(TAG, "Using profile token: %s", profile_token.c_str());
                return true;
            }
        }
    }
    
    return false;
}

bool OnvifCamera::GetSnapshotUri(std::string& uri, const std::string& profile_token) {
    std::string response;
    char body[512];
    snprintf(body, sizeof(body), 
        R"(<trt:GetSnapshotUri xmlns:trt="http://www.onvif.org/ver10/media/wsdl">
          <trt:ProfileToken>%s</trt:ProfileToken>
        </trt:GetSnapshotUri>)", profile_token.c_str());
    
    if (SendSoapRequest(DEVICE_SERVICE_ENDPOINT,
                        "http://www.onvif.org/ver10/media/wsdl/GetSnapshotUri",
                        BuildSoapEnvelope(body), response)) {
        ESP_LOGI(TAG, "GetSnapshotUri response: %s", response.c_str());
        auto start = response.find("<tt:Uri>");
        auto end = response.find("</tt:Uri>");
        if (start != std::string::npos && end != std::string::npos) {
            uri = response.substr(start + 8, end - start - 8);
            // 替换为本地地址
            auto uri_pos = uri.find("192.168.");
            if (uri_pos != std::string::npos) {
                uri = "http://" + ip_ + ":" + std::to_string(port_) + uri.substr(uri.find("/", uri_pos));
            }
            ESP_LOGI(TAG, "Snapshot URI: %s", uri.c_str());
            return true;
        }
    }
    
    return false;
}

bool OnvifCamera::GetSnapshotUri(std::string& uri) {
    return GetSnapshotUri(uri, profile_token_);
}

// 构建截图 URL（优先使用 GetSnapshotUri，否则用 fallback）
static bool BuildSnapshotUrl(const std::string& ip, int port, 
                             const std::string& profile_token,
                             std::string& url) {
    // 尝试用 GetSnapshotUri 获取子码流的 snapshot URI
    std::string snapshot_uri;
    OnvifCamera& camera = OnvifCamera::GetInstance();
    
    if (!profile_token.empty() && camera.GetSnapshotUri(snapshot_uri, profile_token)) {
        url = snapshot_uri;
        ESP_LOGI(TAG, "Using snapshot URI from GetSnapshotUri: %s", url.c_str());
        return true;
    }
    
    // 回退到手动构造 URL
    char buf[128];
    snprintf(buf, sizeof(buf), "http://%s:%d/onvif/getsnapshot/?channel=2", 
             ip.c_str(), port);
    url = buf;
    ESP_LOGI(TAG, "GetSnapshotUri failed, using fallback: %s", url.c_str());
    return false;
}

// 提取 URL 中的路径部分
static std::string ExtractPathFromUrl(const std::string& url, const std::string& ip, int port) {
    size_t ip_pos = url.find(ip);
    if (ip_pos == std::string::npos) {
        // 找不到 IP，尝试找协议后的第一个 /
        size_t slash = url.find("://");
        if (slash != std::string::npos) {
            slash = url.find("/", slash + 3);
            if (slash != std::string::npos) {
                return url.substr(slash);
            }
        }
        return url;
    }
    
    // 跳过 IP:port
    size_t start = ip_pos + ip.length();
    size_t colon = url.find(":", ip_pos);
    size_t slash = url.find("/", ip_pos);
    
    if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) {
        // 有端口号
        char port_str[8];
        snprintf(port_str, sizeof(port_str), ":%d", port);
        if (url.compare(colon, strlen(port_str), port_str) == 0) {
            start = colon + strlen(port_str);
        }
    }
    
    if (slash != std::string::npos && slash >= start) {
        return url.substr(slash);
    }
    return url;
}

// 内部函数：使用 Digest 认证获取截图
static bool FetchSnapshotWithAuth(NetworkInterface* network,
                                  const std::string& url,
                                  const std::string& snapshot_path,
                                  const std::string& username,
                                  const std::string& password,
                                  int timeout_ms,
                                  std::string& image_data) {
    // 第一步：获取 nonce
    auto http = network->CreateHttp(3);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }
    
    http->SetTimeout(timeout_ms);
    
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open connection");
        return false;
    }
    
    int status = http->GetStatusCode();
    
    if (status == 200) {
        image_data = http->ReadAll();
        http->Close();
        return !image_data.empty();
    }
    
    if (status != 401) {
        ESP_LOGW(TAG, "Unexpected status: %d", status);
        http->Close();
        return false;
    }
    
    std::string auth_header = http->GetResponseHeader("WWW-Authenticate");
    ESP_LOGI(TAG, "Got WWW-Authenticate: %s", auth_header.c_str());
    http->Close();
    
    // 第二步：解析认证参数并发起 Digest 认证请求
    std::string auth_params = ExtractAuthHeader(auth_header);
    DigestAuth digest_auth;
    if (!ParseDigestParams(auth_params, digest_auth)) {
        ESP_LOGE(TAG, "Failed to parse digest auth params");
        return false;
    }
    
    http = network->CreateHttp(3);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client for digest auth");
        return false;
    }
    
    http->SetTimeout(timeout_ms);
    
    std::string digest_header = BuildDigestAuthHeader(
        username, password, "GET", snapshot_path, digest_auth);
    http->SetHeader("Authorization", digest_header);
    
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open digest auth connection");
        return false;
    }
    
    status = http->GetStatusCode();
    ESP_LOGI(TAG, "Digest auth status: %d", status);
    
    if (status != 200) {
        http->Close();
        return false;
    }
    
    image_data = http->ReadAll();
    http->Close();
    
    return !image_data.empty();
}

bool OnvifCamera::GetSnapshot(std::string& image_data) {
    if (ip_.empty() || !connected_) {
        ESP_LOGE(TAG, "Camera not initialized or not connected");
        return false;
    }
    
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network not available");
        return false;
    }
    
    std::string url;
    BuildSnapshotUrl(ip_, port_, profile_token_, url);
    
    std::string path = ExtractPathFromUrl(url, ip_, port_);
    ESP_LOGI(TAG, "Getting snapshot from: %s (path: %s)", url.c_str(), path.c_str());
    
    return FetchSnapshotWithAuth(network, url, path, username_, password_, 30000, image_data);
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
        ESP_LOGE(TAG, "Camera not initialized or not connected");
        return R"({"success": false, "message": "摄像头未连接"})";
    }
    
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network not available");
        return R"({"success": false, "message": "网络不可用"})";
    }
    
    std::string url;
    BuildSnapshotUrl(ip_, port_, profile_token_, url);
    
    std::string path = ExtractPathFromUrl(url, ip_, port_);
    ESP_LOGI(TAG, "Getting snapshot from: %s (path: %s)", url.c_str(), path.c_str());
    
    // 获取截图数据
    std::string image_data;
    if (!FetchSnapshotWithAuth(network, url, path, username_, password_, 10000, image_data)) {
        ESP_LOGE(TAG, "Failed to get snapshot");
        return R"({"success": false, "message": "获取截图失败"})";
    }
    
    if (image_data.empty()) {
        ESP_LOGE(TAG, "Snapshot data is empty");
        return R"({"success": false, "message": "截图数据为空"})";
    }
    
    ESP_LOGI(TAG, "Snapshot retrieved: %d bytes, uploading to AI server", (int)image_data.size());
    
    // 上传到AI服务器
    return UploadToExplainServer(question, explain_url, auth_token, image_data);
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
