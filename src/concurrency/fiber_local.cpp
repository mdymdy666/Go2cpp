/**
 * @file fiber_local.cpp
 * @brief Fiber 局部存储的底层实现。
 *
 * ## 这个文件实现什么
 *
 * - `AllocateKey()`：进程内单调递增的键分配。**永不复用**是硬要求 ——
 *   复用会让新 `FiberLocal` 实例读到旧实例的残留值；
 * - `Get/Set/Reset`：在 G 的映射表里存取；
 * - `Cleanup`：G 结束时清空它的全部局部值；
 * - `ThreadValues()`：Fiber 外（普通线程）的退化存储。
 *
 * ## 一个关键实现细节
 *
 * `ThreadValues()` **定义在这里而不是头文件里**。如果把"返回这张 TLS 映射"
 * 写成头文件里的函数局部 static，每个 DSO 会各拿一份不同的映射 —— 表现为
 * "插件里设置的 local 值主程序看不到"这类极难排查的问题。
 */
#include "go2cpp/fiber_local.hpp"

#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace go2cpp::fiber_local::detail {
namespace {

using ValueMap = std::unordered_map<KeyId, std::shared_ptr<void>>;

thread_local ValueMap s_thread_values;

/**
 * @brief 进程级「Fiber 局部存储」注册表：`Fiber* → {KeyId → 值}`。
 *
 * ## 定位
 *
 * `FiberLocal` 与 `ThreadValues()` 的存储后端：每个 Fiber 的局部变量放在
 * `m_values[Fiber::Current()]` 里；普通线程走 `s_thread_values`（thread_local 快路径）。
 *
 * ## 为什么永不析构
 *
 * Fiber 可能在**任意静态析构阶段**被销毁，那时普通静态对象（含 mutex）可能
 * 已经析构。用 `new` 且不注册 atexit，让注册表活到进程结束 —— 代价是泄漏
 * 一个空 map，换来「任何阶段调用都安全」。
 *
 * ## 锁
 *
 * `m_mutex` 保护 `m_values`；Fiber 的读写都要拿它（普通线程有 thread_local 快路径）。
 */
struct Registry final {
    /// @brief 保护 m_values；FiberLocal 的读/写/清理都拿它。
    std::mutex m_mutex;
    /// @brief Fiber* → 该 Fiber 的局部变量表；Fiber 销毁时条目被删除。
    std::unordered_map<Fiber*, ValueMap> m_values;
};

Registry& registry() noexcept {
    // Keep the registry until process exit: Fiber objects may be destroyed
    // during static teardown, when ordinary static destruction order is not
    // sufficient to guarantee that a mutex still exists.
    static Registry* s_registry = new Registry();
    return *s_registry;
}

std::atomic<KeyId> s_next_key{1};

}  // namespace

ThreadValueMap& ThreadValues() noexcept { return s_thread_values; }

KeyId AllocateKey() noexcept {
    // Zero is reserved as the invalid/sentinel key.  A fetch_add based
    // allocator would wrap to an earlier live key after UINT64_MAX and could
    // alias values belonging to another FiberLocalCache.  Publish zero as an
    // exhausted sentinel instead; the practically unreachable next allocation
    // terminates rather than silently violating isolation.
    KeyId candidate = s_next_key.load(std::memory_order_relaxed);
    for (;;) {
        if (candidate == 0) {
            std::terminate();
        }
        const KeyId next =
            candidate == std::numeric_limits<KeyId>::max() ? 0 : candidate + 1;
        if (s_next_key.compare_exchange_weak(candidate, next,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
            return candidate;
        }
    }
}

std::shared_ptr<void> Get(Fiber* fiber, KeyId key) noexcept {
    if (!fiber || key == 0) {
        return {};
    }
    Registry& store = registry();
    std::lock_guard<std::mutex> lock(store.m_mutex);
    const auto fiber_found = store.m_values.find(fiber);
    if (fiber_found == store.m_values.end()) {
        return {};
    }
    const auto value_found = fiber_found->second.find(key);
    return value_found == fiber_found->second.end() ? std::shared_ptr<void>{}
                                                     : value_found->second;
}

void Set(Fiber* fiber, KeyId key, std::shared_ptr<void> value) {
    if (!fiber || key == 0) {
        return;
    }
    Registry& store = registry();
    std::lock_guard<std::mutex> lock(store.m_mutex);
    store.m_values[fiber][key] = std::move(value);
}

void Reset(Fiber* fiber, KeyId key) noexcept {
    if (!fiber || key == 0) {
        return;
    }
    Registry& store = registry();
    for (;;) {
        std::shared_ptr<void> value;
        {
            std::lock_guard<std::mutex> lock(store.m_mutex);
            const auto fiber_found = store.m_values.find(fiber);
            if (fiber_found == store.m_values.end()) {
                return;
            }
            const auto value_found = fiber_found->second.find(key);
            if (value_found == fiber_found->second.end()) {
                return;
            }
            value = std::move(value_found->second);
            fiber_found->second.erase(value_found);
            if (fiber_found->second.empty()) {
                store.m_values.erase(fiber_found);
            }
        }
        // Never invoke user destructors while holding the registry mutex.
        value.reset();
    }
}

void Cleanup(Fiber* fiber) noexcept {
    if (!fiber) {
        return;
    }
    Registry& store = registry();
    for (;;) {
        ValueMap values;
        {
            std::lock_guard<std::mutex> lock(store.m_mutex);
            const auto found = store.m_values.find(fiber);
            if (found == store.m_values.end()) {
                return;
            }
            values = std::move(found->second);
            store.m_values.erase(found);
        }
        // A value destructor may create another local; clean that wave too.
        // Destructors execute without the registry lock and must not suspend
        // the Fiber; doing so would hold this cleanup continuation on its stack.
        values.clear();
    }
}

}  // namespace go2cpp::fiber_local::detail
