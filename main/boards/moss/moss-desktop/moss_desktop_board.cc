#include "application.h"
#include "assets/lang_config.h"
#include "audio_scope.h"
#include "backlight.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "device/face_tracker.h"
#include "device/moss_camera_stream.h"
#include "device/stepper_gimbal.h"
#include "display/display.h"
#include "esp32_camera.h"
#include "mcp_server.h"
#include "moss_spi_lcd_display.h"
#include "pca9685_driver.h"
#include "power_save_timer.h"
#include "press_to_talk_mcp_tool.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/spi_common.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_lcd_io_spi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_st7735.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>
#include <memory>
#include <mutex>
#include <stdexcept>

#define TAG "MossDesktopBoard"

// 在 codec 读写之后抽 PCM 给信号弹窗. 不占用 I2S, 不改采样.
class MossDesktopAudioCodec : public BoxAudioCodec {
public:
    using BoxAudioCodec::BoxAudioCodec;

    void OutputData(std::vector<int16_t>& data) override {
        BoxAudioCodec::OutputData(data);
        moss_splash::audio_scope_feed(moss_splash::AudioScopeSource::Playback, data.data(),
                                      data.size(), output_channels());
    }

    bool InputData(std::vector<int16_t>& data) override {
        const bool ok = BoxAudioCodec::InputData(data);
        if (ok) {
            moss_splash::audio_scope_feed(moss_splash::AudioScopeSource::Capture, data.data(),
                                          data.size(), input_channels());
        }
        return ok;
    }
};

// LCD_BL → PCA9685 LED0（高有效 / PWM 调光）
class Pca9685Backlight : public Backlight {
public:
    explicit Pca9685Backlight(uint8_t channel, bool output_invert = false)
        : Backlight(), channel_(channel), output_invert_(output_invert) {}

    // Splash 前需立刻点亮；跳过 Backlight 渐变（否则开机动画前半段仍看不见）
    void ApplyImmediate(uint8_t brightness) {
        if (brightness > 100) {
            brightness = 100;
        }
        brightness_ = brightness;
        target_brightness_ = brightness;
        SetBrightnessImpl(brightness);
    }

    void SetBrightnessImpl(uint8_t brightness) override {
        if (!Pca9685::GetInstance().IsReady()) {
            return;
        }
        uint8_t percent = brightness;
        if (output_invert_) {
            percent = static_cast<uint8_t>(100 - brightness);
        }
        uint16_t duty = static_cast<uint16_t>((static_cast<uint32_t>(percent) * 4095 + 50) / 100);
        Pca9685::GetInstance().SetDuty(channel_, duty);
    }

private:
    uint8_t channel_;
    bool output_invert_;
};

// 按需启停摄像头：平时释放 DVP DMA，避免与 LCD SPI / WiFi 争抢内部 DMA。
// FaceTrackCamera：跟踪态切 RGB565；默认 JPEG 路径语义不变。
class OnDemandEsp32Camera : public Camera, public FaceTrackCamera, public LiveJpegSource {
public:
    explicit OnDemandEsp32Camera(const camera_config_t& config) : config_(config) {}

    void SetExplainUrl(const std::string& url, const std::string& token) override {
        std::lock_guard<std::mutex> lock(mutex_);
        explain_url_ = url;
        explain_token_ = token;
        if (camera_) {
            camera_->SetExplainUrl(url, token);
        }
    }

    bool Capture() override {
        const bool own_session = !stream_acquired_;
        if (own_session) {
            FaceTracker::GetInstance().PauseForExternalCameraUse();
            PauseLcdForDvp();
        }
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!EnsureStartedLocked()) {
                ok = false;
            } else if (!camera_->Capture()) {
                if (!stream_acquired_) {
                    ReleaseLocked();
                }
                ok = false;
            } else {
                ok = true;
            }
        }
        if (!ok && own_session) {
            FaceTracker::GetInstance().ResumeAfterExternalCameraUse();
            ResumeLcdAfterDvp();
        }
        return ok;
    }

    bool SetHMirror(bool enabled) override {
        if (FaceTracker::GetInstance().IsRunning()) {
            ESP_LOGW(TAG, "SetHMirror ignored while face tracking is active");
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureStartedLocked()) {
            return false;
        }
        return camera_->SetHMirror(enabled);
    }

    bool SetVFlip(bool enabled) override {
        if (FaceTracker::GetInstance().IsRunning()) {
            ESP_LOGW(TAG, "SetVFlip ignored while face tracking is active");
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureStartedLocked()) {
            return false;
        }
        return camera_->SetVFlip(enabled);
    }

    std::string Explain(const std::string& question) override {
        Esp32Camera* cam = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!camera_) {
                FaceTracker::GetInstance().ResumeAfterExternalCameraUse();
                ResumeLcdAfterDvp();
                throw std::runtime_error("Camera not started");
            }
            cam = camera_.get();
        }

        try {
            std::string result = cam->Explain(question);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!stream_acquired_) {
                    ReleaseLocked();
                }
            }
            if (!stream_acquired_) {
                FaceTracker::GetInstance().ResumeAfterExternalCameraUse();
                ResumeLcdAfterDvp();
            }
            return result;
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!stream_acquired_) {
                    ReleaseLocked();
                }
            }
            if (!stream_acquired_) {
                FaceTracker::GetInstance().ResumeAfterExternalCameraUse();
                ResumeLcdAfterDvp();
            }
            throw;
        }
    }

    bool AcquireLiveStream() override {
        FaceTracker::GetInstance().PauseForExternalCameraUse();
        if (VoiceBusy()) {
            std::lock_guard<std::mutex> lock(mutex_);
            stream_refs_++;
            stream_acquired_ = true;
            return true;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (tracking_acquired_) {
            ReleaseTrackingLocked();
        }
        if (!EnsureStartedLocked(true)) {
            if (stream_refs_ == 0) {
                FaceTracker::GetInstance().ResumeAfterExternalCameraUse();
            }
            return false;
        }
        stream_refs_++;
        stream_acquired_ = true;
        return true;
    }

    void ReleaseLiveStream() override {
        bool resume = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stream_refs_ > 0) {
                stream_refs_--;
            }
            if (stream_refs_ == 0 && stream_acquired_) {
                stream_acquired_ = false;
                ReleaseLocked();
                resume = true;
            }
        }
        if (resume) {
            FaceTracker::GetInstance().ResumeAfterExternalCameraUse();
        }
    }

    camera_fb_t* GrabJpeg() override {
        if (VoiceBusy()) {
            std::lock_guard<std::mutex> lock(mutex_);
            ReleaseLocked();
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stream_acquired_) {
            return nullptr;
        }
        if (!camera_ || !camera_->IsInitialized()) {
            if (!EnsureStartedLocked(true)) {
                return nullptr;
            }
        }
        for (int attempt = 0; attempt < 6; ++attempt) {
            camera_fb_t* fb = esp_camera_fb_get();
            if (JpegLooksComplete(fb)) {
                return fb;
            }
            if (fb) {
                ESP_LOGW(TAG, "Drop incomplete JPEG len=%u", (unsigned)fb->len);
                esp_camera_fb_return(fb);
            }
        }
        return nullptr;
    }

    void ReturnJpeg(camera_fb_t* fb) override {
        if (!fb) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        esp_camera_fb_return(fb);
    }

    bool AcquireTracking() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stream_acquired_) {
            ESP_LOGW(TAG, "AcquireTracking blocked while live stream is active");
            return false;
        }
        if (tracking_acquired_) {
            return true;
        }
        // Drop JPEG instance if any.
        ReleaseLocked();

        auto& pca = Pca9685::GetInstance();
        if (pca.IsReady()) {
            pca.SetDvpPowerDown(false);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        camera_config_t cfg = config_;
        cfg.pixel_format = PIXFORMAT_RGB565;
        // HVGA (480x320): ~2.25x pixels vs QVGA — distant faces were ~24px on 320-wide.
        cfg.frame_size = FRAMESIZE_HVGA;
        cfg.jpeg_quality = 12;
        // Detect holds a frame ~600ms. fb_count=1 lets DVP overwrite that buffer → tearing stripes.
        cfg.fb_count = 2;
        cfg.fb_location = CAMERA_FB_IN_PSRAM;
        cfg.grab_mode = CAMERA_GRAB_LATEST;

        const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
        const size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        const size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG,
                 "Tracking OV2640 start (RGB565 HVGA) free_int=%u free_dma=%u largest_dma=%u "
                 "free_psram=%u",
                 (unsigned)free_internal, (unsigned)free_dma, (unsigned)largest_dma,
                 (unsigned)free_spiram);

        esp_err_t err = esp_camera_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Tracking double-buffer init failed: 0x%x, retry fb_count=1", err);
            cfg.fb_count = 1;
            err = esp_camera_init(&cfg);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Tracking esp_camera_init failed: 0x%x", err);
            return false;
        }
        sensor_t* s = esp_camera_sensor_get();
        if (s && s->id.PID == OV2640_PID) {
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
        tracking_acquired_ = true;
        vTaskDelay(pdMS_TO_TICKS(200));
        return true;
    }

    void ReleaseTracking() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ReleaseTrackingLocked();
    }

    bool IsTrackingAcquired() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return tracking_acquired_;
    }

    camera_fb_t* GrabTrackingFrame() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!tracking_acquired_) {
            return nullptr;
        }
        return esp_camera_fb_get();
    }

    void ReturnTrackingFrame(camera_fb_t* fb) override {
        if (!fb) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        esp_camera_fb_return(fb);
    }

private:
    static bool VoiceBusy() {
        const auto state = Application::GetInstance().GetDeviceState();
        return state == kDeviceStateSpeaking || state == kDeviceStateListening ||
               state == kDeviceStateConnecting;
    }

    bool EnsureStartedLocked(bool preview = false) {
        if (tracking_acquired_) {
            // Caller should have paused tracker; force release if still held.
            ReleaseTrackingLocked();
        }
        if (camera_ && camera_->IsInitialized()) {
            return true;
        }
        camera_.reset();

        auto& pca = Pca9685::GetInstance();
        if (pca.IsReady()) {
            // PWDN 低 = OV2640 工作；传感器上电后需留足稳定时间再 SCCB/DVP
            pca.SetDvpPowerDown(false);
            vTaskDelay(pdMS_TO_TICKS(80));
        }

        camera_config_t cfg = config_;
        if (preview) {
            cfg.frame_size = FRAMESIZE_QVGA;
            cfg.jpeg_quality = 12;
        }

        const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
        const size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        const size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG,
                 "On-demand OV2640 start (JPEG XCLK=%dHz size=%d) free_int=%u free_dma=%u "
                 "largest_dma=%u free_psram=%u",
                 (int)cfg.xclk_freq_hz, (int)cfg.frame_size, (unsigned)free_internal,
                 (unsigned)free_dma, (unsigned)largest_dma, (unsigned)free_spiram);
        if (largest_dma < 8192) {
            ESP_LOGW(TAG,
                     "Internal DMA heap fragmented (largest=%u); rely on PSRAM DMA / small DMA buf",
                     (unsigned)largest_dma);
        }

        camera_.reset(new Esp32Camera(cfg));
        if (!camera_->IsInitialized()) {
            ESP_LOGE(TAG, "OV2640 esp_camera_init failed (see Esp32Camera logs)");
            camera_.reset();
            return false;
        }
        if (!explain_url_.empty()) {
            camera_->SetExplainUrl(explain_url_, explain_token_);
        } else {
            ESP_LOGW(TAG, "Vision explain URL not set yet; Capture ok but Explain may fail");
        }
        // OV2640 AEC/AWB lock only after a real continuous stream, not a few fb_get calls.
        vTaskDelay(pdMS_TO_TICKS(200));
        DrainWarmupFramesLocked();
        return true;
    }

    void ReleaseLocked() {
        if (!camera_) {
            return;
        }
        ESP_LOGI(TAG, "On-demand OV2640 stop (free DVP DMA)");
        camera_.reset();
        // Keep PWDN low (sensor powered) like moss-xiaozhi; only free DVP DMA.
    }

    void ReleaseTrackingLocked() {
        if (!tracking_acquired_) {
            return;
        }
        ESP_LOGI(TAG, "Tracking OV2640 stop (free DVP DMA)");
        esp_camera_deinit();
        tracking_acquired_ = false;
    }

    static void PauseLcdForDvp() {
        if (auto* d = Board::GetInstance().GetDisplay()) {
            d->PauseBackgroundAnimation();
        }
    }

    static void ResumeLcdAfterDvp() {
        if (auto* d = Board::GetInstance().GetDisplay()) {
            d->ResumeBackgroundAnimation();
        }
    }

    static bool JpegLooksComplete(const camera_fb_t* fb) {
        if (!fb || !fb->buf || fb->len < 128) {
            return false;
        }
        if (fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
            return false;
        }
        return fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9;
    }

    void DrainWarmupFramesLocked() {
        const int64_t t0 = esp_timer_get_time();
        const int64_t min_us = 2000 * 1000;
        const int64_t max_us = 4000 * 1000;
        int n = 0;
        while (true) {
            camera_fb_t* fb = esp_camera_fb_get();
            if (fb) {
                esp_camera_fb_return(fb);
                ++n;
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            const int64_t elapsed = esp_timer_get_time() - t0;
            if (elapsed >= min_us && n >= 15) {
                break;
            }
            if (elapsed >= max_us) {
                break;
            }
        }
        ESP_LOGI(TAG, "JPEG warmup dropped %d frames in %d ms", n,
                 (int)((esp_timer_get_time() - t0) / 1000));
    }

    camera_config_t config_{};
    std::unique_ptr<Esp32Camera> camera_;
    std::string explain_url_;
    std::string explain_token_;
    mutable std::mutex mutex_;
    bool tracking_acquired_ = false;
    bool stream_acquired_ = false;
    int stream_refs_ = 0;
};

// waveshare esp_lcd_st7735 default init lacks INVON for most 0.96" ST7735S modules.
static void SendSt7735VendorInit(esp_lcd_panel_io_handle_t io) {
    esp_lcd_panel_io_tx_param(io, ST7735_INVON, NULL, 0);
    ESP_LOGI(TAG, "ST7735 vendor init (INVON) sent");
}

class MossDesktopBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    Display* display_ = nullptr;
    OnDemandEsp32Camera* camera_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializePca9685() {
        auto& pca = Pca9685::GetInstance();
        if (!pca.Init(i2c_bus_, PCA9685_I2C_ADDR)) {
            ESP_LOGE(TAG,
                     "PCA9685 init failed; LCD_BL / PA / lamps / eye motor / camera may not work");
            return;
        }
        // LED0 背光先关；真正点亮在 panel on 之后、splash 之前
        pca.SetDuty(PCA9685_CH_LCD_BL, 0);
        // LED1=NS4150B EN：上电保持低电平，等 ES8311/ES7210 初始化成功后再使能
        pca.SetDigital(PCA9685_CH_NS4150B_EN, false);
        // PWDN 低 = OV2640 工作（与 moss-xiaozhi 一致，传感器保持上电）
        pca.SetDvpPowerDown(false);
        ESP_LOGI(TAG, "PCA9685 ready: LCD_BL=LED0, PA_EN=LOW (defer), DVP_PWDN=LOW");
    }

    void InitializeCamera() {
        camera_config_t config = {};
        // 背光已走 PCA9685，LEDC0 留给 XCLK
        config.ledc_channel = LEDC_CHANNEL_0;
        config.ledc_timer = LEDC_TIMER_0;
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        // 复用板级 I2C port1（IO1/IO2）
        config.pin_sccb_sda = -1;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_JPEG;
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        // JPEG 在 PSRAM；双缓冲避免 GrabJpeg 持帧时下一帧撕开
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;

        camera_ = new OnDemandEsp32Camera(config);
        FaceTracker::GetInstance().SetCamera(camera_);
        MossCameraStream::GetInstance().SetSource(camera_);
        ESP_LOGI(TAG, "OV2640 registered (on-demand JPEG XCLK=%d XCLK=IO%d D6=IO%d)", XCLK_FREQ_HZ,
                 (int)CAMERA_PIN_XCLK, (int)CAMERA_PIN_D6);
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus for 0.96\" ST7735");
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_SPI_MAX_TRANSFER;
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeSt7735Display() {
        ESP_LOGI(TAG,
                 "Install ST7735 LCD panel IO (MOSI=%d SCK=%d CS=%d DC=%d clk=%dHz BL=PCA LED%d)",
                 DISPLAY_SPI_MOSI_PIN, DISPLAY_SPI_SCK_PIN, DISPLAY_SPI_CS_PIN, DISPLAY_DC_PIN,
                 DISPLAY_SPI_CLOCK_HZ, PCA9685_CH_LCD_BL);

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_SPI_CLOCK_HZ;
        io_config.trans_queue_depth = 1;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install ST7735 LCD panel driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = (DISPLAY_RST_PIN == GPIO_NUM_NC) ? -1 : DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(panel_io_, &panel_config, &panel_));

        // RST 绑 EN：上电后稍等再 SWRESET / init
        vTaskDelay(pdMS_TO_TICKS(50));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        vTaskDelay(pdMS_TO_TICKS(120));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize LCD panel");
            display_ = new NoDisplay();
            return;
        }

        SendSt7735VendorInit(panel_io_);

        esp_lcd_panel_invert_color(panel_, true);
        esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_set_gap(panel_, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
        ESP_LOGI(TAG, "Panel: invert=true swap_xy=%d mirror_x=%d mirror_y=%d offset=(%d,%d)",
                 (int)DISPLAY_SWAP_XY, (int)DISPLAY_MIRROR_X, (int)DISPLAY_MIRROR_Y,
                 (int)DISPLAY_OFFSET_X, (int)DISPLAY_OFFSET_Y);

        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        // MossSpiLcdDisplay 构造里会阻塞播放 splash；必须先点亮背光才能看见动画。
        static_cast<Pca9685Backlight*>(GetBacklight())->ApplyImmediate(75);

        // MossSpiLcdDisplay plays embedded emote-assets.bin splash then code-scroll loop.
        display_ = new MossSpiLcdDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                         DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                         DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        display_->SetStatus(Lang::Strings::INITIALIZING);

        // Face-track UI: stop code scroll + draw HUD while tracking.
        auto* moss_disp = static_cast<MossSpiLcdDisplay*>(display_);
        FaceTracker::GetInstance().SetUiHooks(
            [this, moss_disp]() {
                moss_disp->EnterFaceTrackMode();
                if (power_save_timer_) {
                    power_save_timer_->SetEnabled(false);
                }
            },
            [this, moss_disp]() {
                moss_disp->ExitFaceTrackMode();
                if (power_save_timer_) {
                    power_save_timer_->SetEnabled(true);
                }
            });
        FaceTracker::GetInstance().SetStatusSink(
            [moss_disp](const FaceTrackerStatus& s) { moss_disp->UpdateFaceTrackOverlay(s); });
    }

    void InitializePowerSaveTimer() {
        // 待命满 60s 只关屏, 唤醒词继续跑. 连接/聆听/说话时停表, 避免对话中熄屏.
        power_save_timer_ = new PowerSaveTimer(-1, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
                return;
            }
            GetDisplay()->SetPowerSaveMode(true);
        });
        power_save_timer_->OnExitSleepMode([this]() { GetDisplay()->SetPowerSaveMode(false); });
        if (panel_ != nullptr && display_ != nullptr) {
            auto* moss_disp = static_cast<MossSpiLcdDisplay*>(display_);
            moss_disp->OnConversationKeepAwake([this](bool keep_awake) {
                if (power_save_timer_ == nullptr) {
                    return;
                }
                power_save_timer_->SetEnabled(!keep_awake);
            });
        }
        power_save_timer_->SetEnabled(true);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting &&
                !WifiManager::GetInstance().IsConnected()) {
                EnterWifiConfigMode();
                return;
            }
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnPressDown([this]() {
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StopListening();
            }
        });
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
                           "End this conversation and enter WiFi configuration mode.\n"
                           "**CAUTION** You must ask the user to confirm this action.",
                           PropertyList(), [this](const PropertyList& properties) {
                               EnterWifiConfigMode();
                               return true;
                           });

        press_to_talk_tool_ = new PressToTalkMcpTool();
        press_to_talk_tool_->Initialize();

#if MOSS_MCP_PERIPHERALS_ENABLE
        ESP_LOGI(TAG, "Moss MCP peripheral tools registered via static constructors");
#endif
    }

public:
    MossDesktopBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializePca9685();
        InitializeSpi();
        InitializeSt7735Display();
        InitializeCamera();
        InitializeButtons();
        InitializeTools();
        InitializePowerSaveTimer();
        // 上电清零 595，避免随机输出导致步进线圈常通发烫
        StepperGimbalDevice::GetInstance().Stop();
    }

    AudioCodec* GetAudioCodec() override {
        // Soft USB reset can leave ES7210 mid-transaction; clear the shared bus first.
        if (i2c_bus_) {
            i2c_master_bus_reset(i2c_bus_);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        static MossDesktopAudioCodec audio_codec(
            i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE, AUDIO_CODEC_INPUT_GAIN, 2, 0.0f);
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }

    Camera* GetCamera() override { return camera_; }

    Backlight* GetBacklight() override {
        static Pca9685Backlight backlight(PCA9685_CH_LCD_BL, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(MossDesktopBoard);
