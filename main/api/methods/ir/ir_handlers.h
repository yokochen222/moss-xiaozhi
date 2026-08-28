#pragma once

#include <esp_err.h>
#include <esp_http_server.h>

namespace api_methods {
namespace ir {

esp_err_t HandleIrSend(httpd_req_t* req);
esp_err_t HandleIrRead(httpd_req_t* req);
esp_err_t HandleIrLearn(httpd_req_t* req);
esp_err_t HandleIrTest(httpd_req_t* req);
esp_err_t HandleIrDevicesGet(httpd_req_t* req);
esp_err_t HandleIrDevicePut(httpd_req_t* req);
esp_err_t HandleIrDeviceDelete(httpd_req_t* req);
esp_err_t HandleIrCommandPut(httpd_req_t* req);
esp_err_t HandleIrCommandDelete(httpd_req_t* req);
esp_err_t HandleIrExport(httpd_req_t* req);
esp_err_t HandleIrImport(httpd_req_t* req);

}  // namespace ir
}  // namespace api_methods
