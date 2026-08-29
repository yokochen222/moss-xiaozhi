#pragma once

#include <cJSON.h>
#include <string>

struct HwApplyResult {
    bool ok = false;
    std::string message = "ok";
};

HwApplyResult MossHwApply(cJSON* payload);
cJSON* MossHwStateJson(bool ok, const std::string& message);
