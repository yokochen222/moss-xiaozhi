#include "infrared.h"
#include "api/api.h"
#include <esp_log.h>
#include <driver/gpio.h>

#define TAG "InfraredDevice"

InfraredDevice::InfraredDevice() : uart_num_(UART_NUM_2), uart_listener_task_handle_(nullptr), uart_ok_(false) {
    InitializeUart();
    StartUartListenerTask();
}

InfraredDevice::~InfraredDevice() {
    if (uart_ok_) {
        uart_ok_ = false;
        if (uart_listener_task_handle_) {
            vTaskDelete(uart_listener_task_handle_);
        }
        uart_driver_delete(uart_num_);
    }
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
    
    esp_err_t ret;
    ret = uart_driver_install(uart_num_, 1024, 0, 0, NULL, 0);
    ESP_LOGI(TAG, "uart_driver_install: %d", ret);
    if (ret != ESP_OK) { 
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret)); 
        return; 
    }
    
    ret = uart_param_config(uart_num_, &uart_config);
    ESP_LOGI(TAG, "uart_param_config: %d", ret);
    if (ret != ESP_OK) { 
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret)); 
        return; 
    }
    
    ret = uart_set_pin(uart_num_, GPIO_NUM_17, GPIO_NUM_18, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(TAG, "uart_set_pin: %d", ret);
    if (ret != ESP_OK) { 
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret)); 
        return; 
    }
    
    uart_ok_ = true;
}

bool InfraredDevice::SendIrCommand(const std::string &command) {
    if (!uart_ok_) {
        ESP_LOGE(TAG, "UART not initialized, cannot send IR command");
        return false;
    }
    
    ESP_LOGI(TAG, "Sending IR command: %s", command.c_str());
    int ret = uart_write_bytes(uart_num_, command.c_str(), command.length());
    return ret > 0;
}

std::string InfraredDevice::ReadFromUart() {
    if (!uart_ok_) return "";
    
    uint8_t data[2048];
    int len = uart_read_bytes(uart_num_, data, sizeof(data), pdMS_TO_TICKS(1000));
    if (len > 0) {
        return std::string(reinterpret_cast<char *>(data), len);
    }
    return "";
}

void InfraredDevice::UartListenerTask(void *pvParameters) {
    InfraredDevice *infrared_instance = reinterpret_cast<InfraredDevice *>(pvParameters);
    while (infrared_instance->uart_ok_) {
        std::string response = infrared_instance->ReadFromUart();
        if (!response.empty()) {
            ESP_LOGI(TAG, "Received serial data: %s", response.c_str());
            // 将接收到的数据传递给API服务器
            ApiServer::GetInstance().AddIrReceivedData(response);
        }
    }
    vTaskDelete(NULL);
}

void InfraredDevice::StartUartListenerTask() {
    if (!uart_ok_) {
        ESP_LOGE(TAG, "UART not initialized, listener task not started!");
        return;
    }
    
    BaseType_t xReturned = xTaskCreate(
        UartListenerTask,
        "UartListenerTask",
        4096,
        this,
        tskIDLE_PRIORITY + 1,
        &uart_listener_task_handle_);
        
    if (xReturned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART listener task");
    }
}

InfraredDevice& InfraredDevice::GetInstance() {
    static InfraredDevice instance;
    return instance;
}
