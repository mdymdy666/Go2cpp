#pragma once

/**
 * @file fd_manager.hpp
 * @brief 文件描述符生命周期和超时元数据。
 * @details 该表不拥有文件描述符，只保存 hook、Socket 与 IOManager 共享的
 *          用户/系统非阻塞状态和超时。close/dup2 时必须显式删除或复制。
 */

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace go2cpp::net {

/**
 * @brief 单个 fd 的元数据。
 * @warning 这些 getter **没有任何同步保护**：`FdManager` 只在写入侧
 *          （`get()` 设置 `initialized`、`erase()` 设置 `closed`）持有内部互斥，
 *          读取侧是无锁直读。因此当同一 fd 的元数据被并发改写时，
 *          「读字段」与「改字段」构成**数据竞争**（C++ 内存模型下为未定义行为）。
 *          调用方应只在确认该 fd 没有并发写入时读取，或在更高层自行同步。
 */
class FdContext final {
public:
    explicit FdContext(int fd) noexcept : m_fd(fd) {}
    int fd() const noexcept { return m_fd; }
    bool initialized() const noexcept { return m_initialized; }
    bool closed() const noexcept { return m_closed; }
    bool user_nonblock() const noexcept { return m_user_nonblock; }
    bool system_nonblock() const noexcept { return m_system_nonblock; }
    std::chrono::milliseconds recv_timeout() const noexcept { return m_recv_timeout; }
    std::chrono::milliseconds send_timeout() const noexcept { return m_send_timeout; }

private:
    int m_fd;
    bool m_initialized{false};
    bool m_closed{false};
    bool m_user_nonblock{false};
    bool m_system_nonblock{false};
    std::chrono::milliseconds m_recv_timeout{0};
    std::chrono::milliseconds m_send_timeout{0};
    friend class FdManager;
};

/** @brief 线程安全的 fd 元数据表。 */
class FdManager final {
public:
    using ContextPtr = std::shared_ptr<FdContext>;
    static FdManager& instance() noexcept;

    /** @brief 获取 fd 元数据，auto_create 为真时首次自动创建。 */
    ContextPtr get(int fd, bool auto_create = true);
    /** @brief 删除 fd 元数据并标记旧对象关闭。 */
    void erase(int fd) noexcept;
    /** @brief 将 old_fd 的用户状态复制给 new_fd。 */
    ContextPtr duplicate(int old_fd, int new_fd);
    /** @brief 更新非阻塞和超时状态。 */
    bool set_nonblock(int fd, bool user, bool system);
    bool set_timeout(int fd, bool receive, std::chrono::milliseconds timeout);

private:
    FdManager() = default;
    std::mutex m_mutex;
    std::unordered_map<int, ContextPtr> m_contexts;
};

}  // namespace go2cpp::net