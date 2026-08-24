#pragma once

#include <esp_err.h>
#include <esp_http_server.h>

namespace api_methods {
namespace camera {

esp_err_t HandleStream(httpd_req_t* req);
esp_err_t HandleSnapshot(httpd_req_t* req);
esp_err_t HandleGimbal(httpd_req_t* req);
esp_err_t HandleFaceTrack(httpd_req_t* req);

}  // namespace camera
}  // namespace api_methods
