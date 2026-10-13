#include "go2cpp/io_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// io_backend 全面版：同一个组件的 7 个使用场景。
//   1  默认后端名           DefaultBackendName 由平台实现提供
//   2  注册与创建           RegisterBackend + CreateBackend 按名实例化
//   3  协议调用序列         initialize / wake / drain_wake / shutdown
//   4  ★ 同名重复注册被拒   注册表不允许覆盖已存在的名字
//   5  ★ 注销后创建返回空   UnregisterBackend 之后 CreateBackend 得到 nullptr
//   6  ★ 默认后端不可注销   平台默认后端是 IOManager 的启动依赖（台账 D-13）
//   7  空名创建默认实例     CreateBackend() 返回平台默认后端
//
// 说明：IOBackend 只处理 readiness 注册、等待、唤醒与关闭，
//       不接触 Fiber / Task / Context —— 本示例用计数后端验证协议，不涉及真实 I/O。

namespace {

using go2cpp::io::BackendEvent;
using go2cpp::io::IOBackend;

/** 记录每次协议调用的后端实现。 */
class RecordingBackend final : public IOBackend {
public:
    bool initialize(int* error) noexcept override {
        ++initialize_calls;
        if (error != nullptr) {
            *error = 0;
        }
        return true;
    }
    int add_or_modify(int fd, std::uint32_t events, std::uint64_t data,
                      bool enable) noexcept override {
        ++add_calls;
        last_fd = fd;
        last_events = events;
        last_data = data;
        last_enable = enable;
        return 0;
    }
    int remove(int fd) noexcept override {
        ++remove_calls;
        last_removed_fd = fd;
        return 0;
    }
    int wait(BackendEvent* events, std::size_t capacity, int timeout_ms) noexcept override {
        ++wait_calls;
        last_wait_capacity = capacity;
        last_wait_timeout = timeout_ms;
        (void)events;
        return 0;                     // 没有就绪事件
    }
    void wake() noexcept override { ++wake_calls; }
    void drain_wake() noexcept override { ++drain_calls; }
    void shutdown() noexcept override { ++shutdown_calls; }

    int initialize_calls{0};
    int add_calls{0};
    int remove_calls{0};
    int wait_calls{0};
    int wake_calls{0};
    int drain_calls{0};
    int shutdown_calls{0};
    int last_fd{-1};
    std::uint32_t last_events{0};
    std::uint64_t last_data{0};
    bool last_enable{false};
    int last_removed_fd{-1};
    std::size_t last_wait_capacity{0};
    int last_wait_timeout{0};
};

std::shared_ptr<RecordingBackend> make_recording() {
    return std::make_shared<RecordingBackend>();
}

bool scenario_default_name() {
    std::clog << "[full][场景1] 平台默认后端名\n";
    const auto name = go2cpp::io::DefaultBackendName();
    const bool ok = !name.empty();
    std::cout << "  default=\"" << name << "\" 非空=" << ok << "（预期 1）\n";
    return ok;
}

bool scenario_register_and_create() {
    std::clog << "[full][场景2] 注册与创建\n";
    go2cpp::io::UnregisterBackend("rec-a");        // 清理可能的残留
    const bool registered = go2cpp::io::RegisterBackend("rec-a", make_recording);
    auto backend = go2cpp::io::CreateBackend("rec-a");
    const auto* typed = dynamic_cast<RecordingBackend*>(backend.get());
    const bool ok = registered && backend != nullptr && typed != nullptr;
    std::cout << "  registered=" << registered << " created=" << (backend != nullptr)
              << " 类型正确=" << (typed != nullptr) << "（预期 1 1 1）\n";
    go2cpp::io::UnregisterBackend("rec-a");
    return ok;
}

bool scenario_protocol_sequence() {
    std::clog << "[full][场景3] 协议调用序列\n";
    go2cpp::io::UnregisterBackend("rec-b");
    (void)go2cpp::io::RegisterBackend("rec-b", make_recording);
    auto backend = go2cpp::io::CreateBackend("rec-b");
    auto* typed = dynamic_cast<RecordingBackend*>(backend.get());
    if (typed == nullptr) {
        go2cpp::io::UnregisterBackend("rec-b");
        return false;
    }

    int error = -1;
    const bool initialized = backend->initialize(&error);
    const int add_result = backend->add_or_modify(7, 0x1U, 0xABCDULL, true);
    const int remove_result = backend->remove(7);

    std::vector<BackendEvent> events(4);
    const int wait_result = backend->wait(events.data(), events.size(), 25);
    backend->wake();
    backend->drain_wake();
    backend->shutdown();

    const bool ok = initialized && error == 0 && add_result == 0 && remove_result == 0 &&
                    wait_result == 0 &&
                    typed->initialize_calls == 1 && typed->add_calls == 1 &&
                    typed->remove_calls == 1 && typed->wait_calls == 1 &&
                    typed->wake_calls == 1 && typed->drain_calls == 1 &&
                    typed->shutdown_calls == 1 &&
                    // 参数确实原样传给了后端
                    typed->last_fd == 7 && typed->last_events == 0x1U &&
                    typed->last_data == 0xABCDULL && typed->last_enable &&
                    typed->last_removed_fd == 7 && typed->last_wait_capacity == 4 &&
                    typed->last_wait_timeout == 25;
    std::cout << "  initialize/wake/drain/shutdown 各 1 次="
              << (typed->initialize_calls == 1 && typed->wake_calls == 1 &&
                  typed->drain_calls == 1 && typed->shutdown_calls == 1)
              << " 参数原样透传=" << (typed->last_fd == 7 && typed->last_events == 0x1U)
              << "（预期 1 1）\n";
    go2cpp::io::UnregisterBackend("rec-b");
    return ok;
}

bool scenario_duplicate_rejected() {
    std::clog << "[full][场景4] 同名重复注册被拒\n";
    go2cpp::io::UnregisterBackend("rec-c");
    const bool first = go2cpp::io::RegisterBackend("rec-c", make_recording);
    const bool second = go2cpp::io::RegisterBackend("rec-c", make_recording);
    const bool ok = first && !second;
    std::cout << "  首次注册=" << first << " 重复注册=" << second
              << "（预期 1 0，注册表不允许覆盖）\n";
    go2cpp::io::UnregisterBackend("rec-c");
    return ok;
}

bool scenario_unregister_then_create_fails() {
    std::clog << "[full][场景5] 注销后创建返回空\n";
    go2cpp::io::UnregisterBackend("rec-d");
    (void)go2cpp::io::RegisterBackend("rec-d", make_recording);
    const auto before = go2cpp::io::CreateBackend("rec-d");
    const bool unregistered = go2cpp::io::UnregisterBackend("rec-d");
    const auto after = go2cpp::io::CreateBackend("rec-d");
    const bool ok = before != nullptr && unregistered && after == nullptr;
    std::cout << "  注销前创建=" << (before != nullptr ? "有实例" : "nullptr")
              << " 注销=" << unregistered
              << " 注销后创建=" << (after == nullptr ? "nullptr" : "有实例")
              << "（预期 有实例 1 nullptr）\n";
    return ok;
}

bool scenario_default_protected() {
    std::clog << "[full][场景6] 默认后端不可注销\n";
    const auto name = go2cpp::io::DefaultBackendName();
    // 平台默认后端是 IOManager 的启动依赖：允许注销会让后续默认创建静默拿到空实例。
    // 该约束在头文件中没有记录，属台账 D-13。
    const bool rejected = !go2cpp::io::UnregisterBackend(name);
    // 拒绝注销后，默认创建仍然可用
    auto backend = go2cpp::io::CreateBackend();
    const bool still_available = backend != nullptr;
    if (backend) {
        backend->shutdown();
    }
    std::cout << "  注销默认后端被拒=" << rejected
              << " 默认创建仍可用=" << still_available << "（预期 1 1）\n";
    return rejected && still_available;
}

bool scenario_empty_name_creates_default() {
    std::clog << "[full][场景7] 空名创建平台默认实例\n";
    auto backend = go2cpp::io::CreateBackend();
    const bool created = backend != nullptr;
    if (backend) {
        int error = -1;
        const bool initialized = backend->initialize(&error);
        backend->shutdown();
        std::cout << "  空名创建=" << created << " 可初始化=" << initialized
                  << "（预期 1 1）\n";
        return created && initialized;
    }
    std::cout << "  空名创建=0（预期 1）\n";
    return false;
}

}  // namespace

/**
 * @brief io_backend 全面版：7 个场景，覆盖注册表语义与协议调用约定。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_default_name() && ok;
    ok = scenario_register_and_create() && ok;
    ok = scenario_protocol_sequence() && ok;
    ok = scenario_duplicate_rejected() && ok;
    ok = scenario_unregister_then_create_fails() && ok;
    ok = scenario_default_protected() && ok;
    ok = scenario_empty_name_creates_default() && ok;

    std::clog << (ok ? "[full] 7 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
