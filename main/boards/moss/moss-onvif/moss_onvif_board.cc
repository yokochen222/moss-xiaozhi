#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "assets/lang_config.h"
#include "backlight.h"
#include "mcp_server.h"
#include "press_to_talk_mcp_tool.h"
#include "moss_spi_lcd_display.h"
#include "power_save_timer.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_io_spi.h>
#include <esp_lcd_st7735.h>
#include <driver/spi_common.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#define TAG "MossOnvifBoard"

// Idle backlight/panel blank timeout (seconds). Wake word stays on (cpu_max_freq=-1).
static constexpr int kScreenOffIdleSeconds = 30;

// waveshare esp_lcd_st7735 default init lacks INVON for most 0.96" ST7735S modules.
static void SendSt7735VendorInit(esp_lcd_panel_io_handle_t io) {
    esp_lcd_panel_io_tx_param(io, ST7735_INVON, NULL, 0);
    ESP_LOGI(TAG, "ST7735 vendor init (INVON) sent");
}

class MossOnvifBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    Display* display_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;

    void InitializePowerSaveTimer() {
        // cpu_max_freq=-1: only blank the screen, keep wake-word / audio running.
        power_save_timer_ = new PowerSaveTimer(-1, kScreenOffIdleSeconds, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Idle %ds -> screen off", kScreenOffIdleSeconds);
            if (auto* moss = dynamic_cast<MossSpiLcdDisplay*>(GetDisplay())) {
                moss->SetScreenOn(false);
            } else {
                GetDisplay()->SetPowerSaveMode(true);
            }
            GetBacklight()->SetBrightness(0);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "Wake -> screen on");
            // Backlight first: disp_on_off shares SPI with splash and can wait.
            GetBacklight()->RestoreBrightness();
            if (auto* moss = dynamic_cast<MossSpiLcdDisplay*>(GetDisplay())) {
                moss->SetScreenOn(true);
            } else {
                GetDisplay()->SetPowerSaveMode(false);
            }
        });
        // Enabled only after the device enters idle.
    }

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
        ESP_LOGI(TAG, "Install ST7735 LCD panel IO");

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 1;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install ST7735 LCD panel driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(panel_io_, &panel_config, &panel_));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
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

        // MossSpiLcdDisplay plays embedded emote-assets.bin splash then code-scroll loop.
        display_ = new MossSpiLcdDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                         DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                         DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
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
    MossOnvifBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7735Display();
        InitializeButtons();
        InitializeTools();
        InitializePowerSaveTimer();
        GetBacklight()->RestoreBrightness();
    }

    AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec* codec = nullptr;
        if (codec == nullptr) {
            if (i2c_bus_) {
                i2c_master_bus_reset(i2c_bus_);
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            static BoxAudioCodec audio_codec(
                i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
                AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
                AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
                AUDIO_INPUT_REFERENCE, AUDIO_CODEC_INPUT_GAIN, AUDIO_CODEC_REFERENCE_CHANNEL,
                AUDIO_CODEC_REFERENCE_GAIN);
            codec = &audio_codec;
        }
        return codec;
    }

    Display* GetDisplay() override { return display_; }

    Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    // Screen-off timer only runs in idle (LOW_POWER). Any active state keeps the screen on.
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

DECLARE_BOARD(MossOnvifBoard);
