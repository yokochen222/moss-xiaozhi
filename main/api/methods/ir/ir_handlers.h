#pragma once

#include <esp_err.h>
#include <esp_http_server.h>

namespace api_methods {
namespace ir {

// 红外发送处理器
esp_err_t HandleIrSend(httpd_req_t *req);

// 红外读取处理器
esp_err_t HandleIrRead(httpd_req_t *req);

} // namespace ir
} // namespace api_methods
