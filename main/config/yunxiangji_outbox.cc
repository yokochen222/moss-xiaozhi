#include "yunxiangji_outbox.h"

#include <esp_timer.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

std::string FormatYunxiangjiSchedule(const YunxiangjiCreateItem& item) {
    if (item.in_seconds > 0) {
        return std::to_string(item.in_seconds) + "秒后";
    }
    if (!item.datetime.empty()) {
        return item.datetime;
    }
    if (item.year >= 2000 && item.month >= 1 && item.day >= 1 && item.hour >= 0) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", item.year, item.month, item.day,
                 item.hour, item.minute, item.second);
        return buf;
    }
    return "未定时";
}

std::string NormalizeYunxiangjiText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        if (ch <= 32) {
            continue;
        }
        if (ch < 128 && !std::isalnum(ch)) {
            continue;
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

YunxiangjiOutbox& YunxiangjiOutbox::GetInstance() {
    static YunxiangjiOutbox instance;
    return instance;
}

std::string YunxiangjiOutbox::Push(YunxiangjiCreateItem item) {
    std::lock_guard<std::mutex> lock(mutex_);
    char id[40];
    snprintf(id, sizeof(id), "d-%lu-%lld", static_cast<unsigned long>(next_id_++),
             static_cast<long long>(esp_timer_get_time()));
    item.id = id;
    if (item.op.empty()) {
        item.op = "create";
    }
    if (items_.size() >= 16) {
        items_.erase(items_.begin());
    }
    items_.push_back(item);
    return item.id;
}

std::vector<YunxiangjiCreateItem> YunxiangjiOutbox::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

void YunxiangjiOutbox::Ack(const std::vector<std::string>& ids) {
    if (ids.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&ids](const YunxiangjiCreateItem& item) {
                                    return std::find(ids.begin(), ids.end(), item.id) != ids.end();
                                }),
                 items_.end());
}

size_t YunxiangjiOutbox::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

cJSON* YunxiangjiOutbox::ToJsonArray() const {
    cJSON* arr = cJSON_CreateArray();
    for (const auto& item : Snapshot()) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", item.id.c_str());
        cJSON_AddStringToObject(obj, "op", item.op.empty() ? "create" : item.op.c_str());
        if (!item.content.empty()) {
            cJSON_AddStringToObject(obj, "content", item.content.c_str());
        }
        if (!item.title.empty()) {
            cJSON_AddStringToObject(obj, "title", item.title.c_str());
        }
        if (!item.query.empty()) {
            cJSON_AddStringToObject(obj, "query", item.query.c_str());
        }
        if (!item.target_id.empty()) {
            cJSON_AddStringToObject(obj, "targetId", item.target_id.c_str());
        }
        if (!item.device_create_id.empty()) {
            cJSON_AddStringToObject(obj, "deviceCreateId", item.device_create_id.c_str());
        }
        if (!item.datetime.empty()) {
            cJSON_AddStringToObject(obj, "datetime", item.datetime.c_str());
        }
        if (item.in_seconds > 0) {
            cJSON_AddNumberToObject(obj, "inSeconds", item.in_seconds);
        }
        if (item.year >= 2000) {
            cJSON_AddNumberToObject(obj, "year", item.year);
            cJSON_AddNumberToObject(obj, "month", item.month);
            cJSON_AddNumberToObject(obj, "day", item.day);
            cJSON_AddNumberToObject(obj, "hour", item.hour);
            cJSON_AddNumberToObject(obj, "minute", item.minute);
            cJSON_AddNumberToObject(obj, "second", item.second);
        }
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}

YunxiangjiInbox& YunxiangjiInbox::GetInstance() {
    static YunxiangjiInbox instance;
    return instance;
}

void YunxiangjiInbox::Replace(std::vector<YunxiangjiInboxItem> items) {
    if (items.size() > 40) {
        items.resize(40);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    items_ = std::move(items);
}

void YunxiangjiInbox::RemoveIds(const std::vector<std::string>& ids) {
    if (ids.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&ids](const YunxiangjiInboxItem& item) {
                                    return std::find(ids.begin(), ids.end(), item.id) !=
                                               ids.end() ||
                                           (!item.device_create_id.empty() &&
                                            std::find(ids.begin(), ids.end(),
                                                      item.device_create_id) != ids.end());
                                }),
                 items_.end());
}

std::vector<YunxiangjiInboxItem> YunxiangjiInbox::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

std::vector<YunxiangjiInboxItem> YunxiangjiInbox::Match(const std::string& query) const {
    const std::string wanted = NormalizeYunxiangjiText(query);
    std::vector<YunxiangjiInboxItem> matched;
    if (wanted.empty() && query.empty()) {
        return matched;
    }
    for (const auto& item : Snapshot()) {
        if (!query.empty() && (item.id == query || item.device_create_id == query)) {
            matched.push_back(item);
            continue;
        }
        const std::string hay = NormalizeYunxiangjiText(item.content + item.title);
        if (!wanted.empty() && hay.find(wanted) != std::string::npos) {
            matched.push_back(item);
        }
    }
    return matched;
}

std::string YunxiangjiInbox::FormatList() const {
    const auto items = Snapshot();
    if (items.empty()) {
        return "暂无已同步的提醒。若刚对着板子添加，可用 query 取消或删除刚才那条。";
    }
    std::string out;
    int index = 1;
    for (const auto& item : items) {
        if (!out.empty()) {
            out += "\n";
        }
        out += std::to_string(index++);
        out += ". [";
        out += item.status.empty() ? "pending" : item.status;
        out += "] ";
        out += item.content.empty() ? item.title : item.content;
        if (!item.at.empty()) {
            out += " @ ";
            out += item.at;
        }
        out += " id=";
        out += item.id;
    }
    return out;
}

size_t YunxiangjiInbox::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}
