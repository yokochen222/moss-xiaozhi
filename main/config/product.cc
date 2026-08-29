#include "product.h"

namespace MossProduct {

void AddIdentity(cJSON* obj) {
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "product", kId);
    cJSON_AddStringToObject(obj, "board", BOARD_TYPE);
}

}  // namespace MossProduct
