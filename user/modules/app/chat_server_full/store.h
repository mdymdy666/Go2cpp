#pragma once

/**
 * @file store.h
 * @brief 消息持久化：追加写文本文件，按行回放最近消息。
 * @details 格式是 `epoch_ms|room|from|text`，每行一条。用户输入里的 `|` 与
 *          换行会被替换，避免破坏分隔；这是示例的取舍——真实系统应使用
 *          长度前缀或 JSON Lines + 严格转义。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace chat {

/// @brief 一条持久化消息。
struct StoredMessage {
    std::uint64_t epoch_ms{0};
    std::string room;
    std::string from;
    std::string text;
};

/// @brief 线程安全的消息存储；每次 Append 打开/关闭文件，崩溃最多丢最后一行。
class MessageStore final {
   public:
    explicit MessageStore(std::string path);

    /// @brief 追加一条消息；IO 失败返回 false 并写错误（调用方只记录，不阻塞聊天）。
    bool Append(const StoredMessage& message, std::string* error);

    /// @brief 读取文件最后 max_lines 条消息；文件不存在返回空。
    std::vector<StoredMessage> Recent(std::size_t max_lines) const;

    const std::string& path() const { return m_path; }

   private:
    mutable std::mutex m_mutex;  ///< 串行化同进程内的追加与读取。
    std::string m_path;          ///< 持久化文件路径。
};

}  // namespace chat
