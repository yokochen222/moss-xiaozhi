#ifndef SDCARD_UTIL_H
#define SDCARD_UTIL_H

#include <string>
#include <vector>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <sdmmc_cmd.h>
#include <esp_vfs_fat.h>
#include <esp_log.h>
#include <esp_err.h>
#include <driver/gpio.h>
#include "sdcard_config.h"

/**
 * @brief SD卡工具类，提供SD卡读写操作
 * 
 * 引脚配置通过 sdcard_config.h 文件进行管理：
 * - 默认配置：CS=48, SCK=39, MOSI=42, MISO=46
 * - 标准配置：CS=10, SCK=12, MOSI=11, MISO=13 (推荐)
 * - 备用配置：CS=21, SCK=20, MOSI=19, MISO=18
 * 
 * 要切换配置，请编辑 sdcard_config.h 文件中的宏定义。
 * 
 * 硬件连接注意事项：
 * 1. 确保SD卡模块的供电电压正确 (通常需要3.3V逻辑，5V供电)
 * 2. 添加上拉电阻 (10kΩ) 到所有数据线
 * 3. 确保引脚连接牢固，接触良好
 */
class SdcardUtil {
public:
    /**
     * @brief 获取单例实例
     * @return SdcardUtil实例引用
     */
    static SdcardUtil& GetInstance();

    /**
     * @brief 初始化SD卡
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t Initialize();

    /**
     * @brief 反初始化SD卡
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t Deinitialize();

    /**
     * @brief 检查SD卡是否已初始化
     * @return true表示已初始化，false表示未初始化
     */
    bool IsInitialized() const;

    /**
     * @brief 读取文件内容
     * @param file_path 文件路径
     * @param content 输出参数，存储文件内容
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t ReadFile(const std::string& file_path, std::string& content);

    /**
     * @brief 写入文件内容
     * @param file_path 文件路径
     * @param content 要写入的内容
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t WriteFile(const std::string& file_path, const std::string& content);

    /**
     * @brief 追加内容到文件
     * @param file_path 文件路径
     * @param content 要追加的内容
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t AppendFile(const std::string& file_path, const std::string& content);

    /**
     * @brief 删除文件
     * @param file_path 文件路径
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t DeleteFile(const std::string& file_path);

    /**
     * @brief 检查文件是否存在
     * @param file_path 文件路径
     * @return true表示存在，false表示不存在
     */
    bool FileExists(const std::string& file_path);

    /**
     * @brief 获取文件大小
     * @param file_path 文件路径
     * @return 文件大小（字节），-1表示失败
     */
    int64_t GetFileSize(const std::string& file_path);

    /**
     * @brief 列出目录下的文件
     * @param dir_path 目录路径
     * @param files 输出参数，存储文件列表
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t ListFiles(const std::string& dir_path, std::vector<std::string>& files);

    /**
     * @brief 创建目录
     * @param dir_path 目录路径
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t CreateDirectory(const std::string& dir_path);

    /**
     * @brief 获取SD卡信息
     * @param total_size 输出参数，总容量（字节）
     * @param free_size 输出参数，剩余容量（字节）
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t GetCardInfo(uint64_t& total_size, uint64_t& free_size);

private:
    SdcardUtil() = default;
    ~SdcardUtil() = default;
    
    // 禁止拷贝构造和赋值
    SdcardUtil(const SdcardUtil&) = delete;
    SdcardUtil& operator=(const SdcardUtil&) = delete;

    static const char* TAG;
    static constexpr gpio_num_t CS_PIN = SDCARD_CS_PIN;
    static constexpr gpio_num_t SCK_PIN = SDCARD_SCK_PIN;
    static constexpr gpio_num_t MOSI_PIN = SDCARD_MOSI_PIN;
    static constexpr gpio_num_t MISO_PIN = SDCARD_MISO_PIN;
    static constexpr int SPI_FREQUENCY = SDCARD_SPI_FREQUENCY;

    bool initialized_;
    sdmmc_card_t* card_;
    sdmmc_host_t host_;
    sdspi_device_config_t slot_config_;
    esp_vfs_fat_sdmmc_mount_config_t mount_config_;
};

#endif // SDCARD_UTIL_H
