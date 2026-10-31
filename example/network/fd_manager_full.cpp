#include "go2cpp/fd_manager.hpp"

#include <chrono>
#include <iostream>

// fd_manager 全面版：同一个组件的 7 个使用场景。
//   1  自动创建            首次 get(fd, true) 建立元数据
//   2  非阻塞标志          user 与 system 是两个独立维度
//   3  超时设置            recv 与 send 是两个独立方向
//   4  erase 生命周期      erase 把已发出的句柄标记为 closed；编号复用得到新对象
//   5  只读查询边界        auto_create=false 时不存在返回 nullptr
//   6  duplicate           把元数据复制到另一个编号
//   7  多 fd 隔离          不同编号的状态互不影响
//
// ★ 重要语义：FdManager 不拥有 fd，只保存 hook / Socket / IOManager 共享的
//   非阻塞与超时状态。本示例使用远离真实 fd 空间的编号（91001 起），
//   不涉及任何真实文件描述符或网络操作。

namespace {

using Ms = std::chrono::milliseconds;
using go2cpp::net::FdManager;

constexpr int kBase = 91001;

bool scenario_auto_create() {
    std::clog << "[full][场景1] 首次 get 自动创建元数据\n";
    auto& manager = FdManager::instance();
    manager.erase(kBase);

    const auto fresh = manager.get(kBase, false);      // 尚未创建
    const auto created = manager.get(kBase, true);     // 自动创建
    const bool ok = fresh == nullptr && created != nullptr &&
                    created->fd() == kBase && created->initialized() &&
                    !created->closed();
    std::cout << "  创建前=" << (fresh == nullptr ? "nullptr" : "有对象")
              << " 创建后 fd=" << (created ? created->fd() : -1)
              << " initialized=" << (created ? created->initialized() : false)
              << "（预期 nullptr / 91001 / 1）\n";
    return ok;
}

bool scenario_nonblock_dimensions() {
    std::clog << "[full][场景2] 非阻塞：user 与 system 是两个独立维度\n";
    auto& manager = FdManager::instance();
    const int fd = kBase + 1;
    manager.erase(fd);
    (void)manager.get(fd, true);

    // 只设 user，不设 system
    const bool set_ok = manager.set_nonblock(fd, true, false);
    auto context = manager.get(fd, false);
    const bool user_only = context && context->user_nonblock() &&
                           !context->system_nonblock();

    manager.set_nonblock(fd, true, true);              // 两个都设
    context = manager.get(fd, false);
    const bool both = context && context->user_nonblock() &&
                      context->system_nonblock();

    manager.set_nonblock(fd, false, false);            // 都清掉
    context = manager.get(fd, false);
    const bool cleared = context && !context->user_nonblock() &&
                         !context->system_nonblock();
    manager.erase(fd);
    std::cout << "  只设 user=" << user_only << " 两个都设=" << both
              << " 都清掉=" << cleared << "（预期 1 1 1）\n";
    return set_ok && user_only && both && cleared;
}

bool scenario_timeout_directions() {
    std::clog << "[full][场景3] 超时：recv 与 send 是两个独立方向\n";
    auto& manager = FdManager::instance();
    const int fd = kBase + 2;
    manager.erase(fd);
    (void)manager.get(fd, true);

    const bool recv_ok = manager.set_timeout(fd, true, Ms(250));
    const bool send_ok = manager.set_timeout(fd, false, Ms(500));
    auto context = manager.get(fd, false);
    const bool ok = recv_ok && send_ok && context &&
                    context->recv_timeout() == Ms(250) &&
                    context->send_timeout() == Ms(500);
    std::cout << "  recv=" << (context ? context->recv_timeout().count() : -1)
              << "ms send=" << (context ? context->send_timeout().count() : -1)
              << "ms（预期 250 500）\n";
    manager.erase(fd);
    return ok;
}

bool scenario_erase_lifecycle() {
    std::clog << "[full][场景4] erase 生命周期：旧句柄 closed，编号复用得到新对象\n";
    auto& manager = FdManager::instance();
    const int fd = kBase + 3;
    manager.erase(fd);

    auto first = manager.get(fd, true);
    manager.erase(fd);
    const bool old_closed = first && first->closed();

    auto second = manager.get(fd, true);
    const bool fresh = second && second != first && !second->closed();
    const bool same_number = second && second->fd() == fd;

    manager.erase(fd);
    std::cout << "  旧句柄 closed=" << old_closed << " 新对象=" << fresh
              << " 编号相同=" << same_number << "（预期 1 1 1）\n";
    return old_closed && fresh && same_number;
}

bool scenario_readonly_boundary() {
    std::clog << "[full][场景5] 只读查询边界：auto_create=false 不创建\n";
    auto& manager = FdManager::instance();
    const int fd = kBase + 4;
    manager.erase(fd);

    const auto absent = manager.get(fd, false);        // 不创建，返回空
    const auto still_absent = manager.get(fd, false);  // 再查一次仍为空
    const bool ok = absent == nullptr && still_absent == nullptr;

    const auto created = manager.get(fd, true);        // 这次创建
    manager.erase(fd);
    std::cout << "  首次只读=" << (absent == nullptr ? "nullptr" : "有对象")
              << " 再次只读=" << (still_absent == nullptr ? "nullptr" : "有对象")
              << " 显式创建=" << (created != nullptr ? "有对象" : "nullptr")
              << "（预期 nullptr nullptr 有对象）\n";
    return ok && created != nullptr;
}

bool scenario_duplicate() {
    std::clog << "[full][场景6] duplicate：把元数据复制到另一个编号\n";
    auto& manager = FdManager::instance();
    const int source = kBase + 5;
    const int target = kBase + 6;
    manager.erase(source);
    manager.erase(target);

    (void)manager.get(source, true);
    (void)manager.set_nonblock(source, true, false);
    (void)manager.set_timeout(source, true, Ms(120));

    const auto copied = manager.duplicate(source, target);
    const bool target_ok = copied != nullptr && copied->fd() == target;
    const bool inherited = copied && copied->user_nonblock() &&
                           copied->recv_timeout() == Ms(120);
    const auto source_ctx = manager.get(source, false);

    manager.erase(source);
    manager.erase(target);
    std::cout << "  目标 fd=" << (copied ? copied->fd() : -1)
              << " 继承 user_nonblock=" << (copied ? copied->user_nonblock() : false)
              << " 继承 recv_timeout="
              << (copied ? copied->recv_timeout().count() : -1)
              << "ms 源仍存在=" << (source_ctx != nullptr)
              << "（预期 " << target << " 1 120 1）\n";
    return target_ok && inherited && source_ctx != nullptr;
}

bool scenario_isolation() {
    std::clog << "[full][场景7] 多 fd 隔离：状态互不影响\n";
    auto& manager = FdManager::instance();
    const int a = kBase + 7;
    const int b = kBase + 8;
    manager.erase(a);
    manager.erase(b);

    (void)manager.get(a, true);
    (void)manager.get(b, true);
    (void)manager.set_nonblock(a, true, false);
    (void)manager.set_timeout(a, true, Ms(300));

    const auto ctx_a = manager.get(a, false);
    const auto ctx_b = manager.get(b, false);
    const bool isolated = ctx_a && ctx_b &&
                          ctx_a->user_nonblock() && !ctx_b->user_nonblock() &&
                          ctx_a->recv_timeout() == Ms(300) &&
                          ctx_b->recv_timeout() != Ms(300);

    // 擦掉 a 不应影响 b
    manager.erase(a);
    const auto after = manager.get(b, false);
    const bool survives = after != nullptr && !after->closed();

    manager.erase(b);
    std::cout << "  两个 fd 状态独立=" << isolated
              << " 擦掉一个后另一个仍在=" << survives << "（预期 1 1）\n";
    return isolated && survives;
}

}  // namespace

/**
 * @brief fd_manager 全面版：7 个场景，覆盖创建、标志、超时、生命周期与隔离。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_auto_create() && ok;
    ok = scenario_nonblock_dimensions() && ok;
    ok = scenario_timeout_directions() && ok;
    ok = scenario_erase_lifecycle() && ok;
    ok = scenario_readonly_boundary() && ok;
    ok = scenario_duplicate() && ok;
    ok = scenario_isolation() && ok;

    std::clog << (ok ? "[full] 7 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
