#include "application.h"
#include "config/yunxiangji_outbox.h"
#include "mcp_tools.h"

#include <esp_log.h>
#include <cJSON.h>
#include <cstdio>
#include <string>
#include <vector>

#define TAG "YunxiangjiTool"

namespace mcp_tools {

namespace {

const char* kCreateDescription =
    "添加一条云享记定时提醒。用户说「提醒我…」「到点叫我…」时必须调用本工具，不要只口头答应。"
    "content 是到点后口头播报的完整句子，必须保留人称和动作，不要只抽关键词。"
    "主信息是「提醒谁、做什么」；时间只写入时间参数，不能代替内容。"
    "相对时间必须用 inSeconds 或 inMinutes，禁止把延迟写进 hour/minute，禁止只口头答应。"
    "用户说「五分钟后提醒我喝水」：inMinutes=5 或 inSeconds=300，content "
    "写成「提醒你喝水」或「该提醒你喝水了」，"
    "禁止只写「喝水」，不要填 year/month/day/hour/minute。"
    "用户说「一分钟后提醒我喝水」：inSeconds=60。"
    "用户说「提醒上校开会」：content 写「提醒上校开会」，不要改成「开会」。"
    "对用户说话时把「我」换成「你」，不要编造用户没提到的对象。"
    "添加成功后会出现在电脑云享记列表。绝对时间用用户本地时区的 datetime（YYYY-MM-DD HH:mm:ss），"
    "或 year/month/day/hour/minute/second；未用的年月日时分请省略。";

const char* kListDescription =
    "列出电脑云享记里已同步的提醒。用户问「有哪些提醒」「取消刚才那个」之前应先调用，以便对照 id "
    "或内容。"
    "返回每条的状态、完整内容和 id。";

const char* kCancelDescription =
    "取消尚未播报的云享记。用户说「取消提醒」「不要叫我了」时必须调用。"
    "优先用 list 得到的 id；否则用 query 匹配内容（例如「喝水」「提醒你开会」）。"
    "不要只口头答应。取消后这条不会再播报，但仍留在电脑列表里显示为已取消。";

const char* kDeleteDescription =
    "从电脑云享记列表删除一条提醒，已播报的也可以删。"
    "用户说「删掉这条提醒」「把喝水的提醒去掉」时必须调用。"
    "优先用 list 得到的 id；否则用 query 匹配完整内容，不要只匹配单个词导致删错。"
    "删除后列表里不再出现。";

std::string RelayMutation(const YunxiangjiCreateItem& item, const char* marker) {
    const std::string id = YunxiangjiOutbox::GetInstance().Push(item);
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", id.c_str());
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
    char* printed = cJSON_PrintUnformatted(obj);
    std::string payload = printed ? printed : "{}";
    if (printed) {
        cJSON_free(printed);
    }
    cJSON_Delete(obj);
    Application::GetInstance().RelayChat("message", "system", std::string(marker) + payload);
    return id;
}

std::vector<YunxiangjiInboxItem> ResolveTargets(const std::string& id, const std::string& query) {
    auto& inbox = YunxiangjiInbox::GetInstance();
    if (!id.empty()) {
        auto matched = inbox.Match(id);
        if (!matched.empty()) {
            return matched;
        }
    }
    if (!query.empty()) {
        auto matched = inbox.Match(query);
        if (!matched.empty()) {
            return matched;
        }
    }
    std::vector<YunxiangjiInboxItem> from_outbox;
    const std::string wanted = NormalizeYunxiangjiText(query.empty() ? id : query);
    for (const auto& item : YunxiangjiOutbox::GetInstance().Snapshot()) {
        if (item.op != "create" && !item.op.empty()) {
            continue;
        }
        if (!id.empty() && (item.id == id || item.device_create_id == id)) {
            YunxiangjiInboxItem row;
            row.id = item.id;
            row.content = item.content;
            row.title = item.title;
            row.status = "pending";
            row.device_create_id = item.id;
            from_outbox.push_back(row);
            continue;
        }
        if (wanted.empty()) {
            continue;
        }
        const std::string hay = NormalizeYunxiangjiText(item.content + item.title);
        if (hay.find(wanted) != std::string::npos) {
            YunxiangjiInboxItem row;
            row.id = item.id;
            row.content = item.content;
            row.title = item.title;
            row.status = "pending";
            row.device_create_id = item.id;
            from_outbox.push_back(row);
        }
    }
    return from_outbox;
}

bool ParseDatetimeLocal(const std::string& raw, YunxiangjiCreateItem& item) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    char sep = 0;
    int n = sscanf(raw.c_str(), "%d-%d-%d%c%d:%d:%d", &year, &month, &day, &sep, &hour, &minute,
                   &second);
    if (n < 7) {
        second = 0;
        n = sscanf(raw.c_str(), "%d-%d-%d%c%d:%d", &year, &month, &day, &sep, &hour, &minute);
        if (n != 6) {
            return false;
        }
    }
    if ((sep != ' ' && sep != 'T') || year < 2000 || year > 2100 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
        second > 59) {
        return false;
    }
    item.year = year;
    item.month = month;
    item.day = day;
    item.hour = hour;
    item.minute = minute;
    item.second = second;
    return true;
}

std::string ApplyMutation(const char* op, const std::string& id, const std::string& query) {
    if (id.empty() && query.empty()) {
        return "请提供 id 或 query";
    }
    auto matched = ResolveTargets(id, query);
    YunxiangjiCreateItem item;
    item.op = op;
    item.query = query;
    item.target_id = id;
    if (matched.size() == 1) {
        item.target_id = matched[0].id;
        item.content = matched[0].content;
        item.device_create_id = matched[0].device_create_id;
        if (item.query.empty()) {
            item.query = matched[0].content;
        }
    } else if (matched.empty()) {
        item.content = query;
    } else if (id.empty()) {
        std::string lines = "找到多条，请用更完整的 query 或 id：\n";
        for (const auto& row : matched) {
            lines += "- ";
            lines += row.content.empty() ? row.title : row.content;
            lines += " id=";
            lines += row.id;
            lines += "\n";
        }
        return lines;
    }
    const char* marker = (item.op == "delete") ? "yunxiangji.delete:" : "yunxiangji.cancel:";
    const std::string mutation_id = RelayMutation(item, marker);
    std::vector<std::string> remove_ids;
    if (!item.target_id.empty()) {
        remove_ids.push_back(item.target_id);
    }
    if (!item.device_create_id.empty()) {
        remove_ids.push_back(item.device_create_id);
    }
    if (item.op == "delete" && !remove_ids.empty()) {
        YunxiangjiInbox::GetInstance().RemoveIds(remove_ids);
    }
    ESP_LOGI(TAG, "queued %s %s", op, mutation_id.c_str());
    if (item.op == "delete") {
        return matched.empty() ? "已提交删除，将从电脑云享记列表去掉。"
                               : "已删除，将从电脑云享记列表去掉。";
    }
    return matched.empty() ? "已提交取消，到点将不再播报。" : "已取消，到点将不再播报。";
}

}  // namespace

class YunxiangjiTool : public McpTool {
public:
    static YunxiangjiTool& GetInstance() {
        static YunxiangjiTool instance;
        return instance;
    }
    YunxiangjiTool()
        : McpTool("self.yunxiangji.create", "添加一条云享记定时提醒，会同步到电脑云享记列表。") {}
    void Register() override;
};

void YunxiangjiTool::Register() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    ESP_LOGI(TAG, "register self.yunxiangji.create / list / cancel / delete / take / ack");
    McpServer::GetInstance().AddTool(
        "self.yunxiangji.create", kCreateDescription,
        PropertyList({
            Property("content", kPropertyTypeString),
            Property("title", kPropertyTypeString, std::string("")),
            Property("datetime", kPropertyTypeString, std::string("")),
            Property("inSeconds", kPropertyTypeInteger, 0, 0, 366 * 24 * 60 * 60),
            Property("inMinutes", kPropertyTypeInteger, 0, 0, 366 * 24 * 60),
            Property("year", kPropertyTypeInteger, -1, -1, 2100),
            Property("month", kPropertyTypeInteger, -1, -1, 12),
            Property("day", kPropertyTypeInteger, -1, -1, 31),
            Property("hour", kPropertyTypeInteger, -1, -1, 23),
            Property("minute", kPropertyTypeInteger, -1, -1, 59),
            Property("second", kPropertyTypeInteger, 0, 0, 59),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            YunxiangjiCreateItem item;
            item.op = "create";
            item.content = properties["content"].value<std::string>();
            item.title = properties["title"].value<std::string>();
            item.datetime = properties["datetime"].value<std::string>();
            item.in_seconds = properties["inSeconds"].value<int>();
            const int in_minutes = properties["inMinutes"].value<int>();
            item.year = properties["year"].value<int>();
            item.month = properties["month"].value<int>();
            item.day = properties["day"].value<int>();
            item.hour = properties["hour"].value<int>();
            item.minute = properties["minute"].value<int>();
            item.second = properties["second"].value<int>();
            if (item.content.empty()) {
                return "缺少提醒内容。请写完整句子，例如「提醒你喝水」，不要只写「喝水」。";
            }
            if (item.in_seconds <= 0 && in_minutes > 0) {
                item.in_seconds = in_minutes * 60;
            }
            if (item.in_seconds > 0) {
                // Relative delay is authoritative; drop wall-clock fields so the
                // desktop scheduler cannot treat hour/minute as 00:05.
                item.datetime.clear();
                item.year = 0;
                item.month = 0;
                item.day = 0;
                item.hour = 0;
                item.minute = 0;
                item.second = 0;
            } else if (!item.datetime.empty()) {
                if (!ParseDatetimeLocal(item.datetime, item)) {
                    return "datetime 必须是 YYYY-MM-DD HH:mm:ss（本地时区）。相对时间请用 "
                           "inSeconds 或 inMinutes。";
                }
            }
            const bool has_delay = item.in_seconds > 0;
            const bool has_datetime = !item.datetime.empty();
            const bool has_ymd = item.year >= 2000 && item.month >= 1 && item.day >= 1 &&
                                 item.hour >= 0 && item.minute >= 0;
            if (!has_delay && !has_datetime && !has_ymd) {
                return "请提供时间：相对时间用 inSeconds 或 inMinutes，绝对时间用 datetime "
                       "或年月日时分";
            }
            const std::string id = RelayMutation(item, "yunxiangji.create:");
            const std::string when = FormatYunxiangjiSchedule(item);
            ESP_LOGI(TAG, "queued create %s when=%s inSeconds=%d datetime=%s", id.c_str(),
                     when.c_str(), item.in_seconds, item.datetime.c_str());
            return std::string("已记下（") + when +
                   "），将出现在电脑云享记列表。不要再说已经记下以外的寒暄。";
        });

    McpServer::GetInstance().AddTool(
        "self.yunxiangji.list", kListDescription, PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            std::string text = YunxiangjiInbox::GetInstance().FormatList();
            std::string pending;
            for (const auto& item : YunxiangjiOutbox::GetInstance().Snapshot()) {
                if (item.op != "create" && !item.op.empty()) {
                    continue;
                }
                if (item.content.empty()) {
                    continue;
                }
                pending += "\n- 待同步: ";
                pending += item.content;
                pending += " @ ";
                pending += FormatYunxiangjiSchedule(item);
                pending += " id=";
                pending += item.id;
            }
            if (!pending.empty()) {
                text += "\n刚在板子添加、尚未同步：";
                text += pending;
            }
            return text;
        });

    McpServer::GetInstance().AddTool("self.yunxiangji.cancel", kCancelDescription,
                                     PropertyList({
                                         Property("id", kPropertyTypeString, std::string("")),
                                         Property("query", kPropertyTypeString, std::string("")),
                                     }),
                                     [](const PropertyList& properties) -> ReturnValue {
                                         return ApplyMutation(
                                             "cancel", properties["id"].value<std::string>(),
                                             properties["query"].value<std::string>());
                                     });

    McpServer::GetInstance().AddTool("self.yunxiangji.delete", kDeleteDescription,
                                     PropertyList({
                                         Property("id", kPropertyTypeString, std::string("")),
                                         Property("query", kPropertyTypeString, std::string("")),
                                     }),
                                     [](const PropertyList& properties) -> ReturnValue {
                                         return ApplyMutation(
                                             "delete", properties["id"].value<std::string>(),
                                             properties["query"].value<std::string>());
                                     });

    McpServer::GetInstance().AddTool(
        "self.yunxiangji.take",
        "查询待播报的云享记提醒内容。每次对话开始若消息是「请调用工具查询提醒内容」，必须先调用本工"
        "具，且整轮只调用一次。"
        "返回 none 表示没有待播报或已经播报过，不要向用户播报、不要寒暄、不要重复查询、不要再说话。"
        "否则把返回内容完整口头播报给用户恰好一次，不要改写、不要再说第二遍，然后立刻调用 "
        "self.yunxiangji.ack。ack 之后禁止再播报同一句话，也不要说再见或结束会话。",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            std::string text = Application::GetInstance().TakePendingAnnounce();
            if (text.empty()) {
                return "none";
            }
            return text;
        });

    McpServer::GetInstance().AddTool(
        "self.yunxiangji.ack",
        "标记待播报云享记为已播报。口头播报一次后必须立刻调用。"
        "返回「已播报」。调用后不要再说同一句、不要寒暄、不要说再见，保持聆听等用户说话。",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            return Application::GetInstance().AckPendingAnnounce();
        });
}

void RegisterYunxiangjiTools() { YunxiangjiTool::GetInstance().Register(); }

}  // namespace mcp_tools

static auto& g_yunxiangji_tool_instance __attribute__((used)) =
    mcp_tools::YunxiangjiTool::GetInstance();
DECLARE_MCP_TOOL_INSTANCE(g_yunxiangji_tool_instance);
