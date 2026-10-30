/**
 * @file socket_autoregister.cpp
 * @brief 观察者安装/卸载与 `SocketRegistry` 同步实现。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * 线程安全：观察者可能来自任意创建/关闭 socket 的线程；`g_auto_mutex` 只保护
 * 「自动登记 fd 集合 + 排除集合 + 安装标志」，注册表的系统调用与锁都在它之外
 * 完成，避免持锁做 `readlink/getsockopt`。
 */

#include "mcp/tools/socket_autoregister.hpp"

#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "mcp/tools/socket_registry.hpp"

namespace go2cpp::mcp::tools {

namespace {

std::mutex g_auto_mutex;
/// 由本模块自动登记、当前仍在白名单内的 fd。
std::unordered_set<int> g_auto_fds;
/// MCP 自身 fd（监听 socket/接入连接），自动登记时跳过。
std::unordered_set<int> g_excluded_fds;
bool g_installed = false;

/// 观察者入口：任何异常都不能跨过 C ABI 边界影响 socket 生命周期。
void OnSocketLifecycle(int fd, int event) noexcept {
    if (fd < 0) {
        return;
    }
    try {
        SocketRegistry& registry = SocketRegistry::Instance();
        if (event == kSocketObserverCreated) {
            {
                std::lock_guard<std::mutex> lock(g_auto_mutex);
                if (!g_installed || g_excluded_fds.count(fd) != 0) {
                    return;
                }
                g_auto_fds.insert(fd);
            }
            std::string error;
            if (!registry.Register(fd, kSocketAutoRegisterLabel, &error)) {
                // 不是 socket（如 socketpair/管道）或注册表已满：回滚记录。
                std::lock_guard<std::mutex> lock(g_auto_mutex);
                g_auto_fds.erase(fd);
            }
            return;
        }
        if (event == kSocketObserverClosed || event == kSocketObserverDetached) {
            bool owned = false;
            {
                std::lock_guard<std::mutex> lock(g_auto_mutex);
                // fd 即将关闭/复用：排除标记随号码一起失效。
                g_excluded_fds.erase(fd);
                owned = g_auto_fds.erase(fd) != 0;
            }
            if (owned) {
                // closed 写墓碑；detached（release 移交）只移出白名单。
                (void)registry.UnregisterIfSameSocket(
                    fd, event == kSocketObserverClosed);
            }
            return;
        }
    } catch (...) {
        // best-effort：观测失败不影响 socket 本身。
    }
}

}  // namespace

void InstallSocketAutoRegister() {
    std::lock_guard<std::mutex> lock(g_auto_mutex);
    g_installed = true;
    go2cpp_network_set_socket_observer(&OnSocketLifecycle);
}

void UninstallSocketAutoRegister() {
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(g_auto_mutex);
        go2cpp_network_set_socket_observer(nullptr);
        g_installed = false;
        fds.assign(g_auto_fds.begin(), g_auto_fds.end());
        g_auto_fds.clear();
        g_excluded_fds.clear();
    }
    SocketRegistry& registry = SocketRegistry::Instance();
    for (const int fd : fds) {
        (void)registry.UnregisterIfSameSocket(fd);
    }
}

void ExcludeSocketFromAutoRegister(int fd) {
    if (fd < 0) {
        return;
    }
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(g_auto_mutex);
        g_excluded_fds.insert(fd);
        owned = g_auto_fds.erase(fd) != 0;
    }
    if (owned) {
        // fd 仍然存活，只移出白名单，不写 closed 事件。
        (void)SocketRegistry::Instance().UnregisterIfSameSocket(fd, false);
    }
}

void IncludeSocketInAutoRegister(int fd) {
    std::lock_guard<std::mutex> lock(g_auto_mutex);
    g_excluded_fds.erase(fd);
}

void ForgetAutoRegisteredSocket(int fd) {
    std::lock_guard<std::mutex> lock(g_auto_mutex);
    g_auto_fds.erase(fd);
    g_excluded_fds.erase(fd);
}

std::size_t AutoRegisteredSocketCount() {
    std::lock_guard<std::mutex> lock(g_auto_mutex);
    return g_auto_fds.size();
}

bool IsSocketAutoRegistered(int fd) {
    std::lock_guard<std::mutex> lock(g_auto_mutex);
    return g_auto_fds.count(fd) != 0;
}

}  // namespace go2cpp::mcp::tools
