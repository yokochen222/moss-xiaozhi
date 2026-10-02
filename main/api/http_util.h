#pragma once

#include <esp_http_server.h>
#include <cJSON.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace http_util {

inline void SetCors(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,PUT,POST,OPTIONS");
}

inline esp_err_t SendJson(httpd_req_t* req, const std::string& json, const char* status = "200 OK") {
    SetCors(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
}

inline esp_err_t SendError(httpd_req_t* req, const char* http_status, const std::string& message) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "status", "error");
    cJSON_AddStringToObject(obj, "message", message.c_str());
    char* printed = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    std::string json = printed ? printed : "{\"status\":\"error\"}";
    if (printed) {
        cJSON_free(printed);
    }
    return SendJson(req, json, http_status);
}

// Cap body size and recv retries so a stalled client cannot pin an httpd
// worker forever (max_open_sockets is small; wedging them hides /health).
// 256KB covers full IR catalog import (16 appliances x 48 cmds) with headroom;
// chat/hw/config/gimbal payloads are typically << 4KB.
inline constexpr int kMaxBodyBytes = 256 * 1024;
inline constexpr int kMaxRecvTimeouts = 8;

inline void TriggerClose(httpd_req_t* req) {
    const int sock = httpd_req_to_sockfd(req);
    if (sock >= 0) {
        httpd_sess_trigger_close(req->handle, sock);
    }
}

inline std::string ReadBody(httpd_req_t* req) {
    std::string body;
    if (req->content_len < 0) {
        TriggerClose(req);
        return "";
    }
    if (req->content_len > kMaxBodyBytes) {
        // Do not attempt a full drain of a huge Content-Length — that can still
        // pin the worker for the whole timeout budget. Drop the session.
        TriggerClose(req);
        return "";
    }
    int remaining = req->content_len;
    int timeouts = 0;
    char buf[1024];
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf, std::min(remaining, (int)sizeof(buf) - 1));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeouts > kMaxRecvTimeouts) {
                    TriggerClose(req);
                    return "";
                }
                continue;
            }
            TriggerClose(req);
            return "";
        }
        timeouts = 0;
        body.append(buf, ret);
        remaining -= ret;
    }
    return body;
}

inline esp_err_t HandleOptions(httpd_req_t* req) {
    SetCors(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
}

}  // namespace http_util
