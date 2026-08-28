#pragma once

#include <mutex>
#include <string>
#include <vector>

struct IrCommand {
    std::string id;
    std::string name;
    std::string code;
};

struct IrAppliance {
    std::string id;
    std::string name;
    std::string type;
    std::vector<IrCommand> commands;
};

enum class IrCatalogStatus {
    kOk = 0,
    kNotFound,
    kInvalid,
    kWriteFailed,
};

class IrCatalog {
public:
    static IrCatalog& GetInstance();

    bool Initialize();
    bool Save();
    bool Reload();

    std::vector<IrAppliance> GetAppliances() const;
    const IrAppliance* FindAppliance(const std::string& id_or_name) const;
    const IrCommand* FindCommand(const std::string& appliance_id, const std::string& command_id) const;
    std::string FindCode(const std::string& appliance_id, const std::string& command_id) const;

    IrCatalogStatus UpsertAppliance(const IrAppliance& appliance, bool merge_commands);
    IrCatalogStatus DeleteAppliance(const std::string& id);
    IrCatalogStatus UpsertCommand(const std::string& appliance_id, const IrCommand& command);
    IrCatalogStatus DeleteCommand(const std::string& appliance_id, const std::string& command_id);

    std::string MetadataJson() const;
    std::string ExportJson() const;
    IrCatalogStatus ImportCatalog(const std::string& json, bool replace);
    void SeedIfEmpty(const std::vector<IrAppliance>& seed);

    const char* LastErrorMessage() const;
    static const char* StatusMessage(IrCatalogStatus status);
    static std::string NormalizeCode(const std::string& raw);
    static std::string UartPayload(const std::string& code_or_cmd);
    static bool ValidId(const std::string& id);
    static bool ValidName(const std::string& name);
    static bool ValidType(const std::string& type);
    static bool ValidCode(const std::string& raw);

private:
    IrCatalog() = default;

    bool LoadUnlocked();
    bool WriteUnlocked();
    IrAppliance* FindApplianceMutable(const std::string& id_or_name);

    mutable std::mutex mutex_;
    std::vector<IrAppliance> appliances_;
    mutable std::string last_error_;
    bool mounted_ = false;
};

void SeedIrCatalogFromBuiltin();
