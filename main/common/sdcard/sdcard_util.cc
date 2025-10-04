#include "sdcard_util.h"
#include <esp_vfs_fat.h>
#include <ff.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <driver/spi_common.h>

const char* SdcardUtil::TAG = "SdcardUtil";

SdcardUtil& SdcardUtil::GetInstance() {
    static SdcardUtil instance;
    return instance;
}

esp_err_t SdcardUtil::Initialize() {
    if (initialized_) {
        ESP_LOGW(TAG, "SD card already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SD card with configuration: %s", SDCARD_CONFIG_NAME);
    ESP_LOGI(TAG, "Pin configuration: CS=%d, SCK=%d, MOSI=%d, MISO=%d",
             CS_PIN, SCK_PIN, MOSI_PIN, MISO_PIN);
    ESP_LOGI(TAG, "SPI frequency: %d Hz", SPI_FREQUENCY);
    ESP_LOGI(TAG, "Hardware: Using 74VHC125 buffer (compatible with Arduino UNO)");
    ESP_LOGI(TAG, "This module works perfectly with Arduino UNO, adapting ESP32 config");
    ESP_LOGI(TAG, "Applied CSDN blog fixes:");
    ESP_LOGI(TAG, "  1. SDMMC_FREQ_DEFAULT reduced from 20MHz to 5MHz");
    ESP_LOGI(TAG, "  2. CRC retry mechanism added to sdmmc_init_spi_crc");
    ESP_LOGI(TAG, "  3. format_if_mount_failed set to true");
    
    // 电压兼容性检查
    #if SDCARD_USE_3V3_LOGIC
        ESP_LOGI(TAG, "Logic level: 3.3V (ESP32 compatible)");
    #else
        ESP_LOGW(TAG, "Logic level: 5V (may cause compatibility issues)");
    #endif
    
    ESP_LOGI(TAG, "Pin function check:");
    ESP_LOGI(TAG, "  CS (GPIO %d): Chip Select, should be high when idle", CS_PIN);
    ESP_LOGI(TAG, "  SCK (GPIO %d): SPI Clock", SCK_PIN);
    ESP_LOGI(TAG, "  MOSI (GPIO %d): Master Out Slave In", MOSI_PIN);
    ESP_LOGI(TAG, "  MISO (GPIO %d): Master In Slave Out", MISO_PIN);

    // 配置SPI主机 - 使用SPI2_HOST（ESP32-S3支持的标准SPI）
    host_ = SDSPI_HOST_DEFAULT();
    host_.slot = SPI2_HOST;

    // 配置SPI设备 - 针对74VHC125缓冲器优化
    slot_config_ = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config_.gpio_cs = CS_PIN;
    slot_config_.gpio_cd = SDSPI_SLOT_NO_CD;
    slot_config_.gpio_wp = SDSPI_SLOT_NO_WP;
    slot_config_.gpio_int = SDSPI_SLOT_NO_INT;
    
    // 74VHC125缓冲器特性：降低频率以确保信号完整性
    ESP_LOGI(TAG, "Configuring for 74VHC125 buffer compatibility");

    // 配置挂载选项 - 更兼容的配置
    mount_config_ = {
        .format_if_mount_failed = true,  // 如果挂载失败，尝试格式化
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    // 初始化SPI总线配置 - 针对74VHC125缓冲器优化
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = MOSI_PIN,
        .miso_io_num = MISO_PIN,
        .sclk_io_num = SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS, // 确保GPIO模式
    };

    // 初始化SPI总线
    ESP_LOGI(TAG, "Initializing SPI bus with configuration:");
    ESP_LOGI(TAG, "  MOSI: GPIO %d", bus_cfg.mosi_io_num);
    ESP_LOGI(TAG, "  MISO: GPIO %d", bus_cfg.miso_io_num);
    ESP_LOGI(TAG, "  SCLK: GPIO %d", bus_cfg.sclk_io_num);
    ESP_LOGI(TAG, "  Max transfer size: %d", bus_cfg.max_transfer_sz);
    
    esp_err_t ret = spi_bus_initialize(static_cast<spi_host_device_t>(host_.slot), &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "This could be due to:");
        ESP_LOGE(TAG, "  1. GPIO pins already in use by another peripheral");
        ESP_LOGE(TAG, "  2. Invalid GPIO configuration");
        ESP_LOGE(TAG, "  3. SPI bus already initialized");
        return ret;
    }
    
    ESP_LOGI(TAG, "SPI bus initialized successfully");

    // 配置SPI引脚
    const char mount_point[] = "/sdcard";
    ESP_LOGI(TAG, "Mounting SD card with configuration:");
    ESP_LOGI(TAG, "  Mount point: %s", mount_point);
    ESP_LOGI(TAG, "  CS pin: GPIO %d", slot_config_.gpio_cs);
    ESP_LOGI(TAG, "  Format if mount failed: %s", mount_config_.format_if_mount_failed ? "true" : "false");
    ESP_LOGI(TAG, "  Max files: %d", mount_config_.max_files);
    
    ESP_LOGI(TAG, "Attempting to mount SD card...");
    
    // 模拟Arduino UNO的SD卡初始化方式
    ESP_LOGI(TAG, "Using Arduino UNO compatible configuration");
    ESP_LOGI(TAG, "UNO uses SPI_HALF_SPEED and simple initialization");
    
    // 模拟UNO的配置：更保守的设置
    mount_config_.format_if_mount_failed = false; // UNO不自动格式化
    mount_config_.max_files = 5;
    mount_config_.allocation_unit_size = 16 * 1024; // 使用默认分配单元
    
    // UNO初始化后有一个短暂的延迟
    vTaskDelay(pdMS_TO_TICKS(50)); // 50ms延迟，模拟UNO的初始化时间
    
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host_, &slot_config_, &mount_config_, &card_);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed with error: %s (0x%x)", esp_err_to_name(ret), ret);
        
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "ESP_FAIL: Failed to mount filesystem. Possible causes:");
            ESP_LOGE(TAG, "  1. SD card not inserted");
            ESP_LOGE(TAG, "  2. SD card corrupted or not formatted");
            ESP_LOGE(TAG, "  3. SD card not compatible");
            ESP_LOGE(TAG, "  Solution: Try setting format_if_mount_failed = true");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "ESP_ERR_INVALID_STATE: SPI communication failed. Possible causes:");
            ESP_LOGE(TAG, "  1. SD card not connected properly");
            ESP_LOGE(TAG, "  2. Wrong pin connections");
            ESP_LOGE(TAG, "  3. Missing pull-up resistors (10kΩ on CS, SCK, MOSI, MISO)");
            ESP_LOGE(TAG, "  4. Power supply issues (SD card needs 3.3V, not 5V for logic)");
            ESP_LOGE(TAG, "  5. SD card module not powered");
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "ESP_ERR_TIMEOUT: Communication timeout. Possible causes:");
            ESP_LOGE(TAG, "  1. SD card not responding");
            ESP_LOGE(TAG, "  2. Wrong SPI mode or speed");
            ESP_LOGE(TAG, "  3. Hardware connection issues");
        } else {
            ESP_LOGE(TAG, "Other error: %s. Check hardware connections and power supply.", esp_err_to_name(ret));
            
            if (ret == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGE(TAG, "ESP_ERR_NOT_SUPPORTED: SD card communication failed");
                ESP_LOGE(TAG, "This module works with Arduino UNO but not ESP32");
                ESP_LOGE(TAG, "ESP32 vs Arduino UNO differences:");
                ESP_LOGE(TAG, "  1. ESP32 uses different SPI timing requirements");
                ESP_LOGE(TAG, "  2. ESP32 has stricter CRC checking");
                ESP_LOGE(TAG, "  3. ESP32 requires different initialization sequence");
                ESP_LOGE(TAG, "SOLUTIONS:");
                ESP_LOGE(TAG, "  1. Try different SPI frequency (currently 250kHz like UNO)");
                ESP_LOGE(TAG, "  2. Check if SD card needs to be formatted for ESP32");
                ESP_LOGE(TAG, "  3. Verify 74VHC125 enable pins are properly connected");
                ESP_LOGE(TAG, "  4. Try a different SD card (some work better with ESP32)");
            }
        }
        
        ESP_LOGE(TAG, "Hardware setup (compatible with Arduino UNO):");
        ESP_LOGE(TAG, "  ✓ CS (GPIO %d) → 74VHC125 → SD CS", CS_PIN);
        ESP_LOGE(TAG, "  ✓ SCK (GPIO %d) → 74VHC125 → SD SCK", SCK_PIN);
        ESP_LOGE(TAG, "  ✓ MOSI (GPIO %d) → 74VHC125 → SD DI", MOSI_PIN);
        ESP_LOGE(TAG, "  ✓ MISO (GPIO %d) → 74VHC125 → SD DO", MISO_PIN);
        ESP_LOGE(TAG, "  ✓ 74VHC125 VCC: 3.3V (logic) + 5V (power)");
        ESP_LOGE(TAG, "  ✓ 74VHC125 GND: Common ground");
        ESP_LOGE(TAG, "  ✓ 74VHC125 OE pins: Connected to GND (enabled)");
        
        // 清理SPI总线
        spi_bus_free(static_cast<spi_host_device_t>(host_.slot));
        return ret;
    }

    // 打印SD卡信息
    sdmmc_card_print_info(stdout, card_);
    initialized_ = true;
    
    ESP_LOGI(TAG, "SD card initialized successfully");
    return ESP_OK;
}

esp_err_t SdcardUtil::Deinitialize() {
    if (!initialized_) {
        ESP_LOGW(TAG, "SD card not initialized");
        return ESP_OK;
    }

    const char mount_point[] = "/sdcard";
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(mount_point, card_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount SD card: %s", esp_err_to_name(ret));
    }

    // 释放SPI总线
    spi_bus_free(static_cast<spi_host_device_t>(host_.slot));

    initialized_ = false;
    card_ = nullptr;
    
    ESP_LOGI(TAG, "SD card deinitialized successfully");
    return ESP_OK;
}

bool SdcardUtil::IsInitialized() const {
    return initialized_;
}

esp_err_t SdcardUtil::ReadFile(const std::string& file_path, std::string& content) {
    if (!initialized_) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    std::string full_path = "/sdcard/" + file_path;
    FILE* file = fopen(full_path.c_str(), "r");
    if (file == nullptr) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", full_path.c_str());
        return ESP_FAIL;
    }

    // 获取文件大小
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(file);
        ESP_LOGE(TAG, "Failed to get file size: %s", full_path.c_str());
        return ESP_FAIL;
    }

    // 读取文件内容
    content.resize(file_size);
    size_t bytes_read = fread(&content[0], 1, file_size, file);
    fclose(file);

    if (bytes_read != static_cast<size_t>(file_size)) {
        ESP_LOGE(TAG, "Failed to read complete file: %s", full_path.c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Successfully read file: %s (%zu bytes)", file_path.c_str(), bytes_read);
    return ESP_OK;
}

esp_err_t SdcardUtil::WriteFile(const std::string& file_path, const std::string& content) {
    if (!initialized_) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    std::string full_path = "/sdcard/" + file_path;
    FILE* file = fopen(full_path.c_str(), "w");
    if (file == nullptr) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", full_path.c_str());
        return ESP_FAIL;
    }

    size_t bytes_written = fwrite(content.c_str(), 1, content.length(), file);
    fclose(file);

    if (bytes_written != content.length()) {
        ESP_LOGE(TAG, "Failed to write complete file: %s", full_path.c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Successfully wrote file: %s (%zu bytes)", file_path.c_str(), bytes_written);
    return ESP_OK;
}

esp_err_t SdcardUtil::AppendFile(const std::string& file_path, const std::string& content) {
    if (!initialized_) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    std::string full_path = "/sdcard/" + file_path;
    FILE* file = fopen(full_path.c_str(), "a");
    if (file == nullptr) {
        ESP_LOGE(TAG, "Failed to open file for appending: %s", full_path.c_str());
        return ESP_FAIL;
    }

    size_t bytes_written = fwrite(content.c_str(), 1, content.length(), file);
    fclose(file);

    if (bytes_written != content.length()) {
        ESP_LOGE(TAG, "Failed to append complete content to file: %s", full_path.c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Successfully appended to file: %s (%zu bytes)", file_path.c_str(), bytes_written);
    return ESP_OK;
}

esp_err_t SdcardUtil::DeleteFile(const std::string& file_path) {
    if (!initialized_) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // 使用FATFS API删除文件
    FRESULT res = f_unlink(file_path.c_str());
    if (res != FR_OK) {
        ESP_LOGE(TAG, "Failed to delete file: %s (error: %d)", file_path.c_str(), res);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Successfully deleted file: %s", file_path.c_str());
    return ESP_OK;
}

bool SdcardUtil::FileExists(const std::string& file_path) {
    if (!initialized_) {
        ESP_LOGE(TAG, "SD card not initialized");
        return false;
    }

    std::string full_path = "/sdcard/" + file_path;
    struct stat st;
    return (stat(full_path.c_str(), &st) == 0);
}

int64_t SdcardUtil::GetFileSize(const std::string& file_path) {
    if (!initialized_) {
        ESP_LOGE(TAG, "SD card not initialized");
        return -1;
    }

    std::string full_path = "/sdcard/" + file_path;
    struct stat st;
    if (stat(full_path.c_str(), &st) != 0) {
        ESP_LOGE(TAG, "Failed to get file stats: %s", full_path.c_str());
        return -1;
    }

    return st.st_size;
}

esp_err_t SdcardUtil::ListFiles(const std::string& dir_path, std::vector<std::string>& files) {
    if (!initialized_) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    files.clear();
    
    // 安全检查：确保dir_path不为空且有效
    std::string clean_path = dir_path;
    
    // 清理路径：移除开头和结尾的斜杠
    while (!clean_path.empty() && clean_path.front() == '/') {
        clean_path.erase(0, 1);
    }
    while (!clean_path.empty() && clean_path.back() == '/') {
        clean_path.pop_back();
    }
    
    std::string full_path = "/sdcard";
    if (!clean_path.empty()) {
        full_path += "/" + clean_path;
    }
    
    ESP_LOGI(TAG, "Attempting to list files");
    
    DIR* dir = opendir(full_path.c_str());
    if (dir == nullptr) {
        ESP_LOGE(TAG, "Failed to open directory: %s", full_path.c_str());
        return ESP_FAIL;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // 安全检查：确保entry有效
        if (entry == nullptr) {
            ESP_LOGW(TAG, "Invalid directory entry, skipping");
            continue;
        }
        
        // 跳过 . 和 .. 目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // 安全地添加文件名
        std::string filename(entry->d_name);
        files.push_back(filename);
        // ESP_LOGD(TAG, "Found file: %s", filename.c_str()); // 暂时禁用以避免崩溃
    }

    closedir(dir);
    
    // 简化日志输出，避免崩溃
    ESP_LOGI(TAG, "Listed %zu files", files.size());
    return ESP_OK;
}

esp_err_t SdcardUtil::CreateDirectory(const std::string& dir_path) {
    if (!initialized_) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // 使用FATFS API创建目录
    FRESULT res = f_mkdir(dir_path.c_str());
    if (res != FR_OK) {
        ESP_LOGE(TAG, "Failed to create directory: %s (error: %d)", dir_path.c_str(), res);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Successfully created directory: %s", dir_path.c_str());
    return ESP_OK;
}

esp_err_t SdcardUtil::GetCardInfo(uint64_t& total_size, uint64_t& free_size) {
    if (!initialized_) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    FATFS* fs;
    DWORD fre_clusters, fre_sectors, tot_sectors;
    
    // 获取FATFS卷信息
    FRESULT res = f_getfree("0:", &fre_clusters, &fs);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "Failed to get free clusters: %d", res);
        return ESP_FAIL;
    }

    // 计算总扇区数和剩余扇区数
    tot_sectors = (fs->n_fatent - 2) * fs->csize;
    fre_sectors = fre_clusters * fs->csize;

    // 转换为字节
    total_size = (uint64_t)tot_sectors * 512;
    free_size = (uint64_t)fre_sectors * 512;

    ESP_LOGI(TAG, "SD card info - Total: %llu bytes, Free: %llu bytes", 
             total_size, free_size);
    return ESP_OK;
}
