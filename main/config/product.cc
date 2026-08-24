#include "product.h"

namespace MossProduct {

void AddIdentity(cJSON* obj) {
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "product", kId);
    cJSON* caps = cJSON_CreateObject();
    cJSON_AddBoolToObject(caps, "onboard_camera", true);
    cJSON_AddBoolToObject(caps, "onvif_camera", false);
    cJSON_AddBoolToObject(caps, "ir", false);
    cJSON_AddBoolToObject(caps, "gimbal", true);
    cJSON_AddBoolToObject(caps, "face_track", true);
    cJSON_AddBoolToObject(caps, "lamps", true);
    cJSON_AddBoolToObject(caps, "motor", true);
    cJSON_AddBoolToObject(caps, "custom_wake_word", false);
    cJSON_AddItemToObject(obj, "capabilities", caps);
}

}  // namespace MossProduct
