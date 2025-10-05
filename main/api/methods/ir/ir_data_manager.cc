#include "ir_data_manager.h"
#include <esp_log.h>
#include <algorithm>

#define TAG "IrDataManager"

namespace api_methods {
namespace ir {

IrDataManager& IrDataManager::GetInstance() {
    static IrDataManager instance;
    return instance;
}

void IrDataManager::AddIrReceivedData(const std::string& data) {
    std::lock_guard<std::mutex> lock(ir_data_mutex_);
    
    // 去除换行符和回车符
    std::string clean_data = data;
    clean_data.erase(std::remove(clean_data.begin(), clean_data.end(), '\n'), clean_data.end());
    clean_data.erase(std::remove(clean_data.begin(), clean_data.end(), '\r'), clean_data.end());
    
    // 限制存储的数据条数，避免内存溢出
    const size_t max_data_count = 1;
    if (ir_received_data_.size() >= max_data_count) {
        ir_received_data_.erase(ir_received_data_.begin());
    }
    
    ir_received_data_.push_back(clean_data);
    ESP_LOGI(TAG, "Added IR data: %s", clean_data.c_str());
}

std::vector<std::string> IrDataManager::GetIrReceivedData() {
    std::lock_guard<std::mutex> lock(ir_data_mutex_);
    return ir_received_data_;
}

void IrDataManager::ClearIrReceivedData() {
    std::lock_guard<std::mutex> lock(ir_data_mutex_);
    ir_received_data_.clear();
    ESP_LOGI(TAG, "Cleared IR received data");
}

} // namespace ir
} // namespace api_methods
