#pragma once

#include <esp_err.h>
#include <esp_http_server.h>

namespace api_methods {
namespace hw {

esp_err_t HandleHw(httpd_req_t* req);

}  // namespace hw
}  // namespace api_methods
