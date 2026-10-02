#pragma once

#include "go2cpp/fiber.hpp"

#include <cstdint>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace go2cpp::fiber_local::detail {

using KeyId = std::uint64_t;

// 键分配只在当前进程内进行并单调递增；进程存活期间不会复用键。
KeyId AllocateKey() noexcept;

std::shared_ptr<void> Get(Fiber* fiber, KeyId key) noexcept;
void Set(Fiber* fiber, KeyId key, std::shared_ptr<void> value);
void Reset(Fiber* fiber, KeyId key) noexcept;
void Cleanup(Fiber* fiber) noexcept;

using ThreadValueMap =
    std::unordered_map<KeyId, std::shared_ptr<void>>;

// 在 Fiber 模块中定义，确保所有编译单元/DSO 每个 OS 线程只使用一张 TLS
// 映射；模板局部静态变量可能悄悄为每个 DSO 创建不同映射。
ThreadValueMap& ThreadValues() noexcept;

}  // namespace go2cpp::fiber_local::detail

namespace go2cpp {

/**
 * 具有 Fiber 局部生命周期的命名值槽。
 *
 * 依赖：Fiber 提供当前 G，内部注册表保存值；Scheduler 迁移 G 时值随 G
 * 迁移。对上层提供 GetOrCreate/TryGet/Reset。值在 Fiber 内属于该 G，跨 M
 * 迁移仍然存在，并在 Fiber trampoline 结束时销毁；Fiber 外调用使用线程
 * 局部后备值，于线程退出时销毁。
 *
 * 只能从当前执行上下文访问值槽。注册表在迁移/销毁时受互斥锁保护，
 * 但 TryGet() 返回的裸 T* 在当前 Fiber 调用 Reset 或结束后不能继续保存。
 * 值析构函数应为 noexcept，且不能挂起 Fiber。
 *
 * 该类存储值，不复用 Fiber 栈。栈池属于独立后端，因为栈可能在不同 M 上
 * 结束，并且必须保持 sanitizer/保护页不变量。
 */
template <typename T>
class FiberLocalCache final {
    static_assert(!std::is_void_v<T>, "FiberLocalCache<void> is invalid");

public:
    /** @brief 分配当前实例的 Fiber-local 键。 */
    FiberLocalCache() noexcept : m_key(fiber_local::detail::AllocateKey()) {}
    // 值刻意长于键对象，由 Fiber trampoline（或线程退出 TLS 清理）回收。
    // 这样不会因为缓存包装器离开作用域就销毁仍存活 Fiber 的值。调用方在
    // 仍需访问槽时应保持缓存对象存活。
    ~FiberLocalCache() = default;

    FiberLocalCache(const FiberLocalCache&) = delete;
    FiberLocalCache& operator=(const FiberLocalCache&) = delete;
    FiberLocalCache(FiberLocalCache&&) = delete;
    FiberLocalCache& operator=(FiberLocalCache&&) = delete;

    /** @brief 读取当前 Fiber 值，不存在时按参数构造并保存。 */
    template <typename... Args>
    T& GetOrCreate(Args&&... args) {
        if (auto value = load()) {
            return *std::static_pointer_cast<T>(std::move(value));
        }
        auto value = std::make_shared<T>(std::forward<Args>(args)...);
        if (Fiber* const fiber = Fiber::Current()) {
            fiber_local::detail::Set(fiber, m_key, value);
        } else {
            fiber_local::detail::ThreadValues()[m_key] = value;
        }
        return *value;
    }

    template <typename... Args>
    T& get_or_create(Args&&... args) {
        return GetOrCreate(std::forward<Args>(args)...);
    }

    /** @brief 尝试读取当前 Fiber 值，不存在返回空指针。 */
    T* TryGet() const noexcept {
        if (Fiber* const fiber = Fiber::Current()) {
            auto value = fiber_local::detail::Get(fiber, m_key);
            return value ? std::static_pointer_cast<T>(value).get() : nullptr;
        }
        const auto& values = fiber_local::detail::ThreadValues();
        const auto found = values.find(m_key);
        return found == values.end()
                   ? nullptr
                   : std::static_pointer_cast<T>(found->second).get();
    }

    T* try_get() const noexcept { return TryGet(); }

    /** @brief 删除当前 Fiber 或线程中的缓存值。 */
    void Reset() noexcept {
        if (Fiber* const fiber = Fiber::Current()) {
            fiber_local::detail::Reset(fiber, m_key);
        } else {
            fiber_local::detail::ThreadValues().erase(m_key);
        }
    }

    void reset() noexcept { Reset(); }
    /** @brief 返回内部键 ID，仅用于诊断。 */
    std::uint64_t key_id() const noexcept { return m_key; }

private:
    std::shared_ptr<void> load() const noexcept {
        if (Fiber* const fiber = Fiber::Current()) {
            return fiber_local::detail::Get(fiber, m_key);
        }
        const auto& values = fiber_local::detail::ThreadValues();
        const auto found = values.find(m_key);
        return found == values.end() ? std::shared_ptr<void>{}
                                     : found->second;
    }

    const fiber_local::detail::KeyId m_key;
};

template <typename T>
using FiberLocal = FiberLocalCache<T>;

}  // namespace go2cpp
