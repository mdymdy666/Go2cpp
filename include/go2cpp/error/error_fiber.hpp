#pragma once

/**
 * @file error_fiber.hpp
 * @brief 把 `ErrorLocal` 接到 `FiberLocal`：错误工作状态随 G 迁移。
 *
 * @details 这是 `error_local.hpp` 的**可选**后端。include 本头即完成安装
 *          （静态初始化；可用 `GO2CPP_ERROR_FIBER_NO_AUTO_INSTALL` 关闭后
 *          手动调用 `InstallFiberErrorLocalStorage()`），此后：
 *
 * - **在 Fiber 内**：值存进 `FiberLocal`，绑定当前 G；G 被调度到另一个 M
 *   后仍然读得到，G 结束时由 trampoline 回收；
 * - **在 Fiber 外**（普通线程）：`FiberLocal` 自动退化为线程局部，语义与
 *   默认后端一致，不会把状态泄漏到别的线程；
 * - **迁移期间**：G 挂起/恢复不经过任何错误代码，值只由 G 的身份决定，
 *   因此跨 M 读到的状态一致。
 *
 * 为什么单独一个头：错误头默认零依赖（不链接 Fiber 运行时）。只有确实
 * 链接了 Fiber 的程序才 include 本头，避免 `bufio`/`compress` 这类只用错误
 * 码的模块被拖进整个调度器。
 *
 * @note 安装必须发生在创建 `ErrorLocal` 值之前。本头的自动安装发生在静态
 *       初始化阶段，正常业务代码（`main` 之后）都晚于它；但**不要在更早的
 *       静态初始化里**使用错误工作状态。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/error/error_local.hpp"

#include "go2cpp/concurrency/fiber_local.hpp"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

namespace go2cpp::error {

namespace detail {

/// @brief 每个 Fiber/线程一份的「槽 → 值」表。
using FiberSlotMap = std::unordered_map<ErrorLocalSlot, std::shared_ptr<void>>;

/**
 * @brief 当前上下文的槽表。
 *
 * @details 只创建一个 `FiberLocal` 实例：键在首次调用时分配，所有槽共享
 *          同一张表（若每个函数各建一个 `FiberLocal`，键不同、表也不同，
 *          会互相看不见）。
 */
inline FiberLocal<FiberSlotMap>& FiberSlot() noexcept {
    static FiberLocal<FiberSlotMap> slot;
    return slot;
}

inline std::shared_ptr<void> FiberGet(ErrorLocalSlot id) noexcept {
    const FiberSlotMap* const map = FiberSlot().TryGet();
    if (map == nullptr) {
        return {};
    }
    const auto found = map->find(id);
    return found == map->end() ? std::shared_ptr<void>() : found->second;
}

inline void FiberSet(ErrorLocalSlot id, std::shared_ptr<void> value) {
    FiberSlot().GetOrCreate()[id] = std::move(value);
}

inline void FiberReset(ErrorLocalSlot id) noexcept {
    FiberSlotMap* const map = FiberSlot().TryGet();
    if (map == nullptr) {
        return;
    }
    const auto found = map->find(id);
    if (found == map->end()) {
        return;
    }
    // 与线程后端一致：先摘出容器，再在容器外析构。
    std::shared_ptr<void> removed = std::move(found->second);
    map->erase(found);
}

/// @brief Fiber 后端函数表。
inline constexpr ErrorLocalStorage kFiberStorage{
    ErrorLocalBackendKind::kFiber, &FiberGet, &FiberSet, &FiberReset};

} // namespace detail

/**
 * @brief 安装 Fiber 后端（幂等）。
 * @return 首次安装或已经是 Fiber 后端返回 true；已被其他后端占用返回 false。
 * @note 此后新建的 `ErrorLocal` 值随 G 迁移；安装前已存在的值留在旧后端。
 */
inline bool InstallFiberErrorLocalStorage() noexcept {
    return detail::InstallStorage(&detail::kFiberStorage);
}

/// @brief 当前是否使用 Fiber 感知后端。
inline bool IsFiberErrorLocalEnabled() noexcept {
    return CurrentErrorLocalBackend() == ErrorLocalBackendKind::kFiber;
}

#if !defined(GO2CPP_ERROR_FIBER_NO_AUTO_INSTALL)

namespace detail {

/**
 * @brief 静态初始化期自动安装 Fiber 后端。
 * @note 多个翻译单元会各有一个初始化器；安装本身幂等，重复调用无副作用。
 */
struct FiberStorageAutoInstaller {
    FiberStorageAutoInstaller() noexcept { InstallFiberErrorLocalStorage(); }
};

[[maybe_unused]] inline const FiberStorageAutoInstaller kFiberStorageAutoInstaller{};

} // namespace detail

#endif // GO2CPP_ERROR_FIBER_NO_AUTO_INSTALL

} // namespace go2cpp::error
