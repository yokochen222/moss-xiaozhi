#pragma once

#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>
#include <string>

#include "config.h"

class InfraredDevice {
private:
    static constexpr uart_port_t kUartPort = MOSS_IR_UART_PORT;
    static constexpr gpio_num_t kTxPin = MOSS_IR_UART_TX_PIN;
    static constexpr gpio_num_t kRxPin = MOSS_IR_UART_RX_PIN;
    static constexpr size_t kRxChunkSize = 256;
    static constexpr size_t kRxDriverBuf = 4096;
    static constexpr size_t kMaxFrameBytes = 8192;

    uart_port_t uart_num_;
    TaskHandle_t uart_listener_task_handle_;
    bool uart_ok_;
    std::mutex rx_mutex_;
    std::string rx_acc_;
    bool learning_;
    int64_t learn_deadline_us_;

    void EnsureUart();
    void InitializeUart();
    void StartUartListenerTask();
    static void UartListenerTask(void* pvParameters);
    static void HandleReceivedFrame(const std::string& frame);
    static bool HasLenTerminator(const std::string& data);
    static std::string StripLearnPrefix(std::string data);

public:
    InfraredDevice();
    ~InfraredDevice();

    void Start();
    bool StartLearn();
    bool SendIrCommand(const std::string& command);
    bool IsReady() const { return uart_ok_; }
    static InfraredDevice& GetInstance();
};
