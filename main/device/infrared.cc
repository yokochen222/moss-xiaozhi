#include "infrared.h"

#include "api/api.h"
#include "application.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cctype>

#define TAG "InfraredDevice"

namespace {
constexpr TickType_t kRxPollTicks = pdMS_TO_TICKS(40);
constexpr int64_t kLearnTimeoutUs = 20LL * 1000LL * 1000LL;
}  // namespace

InfraredDevice::InfraredDevice()
    : uart_num_(kUartPort),
      uart_listener_task_handle_(nullptr),
      uart_ok_(false),
      learning_(false),
      learn_deadline_us_(0) {}

InfraredDevice::~InfraredDevice() {
    if (uart_ok_) {
        uart_ok_ = false;
        if (uart_listener_task_handle_) {
            vTaskDelete(uart_listener_task_handle_);
            uart_listener_task_handle_ = nullptr;
        }
        uart_driver_delete(uart_num_);
    }
}

InfraredDevice& InfraredDevice::GetInstance() {
    static InfraredDevice instance;
    return instance;
}

void InfraredDevice::Start() { EnsureUart(); }

void InfraredDevice::EnsureUart() {
    if (uart_ok_) {
        return;
    }
    InitializeUart();
    StartUartListenerTask();
}

void InfraredDevice::InitializeUart() {
    const uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(uart_num_, kRxDriverBuf, 256, 0, NULL, 0);
    ESP_LOGI(TAG, "uart_driver_install: %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed");
        return;
    }

    ret = uart_param_config(uart_num_, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        uart_driver_delete(uart_num_);
        return;
    }

    ret = uart_set_pin(uart_num_, kTxPin, kRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        uart_driver_delete(uart_num_);
        return;
    }

    gpio_set_pull_mode(kRxPin, GPIO_PULLUP_ONLY);
    uart_flush(uart_num_);
    uart_ok_ = true;
    ESP_LOGI(TAG, "UART ready port=%d tx=%d rx=%d baud=9600", static_cast<int>(uart_num_),
             static_cast<int>(kTxPin), static_cast<int>(kRxPin));
}

bool InfraredDevice::StartLearn() {
    EnsureUart();
    if (!uart_ok_) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        learning_ = true;
        rx_acc_.clear();
        rx_acc_.reserve(2048);
        learn_deadline_us_ = esp_timer_get_time() + kLearnTimeoutUs;
    }
    uart_flush(uart_num_);
    ESP_LOGI(TAG, "Enter IR learn mode");
    return SendIrCommand("xx00");
}

bool InfraredDevice::SendIrCommand(const std::string& command) {
    EnsureUart();
    if (!uart_ok_) {
        ESP_LOGE(TAG, "UART not initialized, cannot send IR command");
        return false;
    }

    const bool is_learn = command.rfind("xx", 0) == 0;
    if (!is_learn) {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        learning_ = false;
        rx_acc_.clear();
    }

    std::string wire = command;
    if (!wire.empty() && wire.back() != '\r' && wire.back() != '\n') {
        wire.push_back('\r');
    }

    ESP_LOGI(TAG, "Sending IR command (%u bytes): %s", static_cast<unsigned>(wire.size()),
             command.c_str());
    int ret = uart_write_bytes(uart_num_, wire.c_str(), wire.size());
    if (ret <= 0) {
        ESP_LOGE(TAG, "uart_write_bytes failed: %d", ret);
        return false;
    }
    uart_wait_tx_done(uart_num_, pdMS_TO_TICKS(200));
    return true;
}

bool InfraredDevice::HasLenTerminator(const std::string& data) {
    size_t pos = data.rfind("len=");
    if (pos == std::string::npos) {
        pos = data.rfind("#len");
        if (pos == std::string::npos) {
            return false;
        }
        pos += 4;
        if (pos < data.size() && data[pos] == '=') {
            ++pos;
        }
    } else {
        pos += 4;
    }
    if (pos >= data.size() || !std::isdigit(static_cast<unsigned char>(data[pos]))) {
        return false;
    }
    while (pos < data.size() && std::isdigit(static_cast<unsigned char>(data[pos]))) {
        ++pos;
    }
    bool had_newline = false;
    while (pos < data.size()) {
        char ch = data[pos];
        if (ch == '\n' || ch == '\r') {
            had_newline = true;
        } else if (ch != ' ' && ch != '\t') {
            return false;
        }
        ++pos;
    }
    return had_newline;
}

std::string InfraredDevice::StripLearnPrefix(std::string data) {
    if (data.rfind("#xx=", 0) == 0) {
        size_t newline = data.find('\n');
        if (newline != std::string::npos) {
            data = data.substr(newline + 1);
        } else {
            size_t index = 4;
            while (index < data.size() && std::isdigit(static_cast<unsigned char>(data[index]))) {
                ++index;
            }
            data = data.substr(index);
        }
    }
    data.erase(std::remove(data.begin(), data.end(), '\r'), data.end());
    data.erase(std::remove(data.begin(), data.end(), '\n'), data.end());
    while (!data.empty() && (data.front() == ' ' || data.front() == '\t')) {
        data.erase(data.begin());
    }
    while (!data.empty() && (data.back() == ' ' || data.back() == '\t')) {
        data.pop_back();
    }
    return data;
}

void InfraredDevice::HandleReceivedFrame(const std::string& frame) {
    Application::GetInstance().Schedule(
        [frame]() { ApiServer::GetInstance().AddIrReceivedData(frame); });
}

void InfraredDevice::UartListenerTask(void* pvParameters) {
    auto* self = reinterpret_cast<InfraredDevice*>(pvParameters);
    ESP_LOGI(TAG, "UART listener started");

    uint8_t chunk[kRxChunkSize];
    int idle_after_len = 0;
    while (self->uart_ok_) {
        int len = uart_read_bytes(self->uart_num_, chunk, sizeof(chunk), kRxPollTicks);
        std::string complete;
        bool overflow = false;
        bool timed_out = false;

        {
            std::lock_guard<std::mutex> lock(self->rx_mutex_);
            if (!self->learning_) {
                idle_after_len = 0;
                continue;
            }
            if (esp_timer_get_time() > self->learn_deadline_us_) {
                self->learning_ = false;
                self->rx_acc_.clear();
                idle_after_len = 0;
                timed_out = true;
            } else if (len > 0) {
                self->rx_acc_.append(reinterpret_cast<char*>(chunk), static_cast<size_t>(len));
                idle_after_len = 0;
                ESP_LOGI(TAG, "Learn acc %u bytes (+%d)",
                         static_cast<unsigned>(self->rx_acc_.size()), len);
                if (self->rx_acc_.size() > kMaxFrameBytes) {
                    self->learning_ = false;
                    self->rx_acc_.clear();
                    overflow = true;
                } else if (HasLenTerminator(self->rx_acc_)) {
                    complete = StripLearnPrefix(self->rx_acc_);
                    self->rx_acc_.clear();
                    self->learning_ = false;
                }
            } else if (self->rx_acc_.find("len=") != std::string::npos ||
                       self->rx_acc_.find("#len") != std::string::npos) {
                ++idle_after_len;
                if (idle_after_len >= 5) {
                    complete = StripLearnPrefix(self->rx_acc_);
                    self->rx_acc_.clear();
                    self->learning_ = false;
                    idle_after_len = 0;
                }
            }
        }

        if (timed_out) {
            ESP_LOGW(TAG, "IR learn timed out waiting for len=");
            continue;
        }
        if (overflow) {
            ESP_LOGW(TAG, "IR learn frame exceeded %u bytes, dropped",
                     static_cast<unsigned>(kMaxFrameBytes));
            continue;
        }
        if (complete.empty()) {
            continue;
        }

        size_t preview_len = std::min(complete.size(), static_cast<size_t>(160));
        ESP_LOGI(TAG, "Learned IR frame %u bytes: %.*s", static_cast<unsigned>(complete.size()),
                 static_cast<int>(preview_len), complete.c_str());
        HandleReceivedFrame(complete);
    }

    ESP_LOGW(TAG, "UART listener exiting");
    vTaskDelete(NULL);
}

void InfraredDevice::StartUartListenerTask() {
    if (!uart_ok_) {
        ESP_LOGE(TAG, "UART not initialized, listener task not started");
        return;
    }
    if (uart_listener_task_handle_) {
        return;
    }

    BaseType_t created = xTaskCreate(UartListenerTask, "UartListenerTask", 4096, this,
                                     tskIDLE_PRIORITY + 2, &uart_listener_task_handle_);
    if (created != pdPASS) {
        uart_listener_task_handle_ = nullptr;
        ESP_LOGE(TAG, "Failed to create UART listener task");
    }
}
