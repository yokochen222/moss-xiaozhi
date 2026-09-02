#pragma once

#include <esp_err.h>
#include <esp_http_server.h>

namespace api_methods {
namespace chat {

esp_err_t HandleChatWake(httpd_req_t* req);
esp_err_t HandleChatSay(httpd_req_t* req);
esp_err_t HandleChatSync(httpd_req_t* req);
esp_err_t HandleYunxiangjiOutboxGet(httpd_req_t* req);
esp_err_t HandleYunxiangjiOutboxAck(httpd_req_t* req);
esp_err_t HandleYunxiangjiInboxPut(httpd_req_t* req);

}  // namespace chat
}  // namespace api_methods
