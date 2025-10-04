# SD卡工具 (SD Card Utility)

这个模块提供了SD卡的读写操作功能，使用SPI接口连接SD卡。

## 引脚配置

根据用户要求，SD卡模块使用以下引脚连接：

- **CS (片选)**: GPIO 48
- **SCK (时钟)**: GPIO 39  
- **MOSI (主出从入)**: GPIO 42
- **MISO (主入从出)**: GPIO 46

## 功能特性

### SdcardUtil 类

提供以下主要功能：

1. **文件操作**
   - `ReadFile()` - 读取文件内容
   - `WriteFile()` - 写入文件内容
   - `AppendFile()` - 追加内容到文件
   - `DeleteFile()` - 删除文件

2. **文件系统操作**
   - `FileExists()` - 检查文件是否存在
   - `GetFileSize()` - 获取文件大小
   - `ListFiles()` - 列出目录中的文件
   - `CreateDirectory()` - 创建目录

3. **SD卡信息**
   - `GetCardInfo()` - 获取SD卡总容量和剩余空间

## MCP工具集成

SD卡功能已集成到MCP工具系统中，可以通过大模型调用以下操作：

### 支持的操作

1. **read_file** - 读取文件
   - 参数: `file_path` (文件路径)

2. **write_file** - 写入文件
   - 参数: `file_path` (文件路径), `content` (内容)

3. **append_file** - 追加内容
   - 参数: `file_path` (文件路径), `content` (内容)

4. **delete_file** - 删除文件
   - 参数: `file_path` (文件路径)

5. **file_exists** - 检查文件存在
   - 参数: `file_path` (文件路径)

6. **get_file_size** - 获取文件大小
   - 参数: `file_path` (文件路径)

7. **list_files** - 列出文件
   - 参数: `dir_path` (目录路径，可选，默认为根目录)

8. **create_directory** - 创建目录
   - 参数: `dir_path` (目录路径)

9. **get_card_info** - 获取SD卡信息
   - 无需参数

## 使用示例

### 通过MCP工具使用

```json
{
  "action": "read_file",
  "file_path": "test.txt"
}
```

```json
{
  "action": "write_file", 
  "file_path": "data.txt",
  "content": "Hello, World!"
}
```

```json
{
  "action": "list_files",
  "dir_path": "/"
}
```

### 直接使用SdcardUtil类

```cpp
#include "sdcard_util.h"

// 获取实例
auto& sdcard = SdcardUtil::GetInstance();

// 初始化
esp_err_t ret = sdcard.Initialize();
if (ret != ESP_OK) {
    ESP_LOGE("MAIN", "SD card init failed");
    return;
}

// 读取文件
std::string content;
ret = sdcard.ReadFile("config.json", content);
if (ret == ESP_OK) {
    ESP_LOGI("MAIN", "File content: %s", content.c_str());
}

// 写入文件
ret = sdcard.WriteFile("log.txt", "System started");
if (ret == ESP_OK) {
    ESP_LOGI("MAIN", "File written successfully");
}
```

## 注意事项

1. 所有文件路径都是相对于SD卡根目录的路径，不需要包含 `/sdcard/` 前缀
2. SD卡挂载点为 `/sdcard/`
3. 支持FAT文件系统
4. 文件大小限制取决于SD卡容量和FAT32限制
5. 确保SD卡引脚连接正确，并具有上拉电阻

### GPIO引脚冲突警告

当前配置的引脚可能与某些板级配置冲突：

- **GPIO 48 (CS)**: 在 `lichuang-dev` 板中用作 `BUILTIN_LED`
- **GPIO 39 (SCK)**: 在 `lichuang-dev` 板中用作 `VOLUME_DOWN_BUTTON`

如果遇到冲突，可以：

1. **修改引脚定义**: 在 `sdcard_util.h` 中修改 `CS_PIN`, `SCK_PIN`, `MOSI_PIN`, `MISO_PIN` 常量
2. **禁用冲突功能**: 在板级配置中注释掉冲突的GPIO定义
3. **使用不同板型**: 选择没有GPIO冲突的板型配置

推荐的替代引脚组合（无冲突）：
- CS: GPIO 5
- SCK: GPIO 18  
- MOSI: GPIO 23
- MISO: GPIO 19

## 错误处理

所有函数都返回 `esp_err_t` 类型，使用ESP-IDF标准错误代码：
- `ESP_OK` - 操作成功
- `ESP_FAIL` - 操作失败
- `ESP_ERR_INVALID_STATE` - SD卡未初始化

建议在使用前检查返回值并适当处理错误情况。
