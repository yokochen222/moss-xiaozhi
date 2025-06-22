#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "boards/common/wifi_board.h"
#include <wifi_station.h>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_timer.h>

static const char* TAG = "chat_web_server";
static httpd_handle_t server = NULL;

// 消息结构
typedef struct {
    char role[16];
    char content[512];
    char type[16];
    uint32_t id;
    uint32_t timestamp;
} chat_message_t;

// 消息队列
#define MAX_MESSAGES 10
static chat_message_t message_queue[MAX_MESSAGES];
static int queue_head = 0;
static int queue_tail = 0;
static uint32_t message_id_counter = 0;
static SemaphoreHandle_t queue_mutex = NULL;

// JSON转义函数
void json_escape(const char* input, char* output, size_t output_size) {
    size_t j = 0;
    for (size_t i = 0; input[i] && j < output_size - 1; i++) {
        switch (input[i]) {
            case '"': 
                if (j + 2 < output_size) { output[j++] = '\\'; output[j++] = '"'; }
                break;
            case '\\': 
                if (j + 2 < output_size) { output[j++] = '\\'; output[j++] = '\\'; }
                break;
            case '\n': 
                if (j + 2 < output_size) { output[j++] = '\\'; output[j++] = 'n'; }
                break;
            case '\r': 
                if (j + 2 < output_size) { output[j++] = '\\'; output[j++] = 'r'; }
                break;
            case '\t': 
                if (j + 2 < output_size) { output[j++] = '\\'; output[j++] = 't'; }
                break;
            default:
                output[j++] = input[i];
                break;
        }
    }
    output[j] = '\0';
}

// 添加消息到队列
void add_message(const char* role, const char* content, const char* type) {
    if (!queue_mutex) return;
    
    xSemaphoreTake(queue_mutex, portMAX_DELAY);
    
    strncpy(message_queue[queue_tail].role, role, sizeof(message_queue[queue_tail].role) - 1);
    strncpy(message_queue[queue_tail].content, content, sizeof(message_queue[queue_tail].content) - 1);
    strncpy(message_queue[queue_tail].type, type ? type : "text", sizeof(message_queue[queue_tail].type) - 1);
    message_queue[queue_tail].id = ++message_id_counter;
    message_queue[queue_tail].timestamp = esp_timer_get_time() / 1000000; // 转换为秒
    
    queue_tail = (queue_tail + 1) % MAX_MESSAGES;
    if (queue_tail == queue_head) {
        // 队列满了，移除最旧的消息
        queue_head = (queue_head + 1) % MAX_MESSAGES;
    }
    
    xSemaphoreGive(queue_mutex);
}

// 获取所有消息的JSON
char* get_messages_json() {
    if (!queue_mutex) return NULL;
    
    xSemaphoreTake(queue_mutex, portMAX_DELAY);
    
    // 计算JSON大小
    int json_size = 200; // 基础大小
    int count = 0;
    int i = queue_head;
    while (i != queue_tail) {
        json_size += strlen(message_queue[i].content) * 3 + 200; // 转义字符 + 额外字段
        i = (i + 1) % MAX_MESSAGES;
        count++;
    }
    
    char* json = (char*)malloc(json_size);
    if (!json) {
        xSemaphoreGive(queue_mutex);
        return NULL;
    }
    
    // 构建JSON
    strcpy(json, "{\"messages\":[");
    i = queue_head;
    int first = 1;
    while (i != queue_tail) {
        if (!first) strcat(json, ",");
        
        // 转义消息内容
        char escaped_content[1024];
        json_escape(message_queue[i].content, escaped_content, sizeof(escaped_content));
        
        snprintf(json + strlen(json), json_size - strlen(json),
                "{\"id\":%lu,\"role\":\"%s\",\"content\":\"%s\",\"type\":\"%s\",\"timestamp\":%lu}",
                message_queue[i].id, message_queue[i].role, escaped_content, message_queue[i].type, message_queue[i].timestamp);
        i = (i + 1) % MAX_MESSAGES;
        first = 0;
    }
    strcat(json, "]}");
    
    xSemaphoreGive(queue_mutex);
    return json;
}

// 获取本机IP（WiFi）
extern "C" const char* get_local_ip() {
    static std::string ip;
    ip = WifiStation::GetInstance().GetIpAddress();
    return ip.c_str();
}

static const char* INDEX_HTML =
"<!DOCTYPE html>"
"<html lang=\"zh-CN\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">"
"<title>MOSS 550W</title>"
"<script src=\"https://cdn.jsdelivr.net/npm/vue@3.5.16/dist/vue.global.min.js\"></script>"
"<link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/npm/highlight.js@11.7.0/styles/github-dark.css\">"
"<style>"
"*{padding:0;margin:0}"
"html{background-color:#181a1f;overflow:hidden;width:100%}"
".moss-bg{position:fixed;pointer-events:none;background-image:url(https://s21.ax1x.com/2025/04/13/pERzECt.png);background-repeat:no-repeat;background-position:top left;width:100%;height:100%;z-index:2;top:-100px;left:0;animation:opa 1s ease alternate infinite}"
".moss-bg::after{content:'';display:block;width:10px;height:10px;background-color:#e51c20;position:absolute;border-radius:100px;top:324px;left:154px;animation:opa 1s ease alternate infinite;background-color:red}"
"body{display:flex;justify-content:center;align-items:center;overflow:hidden;height:100vh;width:100vw}"
".moss-bg-video-v{position:fixed;z-index:0;top:0;left:0;right:0;bottom:0;object-fit:cover}"
".moss-bg-video-v::after{content:'';display:block;position:absolute;top:0;left:0;right:0;bottom:0;background-color:rgba(0,0,0,0.6);backdrop-filter:blur(10px);z-index:1}"
".moss-bg-video{width:100%;height:100%;object-fit:cover}"
".moss-logo{margin-bottom:10px;pointer-events:none;position:relative;z-index:3}"
".moss-logo img{width:140px}"
".moss-code-section{font-family:'Courier New',Courier,monospace;max-height:80vh;overflow:auto}"
".moss-code-box{max-height:80vh;overflow:auto}"
".copy-btn{background-color:#ff7b72;color:#fff;border:none;padding:2px 6px;border-radius:5px;cursor:pointer;margin-bottom:10px;font-size:12px;transition:background-color 0.3s ease;position:absolute;right:10px;top:10px}"
"@keyframes opa{from{opacity:0.3}to{opacity:0.7}}"
".moss-chat-item{font-size:16px;line-height:1.5;background-color:rgba(0,0,0,0.6);padding:20px;border-radius:5px;white-space:pre-wrap;word-wrap:break-word;position:relative;color:#fff;border:1px solid #555;overflow:hidden;z-index:3;margin-bottom:20px;max-width:70%}"
".moss-chat{display:flex;flex-direction:row-reverse}"
".moss-chat.moss{flex-direction:row}"
".moss-chat-avatar{width:50px;border-radius:50%;margin-left:20px;padding-top:10px}"
".moss-chat-avatar img{width:50px;height:50px;border-radius:50%;display:block}"
".moss-chat.moss .moss-chat-avatar{margin-right:20px;margin-left:0px}"
".moss-chat-screen{color:#fff;font-size:16px;position:relative;z-index:4;width:1024px;height:80vh;overflow-y:auto;-webkit-overflow-scrolling:touch;scrollbar-width:none;-ms-overflow-style:none}"
".moss-chat-screen::-webkit-scrollbar{display:none}"
"</style>"
"</head>"
"<body>"
"<div id=\"app\"></div>"
"<script type=\"module\">"
"import{marked}from'https://cdn.jsdelivr.net/npm/marked@4.2.12/lib/marked.esm.js';"
"import highlightJs from'https://cdn.jsdelivr.net/npm/highlight.js@11.11.1/+esm';"
"function encodeHtmlEntities(str){return str.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;').replace(/'/g,'&#39;')}"
"marked.setOptions({highlight:function(code,lang){return highlightJs.highlightAuto(code,[lang]).value}});"
"const{createApp,ref,computed,onMounted,watch,nextTick}=Vue;"
"const app=createApp({"
"template:'<div class=\"moss-bg-video-v\"><video class=\"moss-bg-video\" src=\"http://bithubs.cn/moss-bg-video.mp4\" autoplay loop muted></video></div><div class=\"moss-bg\"></div><div ref=\"messagesContainer\" class=\"moss-chat-screen\"><template v-for=\"(item,index) in message\" :key=\"item.id\"><code-block v-if=\"item.type===\\'code\\'\" :role=\"item.role\" :markdown=\"item.content\"/><text-block v-else :text=\"item.content\" :role=\"item.role\"/></template></div>',"
"setup(){"
"const messagesContainer=ref();"
"const scrollToBottom=()=>{if(messagesContainer.value){messagesContainer.value.scrollTo({top:messagesContainer.value.scrollHeight,behavior:'smooth'})}};"
"const message=ref([]);"
"let lastMessageId=0;"
"const isCodeMessage=(content)=>{return content.includes('```')||content.includes('function')||content.includes('class')||content.includes('import')||content.includes('const')||content.includes('let')||content.includes('var')};"
"const pollMessages=async()=>{"
"try{"
"const protocol=window.location.protocol;"
"const host=window.location.host;"
"const response=await fetch(protocol+'//'+host+'/api/messages');"
"if(response.ok){"
"const data=await response.json();"
"if(data.messages){"
"const newMessages=data.messages.filter(msg=>msg.id>lastMessageId);"
"newMessages.forEach(msg=>{"
"const messageType=isCodeMessage(msg.content)?'code':'text';"
"message.value.push({id:msg.id,type:messageType,role:msg.role,content:msg.content,timestamp:msg.timestamp});"
"lastMessageId=Math.max(lastMessageId,msg.id)"
"})"
"}"
"}"
"}catch(e){console.error('轮询失败:',e)}"
"finally{setTimeout(pollMessages,1000)}"
"};"
"const startPolling=()=>{pollMessages()};"
"watch(message,()=>{nextTick(()=>{scrollToBottom()})},{deep:true});"
"onMounted(()=>{startPolling();scrollToBottom()});"
"return{message,messagesContainer}"
"}"
"});"
"app.component('text-block',{"
"props:['text','role'],"
"template:'<div :class=\"[\\'moss-chat\\',{\\'moss\\':role!==\\'user\\'}]\"><div class=\"moss-chat-avatar\"><img v-if=\"role===\\'user\\'\" class=\"avatar-img avatar-user\" src=\"https://q1.qlogo.cn/g?b=qq&nk=8144064&s=640\"/><img v-else class=\"avatar-img avatar-moss\" src=\"https://s21.ax1x.com/2025/06/21/pVZK110.png\"/></div><div class=\"moss-chat-item moss-code-section\"><div class=\"moss-code-box\">{{text}}</div></div></div>'"
"});"
"app.component('code-block',{"
"props:['markdown','role'],"
"template:'<div class=\"moss-chat moss\"><div class=\"moss-chat-avatar\"><img v-if=\"role===\\'user\\'\" class=\"avatar-img avatar-user\" src=\"https://q1.qlogo.cn/g?b=qq&nk=8144064&s=640\"/><img v-else class=\"avatar-img avatar-moss\" src=\"https://s21.ax1x.com/2025/06/21/pVZK110.png\"/></div><div class=\"moss-chat-item moss-code-section\"><button @click=\"copyCode\" class=\"copy-btn\">{{isCopied?\\'已复制\\':\\'复制\\'}}</button><div class=\"moss-code-box\" ref=\"codeBlockRef\"><div v-html=\"renderedMarkdown\"></div></div></div></div>',"
"setup(props){"
"const isCopied=ref(false);"
"const renderedMarkdown=computed(()=>{return marked.parse(props.markdown)});"
"const codeBlockRef=ref(null);"
"const copyCode=async()=>{"
"try{"
"const pureCode=codeBlockRef.value.textContent;"
"await navigator.clipboard.writeText(pureCode);"
"isCopied.value=true"
"}catch(err){"
"isCopied.value=false;"
"console.error('复制失败:',err)"
"}"
"setTimeout(()=>{isCopied.value=false},2000)"
"};"
"return{isCopied,renderedMarkdown,codeBlockRef,copyCode}"
"}"
"});"
"app.mount('#app');"
"</script>"
"</body>"
"</html>";

static esp_err_t index_handler(httpd_req_t *req) {
    // 检查条件请求
    size_t if_none_match_len = httpd_req_get_hdr_value_len(req, "If-None-Match");
    if (if_none_match_len > 0) {
        char if_none_match[64];
        if (httpd_req_get_hdr_value_str(req, "If-None-Match", if_none_match, sizeof(if_none_match)) == ESP_OK) {
            if (strcmp(if_none_match, "\"moss-html-v1\"") == 0) {
                // 浏览器有缓存且未过期，返回304
                httpd_resp_set_status(req, "304 Not Modified");
                httpd_resp_send(req, NULL, 0);
                return ESP_OK;
            }
        }
    }
    
    // 设置响应头
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600"); // 缓存1小时
    httpd_resp_set_hdr(req, "ETag", "\"moss-html-v1\""); // 添加ETag
    httpd_resp_set_hdr(req, "Last-Modified", "Mon, 01 Jan 2024 00:00:00 GMT");
    
    // 直接返回HTML内容，无需动态替换
    return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

// API处理函数 - 获取消息
static esp_err_t api_messages_handler(httpd_req_t *req) {
    char* json = get_messages_json();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get messages");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

// 只对外接口函数用C linkage
#ifdef __cplusplus
extern "C" {
#endif
void start_web_server(void);
void stop_web_server(void);
void forward_chat_message(const char *role, const char *content, const char *type);
const char* get_local_ip(void);
#ifdef __cplusplus
}
#endif

// 转发聊天消息到队列
extern "C" void forward_chat_message(const char *role, const char *content, const char *type) {
    if (!role || !content) {
        return;
    }
    add_message(role, content, type);
}

// 实现
void start_web_server(void) {
    if (server) {
        ESP_LOGI(TAG, "Web server already running");
        return;
    }
    
    // 初始化消息队列
    queue_mutex = xSemaphoreCreateMutex();
    if (!queue_mutex) {
        ESP_LOGE(TAG, "Failed to create queue mutex");
        return;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 9000;
    config.max_uri_handlers = 2;
    config.stack_size = 4096;  // 从2048增加到4096
    config.core_id = tskNO_AFFINITY;
    config.max_open_sockets = 2;
    config.backlog_conn = 1;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error starting server");
        vSemaphoreDelete(queue_mutex);
        queue_mutex = NULL;
        return;
    }
    
    // 注册URI处理程序
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &index_uri);
    
    httpd_uri_t api_messages_uri = {
        .uri = "/api/messages",
        .method = HTTP_GET,
        .handler = api_messages_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_messages_uri);
    
    ESP_LOGI(TAG, "Web server started successfully on port 9000");
}

void stop_web_server(void) {
    if (server) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }
    
    // 清理消息队列
    if (queue_mutex) {
        vSemaphoreDelete(queue_mutex);
        queue_mutex = NULL;
    }
    
    // 清空消息队列
    queue_head = 0;
    queue_tail = 0;
} 