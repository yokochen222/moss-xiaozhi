#pragma once

#include <cJSON.h>
#include <string>

namespace DeviceConfig {

cJSON* BuildJson();
bool Apply(cJSON* payload, std::string* error);

int DefaultMotorSpeedPercent();
bool SetDefaultMotorSpeedPercent(int speed);

bool LocalAecSupported();
bool LocalAecEnabled();
void SetLocalAecEnabled(bool enabled);

}  // namespace DeviceConfig
