#include "mcp_tools.h"
#include "board.h"
#include "../common/sdcard/sdcard_util.h"
#include <esp_log.h>
#include <vector>
#include <sstream>

#define TAG "SdcardTool"

namespace mcp_tools {

class SdcardTool : public McpTool {
private:
    bool sdcard_initialized_ = false;
    bool sdcard_init_attempted_ = false;
    
    bool EnsureSdcardInitialized() {
        if (sdcard_initialized_) {
            return true;
        }
        
        if (sdcard_init_attempted_) {
            return false;
        }
        
        sdcard_init_attempted_ = true;
        ESP_LOGI(TAG, "尝试初始化SD卡...");
        
        auto& sdcard = SdcardUtil::GetInstance();
        esp_err_t ret = sdcard.Initialize();
        if (ret == ESP_OK) {
            sdcard_initialized_ = true;
            ESP_LOGI(TAG, "SD卡初始化成功");
            return true;
        } else {
            ESP_LOGW(TAG, "SD卡初始化失败: %s (这是正常的，如果没有连接SD卡硬件)", esp_err_to_name(ret));
            return false;
        }
    }

public:
    static SdcardTool& GetInstance() {
        static SdcardTool instance;
        return instance;
    }

    SdcardTool() : McpTool("self.sdcard.control", "MOSS SD卡控制能力，用于读取和写入SD卡中的文件内容") {
        // 不在构造函数中初始化SD卡，避免启动时崩溃
    }

    void Register() override {
        ESP_LOGI(TAG, "注册SD卡工具");
        McpServer::GetInstance().AddTool(
            name(),
            "MOSS SD卡控制能力，支持以下操作（action）及参数：\n"
            "0. 检查SD卡状态：action=\"check_status\"，无需其他参数。\n"
            "1. 读取文件内容：action=\"read_file\"，file_path=\"文件路径\"（相对于SD卡根目录）。\n"
            "2. 写入文件内容：action=\"write_file\"，file_path=\"文件路径\"，content=\"文件内容\"。\n"
            "3. 追加内容到文件：action=\"append_file\"，file_path=\"文件路径\"，content=\"追加内容\"。\n"
            "4. 删除文件：action=\"delete_file\"，file_path=\"文件路径\"。\n"
            "5. 检查文件是否存在：action=\"file_exists\"，file_path=\"文件路径\"。\n"
            "6. 获取文件大小：action=\"get_file_size\"，file_path=\"文件路径\"。\n"
            "7. 列出目录文件：action=\"list_files\"，dir_path=\"目录路径\"（可选，默认为根目录）。\n"
            "8. 创建目录：action=\"create_directory\"，dir_path=\"目录路径\"。\n"
            "9. 获取SD卡信息：action=\"get_card_info\"，无需其他参数。\n"
            "注意：所有文件路径都是相对于SD卡根目录的路径，不需要包含/sdcard/前缀。\n"
            "注意：如果没有连接SD卡硬件，除check_status外的其他操作将失败。\n",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("file_path", kPropertyTypeString, ""),
                Property("dir_path", kPropertyTypeString, ""),
                Property("content", kPropertyTypeString, "")
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto action = properties["action"].value<std::string>();
                auto file_path = properties["file_path"].value<std::string>();
                auto dir_path = properties["dir_path"].value<std::string>();
                auto content = properties["content"].value<std::string>();

                ESP_LOGI(TAG, "SD卡操作参数: action=%s, file_path=%s, dir_path=%s, content_length=%zu", 
                         action.c_str(), file_path.c_str(), dir_path.c_str(), content.length());

                // 检查SD卡状态的操作也会尝试初始化
                if (action == "check_status") {
                    ESP_LOGI(TAG, "检查SD卡状态，尝试初始化...");
                    if (EnsureSdcardInitialized()) {
                        return "SD卡状态：已初始化并可用";
                    } else {
                        if (sdcard_init_attempted_) {
                            return "SD卡状态：初始化失败，请检查硬件连接";
                        } else {
                            return "SD卡状态：初始化过程中发生错误";
                        }
                    }
                }

                // 尝试初始化SD卡
                if (!EnsureSdcardInitialized()) {
                    return "SD卡未连接或初始化失败。请检查SD卡硬件连接，确保：\n"
                           "1. SD卡已正确插入\n"
                           "2. 引脚连接正确（CS=48, SCK=39, MOSI=42, MISO=46）\n"
                           "3. 上拉电阻已连接\n"
                           "4. 电源供应正常";
                }
                
                auto& sdcard = SdcardUtil::GetInstance();

                if (action == "read_file") {
                    if (file_path.empty()) {
                        return "缺少file_path参数";
                    }
                    
                    std::string file_content;
                    esp_err_t ret = sdcard.ReadFile(file_path, file_content);
                    if (ret == ESP_OK) {
                        return "文件读取成功，内容：\n" + file_content;
                    } else {
                        return "读取文件失败：" + std::string(esp_err_to_name(ret));
                    }
                    
                } else if (action == "write_file") {
                    if (file_path.empty()) {
                        return "缺少file_path参数";
                    }
                    
                    esp_err_t ret = sdcard.WriteFile(file_path, content);
                    if (ret == ESP_OK) {
                        return "文件写入成功：" + file_path;
                    } else {
                        return "写入文件失败：" + std::string(esp_err_to_name(ret));
                    }
                    
                } else if (action == "append_file") {
                    if (file_path.empty()) {
                        return "缺少file_path参数";
                    }
                    
                    esp_err_t ret = sdcard.AppendFile(file_path, content);
                    if (ret == ESP_OK) {
                        return "文件追加成功：" + file_path;
                    } else {
                        return "追加文件失败：" + std::string(esp_err_to_name(ret));
                    }
                    
                } else if (action == "delete_file") {
                    if (file_path.empty()) {
                        return "缺少file_path参数";
                    }
                    
                    esp_err_t ret = sdcard.DeleteFile(file_path);
                    if (ret == ESP_OK) {
                        return "文件删除成功：" + file_path;
                    } else {
                        return "删除文件失败：" + std::string(esp_err_to_name(ret));
                    }
                    
                } else if (action == "file_exists") {
                    if (file_path.empty()) {
                        return "缺少file_path参数";
                    }
                    
                    bool exists = sdcard.FileExists(file_path);
                    return exists ? "文件存在：" + file_path : "文件不存在：" + file_path;
                    
                } else if (action == "get_file_size") {
                    if (file_path.empty()) {
                        return "缺少file_path参数";
                    }
                    
                    int64_t size = sdcard.GetFileSize(file_path);
                    if (size >= 0) {
                        return "文件大小：" + std::to_string(size) + " 字节 (" + file_path + ")";
                    } else {
                        return "获取文件大小失败：" + file_path;
                    }
                    
                } else if (action == "list_files") {
                    // 如果没有指定目录，默认使用根目录（空字符串表示根目录）
                    std::string target_dir = dir_path.empty() ? "" : dir_path;
                    
                    std::vector<std::string> files;
                    esp_err_t ret = sdcard.ListFiles(target_dir, files);
                    if (ret == ESP_OK) {
                        std::stringstream ss;
                        std::string display_dir = target_dir.empty() ? "根目录" : target_dir;
                        ss << "目录 " << display_dir << " 中的文件 (" << files.size() << " 个)：\n";
                        for (const auto& file : files) {
                            ss << "- " << file << "\n";
                        }
                        return ss.str();
                    } else {
                        return "列出文件失败：" + std::string(esp_err_to_name(ret));
                    }
                    
                } else if (action == "create_directory") {
                    if (dir_path.empty()) {
                        return "缺少dir_path参数";
                    }
                    
                    esp_err_t ret = sdcard.CreateDirectory(dir_path);
                    if (ret == ESP_OK) {
                        return "目录创建成功：" + dir_path;
                    } else {
                        return "创建目录失败：" + std::string(esp_err_to_name(ret));
                    }
                    
                } else if (action == "get_card_info") {
                    uint64_t total_size, free_size;
                    esp_err_t ret = sdcard.GetCardInfo(total_size, free_size);
                    if (ret == ESP_OK) {
                        std::stringstream ss;
                        ss << "SD卡信息：\n";
                        ss << "总容量：" << (total_size / 1024 / 1024) << " MB\n";
                        ss << "剩余容量：" << (free_size / 1024 / 1024) << " MB\n";
                        ss << "使用率：" << ((total_size - free_size) * 100 / total_size) << "%";
                        return ss.str();
                    } else {
                        return "获取SD卡信息失败：" + std::string(esp_err_to_name(ret));
                    }
                    
                } else {
                    return "未知动作: " + action + "\n支持的动作: read_file, write_file, append_file, delete_file, file_exists, get_file_size, list_files, create_directory, get_card_info";
                }
            }
        );
    }
};

} // namespace mcp_tools

static auto& g_sdcard_tool_instance = mcp_tools::SdcardTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_sdcard_tool_instance);
