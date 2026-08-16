#pragma once

#include <esp_err.h>
#include <esp_http_server.h>

namespace api_methods {
namespace config {

esp_err_t HandleHealth(httpd_req_t* req);
esp_err_t HandleMqttGet(httpd_req_t* req);
esp_err_t HandleMqttPut(httpd_req_t* req);

}  // namespace config
}  // namespace api_methods
