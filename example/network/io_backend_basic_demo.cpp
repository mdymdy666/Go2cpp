#include "go2cpp/io_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>

/**
 * @brief io_backend 简单版：查询默认后端、注册自定义后端、调用其协议。
 * @details IOBackend 只负责 readiness 注册/等待/唤醒/关闭，
 *          不接触 Fiber、Task 或 Context —— 平台差异不应泄漏到上层。
 *          ① DefaultBackendName —— 平台默认后端名；
 *          ② RegisterBackend / CreateBackend —— 注册并按名实例化；
 *          ③ 协议调用 —— initialize / wake / shutdown。
 * @return 全部校验通过返回 0，否则返回 1。
 */
namespace {

/** 只记录调用次数的最小后端实现。 */
class CountingBackend final : public go2cpp::io::IOBackend {
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
    int wait(go2cpp::io::BackendEvent*, std::size_t, int) noexcept override { return 0; }
    void wake() noexcept override { ++wake_calls; }
    void drain_wake() noexcept override {}
    void shutdown() noexcept override { ++shutdown_calls; }

    int initialize_calls{0};
    int add_calls{0};
    int wake_calls{0};
    int shutdown_calls{0};
};

}  // namespace

int main() {
    using namespace go2cpp::io;
    bool ok = true;

    // ① 平台默认后端名
    std::clog << "[io_backend][场景1] 默认后端名\n";
    const auto default_name = DefaultBackendName();
    std::cout << "  default=" << default_name << "（预期非空）\n";
    ok = ok && !default_name.empty();

    // ② 注册并创建
    std::clog << "[io_backend][场景2] 注册与创建\n";
    const bool registered = RegisterBackend("counting", [] {
        return std::make_shared<CountingBackend>();
    });
    auto backend = CreateBackend("counting");
    std::cout << "  registered=" << registered << " created="
              << (backend != nullptr) << "（预期 1 1）\n";
    ok = ok && registered && backend != nullptr;

    // ③ 协议调用
    std::clog << "[io_backend][场景3] 协议调用\n";
    int error = -1;
    if (backend) {
        ok = backend->initialize(&error) && ok;
        backend->wake();
        backend->shutdown();
    }
    auto* counting = dynamic_cast<CountingBackend*>(backend.get());
    const bool protocol_ok = counting != nullptr && counting->initialize_calls == 1 &&
                             counting->wake_calls == 1 &&
                             counting->shutdown_calls == 1;
    std::cout << "  initialize=" << (counting ? counting->initialize_calls : -1)
              << " wake=" << (counting ? counting->wake_calls : -1)
              << " shutdown=" << (counting ? counting->shutdown_calls : -1)
              << "（预期各 1 次）\n";
    ok = ok && protocol_ok && error == 0;

    UnregisterBackend("counting");
    std::clog << (ok ? "[io_backend] 全部通过\n" : "[io_backend] 有失败\n");
    return ok ? 0 : 1;
}
