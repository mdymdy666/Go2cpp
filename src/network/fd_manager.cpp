/**
 * @file fd_manager.cpp
 * @brief 实现 fd 元数据的线程安全注册、删除和 dup 状态复制。
 * @details 元数据表不拥有 fd，Socket/Hook 负责真实 close；删除动作必须先于
 *          数字 fd 重新分配，避免旧超时和非阻塞状态泄漏。
 */
#include "go2cpp/network/fd_manager.hpp"

namespace go2cpp::net {

FdManager& FdManager::instance() noexcept {
    static FdManager manager;
    return manager;
}

FdManager::ContextPtr FdManager::get(int fd, bool auto_create) {
    if (fd < 0) return {};
    std::lock_guard<std::mutex> lock(m_mutex);
    auto found = m_contexts.find(fd);
    if (found != m_contexts.end()) return found->second;
    if (!auto_create) return {};
    auto result = std::make_shared<FdContext>(fd);
    result->m_initialized = true;
    m_contexts.emplace(fd, result);
    return result;
}

void FdManager::erase(int fd) noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto found = m_contexts.find(fd);
    if (found != m_contexts.end()) {
        found->second->m_closed = true;
        m_contexts.erase(found);
    }
}

FdManager::ContextPtr FdManager::duplicate(int old_fd, int new_fd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto old = m_contexts.find(old_fd);
    if (old == m_contexts.end()) {
        auto fresh = std::make_shared<FdContext>(new_fd);
        fresh->m_initialized = true;
        m_contexts[new_fd] = fresh;
        return fresh;
    }
    auto result = std::make_shared<FdContext>(new_fd);
    *result = *old->second;
    result->m_fd = new_fd;
    result->m_closed = false;
    m_contexts[new_fd] = result;
    return result;
}

bool FdManager::set_nonblock(int fd, bool user, bool system) {
    auto context = get(fd, true);
    if (!context) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    context->m_user_nonblock = user;
    context->m_system_nonblock = system;
    return true;
}

bool FdManager::set_timeout(int fd, bool receive, std::chrono::milliseconds timeout) {
    auto context = get(fd, true);
    if (!context || timeout.count() < 0) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (receive) context->m_recv_timeout = timeout;
    else context->m_send_timeout = timeout;
    return true;
}

}  // namespace go2cpp::net
