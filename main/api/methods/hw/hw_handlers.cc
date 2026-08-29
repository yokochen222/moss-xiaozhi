#include "hw_handlers.h"

#include "api/http_util.h"
#include "config/moss_hw.h"

#include <cJSON.h>

namespace api_methods {
namespace hw {

esp_err_t HandleHw(httpd_req_t* req) {
    http_util::SetCors(req);
    HwApplyResult result;
    result.ok = true;
    if (req->method == HTTP_POST) {
        const std::string body = http_util::ReadBody(req);
        cJSON* json = cJSON_Parse(body.c_str());
        result = MossHwApply(json);
        if (json) cJSON_Delete(json);
    }
    cJSON* payload = MossHwStateJson(result.ok, result.message);
    char* printed = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    std::string json = printed ? printed : "{\"ok\":false}";
    if (printed) cJSON_free(printed);
    return http_util::SendJson(req, json, result.ok ? "200 OK" : "400 Bad Request");
}

}  // namespace hw
}  // namespace api_methods
