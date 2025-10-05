#pragma once

#include <string>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class InfraredDevice {
private:
    uart_port_t uart_num_;
    TaskHandle_t uart_listener_task_handle_;
    bool uart_ok_;

    void InitializeUart();
    void StartUartListenerTask();
    std::string ReadFromUart();
    static void UartListenerTask(void *pvParameters);

public:
    InfraredDevice();
    ~InfraredDevice();
    
    // 发送红外指令
    bool SendIrCommand(const std::string &command);
    
    // 获取设备状态
    bool IsReady() const { return uart_ok_; }
    
    // 获取单例实例
    static InfraredDevice& GetInstance();
};
