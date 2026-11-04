/**
 * @file idempotency.hpp
 * @brief 写接口幂等键中间件：用 SQLite 去重表把「同 key 重发」折叠成一次执行。
 *
 * @details 抖音的评论 / 点赞 / 上传 / 下单在切网、断线重连后可能被客户端重发。
 *          这些接口本身不幂等（重发就再插一条评论），所以由本中间件在路由层
 *          兜底：
 *
 *          1. 只处理写方法（POST/PUT/PATCH/DELETE）；GET/HEAD 直接放行；
 *          2. 请求头 `Idempotency-Key` 缺失 → 不做幂等，放行并记 WARN
 *             （老客户端没有该头，不能因为缺头就拒绝服务）；
 *          3. key 已存在且同一 method+path → 直接回放第一次的状态码与响应体，
 *             并加 `Idempotency-Replayed: true` 头；handler 不再执行；
 *          4. key 不存在 → 执行 handler；只有 2xx 响应才写入去重表
 *             （4xx/5xx 不缓存：业务失败允许客户端换个 key 重试，且没有副作用）；
 *          5. 去重表 `idempotency_keys` 与抖音业务表同库（`data/douyin.db`），
 *             TTL 默认 24 h；读取时顺手删除过期行（懒删除）。
 *
 * ## 表结构（表前缀 `idempotency_`）
 *
 * ```sql
 * CREATE TABLE idempotency_keys (
 *   key TEXT PRIMARY KEY,      -- 客户端提供的 Idempotency-Key
 *   method TEXT NOT NULL,      -- 首次请求的方法（用于冲突检测）
 *   path TEXT NOT NULL,        -- 首次请求的路径（不含 query）
 *   status INTEGER NOT NULL,   -- 首次响应的状态码
 *   response_body TEXT NOT NULL, -- 首次响应的 body（原样回放）
 *   created_at INTEGER NOT NULL  -- 首次写入的 Unix 秒（TTL 依据）
 * );
 * ```
 *
 * ## 已知边界（写清楚，避免误用）
 *
 * - 同一 key 但换了请求体：返回第一次的结果（表里不存 body 指纹）。客户端必须
 *   对「同一次业务操作」复用同一个 key；
 * - 同一 key 换了 method/path：回 409，避免把评论的响应回放给点赞；
 * - handler 成功、写表失败：重发会再执行一次（尽力而为）。写表失败只记 WARN，
 *   不把已成功的业务改成 500；
 * - 本中间件依赖「单进程串行处理」的骨架服务器；多 worker 下仍靠 SQLite
 *   主键约束去重，但两个 worker 可能同时看不到行而各执行一次（需要事务级
 *   占位表才能彻底解决）。
 *
 * @author douyin demo
 * @date 2026-11-04
 * @copyright MIT License
 */

#pragma once

#include "douyin_http.hpp"
#include "go2cpp/storage/db.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

namespace douyin {

/// @brief 幂等中间件的可调参数。
struct IdempotencyOptions {
    /// @brief 去重记录的存活秒数；<=0 视为非法（构造时夹到 1）。
    std::int64_t ttl_seconds{24 * 60 * 60};
    /// @brief 允许缓存的响应体上限；超过时仍执行 handler 但不写表。
    std::size_t max_response_bytes{4 * 1024 * 1024};
    /// @brief 缺少 Idempotency-Key 时逐条打 WARN 的条数上限；之后限流。
    std::uint64_t missing_key_warn_limit{100};
};

/// @brief 是否是需要幂等保护的写方法。
inline bool IsWriteMethod(std::string_view method) {
    return method == "POST" || method == "PUT" || method == "PATCH" ||
           method == "DELETE";
}

/**
 * @brief 校验 Idempotency-Key 的字符集与长度。
 *
 * @param key [in] 请求头原值（已由 HttpRequest::Header 去掉首尾空白）；
 *            长度 1~255，字符必须是可见 ASCII（0x21~0x7E）。
 * @return true 表示可以安全写日志与 SQL；false 时调用方**必须**回 400，
 *         不能静默忽略（否则客户端以为受保护、实际每次重发都执行）。
 *
 * ## 定位
 * key 会进 SQLite 参数绑定与日志，只接受可见 ASCII 可以杜绝换行注入日志、
 * 控制字符污染终端。错了会让非法 key 静默降级成「无保护」。
 */
inline bool IsValidIdempotencyKey(std::string_view key) {
    if (key.empty() || key.size() > 255) {
        return false;
    }
    for (const char raw : key) {
        const auto c = static_cast<unsigned char>(raw);
        if (c < 0x21U || c > 0x7EU) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 写接口幂等键中间件（SQLite 去重表 + 响应回放）。
 *
 * @note 生命周期：内部只借用 `go2cpp::storage::DB*`，不拥有连接；调用方必须
 *       保证 DB 在中间件之前打开、在中间件之后销毁。
 */
class IdempotencyMiddleware {
   public:
    /**
     * @brief 构造中间件。
     * @param db [in] 借用抖音业务库连接，可为 nullptr（此时所有写请求降级直通）；
     *           所有权仍在 Store，本对象不 Close。
     * @param options [in] TTL / 缓存上限 / WARN 限流；ttl_seconds<=0 时夹到 1。
     */
    explicit IdempotencyMiddleware(go2cpp::storage::DB* db,
                                   IdempotencyOptions options = {})
        : m_db(db), m_options(options) {
        if (m_options.ttl_seconds <= 0) {
            m_options.ttl_seconds = 1;
        }
    }

    /**
     * @brief 建去重表与 TTL 索引（幂等）。
     * @param error [out] 失败时写入错误文本；成功时保持原值。
     * @return 成功返回 true；调用方**必须**在失败时终止启动（否则表不存在，
     *         每个带 key 的请求都会降级成无保护执行）。
     */
    bool EnsureSchema(std::string* error) {
        if (m_db == nullptr || !m_db->IsOpen()) {
            if (error != nullptr) {
                *error = "幂等表初始化失败：数据库未打开";
            }
            return false;
        }
        static constexpr const char* kSchemaSql =
            "CREATE TABLE IF NOT EXISTS idempotency_keys ("
            "  key TEXT PRIMARY KEY,"
            "  method TEXT NOT NULL,"
            "  path TEXT NOT NULL,"
            "  status INTEGER NOT NULL,"
            "  response_body TEXT NOT NULL,"
            "  created_at INTEGER NOT NULL"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_idempotency_created_at "
            "ON idempotency_keys(created_at);";
        auto executed = m_db->Exec(kSchemaSql);
        if (!executed) {
            if (error != nullptr) {
                *error = "建幂等表失败: " + executed.Err().ToString();
            }
            return false;
        }
        return true;
    }

    /**
     * @brief 包住业务 handler：命中缓存则回放，否则执行并记录。
     *
     * @param request [in] 已解析的请求；只读，生命周期覆盖本函数。
     * @param response [out] 输出响应——成功回放时写入缓存的状态码/body 并加
     *        `Idempotency-Replayed: true`；执行 handler 时由 handler 填充。
     *        失败（返回 false）时内容未定义，由 HttpServer 覆盖成 404。
     * @param handler [in] 业务路由；返回 false 表示没有匹配路由。
     *
     * @return
     * - true：响应有效（可能来自缓存，也可能是本次执行）——调用方**必须**原样发送；
     * - false：handler 未匹配路由——调用方**必须**按 404 处理，且本函数**没有**
     *   缓存任何内容（404 不保护）。
     *
     * ## 定位
     * 唯一的幂等决策点：读方法直通、缺 key 直通、key 命中回放、未命中执行。
     * 错了会怎样：把「同一次业务操作」执行两次（重复评论）；或把一次正常写
     * 请求错误地回放成旧响应（用户以为新评论没发出去）。
     *
     * ## 步骤
     * step1. 非写方法直接交给 handler，不查库、不占 TTL；
     * step2. 缺 `Idempotency-Key` → 记 WARN 后直通（明确「不保证幂等」）；
     * step3. key 非法（空已排除，超长/含控制字符）→ 400，避免静默降级；
     * step4. DB 不可用 → 降级直通并 WARN（可用性优先，幂等退化成不保证）；
     * step5. 懒删除过期行（`created_at <= now - ttl`）；
     * step6. 查 key：命中且未过期 → 校验 method/path → 回放缓存；
     * step7. 未命中/已过期 → 执行 handler；2xx 且 body 不超限才写表。
     *
     * ## 并发/失败语义
     * - 调用约束：HttpServer 单线程串行调用；多线程时 DB 自身有互斥量，逻辑仍
     *   安全，但同一 key 的首次并发可能各执行一次（见文件头「已知边界」）。
     * - 原子性：handler 副作用与写表**不在同一事务**，写表失败只记 WARN；
     *   因此本中间件保证「正常情况下去重」，不保证崩溃恢复级 exactly-once。
     * - 失败语义：本函数不抛异常；数据库错误一律降级直通，不阻断业务。
     * - 不变量：只有 handler 返回 true 且状态码 2xx 的响应才可能写表；
     *   `Idempotency-Replayed: true` 只出现在缓存命中路径。
     */
    bool Handle(const HttpRequest& request, HttpResponse* response,
                const HttpHandler& handler) {
        // step1: 读接口无副作用，直通不走表。
        if (!IsWriteMethod(request.method)) {
            return handler(request, response);
        }

        // step2: 缺 key 放行并记 WARN；每请求 1 条，超过上限后限流防刷屏。
        const std::string key = request.Header("idempotency-key");
        if (key.empty()) {
            WarnMissingKey(request);
            return handler(request, response);
        }

        // step3: key 会进 SQL 与日志，非法直接 400，不能静默当没有。
        if (!IsValidIdempotencyKey(key)) {
            response->status = 400;
            response->body = ErrorBody(400, "Idempotency-Key 非法（1~255 个可见 ASCII 字符）");
            return true;
        }

        // step4: 库不可用时降级直通——不能因为幂等表故障让评论功能整体不可用。
        if (m_db == nullptr || !m_db->IsOpen()) {
            std::fprintf(stderr,
                         "[idempotency] WARN 数据库不可用，key=%s 降级直通（不保证幂等）\n",
                         key.c_str());
            return handler(request, response);
        }

        const std::int64_t now = NowSeconds();
        std::string error;
        // step5: 懒删除——以本次访问为触发点清理过期行，失败不影响业务。
        if (!CleanupExpired(now, &error)) {
            std::fprintf(stderr, "[idempotency] WARN 清理过期记录失败: %s\n",
                         error.c_str());
        }

        // step6: 查缓存；查询失败同样降级直通。
        CacheRow row;
        const Lookup lookup = LookupRow(key, &row, &error);
        if (lookup == Lookup::kError) {
            std::fprintf(stderr,
                         "[idempotency] WARN 查询去重表失败（降级直通）: %s\n",
                         error.c_str());
            return handler(request, response);
        }
        if (lookup == Lookup::kHit) {
            if (row.method != request.method || row.path != request.path) {
                response->status = 409;
                response->body = ErrorBody(409, "Idempotency-Key 已用于其它请求");
                response->headers["Idempotency-Key"] = key;
                return true;
            }
            if (!IsExpired(row.created_at, now)) {
                response->status = row.status;
                response->body = std::move(row.body);
                response->headers["Idempotency-Replayed"] = "true";
                response->headers["Idempotency-Key"] = key;
                ++m_replayed;
                return true;
            }
            // 过期但清理没删掉（同一秒边界/删除失败）：按未命中处理，走重执行。
        }

        // step7: 执行 handler。用独立响应对象，避免失败路径留下半截响应。
        HttpResponse fresh;
        const bool routed = handler(request, &fresh);
        *response = std::move(fresh);
        if (!routed) {
            return false;
        }
        // step8: 只缓存成功响应；4xx/5xx 允许客户端换 key 重试。
        if (response->status >= 200 && response->status < 300 &&
            response->body.size() <= m_options.max_response_bytes) {
            if (!StoreRow(request, *response, key, &error)) {
                std::fprintf(stderr,
                             "[idempotency] WARN 写入去重表失败（本次已执行，重发会再执行）: %s\n",
                             error.c_str());
            } else {
                response->headers["Idempotency-Key"] = key;
            }
        }
        return true;
    }

    /// @brief 命中缓存并回放的次数（/metrics 可选暴露）。
    std::uint64_t ReplayedCount() const noexcept { return m_replayed; }
    /// @brief 缺少 Idempotency-Key 的写请求数。
    std::uint64_t MissingKeyCount() const noexcept { return m_missing_key; }

   private:
    /// @brief 去重表里的一行。
    struct CacheRow {
        std::string method;          ///< 首次请求的方法。
        std::string path;            ///< 首次请求的路径。
        int status{0};               ///< 首次响应的状态码。
        std::string body;            ///< 首次响应的 body。
        std::int64_t created_at{0};  ///< 首次写入的 Unix 秒。
    };

    /// @brief 查询三态：命中 / 未命中 / 数据库错误（错误要降级直通）。
    enum class Lookup { kMiss, kHit, kError };

    /// @brief 当前 Unix 秒。
    static std::int64_t NowSeconds() {
        return static_cast<std::int64_t>(std::time(nullptr));
    }

    /// @brief 是否已超过 TTL（边界按 `>=`，与清理 SQL 的 `<=` 对齐）。
    bool IsExpired(std::int64_t created_at, std::int64_t now) const {
        return now - created_at >= m_options.ttl_seconds;
    }

    /// @brief 缺 key 的 WARN：逐条记到上限，之后每 1000 条记一次。
    void WarnMissingKey(const HttpRequest& request) {
        ++m_missing_key;
        if (m_missing_key <= m_options.missing_key_warn_limit ||
            m_missing_key % 1000 == 0) {
            std::fprintf(stderr,
                         "[idempotency] WARN 缺少 Idempotency-Key：%s %s "
                         "直通执行（不保证幂等；累计 %llu 次）\n",
                         request.method.c_str(), request.path.c_str(),
                         static_cast<unsigned long long>(m_missing_key));
        }
    }

    /// @brief 按 key 查去重行。
    Lookup LookupRow(const std::string& key, CacheRow* row, std::string* error) {
        auto result = m_db->Query(
            "SELECT method, path, status, response_body, created_at "
            "FROM idempotency_keys WHERE key = ?",
            key);
        if (!result) {
            *error = result.Err().ToString();
            return Lookup::kError;
        }
        auto rows = std::move(result).Value();
        if (!rows.Next()) {
            if (const auto& err = rows.Err(); !err.ok()) {
                *error = err.ToString();
                return Lookup::kError;
            }
            return Lookup::kMiss;
        }
        row->method = rows.Text(0);
        row->path = rows.Text(1);
        row->status = static_cast<int>(rows.Int64(2));
        row->body = rows.Text(3);
        row->created_at = rows.Int64(4);
        return Lookup::kHit;
    }

    /// @brief 写一条去重记录（同 key 已存在则覆盖，处理过期后重执行）。
    bool StoreRow(const HttpRequest& request, const HttpResponse& response,
                  const std::string& key, std::string* error) {
        auto executed = m_db->Exec(
            "INSERT OR REPLACE INTO idempotency_keys"
            "(key, method, path, status, response_body, created_at) "
            "VALUES(?, ?, ?, ?, ?, ?)",
            key, request.method, request.path, response.status, response.body,
            NowSeconds());
        if (!executed) {
            *error = executed.Err().ToString();
            return false;
        }
        return true;
    }

    /// @brief 懒删除 `created_at <= now - ttl` 的行。
    bool CleanupExpired(std::int64_t now, std::string* error) {
        auto executed = m_db->Exec(
            "DELETE FROM idempotency_keys WHERE created_at <= ?",
            now - m_options.ttl_seconds);
        if (!executed) {
            *error = executed.Err().ToString();
            return false;
        }
        return true;
    }

    go2cpp::storage::DB* m_db = nullptr;  ///< 借用；不拥有。
    IdempotencyOptions m_options;         ///< 构造后不变。
    std::uint64_t m_missing_key{0};       ///< 缺 key 计数（单线程无需原子）。
    std::uint64_t m_replayed{0};          ///< 回放计数。
};

}  // namespace douyin
