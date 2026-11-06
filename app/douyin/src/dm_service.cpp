/**
 * @file dm_service.cpp
 * @brief 抖音私信（DM）服务（29109）：会话 / 发送 / 增量拉取 / 已读。
 *
 * @details 路由（除 /health、/metrics 外都要 Bearer）：
 *          - POST /api/dm/send                        发私信（1~500 字）
 *          - GET  /api/dm/conversations?cursor=&limit= 我的会话列表
 *          - GET  /api/dm/:id/messages?after=&limit=  会话消息（增量）
 *          - POST /api/dm/:id/read                    标记已读
 *          - GET  /health, /metrics
 *
 *          表由本服务在启动时幂等创建（`CREATE TABLE IF NOT EXISTS`），
 *          与其它服务共用同一个 SQLite 文件；users / blocks / notifications
 *          只读或追加写，不改既有表结构。
 *
 * @author douyin demo
 * @date 2026-11-06
 * @copyright MIT License
 */

#include "douyin_auth.hpp"
#include "douyin_http.hpp"
#include "go2cpp/storage/db.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t kDefaultPort = 29109;
constexpr const char* kServiceName = "dm";
/// @brief 与其它服务共用的默认库路径（与 Store::kDefaultPath 一致）。
constexpr const char* kDefaultDbPath = "data/douyin.db";
constexpr std::size_t kMaxContentChars = 500;
constexpr std::size_t kMaxContentBytes = 2000;

/// @brief 单线程事件循环上的计数器（无需原子）。
struct ServiceMetrics {
    std::uint64_t requests{0};
    std::uint64_t errors{0};
};

ServiceMetrics g_metrics;

/// @brief 当前 Unix 秒。
std::int64_t NowSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

/// @brief 统一错误响应。
bool Fail(douyin::HttpResponse* response, int status, std::string_view message) {
    response->status = status;
    response->body = douyin::ErrorBody(status, message);
    ++g_metrics.errors;
    return true;
}

/// @brief 解析并校验 Bearer token。
bool RequireToken(const douyin::HttpRequest& request, douyin::HttpResponse* response,
                  std::int64_t* user_id) {
    const std::string token = douyin::BearerToken(request);
    if (token.empty()) {
        Fail(response, 401, "缺少 Bearer token");
        return false;
    }
    douyin::TokenInfo info;
    std::string error;
    if (!douyin::VerifyToken(token, NowSeconds(), &info, &error)) {
        Fail(response, 401, error.empty() ? "token 无效" : error);
        return false;
    }
    *user_id = info.user_id;
    return true;
}

/// @brief 读 query 里的非负整数；缺省用 fallback，非法返回 false。
bool ReadQueryInt(const douyin::HttpRequest& request, const char* name,
                  std::int64_t fallback, std::int64_t* out) {
    const std::string text = douyin::QueryParam(request, name);
    if (text.empty()) {
        *out = fallback;
        return true;
    }
    if (text.size() > 18) {
        return false;
    }
    std::int64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    *out = value;
    return true;
}

/// @brief 取 JSON 里的整数字段（只支持正整数，足够本项目固定结构）。
bool JsonIntField(std::string_view body, std::string_view key, std::int64_t* out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t pos = body.find(needle);
    if (pos == std::string_view::npos) {
        return false;
    }
    pos = body.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return false;
    }
    ++pos;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) {
        ++pos;
    }
    if (pos >= body.size() || body[pos] < '0' || body[pos] > '9') {
        return false;
    }
    std::int64_t value = 0;
    while (pos < body.size() && body[pos] >= '0' && body[pos] <= '9') {
        if (value > 100000000000000000LL) {
            return false;
        }
        value = value * 10 + (body[pos] - '0');
        ++pos;
    }
    *out = value;
    return true;
}

/// @brief UTF-8 字符数（只数非续字节；非法字节按 1 个字符计）。
std::size_t Utf8Length(std::string_view text) {
    std::size_t count = 0;
    for (const unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

/// @brief 一条私信消息的 JSON。
std::string MessageJson(std::int64_t id, std::int64_t conversation_id,
                        std::int64_t sender_id, const std::string& content,
                        std::int64_t created_at, std::int64_t read_at) {
    return "{\"id\":" + std::to_string(id) +
           ",\"conversation_id\":" + std::to_string(conversation_id) +
           ",\"sender_id\":" + std::to_string(sender_id) +
           ",\"content\":\"" + douyin::JsonEscape(content) + "\"" +
           ",\"created_at\":" + std::to_string(created_at) +
           ",\"read_at\":" + std::to_string(read_at) + "}";
}

/// @brief 会话对象（列表页用）。
struct ConversationItem {
    std::int64_t id{0};
    std::int64_t peer_id{0};
    std::string peer_name;
    std::int64_t last_message_id{0};
    std::int64_t last_sender_id{0};
    std::string last_content;
    std::int64_t updated_at{0};
    std::int64_t unread{0};
};

/// @brief 一条消息记录。
struct MessageRow {
    std::int64_t id{0};
    std::int64_t conversation_id{0};
    std::int64_t sender_id{0};
    std::string content;
    std::int64_t created_at{0};
    std::int64_t read_at{0};
};

/// @brief 私信存储访问器：一个 SQLite 连接，全部 SQL 参数化。
class DmStore {
public:
    DmStore() = default;
    ~DmStore() = default;
    DmStore(const DmStore&) = delete;
    DmStore& operator=(const DmStore&) = delete;

    /**
     * @brief 打开数据库并建表。
     * @param path [in] SQLite 文件路径（与其它服务共用）；不可为空。
     * @param error [out] 失败原因；成功不变。
     * @return true=就绪；false=打开/建表失败。
     */
    bool Open(const std::string& path, std::string* error) {
        auto opened = go2cpp::storage::DB::Open(path);
        if (!opened) {
            if (error != nullptr) {
                *error = "打开数据库失败";
            }
            return false;
        }
        m_db = std::move(opened).Value();
        if (auto busy = m_db->SetBusyTimeout(5000); busy) {
            if (error != nullptr) {
                *error = "设置 busy_timeout 失败";
            }
            return false;
        }
        for (int attempt = 0; attempt < 40; ++attempt) {
            auto wal = m_db->SetJournalMode("WAL");
            if (!wal) {
                return EnsureSchema(error);
            }
            if (attempt == 39) {
                if (error != nullptr) {
                    *error = "设置 WAL 失败";
                }
                return false;
            }
        }
        return false;
    }

    /// @brief 幂等建表。
    bool EnsureSchema(std::string* error) {
        const std::string sql =
            "CREATE TABLE IF NOT EXISTS dm_conversations ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  user_a INTEGER NOT NULL,"
            "  user_b INTEGER NOT NULL,"
            "  last_message_id INTEGER NOT NULL DEFAULT 0,"
            "  updated_at INTEGER NOT NULL,"
            "  UNIQUE(user_a, user_b)"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_dm_conversations_a "
            "ON dm_conversations(user_a, updated_at DESC);"
            "CREATE INDEX IF NOT EXISTS idx_dm_conversations_b "
            "ON dm_conversations(user_b, updated_at DESC);"
            "CREATE TABLE IF NOT EXISTS dm_messages ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  conversation_id INTEGER NOT NULL,"
            "  sender_id INTEGER NOT NULL,"
            "  content TEXT NOT NULL,"
            "  created_at INTEGER NOT NULL,"
            "  read_at INTEGER NOT NULL DEFAULT 0"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_dm_messages_conv "
            "ON dm_messages(conversation_id, id DESC);";
        auto executed = m_db->Exec(sql);
        if (!executed) {
            if (error != nullptr) {
                *error = "建表失败";
            }
            return false;
        }
        return true;
    }

    bool Ok() const noexcept { return m_db != nullptr && m_db->IsOpen(); }

    /// @brief 查用户昵称；不存在返回 false（error 为空）。
    bool FindUserName(std::int64_t user_id, std::string* name, std::string* error) {
        auto rows = m_db->Query("SELECT name FROM users WHERE id = ?", user_id);
        if (!rows) {
            if (error != nullptr) {
                *error = "查询用户失败";
            }
            return false;
        }
        if (!rows.Value().Next()) {
            return false;
        }
        *name = rows.Value().Text(0);
        return true;
    }

    /// @brief 双向拉黑判定：任一方拉黑另一方即 true。
    bool IsBlocked(std::int64_t a, std::int64_t b, bool* out, std::string* error) {
        auto rows = m_db->Query(
            "SELECT 1 FROM blocks WHERE (user_id = ? AND blocked_id = ?) "
            "OR (user_id = ? AND blocked_id = ?) LIMIT 1",
            a, b, b, a);
        if (!rows) {
            if (error != nullptr) {
                *error = "查询拉黑失败";
            }
            return false;
        }
        *out = rows.Value().Next();
        return true;
    }

    /// @brief 台账内查找会话 id；不存在返回 0。
    std::int64_t FindConversation(std::int64_t a, std::int64_t b,
                                  std::string* error) {
        auto rows = m_db->Query(
            "SELECT id FROM dm_conversations WHERE user_a = ? AND user_b = ?", a, b);
        if (!rows) {
            if (error != nullptr) {
                *error = "查询会话失败";
            }
            return -1;
        }
        if (!rows.Value().Next()) {
            return 0;
        }
        return rows.Value().Int64(0);
    }

    /// @brief 读会话成员；不存在返回 false（error 为空）。
    bool FindConversationMembers(std::int64_t conversation_id, std::int64_t* user_a,
                                 std::int64_t* user_b, std::string* error) {
        auto rows = m_db->Query(
            "SELECT user_a, user_b FROM dm_conversations WHERE id = ?",
            conversation_id);
        if (!rows) {
            if (error != nullptr) {
                *error = "查询会话失败";
            }
            return false;
        }
        if (!rows.Value().Next()) {
            return false;
        }
        *user_a = rows.Value().Int64(0);
        *user_b = rows.Value().Int64(1);
        return true;
    }

    /**
     * @brief 找到或创建私信会话，并写入一条消息（单事务）。
     *
     * @param [in] sender_id 发送者；必须 > 0。
     * @param [in] receiver_id 接收者；必须 > 0 且 != sender_id。
     * @param [in] content 正文；调用方已校验 1~500 字。
     * @param [in] now Unix 秒。
     * @param [out] conversation_id 成功时写入会话 id。
     * @param [out] message 成功时写入刚插入的消息（含自增 id）。
     * @param [out] error 失败原因。
     * @return true=已提交；false=失败（事务已回滚，库内无半成品）。
     */
    bool Send(std::int64_t sender_id, std::int64_t receiver_id,
              const std::string& content, std::int64_t now,
              std::int64_t* conversation_id, MessageRow* message,
              std::string* error) {
        const std::int64_t user_a = std::min(sender_id, receiver_id);
        const std::int64_t user_b = std::max(sender_id, receiver_id);
        if (auto began = m_db->Begin(); began) {
            if (error != nullptr) {
                *error = "开启事务失败";
            }
            return false;
        }
        bool ok = false;
        do {
            auto inserted = m_db->Exec(
                "INSERT OR IGNORE INTO dm_conversations"
                "(user_a, user_b, last_message_id, updated_at) VALUES(?, ?, 0, ?)",
                user_a, user_b, now);
            if (!inserted) {
                if (error != nullptr) {
                    *error = "创建会话失败";
                }
                break;
            }
            auto found = m_db->Query(
                "SELECT id FROM dm_conversations WHERE user_a = ? AND user_b = ?",
                user_a, user_b);
            if (!found) {
                if (error != nullptr) {
                    *error = "查询会话失败";
                }
                break;
            }
            std::int64_t cid = 0;
            if (found.Value().Next()) {
                cid = found.Value().Int64(0);
            }
            if (cid <= 0) {
                if (error != nullptr) {
                    *error = "会话 id 缺失";
                }
                break;
            }
            auto message_inserted = m_db->Exec(
                "INSERT INTO dm_messages(conversation_id, sender_id, content,"
                " created_at, read_at) VALUES(?, ?, ?, ?, 0)",
                cid, sender_id, content, now);
            if (!message_inserted) {
                if (error != nullptr) {
                    *error = "写入消息失败";
                }
                break;
            }
            const std::int64_t mid = message_inserted.Value().last_insert_row_id;
            if (mid <= 0) {
                if (error != nullptr) {
                    *error = "消息 id 缺失";
                }
                break;
            }
            auto updated = m_db->Exec(
                "UPDATE dm_conversations SET last_message_id = ?, updated_at = ?"
                " WHERE id = ?",
                mid, now, cid);
            if (!updated) {
                if (error != nullptr) {
                    *error = "更新会话失败";
                }
                break;
            }
            // 通知只做摘要，正文截断到 80 字（按 UTF-8 字符边界）。
            std::string summary;
            std::size_t chars = 0;
            for (const char c : content) {
                if (((static_cast<unsigned char>(c)) & 0xC0) != 0x80) {
                    if (chars >= 80) {
                        break;
                    }
                    ++chars;
                }
                summary += c;
            }
            auto notified = m_db->Exec(
                "INSERT INTO notifications(user_id, actor_id, kind, target_type,"
                " target_id, content, created_at, read_at)"
                " VALUES(?, ?, 'dm', 'conversation', ?, ?, ?, 0)",
                receiver_id, sender_id, cid, summary, now);
            if (!notified) {
                if (error != nullptr) {
                    *error = "写入通知失败";
                }
                break;
            }
            if (auto committed = m_db->Commit(); committed) {
                if (error != nullptr) {
                    *error = "提交事务失败";
                }
                break;
            }
            *conversation_id = cid;
            message->id = mid;
            message->conversation_id = cid;
            message->sender_id = sender_id;
            message->content = content;
            message->created_at = now;
            message->read_at = 0;
            ok = true;
        } while (false);
        if (!ok) {
            (void)m_db->Rollback();
        }
        return ok;
    }

    /// @brief 会话列表：updated_at DESC, id DESC + 游标。
    bool ListConversations(std::int64_t me, std::int64_t cursor_updated,
                           std::int64_t cursor_id, bool has_cursor,
                           std::int64_t limit, std::vector<ConversationItem>* out,
                           std::string* error) {
        std::string sql =
            "SELECT c.id,"
            " CASE WHEN c.user_a = ? THEN c.user_b ELSE c.user_a END AS peer,"
            " COALESCE(u.name, ''), c.last_message_id,"
            " COALESCE(m.sender_id, 0), COALESCE(m.content, ''),"
            " c.updated_at,"
            " (SELECT COUNT(*) FROM dm_messages x WHERE x.conversation_id = c.id"
            "  AND x.sender_id <> ? AND x.read_at = 0)"
            " FROM dm_conversations c"
            " LEFT JOIN users u ON u.id = CASE WHEN c.user_a = ? THEN c.user_b"
            " ELSE c.user_a END"
            " LEFT JOIN dm_messages m ON m.id = c.last_message_id"
            " WHERE (c.user_a = ? OR c.user_b = ?)";
        if (has_cursor) {
            sql += " AND (c.updated_at < ? OR (c.updated_at = ? AND c.id < ?))";
        }
        sql += " ORDER BY c.updated_at DESC, c.id DESC LIMIT ?";
        auto rows = [&]() {
            if (has_cursor) {
                return m_db->Query(sql, me, me, me, me, me, cursor_updated,
                                   cursor_updated, cursor_id, limit + 1);
            }
            return m_db->Query(sql, me, me, me, me, me, limit + 1);
        }();
        if (!rows) {
            if (error != nullptr) {
                *error = "查询会话列表失败";
            }
            return false;
        }
        out->clear();
        while (rows.Value().Next()) {
            ConversationItem item;
            item.id = rows.Value().Int64(0);
            item.peer_id = rows.Value().Int64(1);
            item.peer_name = rows.Value().Text(2);
            item.last_message_id = rows.Value().Int64(3);
            item.last_sender_id = rows.Value().Int64(4);
            item.last_content = rows.Value().Text(5);
            item.updated_at = rows.Value().Int64(6);
            item.unread = rows.Value().Int64(7);
            out->push_back(std::move(item));
        }
        return true;
    }

    /// @brief 增量拉取消息（id > after，升序）。
    bool ListMessages(std::int64_t conversation_id, std::int64_t after,
                      std::int64_t limit, std::vector<MessageRow>* out,
                      std::string* error) {
        auto rows = m_db->Query(
            "SELECT id, conversation_id, sender_id, content, created_at, read_at"
            " FROM dm_messages WHERE conversation_id = ? AND id > ?"
            " ORDER BY id ASC LIMIT ?",
            conversation_id, after, limit);
        if (!rows) {
            if (error != nullptr) {
                *error = "查询消息失败";
            }
            return false;
        }
        out->clear();
        while (rows.Value().Next()) {
            MessageRow item;
            item.id = rows.Value().Int64(0);
            item.conversation_id = rows.Value().Int64(1);
            item.sender_id = rows.Value().Int64(2);
            item.content = rows.Value().Text(3);
            item.created_at = rows.Value().Int64(4);
            item.read_at = rows.Value().Int64(5);
            out->push_back(std::move(item));
        }
        return true;
    }

    /// @brief 标记「别人发给我的」未读消息为已读；返回影响行数，-1=错误。
    std::int64_t MarkRead(std::int64_t conversation_id, std::int64_t me,
                          std::int64_t now) {
        auto updated = m_db->Exec(
            "UPDATE dm_messages SET read_at = ? WHERE conversation_id = ?"
            " AND sender_id <> ? AND read_at = 0",
            now, conversation_id, me);
        if (!updated) {
            return -1;
        }
        return updated.Value().changes;
    }

private:
    std::unique_ptr<go2cpp::storage::DB> m_db;
};

/// @brief 路径 `/api/dm/<id>/(messages|read)` 的解析结果。
enum class DmAction {
    kNone,
    kMessages,
    kRead,
};

bool ParseDmPath(const std::string& path, std::int64_t* id, DmAction* action) {
    constexpr std::string_view kBase = "/api/dm/";
    if (path.size() <= kBase.size() ||
        path.compare(0, kBase.size(), kBase.data(), kBase.size()) != 0) {
        return false;
    }
    std::string_view rest(path.data() + kBase.size(), path.size() - kBase.size());
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }
    const std::string_view digits = rest.substr(0, slash);
    if (digits.empty() || digits.size() > 18) {
        return false;
    }
    std::int64_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    if (value <= 0) {
        return false;
    }
    const std::string_view tail = rest.substr(slash + 1);
    if (tail == "messages") {
        *action = DmAction::kMessages;
    } else if (tail == "read") {
        *action = DmAction::kRead;
    } else {
        return false;
    }
    *id = value;
    return true;
}

/// @brief POST /api/dm/send
bool HandleSend(DmStore& store, const douyin::HttpRequest& request,
                douyin::HttpResponse* response) {
    std::int64_t me = 0;
    if (!RequireToken(request, response, &me)) {
        return true;
    }
    std::int64_t to_user_id = 0;
    if (!JsonIntField(request.body, "to_user_id", &to_user_id) || to_user_id <= 0) {
        return Fail(response, 400, "to_user_id 非法");
    }
    std::string content;
    if (!douyin::JsonStringField(request.body, "content", &content)) {
        return Fail(response, 400, "content 缺失");
    }
    if (content.size() > kMaxContentBytes ||
        Utf8Length(content) < 1 || Utf8Length(content) > kMaxContentChars) {
        return Fail(response, 400, "content 必须是 1~500 字");
    }
    if (to_user_id == me) {
        return Fail(response, 400, "不能给自己发私信");
    }
    std::string error;
    std::string peer_name;
    if (!store.FindUserName(to_user_id, &peer_name, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "用户不存在");
    }
    bool blocked = false;
    if (!store.IsBlocked(me, to_user_id, &blocked, &error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (blocked) {
        return Fail(response, 403, "对方已拉黑你，无法发送私信");
    }
    std::int64_t conversation_id = 0;
    MessageRow message;
    if (!store.Send(me, to_user_id, content, NowSeconds(), &conversation_id,
                    &message, &error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "发送失败");
    }
    response->status = 200;
    response->body =
        "{\"ok\":true,\"conversation_id\":" + std::to_string(conversation_id) +
        ",\"message\":" +
        MessageJson(message.id, conversation_id, message.sender_id,
                    message.content, message.created_at, message.read_at) +
        "}";
    return true;
}

/// @brief GET /api/dm/conversations
bool HandleConversations(DmStore& store, const douyin::HttpRequest& request,
                         douyin::HttpResponse* response) {
    std::int64_t me = 0;
    if (!RequireToken(request, response, &me)) {
        return true;
    }
    std::int64_t limit = 20;
    if (!ReadQueryInt(request, "limit", 20, &limit) || limit < 1 || limit > 50) {
        return Fail(response, 400, "limit 必须是 1~50");
    }
    std::int64_t cursor_updated = 0;
    std::int64_t cursor_id = 0;
    bool has_cursor = false;
    const std::string cursor = douyin::QueryParam(request, "cursor");
    if (!cursor.empty()) {
        const std::size_t dash = cursor.find('-');
        if (dash == std::string::npos || dash == 0 ||
            dash + 1 >= cursor.size()) {
            return Fail(response, 400, "cursor 非法");
        }
        for (std::size_t i = 0; i < dash; ++i) {
            if (cursor[i] < '0' || cursor[i] > '9') {
                return Fail(response, 400, "cursor 非法");
            }
            cursor_updated = cursor_updated * 10 + (cursor[i] - '0');
        }
        for (std::size_t i = dash + 1; i < cursor.size(); ++i) {
            if (cursor[i] < '0' || cursor[i] > '9') {
                return Fail(response, 400, "cursor 非法");
            }
            cursor_id = cursor_id * 10 + (cursor[i] - '0');
        }
        has_cursor = true;
    }
    std::vector<ConversationItem> items;
    std::string error;
    if (!store.ListConversations(me, cursor_updated, cursor_id, has_cursor,
                                 limit, &items, &error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    const bool has_more = static_cast<std::int64_t>(items.size()) > limit;
    if (has_more) {
        items.resize(static_cast<std::size_t>(limit));
    }
    std::string body = "{\"ok\":true,\"items\":[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"id\":" + std::to_string(items[i].id) +
                ",\"peer_id\":" + std::to_string(items[i].peer_id) +
                ",\"peer_name\":\"" + douyin::JsonEscape(items[i].peer_name) +
                "\"" +
                ",\"last_message_id\":" +
                std::to_string(items[i].last_message_id) +
                ",\"last_sender_id\":" +
                std::to_string(items[i].last_sender_id) +
                ",\"last_content\":\"" +
                douyin::JsonEscape(items[i].last_content) + "\"" +
                ",\"updated_at\":" + std::to_string(items[i].updated_at) +
                ",\"unread\":" + std::to_string(items[i].unread) + "}";
    }
    body += "],\"next_cursor\":\"";
    if (has_more && !items.empty()) {
        body += std::to_string(items.back().updated_at) + "-" +
                std::to_string(items.back().id);
    }
    body += "\"}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/// @brief GET /api/dm/:id/messages
bool HandleMessages(DmStore& store, const douyin::HttpRequest& request,
                    std::int64_t conversation_id, douyin::HttpResponse* response) {
    std::int64_t me = 0;
    if (!RequireToken(request, response, &me)) {
        return true;
    }
    std::int64_t after = 0;
    if (!ReadQueryInt(request, "after", 0, &after) || after < 0) {
        return Fail(response, 400, "after 非法");
    }
    std::int64_t limit = 50;
    if (!ReadQueryInt(request, "limit", 50, &limit) || limit < 1 || limit > 100) {
        return Fail(response, 400, "limit 必须是 1~100");
    }
    std::string error;
    std::int64_t user_a = 0;
    std::int64_t user_b = 0;
    if (!store.FindConversationMembers(conversation_id, &user_a, &user_b, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "会话不存在");
    }
    if (me != user_a && me != user_b) {
        return Fail(response, 403, "不是该会话成员");
    }
    std::vector<MessageRow> messages;
    if (!store.ListMessages(conversation_id, after, limit, &messages, &error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    std::string body = "{\"ok\":true,\"items\":[";
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += MessageJson(messages[i].id, messages[i].conversation_id,
                            messages[i].sender_id, messages[i].content,
                            messages[i].created_at, messages[i].read_at);
    }
    body += "],\"last_id\":";
    body += messages.empty() ? "0" : std::to_string(messages.back().id);
    body += "}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/// @brief POST /api/dm/:id/read
bool HandleRead(DmStore& store, const douyin::HttpRequest& request,
                std::int64_t conversation_id, douyin::HttpResponse* response) {
    std::int64_t me = 0;
    if (!RequireToken(request, response, &me)) {
        return true;
    }
    std::string error;
    std::int64_t user_a = 0;
    std::int64_t user_b = 0;
    if (!store.FindConversationMembers(conversation_id, &user_a, &user_b, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "会话不存在");
    }
    if (me != user_a && me != user_b) {
        return Fail(response, 403, "不是该会话成员");
    }
    const std::int64_t updated = store.MarkRead(conversation_id, me, NowSeconds());
    if (updated < 0) {
        return Fail(response, 500, "数据库错误");
    }
    response->status = 200;
    response->body = "{\"ok\":true,\"updated\":" + std::to_string(updated) + "}";
    return true;
}

/// @brief 总路由。
bool Handle(DmStore& store, const douyin::HttpRequest& request,
            douyin::HttpResponse* response) {
    ++g_metrics.requests;
    if (request.method == "GET" && request.path == "/health") {
        response->status = 200;
        response->body = std::string("{\"ok\":true,\"service\":\"") + kServiceName + "\"}";
        return true;
    }
    if (request.method == "GET" && request.path == "/metrics") {
        response->status = 200;
        response->body = std::string("{\"ok\":true,\"service\":\"") + kServiceName +
                         "\",\"requests\":" + std::to_string(g_metrics.requests) +
                         ",\"errors\":" + std::to_string(g_metrics.errors) + "}";
        return true;
    }
    if (request.path == "/api/dm/send") {
        if (request.method == "POST") {
            return HandleSend(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (request.path == "/api/dm/conversations") {
        if (request.method == "GET") {
            return HandleConversations(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    std::int64_t id = 0;
    DmAction action = DmAction::kNone;
    if (!ParseDmPath(request.path, &id, &action)) {
        return false;
    }
    if (action == DmAction::kMessages) {
        if (request.method == "GET") {
            return HandleMessages(store, request, id, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (action == DmAction::kRead) {
        if (request.method == "POST") {
            return HandleRead(store, request, id, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = kDefaultPort;
    if (argc > 1) {
        port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    }
    std::string db_path = kDefaultDbPath;
    if (const char* env = std::getenv("DOUYIN_DB"); env != nullptr && *env != '\0') {
        db_path = env;
    }
    {
        const std::filesystem::path parent = std::filesystem::path(db_path).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
    }
    DmStore store;
    std::string error;
    if (!store.Open(db_path, &error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return 1;
    }
    douyin::HttpServer server(kServiceName, port);
    if (!server.Start(&error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return 1;
    }
    server.SetReadinessProbe([&store](std::string* detail) {
        if (!store.Ok()) {
            if (detail != nullptr) {
                *detail = "db not open";
            }
            return false;
        }
        return true;
    });
    std::printf("[%s] 监听 0.0.0.0:%u db=%s\n", kServiceName,
                static_cast<unsigned>(port), db_path.c_str());
    std::fflush(stdout);
    server.ServeForever([&store](const douyin::HttpRequest& request,
                                 douyin::HttpResponse* response) {
        return Handle(store, request, response);
    });
    return 0;
}
