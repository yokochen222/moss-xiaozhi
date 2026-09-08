#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "device/lamp_bar.h"
#include "device/lamp_eye.h"
#include "display/display.h"
#include "display/oled_display.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "power_save_timer.h"
#include "press_to_talk_mcp_tool.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_ssd1306.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#define TAG "MossBreadCompactBoard"

static constexpr int kScreenOffIdleSeconds = 30;

class MossBreadCompactBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Display* display_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, kScreenOffIdleSeconds, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Idle %ds -> screen off", kScreenOffIdleSeconds);
            GetDisplay()->SetPowerSaveMode(true);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "Wake -> screen on");
            GetDisplay()->SetPowerSaveMode(false);
        });
    }

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeOledDisplay() {
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags =
                {
                    .dc_low_on_data = 0,
                    .disable_control_phase = 0,
                },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize OLED");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_->SetStatus(Lang::Strings::INITIALIZING);
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

        // Source bread-compact: GPIO47 is a dedicated hold-to-talk pad.
        touch_button_.OnPressDown([this]() {
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() { Application::GetInstance().StopListening(); });
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
    MossBreadCompactBoard()
        : boot_button_(BOOT_BUTTON_GPIO), touch_button_(TOUCH_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeOledDisplay();
        InitializeButtons();
        InitializeTools();
        InitializePowerSaveTimer();
#if MOSS_MCP_PERIPHERALS_ENABLE
        LampBarDevice::GetInstance().Initialize();
        LampEyeDevice::GetInstance().Initialize();
#endif
    }

    Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }

    Backlight* GetBacklight() override { return nullptr; }

    void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (power_save_timer_) {
            if (level == PowerSaveLevel::LOW_POWER) {
                power_save_timer_->WakeUp();
                power_save_timer_->SetEnabled(true);
            } else {
                power_save_timer_->SetEnabled(false);
            }
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(MossBreadCompactBoard);
