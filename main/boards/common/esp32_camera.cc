#include "sdkconfig.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <img_converters.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "board.h"
#include "display.h"
#include "esp32_camera.h"
#include "esp_timer.h"
#include "jpg/image_to_jpeg.h"
#include "device/moss_jpeg_still.h"
#include "lvgl_display.h"
#include "mcp_server.h"
#include "system_info.h"

#define TAG "Esp32Camera"

#if CONFIG_XIAOZHI_CAMERA_MIRROR_CONFIGURED
#if CONFIG_XIAOZHI_CAMERA_HMIRROR
static constexpr bool kConfiguredHMirror = true;
#else
static constexpr bool kConfiguredHMirror = false;
#endif
#if CONFIG_XIAOZHI_CAMERA_VFLIP
static constexpr bool kConfiguredVFlip = true;
#else
static constexpr bool kConfiguredVFlip = false;
#endif
#endif

Esp32Camera::Esp32Camera(const camera_config_t& config) {
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed with error 0x%x", err);
        return;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        if (s->id.PID == GC0308_PID) {
            s->set_hmirror(s, 0);  // Control camera mirror: 1 for mirror, 0 for normal
        } else if (s->id.PID == OV2640_PID) {
            // Auto WB/exposure/gain + lens shading to reduce blur/banding on DVP JPEG.
            s->set_brightness(s, 0);
            s->set_contrast(s, 0);
            s->set_saturation(s, 0);
            s->set_whitebal(s, 1);
            s->set_awb_gain(s, 1);
            s->set_exposure_ctrl(s, 1);
            s->set_aec2(s, 1);
            s->set_gain_ctrl(s, 1);
            s->set_bpc(s, 1);
            s->set_wpc(s, 1);
            s->set_raw_gma(s, 1);
            s->set_lenc(s, 1);
            s->set_dcw(s, 1);
        }
#if CONFIG_XIAOZHI_CAMERA_MIRROR_CONFIGURED
        s->set_hmirror(s, kConfiguredHMirror ? 1 : 0);
        s->set_vflip(s, kConfiguredVFlip ? 1 : 0);
#endif
        ESP_LOGI(TAG, "Camera initialized: format=%d pid=0x%04x", config.pixel_format, s->id.PID);
    }

    streaming_on_ = true;
}

Esp32Camera::~Esp32Camera() {
    if (current_fb_ && streaming_on_) {
        esp_camera_fb_return(current_fb_);
        current_fb_ = nullptr;
    }
    if (encode_buf_) {
        heap_caps_free(encode_buf_);
        encode_buf_ = nullptr;
        encode_buf_size_ = 0;
        jpeg_parked_len_ = 0;
    }
    if (streaming_on_) {
        esp_camera_deinit();
        streaming_on_ = false;
    }
}

void Esp32Camera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool Esp32Camera::Capture() {
    jpeg_parked_len_ = 0;
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    if (!streaming_on_) {
        return false;
    }

    // Discard warmup frames; GRAB_WHEN_EMPTY needs a few complete frames before capture.
    const int warmup = 8;
    const int max_attempts = 15;
    int got_warmup = 0;
    if (current_fb_) {
        esp_camera_fb_return(current_fb_);
        current_fb_ = nullptr;
    }

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (current_fb_) {
            esp_camera_fb_return(current_fb_);
            current_fb_ = nullptr;
        }
        current_fb_ = esp_camera_fb_get();
        if (!current_fb_) {
            ESP_LOGE(TAG, "Camera capture failed");
            return false;
        }

        if (got_warmup < warmup) {
            got_warmup++;
            continue;
        }

        if (current_fb_->format == PIXFORMAT_RGB565) {
            if (!moss_jpeg_still::RgbLooksComplete(current_fb_->len, current_fb_->width,
                                                   current_fb_->height)) {
                ESP_LOGW(TAG, "Drop short RGB565 len=%u %dx%d", (unsigned)current_fb_->len,
                         current_fb_->width, current_fb_->height);
                continue;
            }
        }

        if (current_fb_->format == PIXFORMAT_JPEG) {
            const bool soi =
                current_fb_->len >= 2 && current_fb_->buf[0] == 0xFF && current_fb_->buf[1] == 0xD8;
            const bool eoi = current_fb_->len >= 2 &&
                             current_fb_->buf[current_fb_->len - 2] == 0xFF &&
                             current_fb_->buf[current_fb_->len - 1] == 0xD9;
            if (!soi || !eoi) {
                ESP_LOGW(TAG, "Drop incomplete JPEG len=%u soi=%d eoi=%d",
                         (unsigned)current_fb_->len, (int)soi, (int)eoi);
                continue;
            }
#ifdef CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE
            if (current_fb_->len * 100 >=
                (size_t)CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE * 90) {
                ESP_LOGW(TAG, "Drop truncated JPEG len=%u cap=%d", (unsigned)current_fb_->len,
                         CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE);
                continue;
            }
#endif
        }
        break;
    }

    if (!current_fb_) {
        ESP_LOGE(TAG, "No valid frame");
        return false;
    }

    // Prepare encode buffer for RGB565 format (with optional byte swapping)
    if (current_fb_->format == PIXFORMAT_RGB565) {
        size_t pixel_count = current_fb_->width * current_fb_->height;
        size_t data_size = pixel_count * 2;

        // Allocate or reallocate encode buffer if needed
        if (encode_buf_size_ < data_size) {
            if (encode_buf_) {
                heap_caps_free(encode_buf_);
            }
            encode_buf_ =
                (uint8_t*)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (encode_buf_ == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate memory for encode buffer");
                encode_buf_size_ = 0;
                return false;
            }
            encode_buf_size_ = data_size;
        }

        // Copy data to encode buffer with optional byte swapping
        uint16_t* src = (uint16_t*)current_fb_->buf;
        uint16_t* dst = (uint16_t*)encode_buf_;
        if (swap_bytes_enabled_) {
            for (size_t i = 0; i < pixel_count; i++) {
                dst[i] = __builtin_bswap16(src[i]);
            }
        } else {
            memcpy(encode_buf_, current_fb_->buf, data_size);
        }

        auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
        if (display != nullptr) {
            uint8_t* preview_data =
                (uint8_t*)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (preview_data != nullptr) {
                memcpy(preview_data, encode_buf_, data_size);
                display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(
                    preview_data, data_size, current_fb_->width, current_fb_->height,
                    current_fb_->width * 2, LV_COLOR_FORMAT_RGB565));
            }
        }
    } else if (current_fb_->format == PIXFORMAT_JPEG) {
        // JPEG format preview usually requires decoding, skip preview display for now, just log
        ESP_LOGW(TAG, "JPEG capture success, len=%u, preview skipped",
                 (unsigned)current_fb_->len);
    }

    ESP_LOGI(TAG, "Captured frame: %dx%d, len=%u, format=%d", current_fb_->width,
             current_fb_->height, (unsigned)current_fb_->len, current_fb_->format);

    return true;
}

bool Esp32Camera::ReleaseSensorKeepJpeg() {
    if (!streaming_on_ || current_fb_ == nullptr || current_fb_->format != PIXFORMAT_JPEG ||
        current_fb_->buf == nullptr || current_fb_->len < 128) {
        return false;
    }
    const size_t len = current_fb_->len;
    if (encode_buf_size_ < len) {
        if (encode_buf_) {
            heap_caps_free(encode_buf_);
            encode_buf_ = nullptr;
            encode_buf_size_ = 0;
        }
        encode_buf_ = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (encode_buf_ == nullptr) {
            ESP_LOGE(TAG, "Failed to park JPEG (%u bytes)", (unsigned)len);
            return false;
        }
        encode_buf_size_ = len;
    }
    memcpy(encode_buf_, current_fb_->buf, len);
    jpeg_parked_len_ = len;
    esp_camera_fb_return(current_fb_);
    current_fb_ = nullptr;
    esp_camera_deinit();
    streaming_on_ = false;
    ESP_LOGI(TAG, "DVP released, JPEG parked len=%u", (unsigned)len);
    return true;
}

size_t Esp32Camera::CapturedJpegLen() const {
    if (current_fb_ != nullptr && current_fb_->format == PIXFORMAT_JPEG) {
        return current_fb_->len;
    }
    return jpeg_parked_len_;
}

bool Esp32Camera::SetSensorJpegQuality(int quality) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s || s->set_quality == nullptr) {
        return false;
    }
    if (quality < 0) {
        quality = 0;
    } else if (quality > 63) {
        quality = 63;
    }
    s->set_quality(s, quality);
    return true;
}

bool Esp32Camera::SetFrameSize(framesize_t size) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s || s->set_framesize == nullptr) {
        return false;
    }
    return s->set_framesize(s, size) == 0;
}

static void SwapRgb565BeToLe(uint8_t* buf, size_t len) {
    uint16_t* p = reinterpret_cast<uint16_t*>(buf);
    const size_t n = len / 2;
    for (size_t i = 0; i < n; ++i) {
        p[i] = __builtin_bswap16(p[i]);
    }
}

static uint8_t* DownsampleRgb565X2(uint8_t* src, uint16_t& w, uint16_t& h) {
    const uint16_t src_w = w;
    const uint16_t src_h = h;
    const uint16_t dst_w = static_cast<uint16_t>(src_w / 2);
    const uint16_t dst_h = static_cast<uint16_t>(src_h / 2);
    if (src == nullptr || dst_w == 0 || dst_h == 0) {
        return src;
    }
    const size_t dst_len = static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 2;
    uint8_t* dst = static_cast<uint8_t*>(
        heap_caps_malloc(dst_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (dst == nullptr) {
        return src;
    }
    const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
    uint16_t* d = reinterpret_cast<uint16_t*>(dst);
    for (uint16_t y = 0; y < dst_h; ++y) {
        const uint16_t* row0 = s + static_cast<size_t>(y * 2) * src_w;
        const uint16_t* row1 = row0 + src_w;
        uint16_t* out = d + static_cast<size_t>(y) * dst_w;
        for (uint16_t x = 0; x < dst_w; ++x) {
            const uint16_t a = row0[x * 2];
            const uint16_t b = row0[x * 2 + 1];
            const uint16_t c = row1[x * 2];
            const uint16_t e = row1[x * 2 + 1];
            const unsigned r =
                ((a >> 11) & 31) + ((b >> 11) & 31) + ((c >> 11) & 31) + ((e >> 11) & 31);
            const unsigned g =
                ((a >> 5) & 63) + ((b >> 5) & 63) + ((c >> 5) & 63) + ((e >> 5) & 63);
            const unsigned bl = (a & 31) + (b & 31) + (c & 31) + (e & 31);
            out[x] = static_cast<uint16_t>(((r / 4) << 11) | ((g / 4) << 5) | (bl / 4));
        }
    }
    heap_caps_free(src);
    w = dst_w;
    h = dst_h;
    return dst;
}

bool Esp32Camera::EncodeAndParkJpeg(size_t max_bytes) {
    if (!streaming_on_ || current_fb_ == nullptr) {
        return false;
    }
    if (current_fb_->format == PIXFORMAT_JPEG) {
        if (!moss_jpeg_still::WithinBudget(current_fb_->len) ||
            !moss_jpeg_still::LooksComplete(current_fb_->buf, current_fb_->len)) {
            return false;
        }
        return ReleaseSensorKeepJpeg();
    }
    if (current_fb_->format != PIXFORMAT_RGB565) {
        return false;
    }
    const uint16_t w = current_fb_->width;
    const uint16_t h = current_fb_->height;
    const size_t src_len = static_cast<size_t>(w) * static_cast<size_t>(h) * 2;
    if (current_fb_->buf == nullptr || current_fb_->len < src_len) {
        return false;
    }

    // Sensor-native RGB565BE. encode_buf_ may already be swapped for LVGL.
    uint8_t* rgb = static_cast<uint8_t*>(
        heap_caps_malloc(src_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (rgb == nullptr) {
        ESP_LOGE(TAG, "Failed to copy RGB565 for SW JPEG");
        return false;
    }
    memcpy(rgb, current_fb_->buf, src_len);
    // OV2640 DVP is RGB565BE (same as face detect). image_to_jpeg RGB565 is LE.
    SwapRgb565BeToLe(rgb, src_len);

    esp_camera_fb_return(current_fb_);
    current_fb_ = nullptr;
    esp_camera_deinit();
    streaming_on_ = false;
    if (encode_buf_) {
        heap_caps_free(encode_buf_);
        encode_buf_ = nullptr;
        encode_buf_size_ = 0;
    }

    uint16_t enc_w = w;
    uint16_t enc_h = h;
    size_t enc_len = src_len;
    int quality = moss_jpeg_still::kSwJpegQualityStart;
    uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;
    int used_q = quality;
    int downscales = 0;
    while (true) {
        uint8_t* out = nullptr;
        size_t out_len = 0;
        const bool enc_ok =
            image_to_jpeg(rgb, enc_len, enc_w, enc_h, V4L2_PIX_FMT_RGB565,
                          static_cast<uint8_t>(quality), &out, &out_len);
        if (!enc_ok || out == nullptr || out_len == 0) {
            ESP_LOGE(TAG, "SW JPEG encode failed quality=%d", quality);
            if (out) {
                free(out);
            }
            break;
        }
        ESP_LOGI(TAG, "SW JPEG %dx%d quality=%d len=%u (budget=%u)", (int)enc_w, (int)enc_h,
                 quality, (unsigned)out_len, (unsigned)max_bytes);
        if (moss_jpeg_still::LooksComplete(out, out_len) && out_len <= max_bytes) {
            jpeg = out;
            jpeg_len = out_len;
            used_q = quality;
            break;
        }
        free(out);

        const bool far_over = moss_jpeg_still::JpegFarOverBudget(out_len, max_bytes);
        const bool at_min_q = quality <= moss_jpeg_still::kSwJpegQualityMin;
        if ((far_over || at_min_q) && downscales < moss_jpeg_still::kSwJpegMaxDownscales &&
            moss_jpeg_still::CanDownscaleRgb(enc_w, enc_h)) {
            const uint16_t prev_w = enc_w;
            const uint16_t prev_h = enc_h;
            uint8_t* shrunk = DownsampleRgb565X2(rgb, enc_w, enc_h);
            if (shrunk == rgb) {
                ESP_LOGE(TAG, "SW JPEG downscale alloc failed");
                if (at_min_q) {
                    break;
                }
                quality = moss_jpeg_still::NextLowerSwQuality(quality);
                continue;
            }
            rgb = shrunk;
            enc_len = static_cast<size_t>(enc_w) * static_cast<size_t>(enc_h) * 2;
            downscales++;
            quality = moss_jpeg_still::kSwJpegQualityStart;
            ESP_LOGW(TAG, "SW JPEG downscale %dx%d -> %dx%d (pass=%d)", (int)prev_w, (int)prev_h,
                     (int)enc_w, (int)enc_h, downscales);
            continue;
        }
        if (at_min_q) {
            break;
        }
        quality = moss_jpeg_still::NextLowerSwQuality(quality);
    }
    heap_caps_free(rgb);

    if (jpeg == nullptr || jpeg_len == 0) {
        jpeg_parked_len_ = 0;
        return false;
    }
    encode_buf_ = static_cast<uint8_t*>(heap_caps_malloc(jpeg_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (encode_buf_ == nullptr) {
        free(jpeg);
        jpeg_parked_len_ = 0;
        ESP_LOGE(TAG, "Failed to park SW JPEG (%u bytes)", (unsigned)jpeg_len);
        return false;
    }
    memcpy(encode_buf_, jpeg, jpeg_len);
    free(jpeg);
    encode_buf_size_ = jpeg_len;
    jpeg_parked_len_ = jpeg_len;
    ESP_LOGI(TAG, "DVP released, SW JPEG parked %dx%d len=%u quality=%d", (int)enc_w, (int)enc_h,
             (unsigned)jpeg_len, used_q);
    return true;
}

bool Esp32Camera::SetHMirror(bool enabled) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_hmirror(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetVFlip(bool enabled) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_vflip(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetSwapBytes(bool enabled) {
    swap_bytes_enabled_ = enabled;
    return true;
}

std::string Esp32Camera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        throw std::runtime_error("Image explain URL or token is not set");
    }

    const uint8_t* jpeg_data = nullptr;
    size_t jpeg_len = 0;
    if (current_fb_ != nullptr && current_fb_->format == PIXFORMAT_JPEG) {
        jpeg_data = current_fb_->buf;
        jpeg_len = current_fb_->len;
    } else if (encode_buf_ != nullptr && jpeg_parked_len_ > 0) {
        jpeg_data = encode_buf_;
        jpeg_len = jpeg_parked_len_;
    } else if (current_fb_ == nullptr) {
        throw std::runtime_error("No camera frame captured");
    }

    // Create local JPEG queue
    QueueHandle_t jpeg_queue = xQueueCreate(40, sizeof(JpegChunk));
    if (jpeg_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        throw std::runtime_error("Failed to create JPEG queue");
    }

    auto queue_jpeg_chunk = [jpeg_queue](const uint8_t* data, size_t len) -> bool {
        JpegChunk chunk = {.data = nullptr, .len = len};
        chunk.data = (uint8_t*)heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (chunk.data == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate %u bytes for JPEG chunk", (unsigned)len);
            return false;
        }
        memcpy(chunk.data, data, len);
        xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
        return true;
    };

    if (jpeg_data != nullptr) {
        // OV2640 等传感器已输出 JPEG，直接透传，勿再经 image_to_jpeg 重编码
        ESP_LOGI(TAG, "JPEG passthrough upload, len=%u", (unsigned)jpeg_len);
        if (!queue_jpeg_chunk(jpeg_data, jpeg_len)) {
            vQueueDelete(jpeg_queue);
            throw std::runtime_error("Failed to copy JPEG frame");
        }
        JpegChunk end_chunk = {.data = nullptr, .len = 0};
        xQueueSend(jpeg_queue, &end_chunk, portMAX_DELAY);
    } else {
        encoder_thread_ = std::thread([this, jpeg_queue]() {
            int64_t start_time = esp_timer_get_time();
            uint16_t w = current_fb_->width;
            uint16_t h = current_fb_->height;
            v4l2_pix_fmt_t enc_fmt;
            switch (current_fb_->format) {
                case PIXFORMAT_RGB565:
                    enc_fmt = V4L2_PIX_FMT_RGB565;
                    break;
                case PIXFORMAT_YUV422:
                    enc_fmt = V4L2_PIX_FMT_YUYV;
                    break;
                case PIXFORMAT_YUV420:
                    enc_fmt = V4L2_PIX_FMT_YUV420;
                    break;
                case PIXFORMAT_GRAYSCALE:
                    enc_fmt = V4L2_PIX_FMT_GREY;
                    break;
                case PIXFORMAT_RGB888:
                    enc_fmt = V4L2_PIX_FMT_RGB24;
                    break;
                default:
                    ESP_LOGE(TAG, "Unsupported pixel format: %d", current_fb_->format);
                    return;
            }

            uint8_t* jpeg_src_buf = current_fb_->buf;
            size_t jpeg_src_len = current_fb_->len;
            if (current_fb_->format == PIXFORMAT_RGB565 && encode_buf_ != nullptr) {
                jpeg_src_buf = encode_buf_;
                jpeg_src_len = encode_buf_size_;
            }

            bool ok = image_to_jpeg_cb(
                jpeg_src_buf, jpeg_src_len, w, h, enc_fmt, 80,
                [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                    auto q = static_cast<QueueHandle_t>(arg);
                    JpegChunk chunk = {.data = nullptr, .len = len};
                    if (index == 0 && data != nullptr && len > 0) {
                        chunk.data = (uint8_t*)heap_caps_aligned_alloc(
                            16, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (chunk.data == nullptr) {
                            ESP_LOGE(TAG, "Failed to allocate %u bytes for JPEG chunk",
                                     (unsigned)len);
                            chunk.len = 0;
                        } else {
                            memcpy(chunk.data, data, len);
                        }
                    } else {
                        chunk.len = 0;
                    }
                    xQueueSend(q, &chunk, portMAX_DELAY);
                    return len;
                },
                jpeg_queue);

            if (!ok) {
                JpegChunk err_chunk = {.data = nullptr, .len = 0};
                xQueueSend(jpeg_queue, &err_chunk, portMAX_DELAY);
            }
            int64_t end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "JPEG encoding time: %ld ms", (long)((end_time - start_time) / 1000));
        });
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (!http->Open("POST", explain_url_)) {
        ESP_LOGE(TAG, "Failed to connect to explain URL");
        if (encoder_thread_.joinable()) {
            encoder_thread_.join();
        }
        JpegChunk chunk;
        while (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) == pdPASS) {
            if (chunk.data != nullptr) {
                heap_caps_free(chunk.data);
            } else {
                break;
            }
        }
        vQueueDelete(jpeg_queue);
        throw std::runtime_error("Failed to connect to explain URL");
    }

    {
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        http->Write(question_field.c_str(), question_field.size());
    }
    {
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        http->Write(file_header.c_str(), file_header.size());
    }

    size_t total_sent = 0;
    bool saw_terminator = false;
    while (true) {
        JpegChunk chunk;
        if (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to receive JPEG chunk");
            break;
        }
        if (chunk.data == nullptr) {
            saw_terminator = true;
            break;
        }
        http->Write((const char*)chunk.data, chunk.len);
        total_sent += chunk.len;
        heap_caps_free(chunk.data);
    }
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }
    vQueueDelete(jpeg_queue);

    if (!saw_terminator || total_sent == 0) {
        ESP_LOGE(TAG, "JPEG encoder failed or produced empty output");
        throw std::runtime_error("Failed to encode image to JPEG");
    }

    {
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        http->Write(multipart_footer.c_str(), multipart_footer.size());
    }
    http->Write("", 0);

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", http->GetStatusCode());
        throw std::runtime_error("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();

    size_t remain_stack_size = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "Explain compressed size=%d, remain stack size=%d, question=%s\n%s",
             (int)total_sent, (int)remain_stack_size, question.c_str(), result.c_str());
    return result;
}
