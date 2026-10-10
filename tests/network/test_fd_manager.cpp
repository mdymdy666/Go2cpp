#include "go2cpp/fd_manager.hpp"

#include "test_support.hpp"

#include <chrono>

namespace {

using namespace std::chrono_literals;

/** @brief get() 的 auto_create 语义，以及同一 fd 返回同一元数据对象。 */
void check_get_and_auto_create() {
    auto& manager = go2cpp::net::FdManager::instance();
    constexpr int kFd = 90101;
    manager.erase(kFd);

    auto created = manager.get(kFd, true);
    GO2CPP_REQUIRE(created != nullptr);
    GO2CPP_CHECK(created->fd() == kFd);
    // auto_create 生成的元数据会立即标记为已初始化。
    GO2CPP_CHECK(created->initialized());
    GO2CPP_CHECK(!created->closed());

    auto again = manager.get(kFd, true);
    GO2CPP_CHECK(again == created);

    manager.erase(kFd);
    auto absent = manager.get(kFd, false);
    GO2CPP_CHECK(absent == nullptr);
}

/** @brief set_nonblock 分别更新用户态与系统态非阻塞标记。 */
void check_nonblock_state() {
    auto& manager = go2cpp::net::FdManager::instance();
    constexpr int kFd = 90102;
    manager.erase(kFd);

    GO2CPP_REQUIRE(manager.set_nonblock(kFd, true, false));
    auto context = manager.get(kFd, false);
    GO2CPP_REQUIRE(context != nullptr);
    GO2CPP_CHECK(context->user_nonblock());
    GO2CPP_CHECK(!context->system_nonblock());

    GO2CPP_REQUIRE(manager.set_nonblock(kFd, false, true));
    context = manager.get(kFd, false);
    GO2CPP_REQUIRE(context != nullptr);
    GO2CPP_CHECK(!context->user_nonblock());
    GO2CPP_CHECK(context->system_nonblock());
    manager.erase(kFd);
}

/** @brief set_timeout 分别记录接收与发送超时。 */
void check_timeouts() {
    auto& manager = go2cpp::net::FdManager::instance();
    constexpr int kFd = 90103;
    manager.erase(kFd);

    GO2CPP_REQUIRE(manager.set_timeout(kFd, true, 250ms));
    GO2CPP_REQUIRE(manager.set_timeout(kFd, false, 500ms));
    auto context = manager.get(kFd, false);
    GO2CPP_REQUIRE(context != nullptr);
    GO2CPP_CHECK(context->recv_timeout() == 250ms);
    GO2CPP_CHECK(context->send_timeout() == 500ms);
    manager.erase(kFd);
}

/** @brief erase 把旧句柄标记为 closed，复用的 fd 得到全新句柄。 */
void check_erase_marks_closed() {
    auto& manager = go2cpp::net::FdManager::instance();
    constexpr int kFd = 90104;
    manager.erase(kFd);

    auto first = manager.get(kFd, true);
    GO2CPP_REQUIRE(first != nullptr);
    GO2CPP_CHECK(!first->closed());

    manager.erase(kFd);
    GO2CPP_CHECK(first->closed());

    auto second = manager.get(kFd, true);
    GO2CPP_REQUIRE(second != nullptr);
    GO2CPP_CHECK(second != first);
    GO2CPP_CHECK(!second->closed());
    manager.erase(kFd);
}

/** @brief duplicate 把旧 fd 的非阻塞状态复制到新 fd。 */
void check_duplicate() {
    auto& manager = go2cpp::net::FdManager::instance();
    constexpr int kOldFd = 90105;
    constexpr int kNewFd = 90106;
    manager.erase(kOldFd);
    manager.erase(kNewFd);

    GO2CPP_REQUIRE(manager.set_nonblock(kOldFd, true, true));
    auto copied = manager.duplicate(kOldFd, kNewFd);
    GO2CPP_REQUIRE(copied != nullptr);
    GO2CPP_CHECK(copied->fd() == kNewFd);
    GO2CPP_CHECK(copied->user_nonblock());
    GO2CPP_CHECK(copied->system_nonblock());
    manager.erase(kOldFd);
    manager.erase(kNewFd);
}

}  // namespace

/**
 * @brief 验证 net::FdManager 的元数据生命周期与状态更新。
 * @details FdManager 只保存元数据、不拥有 fd；此前全仓无测试引用（台账 T-04）。
 *          测试使用远离真实 fd 空间的编号，避免影响进程内其它句柄。
 * @return 无；失败项由测试支持代码累计。
 */
void run_fd_manager_tests() {
    go2cpp_tests::announce("fd manager metadata lifecycle");
    check_get_and_auto_create();
    check_nonblock_state();
    check_timeouts();
    check_erase_marks_closed();
    check_duplicate();
}