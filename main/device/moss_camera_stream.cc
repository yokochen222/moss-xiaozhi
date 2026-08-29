#include "moss_camera_stream.h"

#include "api/http_util.h"

#include <esp_log.h>

#define TAG "MossCamStream"

MossCameraStream& MossCameraStream::GetInstance() {
    static MossCameraStream instance;
    return instance;
}

void MossCameraStream::SetSource(LiveJpegSource* source) { source_ = source; }

void MossCameraStream::Disarm() {}

bool MossCameraStream::IsArmed() const { return false; }

esp_err_t MossCameraStream::HandleSnapshot(httpd_req_t* req) {
    ESP_LOGI(TAG, "PC JPEG preview disabled");
    return http_util::SendError(req, "501 Not Implemented", "preview disabled");
}

esp_err_t MossCameraStream::HandleStream(httpd_req_t* req) { return HandleSnapshot(req); }
