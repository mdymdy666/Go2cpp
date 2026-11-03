/**
 * @file bili_store.hpp
 * @brief B 站弹幕存储：SQLite（WAL）上的 danmaku 单表访问器。
 *
 * @details 只包含 FR-03 需要的 danmaku 表，不依赖用户/视频服务。所有方法
 *          复用 `go2cpp::storage::DB`（内部持锁，可跨线程调用）。
 *
 * @author bili demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace go2cpp {
namespace storage {
class DB;
}  // namespace storage
}  // namespace go2cpp

namespace bili {

/// @brief danmaku 表的一行（字段与 REQUIREMENTS.md §4 一致）。
struct DanmakuRow {
    std::int64_t id{0};                      ///< 自增主键。
    std::int64_t video_id{0};                ///< 所属视频。
    std::int64_t user_id{0};                 ///< 发送者；0=游客。
    std::string text;                        ///< 弹幕正文（UTF-8）。
    std::int64_t time_ms{0};                 ///< 弹幕出现时间（毫秒）。
    std::int64_t color{16777215};            ///< RGB 颜色，默认白色。
    std::int64_t created_at{0};              ///< 服务端入库时间（Unix 秒）。
};

/// @brief SQLite 连接 + danmaku 建表 + 三个访问器。
/// @note 同一个实例可被多个连接线程并发调用；`DB` 内部已串行化。
class DanmakuStore {
   public:
    /// @brief 默认数据库路径（相对 app/bilibili/ 工作目录）。
    static constexpr const char* kDefaultPath = "data/bili.db";

    DanmakuStore();
    ~DanmakuStore();
    DanmakuStore(const DanmakuStore&) = delete;
    DanmakuStore& operator=(const DanmakuStore&) = delete;

    /// @brief 打开数据库并设置 WAL / busy_timeout；父目录需已存在。
    bool Open(const std::string& path, std::string* error);

    /// @brief 建 danmaku 表与 `(video_id, time_ms)` 索引，幂等。
    bool EnsureSchema(std::string* error);

    /// @brief 连接是否已打开。
    bool Ok() const noexcept;

    /**
     * @brief 插入一条弹幕。
     * @param row 输入字段（id/created_at 由本方法填充）。
     * @param out 成功时填好 id 与 created_at（可为空）。
     * @param error 失败时写入错误文本。
     */
    bool InsertDanmaku(const DanmakuRow& row, DanmakuRow* out, std::string* error);

    /**
     * @brief 按视频与时间片拉取历史弹幕（time_ms 升序）。
     * @param video_id 视频 id。
     * @param from_ms 起始时间（含）；<0 视为 0。
     * @param to_ms 结束时间（含）；<0 视为无上限。
     * @param limit 最多返回条数；0 用默认 1000，>5000 截到 5000。
     */
    bool ListDanmaku(std::int64_t video_id, std::int64_t from_ms,
                     std::int64_t to_ms, std::size_t limit,
                     std::vector<DanmakuRow>* out, std::string* error);

    /// @brief 某视频的弹幕总数；出错返回 -1 并写 error。
    std::int64_t CountDanmaku(std::int64_t video_id, std::string* error);

   private:
    std::unique_ptr<go2cpp::storage::DB> m_db;
};

}  // namespace bili
