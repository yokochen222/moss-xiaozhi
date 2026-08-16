#include "device/ir_catalog.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_spiffs.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unistd.h>

#define TAG "IrCatalog"

namespace {
constexpr const char* kPartition = "storage";
constexpr const char* kMount = "/storage";
constexpr const char* kPath = "/storage/ir_catalog.json";
constexpr size_t kMaxIdLen = 32;
constexpr size_t kMaxNameLen = 64;
constexpr size_t kMaxCodeLen = 4096;
constexpr size_t kMaxAppliances = 16;
constexpr size_t kMaxCommands = 48;

bool IdEquals(const std::string& a, const std::string& b) {
    return a == b;
}

void WriteEscaped(FILE* file, const std::string& text) {
    for (unsigned char ch : text) {
        switch (ch) {
            case '"':
                fputs("\\\"", file);
                break;
            case '\\':
                fputs("\\\\", file);
                break;
            case '\n':
                fputs("\\n", file);
                break;
            case '\r':
                fputs("\\r", file);
                break;
            case '\t':
                fputs("\\t", file);
                break;
            default:
                if (ch < 0x20) {
                    fprintf(file, "\\u%04x", ch);
                } else {
                    fputc(ch, file);
                }
                break;
        }
    }
}

void LogSpiffsUsage(const char* when) {
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(kPartition, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS %s used=%u total=%u free=%u", when, (unsigned)used, (unsigned)total,
                 (unsigned)(total - used));
    }
}
}  // namespace

IrCatalog& IrCatalog::GetInstance() {
    static IrCatalog instance;
    return instance;
}

bool IrCatalog::ValidId(const std::string& id) {
    if (id.empty() || id.size() > kMaxIdLen) {
        return false;
    }
    for (unsigned char ch : id) {
        if (!(std::isalnum(ch) || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

bool IrCatalog::ValidName(const std::string& name) {
    if (name.empty() || name.size() > kMaxNameLen) {
        return false;
    }
    for (unsigned char ch : name) {
        if (ch < 0x20) {
            return false;
        }
    }
    return true;
}

bool IrCatalog::ValidType(const std::string& type) {
    return type == "light" || type == "ac" || type == "tv" || type == "custom";
}

bool IrCatalog::ValidCode(const std::string& raw) {
    std::string code = NormalizeCode(raw);
    if (code.empty()) {
        return true;
    }
    if (code.size() > kMaxCodeLen) {
        return false;
    }
    if (code.rfind("xx", 0) == 0) {
        return code.size() <= 8;
    }
    size_t index = 0;
    bool has_number = false;
    while (index < code.size()) {
        size_t comma = code.find(',', index);
        std::string token = code.substr(index, comma == std::string::npos ? std::string::npos : comma - index);
        if (token.empty()) {
            return false;
        }
        if (token.rfind("len=", 0) == 0) {
            if (token.size() <= 4) {
                return false;
            }
            for (size_t i = 4; i < token.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(token[i]))) {
                    return false;
                }
            }
            if (comma != std::string::npos) {
                return false;
            }
        } else {
            size_t pos = 0;
            if (token[0] == '-') {
                pos = 1;
            }
            if (pos >= token.size()) {
                return false;
            }
            for (; pos < token.size(); ++pos) {
                if (!std::isdigit(static_cast<unsigned char>(token[pos]))) {
                    return false;
                }
            }
            has_number = true;
        }
        if (comma == std::string::npos) {
            break;
        }
        index = comma + 1;
    }
    return has_number;
}

const char* IrCatalog::StatusMessage(IrCatalogStatus status) {
    switch (status) {
        case IrCatalogStatus::kOk:
            return "ok";
        case IrCatalogStatus::kNotFound:
            return "未找到对应电器或按键";
        case IrCatalogStatus::kInvalid:
            return "数据不完整或不合法";
        case IrCatalogStatus::kWriteFailed:
            return "红外目录无法保存：Flash 上没有 storage 分区。请用 moss-desktop-16m.csv 整片烧录（含分区表），不要只 OTA";
        default:
            return "未知错误";
    }
}

bool IrCatalog::Initialize() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!mounted_) {
            const esp_partition_t* part = esp_partition_find_first(
                ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, kPartition);
            if (!part) {
                ESP_LOGE(TAG, "No SPIFFS partition named '%s'. Current flash table has no storage.",
                         kPartition);
                esp_partition_iterator_t it =
                    esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
                while (it != nullptr) {
                    const esp_partition_t* item = esp_partition_get(it);
                    ESP_LOGE(TAG, "  partition name=%s type=%u subtype=%u offset=0x%lx size=%u",
                             item->label, (unsigned)item->type, (unsigned)item->subtype,
                             (unsigned long)item->address, (unsigned)item->size);
                    it = esp_partition_next(it);
                }
                ESP_LOGE(TAG, "Rebuild with partitions/moss-desktop-16m.csv and flash partition-table+app");
                return false;
            }
            ESP_LOGI(TAG, "Found storage partition size=%u offset=0x%lx", (unsigned)part->size,
                     (unsigned long)part->address);
            esp_vfs_spiffs_conf_t conf = {
                .base_path = kMount,
                .partition_label = kPartition,
                .max_files = 8,
                .format_if_mount_failed = true,
            };
            esp_err_t err = esp_vfs_spiffs_register(&conf);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
                return false;
            }
            mounted_ = true;
            ESP_LOGI(TAG, "SPIFFS mounted at %s", kMount);
            LogSpiffsUsage("mount");
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    LoadUnlocked();
    return true;
}

bool IrCatalog::LoadUnlocked() {
    appliances_.clear();
    std::ifstream in(kPath);
    if (!in.good()) {
        ESP_LOGI(TAG, "No IR catalog file yet");
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    in.close();
    std::string json = buffer.str();
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Invalid IR catalog JSON");
        return false;
    }
    cJSON* devices = cJSON_GetObjectItem(root, "devices");
    if (cJSON_IsArray(devices)) {
        cJSON* device = nullptr;
        cJSON_ArrayForEach(device, devices) {
            IrAppliance appliance;
            auto* id = cJSON_GetObjectItem(device, "id");
            auto* name = cJSON_GetObjectItem(device, "name");
            auto* type = cJSON_GetObjectItem(device, "type");
            if (cJSON_IsString(id)) {
                appliance.id = id->valuestring;
            }
            if (cJSON_IsString(name)) {
                appliance.name = name->valuestring;
            }
            if (cJSON_IsString(type)) {
                appliance.type = type->valuestring;
            }
            auto* commands = cJSON_GetObjectItem(device, "commands");
            if (cJSON_IsArray(commands)) {
                cJSON* cmd = nullptr;
                cJSON_ArrayForEach(cmd, commands) {
                    IrCommand command;
                    auto* cid = cJSON_GetObjectItem(cmd, "id");
                    auto* cname = cJSON_GetObjectItem(cmd, "name");
                    auto* code = cJSON_GetObjectItem(cmd, "code");
                    if (cJSON_IsString(cid)) {
                        command.id = cid->valuestring;
                    }
                    if (cJSON_IsString(cname)) {
                        command.name = cname->valuestring;
                    }
                    if (cJSON_IsString(code)) {
                        command.code = NormalizeCode(code->valuestring);
                    }
                    if (!command.id.empty()) {
                        appliance.commands.push_back(std::move(command));
                    }
                }
            }
            if (!appliance.id.empty()) {
                appliances_.push_back(std::move(appliance));
            }
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d IR appliances", (int)appliances_.size());
    return true;
}

bool IrCatalog::WriteUnlocked() {
    if (!mounted_) {
        ESP_LOGE(TAG, "SPIFFS not mounted, cannot write %s", kPath);
        return false;
    }

    FILE* file = fopen(kPath, "wb");
    if (!file) {
        ESP_LOGW(TAG, "fopen wb failed errno=%d, unlink and retry", errno);
        unlink(kPath);
        file = fopen(kPath, "wb");
    }
    if (!file) {
        LogSpiffsUsage("write-fail");
        ESP_LOGE(TAG, "Failed to write %s errno=%d", kPath, errno);
        return false;
    }

    bool ok = fputs("{\"devices\":[", file) >= 0;
    bool first_device = true;
    for (const auto& appliance : appliances_) {
        if (!ok) {
            break;
        }
        if (!first_device) {
            ok = fputc(',', file) != EOF;
        }
        first_device = false;
        ok = ok && fputs("{\"id\":\"", file) >= 0;
        WriteEscaped(file, appliance.id);
        ok = ok && fputs("\",\"name\":\"", file) >= 0;
        WriteEscaped(file, appliance.name);
        ok = ok && fputs("\",\"type\":\"", file) >= 0;
        WriteEscaped(file, appliance.type);
        ok = ok && fputs("\",\"commands\":[", file) >= 0;
        bool first_cmd = true;
        for (const auto& command : appliance.commands) {
            if (!ok) {
                break;
            }
            if (!first_cmd) {
                ok = fputc(',', file) != EOF;
            }
            first_cmd = false;
            ok = ok && fputs("{\"id\":\"", file) >= 0;
            WriteEscaped(file, command.id);
            ok = ok && fputs("\",\"name\":\"", file) >= 0;
            WriteEscaped(file, command.name);
            ok = ok && fputs("\",\"code\":\"", file) >= 0;
            WriteEscaped(file, command.code);
            ok = ok && fputs("\"}", file) >= 0;
        }
        ok = ok && fputs("]}", file) >= 0;
    }
    ok = ok && fputs("]}", file) >= 0;
    if (fflush(file) != 0) {
        ok = false;
    }
    if (ferror(file)) {
        ok = false;
    }
    fclose(file);
    if (!ok) {
        unlink(kPath);
        LogSpiffsUsage("write-fail");
        ESP_LOGE(TAG, "Failed to flush %s errno=%d", kPath, errno);
        return false;
    }
    LogSpiffsUsage("write-ok");
    return true;
}

bool IrCatalog::Save() {
    std::lock_guard<std::mutex> lock(mutex_);
    return WriteUnlocked();
}

bool IrCatalog::Reload() {
    std::lock_guard<std::mutex> lock(mutex_);
    return LoadUnlocked();
}

std::vector<IrAppliance> IrCatalog::GetAppliances() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return appliances_;
}

IrAppliance* IrCatalog::FindApplianceMutable(const std::string& id_or_name) {
    for (auto& appliance : appliances_) {
        if (IdEquals(appliance.id, id_or_name) || appliance.name == id_or_name) {
            return &appliance;
        }
    }
    return nullptr;
}

const IrAppliance* IrCatalog::FindAppliance(const std::string& id_or_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& appliance : appliances_) {
        if (IdEquals(appliance.id, id_or_name) || appliance.name == id_or_name) {
            return &appliance;
        }
    }
    return nullptr;
}

const IrCommand* IrCatalog::FindCommand(const std::string& appliance_id,
                                        const std::string& command_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& appliance : appliances_) {
        if (!IdEquals(appliance.id, appliance_id) && appliance.name != appliance_id) {
            continue;
        }
        for (const auto& command : appliance.commands) {
            if (command.id == command_id || command.name == command_id) {
                return &command;
            }
        }
    }
    return nullptr;
}

std::string IrCatalog::FindCode(const std::string& appliance_id, const std::string& command_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& appliance : appliances_) {
        if (!IdEquals(appliance.id, appliance_id) && appliance.name != appliance_id) {
            continue;
        }
        for (const auto& command : appliance.commands) {
            if (command.id == command_id || command.name == command_id) {
                return command.code;
            }
        }
    }
    return "";
}

IrCatalogStatus IrCatalog::UpsertAppliance(const IrAppliance& appliance, bool merge_commands) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ValidId(appliance.id) || !ValidName(appliance.name) || !ValidType(appliance.type)) {
        return IrCatalogStatus::kInvalid;
    }
    for (auto& existing : appliances_) {
        if (existing.id == appliance.id) {
            IrAppliance backup = existing;
            existing.name = appliance.name;
            existing.type = appliance.type;
            if (!merge_commands) {
                existing.commands = appliance.commands;
            }
            if (WriteUnlocked()) {
                return IrCatalogStatus::kOk;
            }
            existing = std::move(backup);
            return IrCatalogStatus::kWriteFailed;
        }
    }
    if (appliances_.size() >= kMaxAppliances) {
        return IrCatalogStatus::kInvalid;
    }
    appliances_.push_back(appliance);
    if (WriteUnlocked()) {
        return IrCatalogStatus::kOk;
    }
    appliances_.pop_back();
    return IrCatalogStatus::kWriteFailed;
}

IrCatalogStatus IrCatalog::DeleteAppliance(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::remove_if(appliances_.begin(), appliances_.end(),
                             [&](const IrAppliance& a) { return a.id == id; });
    if (it == appliances_.end()) {
        return IrCatalogStatus::kNotFound;
    }
    std::vector<IrAppliance> removed(it, appliances_.end());
    appliances_.erase(it, appliances_.end());
    if (WriteUnlocked()) {
        return IrCatalogStatus::kOk;
    }
    appliances_.insert(appliances_.end(), removed.begin(), removed.end());
    return IrCatalogStatus::kWriteFailed;
}

IrCatalogStatus IrCatalog::UpsertCommand(const std::string& appliance_id, const IrCommand& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ValidId(appliance_id) || !ValidId(command.id) || !ValidCode(command.code)) {
        return IrCatalogStatus::kInvalid;
    }
    if (!command.name.empty() && !ValidName(command.name)) {
        return IrCatalogStatus::kInvalid;
    }
    auto* appliance = FindApplianceMutable(appliance_id);
    if (!appliance) {
        return IrCatalogStatus::kNotFound;
    }
    for (auto& existing : appliance->commands) {
        if (existing.id == command.id) {
            std::string old_name = existing.name;
            std::string old_code = existing.code;
            if (!command.name.empty()) {
                existing.name = command.name;
            }
            if (!command.code.empty()) {
                existing.code = NormalizeCode(command.code);
            }
            if (WriteUnlocked()) {
                return IrCatalogStatus::kOk;
            }
            existing.name = old_name;
            existing.code = old_code;
            return IrCatalogStatus::kWriteFailed;
        }
    }
    if (appliance->commands.size() >= kMaxCommands) {
        return IrCatalogStatus::kInvalid;
    }
    if (command.name.empty()) {
        return IrCatalogStatus::kInvalid;
    }
    IrCommand copy = command;
    copy.code = NormalizeCode(copy.code);
    appliance->commands.push_back(copy);
    if (WriteUnlocked()) {
        return IrCatalogStatus::kOk;
    }
    appliance->commands.pop_back();
    return IrCatalogStatus::kWriteFailed;
}

IrCatalogStatus IrCatalog::DeleteCommand(const std::string& appliance_id, const std::string& command_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* appliance = FindApplianceMutable(appliance_id);
    if (!appliance) {
        return IrCatalogStatus::kNotFound;
    }
    auto it = std::find_if(appliance->commands.begin(), appliance->commands.end(),
                           [&](const IrCommand& c) { return c.id == command_id || c.name == command_id; });
    if (it == appliance->commands.end()) {
        return IrCatalogStatus::kNotFound;
    }
    IrCommand removed = *it;
    appliance->commands.erase(it);
    if (WriteUnlocked()) {
        return IrCatalogStatus::kOk;
    }
    appliance->commands.push_back(std::move(removed));
    return IrCatalogStatus::kWriteFailed;
}

std::string IrCatalog::MetadataJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = cJSON_CreateObject();
    cJSON* devices = cJSON_CreateArray();
    for (const auto& appliance : appliances_) {
        cJSON* device = cJSON_CreateObject();
        cJSON_AddStringToObject(device, "id", appliance.id.c_str());
        cJSON_AddStringToObject(device, "name", appliance.name.c_str());
        cJSON_AddStringToObject(device, "type", appliance.type.c_str());
        cJSON* commands = cJSON_CreateArray();
        for (const auto& command : appliance.commands) {
            cJSON* cmd = cJSON_CreateObject();
            cJSON_AddStringToObject(cmd, "id", command.id.c_str());
            cJSON_AddStringToObject(cmd, "name", command.name.c_str());
            cJSON_AddBoolToObject(cmd, "learned", !command.code.empty());
            cJSON_AddItemToArray(commands, cmd);
        }
        cJSON_AddItemToObject(device, "commands", commands);
        cJSON_AddItemToArray(devices, device);
    }
    cJSON_AddItemToObject(root, "devices", devices);
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    std::string json = printed ? printed : "{}";
    if (printed) {
        cJSON_free(printed);
    }
    return json;
}

void IrCatalog::SeedIfEmpty(const std::vector<IrAppliance>& seed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!appliances_.empty() || seed.empty()) {
        return;
    }
    appliances_ = seed;
    if (!WriteUnlocked()) {
        ESP_LOGE(TAG, "Seeded catalog is RAM-only until storage writes succeed");
    }
    ESP_LOGI(TAG, "Seeded IR catalog with %d appliances", (int)appliances_.size());
}

std::string IrCatalog::NormalizeCode(const std::string& raw) {
    std::string code = raw;
    code.erase(std::remove(code.begin(), code.end(), '\n'), code.end());
    code.erase(std::remove(code.begin(), code.end(), '\r'), code.end());
    while (!code.empty() && (code.front() == ' ' || code.front() == '\t')) {
        code.erase(code.begin());
    }
    while (!code.empty() && (code.back() == ' ' || code.back() == '\t')) {
        code.pop_back();
    }
    if (code.size() >= 3 && (code.compare(0, 3, "zf=") == 0 || code.compare(0, 3, "ZF=") == 0)) {
        code = code.substr(3);
    }
    return code;
}

std::string IrCatalog::UartPayload(const std::string& code_or_cmd) {
    std::string value = NormalizeCode(code_or_cmd);
    if (value.empty()) {
        return value;
    }
    if (value.rfind("xx", 0) == 0) {
        return value;
    }
    return "zf=" + value;
}
