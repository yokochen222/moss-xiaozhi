#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>

class ApiServer {
public:
    static ApiServer& GetInstance();
    
    // 启动API服务器
    bool Start(int port = 5500);
    
    // 停止API服务器
    void Stop();
    
    // 检查服务器是否运行
    bool IsRunning() const { return is_running_; }
    
    // 获取端口
    int GetPort() const { return port_; }
    
    // 添加接收到的红外数据
    void AddIrReceivedData(const std::string& data);
    
    // 获取所有接收到的红外数据
    std::vector<std::string> GetIrReceivedData();
    
    // 清空接收到的红外数据
    void ClearIrReceivedData();

private:
    ApiServer();
    ~ApiServer();
    
    // 禁用拷贝构造和赋值
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;
    
    bool is_running_;
    int port_;
    void* server_;
    void* config_;
    
    // 红外接收数据存储
    std::vector<std::string> ir_received_data_;
    std::mutex ir_data_mutex_;
    
    friend class InfraredDevice; // 允许红外设备访问数据存储
};
