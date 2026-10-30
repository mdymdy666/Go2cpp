#pragma once

/**
 * @file error_local.hpp
 * @brief `ErrorLocal<T>`：随执行上下文（Fiber 或线程）走的错误工作状态。
 *
 * @details 错误体系里有几类**工作状态**必须绑定「当前执行流」，而不是
 *          「当前 OS 线程」：
 *
 * - 深链析构工作表（`WrappedError`/`JoinError` 递归展开用的待释放队列）；
 * - 旧体系 / Bento 的释放队列；
 * - 调用方想跨挂起点保留的「最近错误」。
 *
 * `thread_local` 绑定的是 M（OS 线程）。本项目的 G（Fiber）会在 M 之间
 * 迁移，挂在 `thread_local` 上的状态不会跟着走。`ErrorLocal<T>` 把存储
 * 选择收敛到一个抽象里：
 *
 * - **默认后端**：线程局部（`thread_local`），零额外依赖，普通线程场景
 *   与直接写 `thread_local` 等价；
 * - **Fiber 后端**：由 `go2cpp/error/error_fiber.hpp` 安装，内部使用
 *   `go2cpp::FiberLocal<T>`。该设施在 Fiber 内把值绑定到 G（迁移跟随、
 *   G 结束回收），在 Fiber 外自动退化为线程局部。
 *
 * 为什么用运行期后端而不是编译期 `#ifdef`：错误头是零依赖、header-only
 * 的，凡是 include 它的模块（bufio/compress/context/...）不应该被迫链接
 * Fiber 运行时。运行期安装让「链接了 Fiber 的程序」获得迁移跟随语义，
 * 没链接的程序保持纯线程语义，二者共用同一份头文件。
 *
 * ## 使用约束
 *
 * 1. **安装时机**：Fiber 后端必须在创建任何 `ErrorLocal` 值之前安装
 *    （通常在 `main` 开头，或由 `error_fiber.hpp` 的静态初始化完成）。
 *    安装前写入的值留在旧后端里，不会迁移；这是有意的——不做跨后端搬运，
 *    避免在静态初始化阶段引入锁与分配。
 * 2. **后端不可替换**：安装是单向的，已安装 Fiber 后端后再装别的后端返回
 *    false，防止运行中把状态分裂到两个存储里。
 * 3. **单次访问一次间接调用**：`get/set/reset` 走函数指针表；热路径上
 *    比裸 `thread_local` 多一次间接跳转，换来的是一份代码同时支持两种语义。
 * 4. **DSO 边界**：与 `FiberLocal` 一样，函数局部静态在插件 DSO 里可能是
 *    独立副本（取决于 `-fgnu-unique` / 可见性）。跨插件共享同一槽位时，
 *    应由主程序统一持有槽对象。
 *
 * ## 生命周期
 *
 * 值由后端以 `shared_ptr<void>` 持有：线程后端在线程退出时按「波次」清空
 * （析构期间新建的值进入下一波，避免在容器析构中重入），Fiber 后端由
 * G 的 trampoline 在 Fiber 结束时清理。`Reset()` 先把值摘出容器再析构，
 * 允许值的析构函数重入本 API。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace go2cpp::error {

namespace detail {

/// @brief 槽位标识；进程内单调递增、永不复用（复用会让新槽读到旧值）。
using ErrorLocalSlot = std::uint64_t;

} // namespace detail

/// @brief 后端类型；用于诊断「当前状态是 Fiber 感知还是纯线程」。
enum class ErrorLocalBackendKind : std::uint8_t {
    kThread = 0, ///< 线程局部（M 专属）。
    kFiber = 1,  ///< Fiber 局部（G 专属；Fiber 外退化为线程局部）。
};

/**
 * @brief `ErrorLocal` 的存储后端函数表。
 *
 * @details 三个操作都以「槽位 ID」为键；`get` 返回值的共享所有权，
 *          保证调用方拿到的 `T*` 在本次访问期间有效。实现必须满足：
 *          - `get` 在槽不存在时返回空指针；
 *          - `set` 覆盖旧值；
 *          - `reset` 允许重入（先摘出容器再析构）。
 */
struct ErrorLocalStorage {
    ErrorLocalBackendKind kind;                                  ///< 后端类型。
    std::shared_ptr<void> (*get)(detail::ErrorLocalSlot) noexcept; ///< 读取。
    void (*set)(detail::ErrorLocalSlot, std::shared_ptr<void>);    ///< 写入。
    void (*reset)(detail::ErrorLocalSlot) noexcept;                ///< 清除。
};

namespace detail {

/**
 * @brief 线程局部后备存储。
 *
 * @details 析构采用「波次清空」：把当前表整体移出，再在表外逐个析构；
 *          若某个值的析构函数又创建了新的 `ErrorLocal` 值，它会落进已经
 *          清空的 `values`，由下一轮循环收走。这样既不会在容器析构期间
 *          重入同一容器，也不会把值漏到线程退出之后。
 */
struct ThreadLocalValues {
    std::unordered_map<ErrorLocalSlot, std::shared_ptr<void>> values; ///< 槽 → 值。

    ~ThreadLocalValues() {
        while (!values.empty()) {
            std::unordered_map<ErrorLocalSlot, std::shared_ptr<void>> wave = std::move(values);
            values.clear();
            wave.clear();
        }
    }
};

/// @brief 当前线程的 `ErrorLocal` 值表。
inline ThreadLocalValues& ThreadValues() noexcept {
    // 函数局部 thread_local：每个线程一份；值的析构由 ~ThreadLocalValues 统管。
    static thread_local ThreadLocalValues values;
    return values;
}

inline std::shared_ptr<void> ThreadGet(ErrorLocalSlot slot) noexcept {
    const auto& values = ThreadValues().values;
    const auto found = values.find(slot);
    return found == values.end() ? std::shared_ptr<void>() : found->second;
}

inline void ThreadSet(ErrorLocalSlot slot, std::shared_ptr<void> value) {
    ThreadValues().values[slot] = std::move(value);
}

inline void ThreadReset(ErrorLocalSlot slot) noexcept {
    auto& values = ThreadValues().values;
    const auto found = values.find(slot);
    if (found == values.end()) {
        return;
    }
    // 先摘出容器，再让 shared_ptr 在容器外析构：值的析构函数可以安全地
    // 重入 ErrorLocal（包括再次 Set 同一个槽）。
    std::shared_ptr<void> removed = std::move(found->second);
    values.erase(found);
}

/// @brief 默认后端：线程局部。
inline constexpr ErrorLocalStorage kThreadStorage{
    ErrorLocalBackendKind::kThread, &ThreadGet, &ThreadSet, &ThreadReset};

/// @brief 进程级后端指针；初始化即线程后端。
inline std::atomic<const ErrorLocalStorage*>& BackendSlot() noexcept {
    static std::atomic<const ErrorLocalStorage*> backend{&kThreadStorage};
    return backend;
}

/// @brief 当前后端（acquire 读，安装后对所有线程可见）。
inline const ErrorLocalStorage& CurrentStorage() noexcept {
    return *BackendSlot().load(std::memory_order_acquire);
}

/// @brief 槽位计数器；0 保留为非法槽。
inline std::atomic<ErrorLocalSlot>& SlotCounter() noexcept {
    static std::atomic<ErrorLocalSlot> counter{1};
    return counter;
}

/// @brief 分配一个永不复用的槽位。
inline ErrorLocalSlot AllocateSlot() noexcept {
    return SlotCounter().fetch_add(1, std::memory_order_relaxed);
}

/// @brief 安装后端；只允许从默认线程后端单向切换。
inline bool InstallStorage(const ErrorLocalStorage* storage) noexcept {
    if (storage == nullptr) {
        return false;
    }
    std::atomic<const ErrorLocalStorage*>& backend = BackendSlot();
    const ErrorLocalStorage* current = backend.load(std::memory_order_acquire);
    if (current->kind == storage->kind) {
        // 同类后端（含跨 DSO 的重复安装）幂等成功。
        return true;
    }
    if (current != &kThreadStorage) {
        // 已经装过另一种后端：拒绝替换，避免状态分裂。
        return false;
    }
    return backend.compare_exchange_strong(current, storage, std::memory_order_acq_rel,
                                           std::memory_order_acquire);
}

} // namespace detail

/**
 * @brief 安装 `ErrorLocal` 的存储后端。
 *
 * @param storage 后端函数表；生命周期必须覆盖进程剩余时间（通常传静态常量）。
 * @return 首次安装/同类幂等返回 true；传入空指针或已安装其他后端返回 false。
 * @note 应在创建任何 `ErrorLocal` 值之前调用；安装前写入的值不会迁移。
 */
inline bool InstallErrorLocalStorage(const ErrorLocalStorage* storage) noexcept {
    return detail::InstallStorage(storage);
}

/// @brief 当前 `ErrorLocal` 后端类型（诊断用）。
inline ErrorLocalBackendKind CurrentErrorLocalBackend() noexcept {
    return detail::CurrentStorage().kind;
}

/**
 * @brief 随执行上下文走的错误工作状态槽。
 *
 * @tparam T 值类型；必须可析构、可移动/构造（`GetOrCreate` 参数转发给构造函数）。
 *
 * @details 同一线程/Fiber 内多次调用返回同一个对象；`Reset()` 后下一次
 *          `GetOrCreate()` 重新构造。槽对象本身很轻（一个 `uint64` 键），
 *          但**必须活得比使用它的值更久**（通常是函数局部静态/成员）。
 */
template <typename T>
class ErrorLocal final {
    static_assert(!std::is_void<T>::value, "ErrorLocal<void> 不合法");

 public:
    ErrorLocal() noexcept : m_slot(detail::AllocateSlot()) {}
    ~ErrorLocal() = default;

    ErrorLocal(const ErrorLocal&) = delete;
    ErrorLocal& operator=(const ErrorLocal&) = delete;
    ErrorLocal(ErrorLocal&&) = delete;
    ErrorLocal& operator=(ErrorLocal&&) = delete;

    /**
     * @brief 读取当前上下文的值；不存在时用参数构造并保存。
     * @return 值的引用；在本次调用返回后仍由后端持有，直到 `Reset()` 或
     *         上下文（线程/Fiber）结束。
     */
    template <typename... Args>
    T& GetOrCreate(Args&&... args) {
        const ErrorLocalStorage& storage = detail::CurrentStorage();
        if (const std::shared_ptr<void> existing = storage.get(m_slot)) {
            return *static_cast<T*>(existing.get());
        }
        std::shared_ptr<T> value = std::make_shared<T>(std::forward<Args>(args)...);
        T* const raw = value.get();
        storage.set(m_slot, std::move(value));
        return *raw;
    }

    /// @brief 读取当前上下文的值；不存在返回空指针。
    T* TryGet() const noexcept {
        const std::shared_ptr<void> value = detail::CurrentStorage().get(m_slot);
        return value ? static_cast<T*>(value.get()) : nullptr;
    }

    /// @brief 清除当前上下文的值（不影响其他线程/Fiber）。
    void Reset() noexcept { detail::CurrentStorage().reset(m_slot); }

    /// @brief 槽位 ID（仅用于诊断/日志）。
    detail::ErrorLocalSlot SlotId() const noexcept { return m_slot; }

 private:
    const detail::ErrorLocalSlot m_slot; ///< 构造时分配，永不复用。
};

} // namespace go2cpp::error
