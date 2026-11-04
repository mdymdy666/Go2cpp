#include "go2cpp/fd_manager.hpp"

#include <chrono>
#include <iostream>

/**
 * @brief 演示 net::FdManager 的元数据生命周期。
 * @details 该表不拥有 fd，只保存 hook、Socket 与 IOManager 共享的非阻塞和
 *          超时状态；示例使用远离真实 fd 空间的编号。
 * @return 所有场景按预期完成时返回 0。
 */
int main() {
    using namespace std::chrono_literals;
    auto& manager = go2cpp::net::FdManager::instance();
    constexpr int kFd = 91001;
    manager.erase(kFd);

    // 场景一（成功路径）：首次 get 自动创建元数据。
    auto context = manager.get(kFd, true);
    const bool created = context != nullptr && context->fd() == kFd;
    std::cout << "[fd_manager][场景一] created=" << created << '\n';

    // 场景二（状态更新）：记录非阻塞与接收超时。
    const bool nonblock_set = manager.set_nonblock(kFd, true, false);
    const bool timeout_set = manager.set_timeout(kFd, true, 250ms);
    context = manager.get(kFd, false);
    const bool updated = context != nullptr && context->user_nonblock() &&
                         context->recv_timeout() == 250ms;
    std::cout << "[fd_manager][场景二] user_nonblock="
              << (context ? context->user_nonblock() : false)
              << " recv_timeout="
              << (context ? context->recv_timeout().count() : -1)
              << "ms（预期 true / 250ms）\n";

    // 场景三（生命周期）：erase 把旧句柄标记为 closed，复用的编号得到新对象。
    manager.erase(kFd);
    const bool closed = context != nullptr && context->closed();
    auto recreated = manager.get(kFd, true);
    const bool fresh = recreated != nullptr && recreated != context &&
                       !recreated->closed();
    std::cout << "[fd_manager][场景三] old_closed=" << closed
              << " new_object=" << fresh << '\n';

    // 场景四（错误/边界路径）：auto_create=false 时不存在的 fd 返回空。
    manager.erase(kFd);
    auto absent = manager.get(kFd, false);
    const bool boundary_ok = absent == nullptr;
    std::cout << "[fd_manager][场景四] absent=nullptr -> " << boundary_ok
              << "（预期 true）\n";

    manager.erase(kFd);
    const bool all = created && nonblock_set && timeout_set && updated && closed &&
                     fresh && boundary_ok;
    std::cout << "[fd_manager][汇总] all=" << std::boolalpha << all << '\n';
    return all ? 0 : 1;
}
