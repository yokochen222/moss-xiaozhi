#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

// 启动web服务器
void start_web_server(void);

// 停止web服务器
void stop_web_server(void);

// 转发聊天消息到SSE
void forward_chat_message(const char *role, const char *content, const char *type);

// 获取本机IP地址
const char* get_local_ip(void);

void add_message(const char* role, const char* content, const char* type);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_H 