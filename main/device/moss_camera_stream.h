#pragma once

#include <esp_http_server.h>
#include "esp_camera.h"

class LiveJpegSource {
public:
    virtual ~LiveJpegSource() = default;
    virtual bool AcquireLiveStream() = 0;
    virtual void ReleaseLiveStream() = 0;
    virtual camera_fb_t* GrabJpeg() = 0;
    virtual void ReturnJpeg(camera_fb_t* fb) = 0;
};

class MossCameraStream {
public:
    static MossCameraStream& GetInstance();

    void SetSource(LiveJpegSource* source);
    LiveJpegSource* source() const { return source_; }
    void Disarm();
    bool IsArmed() const;

    esp_err_t HandleStream(httpd_req_t* req);
    esp_err_t HandleSnapshot(httpd_req_t* req);

private:
    MossCameraStream() = default;
    LiveJpegSource* source_ = nullptr;
};
