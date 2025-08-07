#include "wifi_board.h"
#include "audio/codecs/box_audio_codec.h"
#include "display/oled_display.h"
#include "esp_lcd_panel_sh1106.h"
#include "display/display.h"  // 包含NoDisplay定义
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
// #include "iot/thing_manager.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <wifi_station.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "LichuangDevBoard"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class Pca9557 : public I2cDevice {
public:
    Pca9557(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        // PCA9557初始化 - 暂时跳过，不影响系统运行
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        uint8_t data = ReadReg(0x01);
        data = (data & ~(1 << bit)) | (level << bit);
        WriteReg(0x01, data);
    }
};


class LichuangDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Display* display_;
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    void InitializeI2c() {
        // Initialize I2C peripheral for audio codec and OLED display
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeDisplayI2c() {
        // 显示和音频编解码器共用同一个I2C总线
        display_i2c_bus_ = i2c_bus_;
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
    }

    void InitializeOledDisplay() {
        // 扫描I2C设备
        ESP_LOGI(TAG, "Scanning I2C devices...");
        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
            esp_lcd_panel_io_i2c_config_t test_config = {
                .dev_addr = addr,
                .on_color_trans_done = nullptr,
                .user_ctx = nullptr,
                .control_phase_bytes = 1,
                .dc_bit_offset = 6,
                .lcd_cmd_bits = 8,
                .lcd_param_bits = 8,
                .flags = {
                    .dc_low_on_data = 0,
                    .disable_control_phase = 0,
                },
                .scl_speed_hz = 100 * 1000,  // 降低速度提高稳定性
            };
            
            esp_lcd_panel_io_handle_t test_io = nullptr;
            if (esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &test_config, &test_io) == ESP_OK) {
                ESP_LOGI(TAG, "Found I2C device at address 0x%02X", addr);
                if (addr == 0x3C || addr == 0x3D) {
                    ESP_LOGI(TAG, "Found OLED display at address 0x%02X", addr);
                    // 使用实际发现的地址
                    test_config.dev_addr = addr;
                    break;
                }
                esp_lcd_panel_io_del(test_io);
            }
        }

        // OLED配置
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 50 * 1000,  // 进一步降低I2C速度到50kHz
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install OLED driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        // 尝试SH1106驱动（兼容SSD1306的另一种驱动）
        esp_err_t ret = ESP_OK;
#ifdef SH1106
        ret = esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_);
#else
        ret = esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SSD1306 init failed, trying SH1106...");
            ret = esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_);
        }
#endif
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display: %s", esp_err_to_name(ret));
            display_ = new NoDisplay();
            return;
        }
        ESP_LOGI(TAG, "OLED driver installed");

        // Reset the display
        ESP_LOGI(TAG, "Resetting OLED display...");
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        
        ESP_LOGI(TAG, "Initializing OLED display...");
        vTaskDelay(pdMS_TO_TICKS(100));  // 等待复位完成
        
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        // 清屏并显示测试图案
        ESP_LOGI(TAG, "Initializing OLED display...");
        uint8_t* buffer = (uint8_t*)malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT / 8);
        if (buffer) {
            memset(buffer, 0xFF, DISPLAY_WIDTH * DISPLAY_HEIGHT / 8);  // 全亮测试
            esp_lcd_panel_draw_bitmap(panel_, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, buffer);
            free(buffer);
            ESP_LOGI(TAG, "Display test pattern sent (all pixels on)");
        } else {
            ESP_LOGE(TAG, "Failed to allocate display buffer");
        }

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
            {&font_puhui_20_4, &font_awesome_20_4});
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        // auto& thing_manager = iot::ThingManager::GetInstance();
        // thing_manager.AddThing(iot::CreateThing("Speaker"));
        // thing_manager.AddThing(iot::CreateThing("Screen"));
    }

public:
    LichuangDevBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeDisplayI2c();
        InitializeOledDisplay();
        InitializeButtons();
        InitializeIot();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_NC, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        return nullptr; // OLED doesn't need backlight control
    }
};

DECLARE_BOARD(LichuangDevBoard);
