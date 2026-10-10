#include "go2cpp/io_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

namespace {

/** @brief 记录调用次数的最小后端实现，用于验证注册与创建协议。 */
class RecordingBackend final : public go2cpp::io::IOBackend {
public:
    bool initialize(int* error) noexcept override {
        ++initialize_calls;
        if (error != nullptr) {
            *error = 0;
        }
        return true;
    }
    int add_or_modify(int, std::uint32_t, std::uint64_t, bool) noexcept override {
        ++add_calls;
        return 0;
    }
    int remove(int) noexcept override { return 0; }
    int wait(go2cpp::io::BackendEvent*, std::size_t, int) noexcept override {
        return 0;
    }
    void wake() noexcept override { ++wake_calls; }
    void drain_wake() noexcept override {}
    void shutdown() noexcept override { ++shutdown_calls; }

    int initialize_calls{0};
    int add_calls{0};
    int wake_calls{0};
    int shutdown_calls{0};
};

}  // namespace

/**
 * @brief 演示 IOBackend 的注册、创建与协议调用。
 * @details 后端只处理 readiness 注册、等待、唤醒和关闭，不接触 Fiber、
 *          Task 或 Context；平台适配不应把差异泄漏到上层状态机。
 * @return 所有场景按预期完成时返回 0。
 */
int main() {
    using namespace go2cpp::io;

    // 场景一（成功路径）：Linux 默认后端名由实现提供。
    const auto default_name = DefaultBackendName();
    const bool default_ok = !default_name.empty();
    std::cout << "[io_backend][场景一] default=" << default_name << '\n';

    // 场景二（注册与创建）：命名后端可以注册并实例化。
    const bool registered = RegisterBackend("recording", [] {
        return std::make_shared<RecordingBackend>();
    });
    auto backend = CreateBackend("recording");
    const bool created = backend != nullptr;
    std::cout << "[io_backend][场景二] registered=" << registered
              << " created=" << created << '\n';

    // 场景三（协议调用）：initialize / wake / shutdown 按约定生效。
    int error = -1;
    const bool initialized = backend != nullptr && backend->initialize(&error);
    if (backend) {
        backend->wake();
        backend->shutdown();
    }
    auto* recording = dynamic_cast<RecordingBackend*>(backend.get());
    const bool protocol_ok = initialized && recording != nullptr &&
                             recording->wake_calls == 1 &&
                             recording->shutdown_calls == 1;
    std::cout << "[io_backend][场景三] initialized=" << initialized
              << " wake_calls=" << (recording ? recording->wake_calls : -1)
              << " shutdown_calls=" << (recording ? recording->shutdown_calls : -1)
              << '\n';

    // 场景四（错误/边界路径）：同名重复注册失败；注销后无法再创建。
    const bool duplicate = RegisterBackend("recording", [] {
        return std::make_shared<RecordingBackend>();
    });
    const bool unregistered = UnregisterBackend("recording");
    auto after_remove = CreateBackend("recording");
    const bool boundary_ok = !duplicate && unregistered && after_remove == nullptr;
    std::cout << "[io_backend][场景四] duplicate_rejected=" << !duplicate
              << " unregistered=" << unregistered
              << " create_after_remove=null -> " << (after_remove == nullptr)
              << '\n';

    // 场景五（约束边界）：平台默认后端不能被注销。
    // 源码 `src/network/io_backend.cpp:197` 明确拒绝注销默认后端，因为它
    // 是 IOManager 的启动依赖；若允许注销，后续默认创建会静默得到空实例。
    // 此约束在 `io_backend.hpp` 中没有记录（台账 D-13）。
    const bool default_protected = !UnregisterBackend(default_name);
    std::cout << "[io_backend][场景五] unregister_default_rejected="
              << default_protected << "（预期 true）\n";

    // 场景六（默认后端）：空名称创建平台默认实例。
    auto fallback = CreateBackend();
    const bool fallback_ok = fallback != nullptr;
    if (fallback) {
        fallback->shutdown();
    }

    const bool all = default_ok && registered && created && protocol_ok &&
                     boundary_ok && default_protected && fallback_ok;
    std::cout << "[io_backend][汇总] all=" << std::boolalpha << all << '\n';
    return all ? 0 : 1;
}