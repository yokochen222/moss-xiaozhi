#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <functional>

namespace api_methods {
namespace ir {

class IrDataManager {
public:
    static IrDataManager& GetInstance();

    void AddIrReceivedData(const std::string& data);
    std::vector<std::string> GetIrReceivedData();
    void ClearIrReceivedData();
    void SetOnReceived(std::function<void(const std::string&)> callback);

private:
    IrDataManager() = default;
    ~IrDataManager() = default;

    IrDataManager(const IrDataManager&) = delete;
    IrDataManager& operator=(const IrDataManager&) = delete;

    std::vector<std::string> ir_received_data_;
    std::mutex ir_data_mutex_;
    std::function<void(const std::string&)> on_received_;
};

} // namespace ir
} // namespace api_methods
