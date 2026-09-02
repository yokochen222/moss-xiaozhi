#pragma once

#include <cJSON.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct YunxiangjiCreateItem {
    std::string id;
    std::string op;
    std::string content;
    std::string title;
    std::string query;
    std::string target_id;
    std::string device_create_id;
    std::string datetime;
    int in_seconds = 0;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

struct YunxiangjiInboxItem {
    std::string id;
    std::string content;
    std::string title;
    std::string status;
    std::string at;
    std::string device_create_id;
};

class YunxiangjiOutbox {
public:
    static YunxiangjiOutbox& GetInstance();

    std::string Push(YunxiangjiCreateItem item);
    std::vector<YunxiangjiCreateItem> Snapshot() const;
    void Ack(const std::vector<std::string>& ids);
    size_t Size() const;
    cJSON* ToJsonArray() const;

private:
    mutable std::mutex mutex_;
    std::vector<YunxiangjiCreateItem> items_;
    uint32_t next_id_ = 1;
};

class YunxiangjiInbox {
public:
    static YunxiangjiInbox& GetInstance();

    void Replace(std::vector<YunxiangjiInboxItem> items);
    void RemoveIds(const std::vector<std::string>& ids);
    std::vector<YunxiangjiInboxItem> Snapshot() const;
    std::vector<YunxiangjiInboxItem> Match(const std::string& query) const;
    std::string FormatList() const;
    size_t Size() const;

private:
    mutable std::mutex mutex_;
    std::vector<YunxiangjiInboxItem> items_;
};

std::string NormalizeYunxiangjiText(const std::string& text);
std::string FormatYunxiangjiSchedule(const YunxiangjiCreateItem& item);
