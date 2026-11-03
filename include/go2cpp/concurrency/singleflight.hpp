/**
 * @file singleflight.hpp
 * @brief 同 key 并发调用合并（single-flight），语义对齐 Go `groupcache/singleflight`。
 *
 * @details 对同一 key 的并发 `Do` 只会执行一次 `fn`，其余调用方阻塞等待并共享
 *          同一次执行的结果。这一原语是 groupcache 防止「缓存击穿」的核心：
 *          N 个请求同时未命中同一个 key 时，只有 1 个真正回源，其余复用结果。
 *
 * ## 与 Go `groupcache/singleflight` 的对应关系
 *
 * | Go | 本实现 |
 * |---|---|
 * | `Group.Do(key, fn)` | `Group::Do(key, fn)` |
 * | `Group.Forget(key)` | `Group::Forget(key)` |
 * | `len(g.m)`（无公开 API） | `Group::Size()` |
 * | `fn` 的 panic 转为 error 再抛给调用方 | `fn` 的异常转为 `ErrorPtr` 返回 |
 *
 * ## 与上游的两处有意差异
 *
 * 1. **返回值**：Go 的 `Do` 返回 `(interface{}, error, shared bool)`，其中
 *    `shared` 是 `c.dups > 0`，因而**执行者自己**在有人合并时也会收到
 *    `shared=true`。本实现按任务约定改为「是否被合并」：执行者返回
 *    `shared=false`，每个等待者返回 `shared=true`。N 个并发调用方中恰好
 *    N-1 个为 true，便于直接核对合并计数。
 * 2. **异常**：Go 的 panic 无法穿过 `recover` 以外的边界；C++ 侧把
 *    `std::exception` 与未知异常都捕获成 `error::ErrorPtr`，不向调用方抛出
 *    异常，也不会让等待线程卡死。
 *
 * ## 并发模型
 *
 * - `m_calls` 只在「查表 / 插入 / 删除」时持有 `m_mu`；`fn` 永远在锁外执行。
 * - 每个在途 key 对应一个 `Call`，内含自己的 `mutex`/`condition_variable`；
 *   等待者阻塞在 `Call` 上，不占用 `m_mu`。
 * - `Call` 由 `shared_ptr` 持有：执行者完成并删除表项后，已经拿到 `Call`
 *   的等待者仍能安全读取结果，不存在悬垂引用。
 * - key 只在首次未命中时复制进 `std::string`（表项持有）；命中路径用
 *   `std::string_view` 经透明比较器直接查找，不构造临时 `std::string`。
 *
 * ## 异常与重试
 *
 * `fn` 抛出异常时，执行者把异常文本转成 `error::ErrorPtr`，写入共享
 * `Call` 后唤醒全部等待者；表项随即删除，因此下一次同 key 调用会重新
 * 执行 `fn`（不会缓存错误）。
 *
 * @author DeepSeek Harness
 * @date 2026-11-03
 * @copyright BSD-3-Clause（语义对照 Go `groupcache/singleflight`，实现独立完成）
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>

#include "go2cpp/error/error.hpp"

namespace go2cpp {
namespace concurrency {

/**
 * @brief 一次 `Do` 调用的类型擦除结果。
 *
 * @details `Do` 是模板，执行者与等待者可能在不同线程上返回同一份结果，因此
 *          需要一个统一的返回类型。`Result` 用 `shared_ptr<const Concept>`
 *          持有任意可拷贝值，拷贝 `Result` 只增加引用计数；`Get<T>()` 在
 *          类型不匹配时抛 `std::bad_cast`，`TryGet<T>()` 返回空指针。
 *
 *          空 `Result`（`HasValue() == false`）表示 `fn` 返回 `void`，或
 *          `fn` 抛出了异常（此时错误在 `Do` 的第三个返回值里）。
 *
 * @note `Result` 不可变：`From` 之后没有任何修改接口，因此多个等待者可以
 *       并发调用 `Get`/`TryGet`。
 */
class Result {
 public:
    Result() noexcept = default;
    Result(const Result&) = default;
    Result(Result&&) noexcept = default;
    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) noexcept = default;

    /**
     * @brief 用任意值构造结果（值按 decay 后拷贝/移动进内部持有者）。
     * @tparam T 值类型；`Result` 自身被禁用，避免嵌套。
     * @param value 要共享的值。
     */
    template <typename T, typename U = std::decay_t<T>,
              typename = std::enable_if_t<!std::is_same_v<U, Result>>>
    static Result From(T&& value) {
        return Result(std::make_shared<const Model<U>>(std::forward<T>(value)));
    }

    /// 是否持有值；`fn` 返回 `void` 或抛出异常时为 false。
    bool HasValue() const noexcept { return m_model != nullptr; }

    /**
     * @brief 取回 `T` 类型的值。
     * @tparam T 期望类型（必须与 `From` 时 decay 后的类型完全一致）。
     * @throw std::bad_cast 结果为空或类型不匹配。
     */
    template <typename T>
    const T& Get() const {
        const T* value = TryGet<T>();
        if (value == nullptr) {
            throw std::bad_cast();
        }
        return *value;
    }

    /// 取回 `T` 类型值的指针；结果为空或类型不匹配时返回 nullptr。
    template <typename T>
    const T* TryGet() const noexcept {
        const auto* model = dynamic_cast<const Model<T>*>(m_model.get());
        return model == nullptr ? nullptr : &model->value;
    }

 private:
    /// 类型擦除的接口；虚析构保证 `shared_ptr<const Concept>` 能正确释放。
    struct Concept {
        virtual ~Concept() = default;
    };

    /// 具体持有者；`value` 构造后不再修改。
    template <typename T>
    struct Model final : Concept {
        template <typename U>
        explicit Model(U&& source) : value(std::forward<U>(source)) {}
        T value;
    };

    explicit Result(std::shared_ptr<const Concept> model) noexcept
        : m_model(std::move(model)) {}

    std::shared_ptr<const Concept> m_model;  ///< 共享的不可变结果；空表示无值
};

/**
 * @brief 同 key 并发调用合并组。
 *
 * @details 一个 `Group` 对应 groupcache 里的一个 `singleflight.Group`，
 *          通常按「后端 peer」或「缓存命名空间」各持有一个。实例可被多线程
 *          并发调用；不可拷贝（内部有互斥量）。
 */
class Group {
 public:
    Group() = default;
    ~Group() = default;

    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
    Group(Group&&) = delete;
    Group& operator=(Group&&) = delete;

    /**
     * @brief 对 `key` 执行 `fn`；相同 key 的并发调用合并为一次执行。
     *
     * @tparam Fn 可调用类型，签名为 `R()`，`R` 可为 `void`。
     * @param key 合并键。只在 `Do` 返回前被读取；未命中时会复制进内部表。
     * @param fn 真正的工作函数；只在执行者线程上调用一次（每次合并窗口）。
     * @return `(结果, shared, 错误)`：
     *         - `shared == false`：本调用是执行者；
     *         - `shared == true`：本调用被合并到别人的执行上；
     *         - `fn` 抛异常时 `错误` 非空、`结果` 为空；否则 `错误` 为空。
     *
     * @note `fn` 在 `m_mu` 之外执行，内部可以再次调用同一个 `Group` 的
     *       `Do`；只要不形成同 key 的自等待，就不会死锁。同 key 重入会
     *       等待自己，属于调用方错误。
     */
    template <typename Fn>
    std::tuple<Result, bool, error::ErrorPtr> Do(std::string_view key, Fn&& fn) {
        using Return = std::invoke_result_t<Fn>;

        CallPtr call;
        bool merged = false;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            auto it = m_calls.find(key);
            if (it != m_calls.end()) {
                call = it->second;
                ++call->dups;
                merged = true;
            } else {
                call = std::make_shared<Call>();
                m_calls.emplace(std::string(key), call);
            }
        }

        // 等待者：阻塞在自己的 Call 上，共享执行者的结果/错误。
        if (merged) {
            std::unique_lock<std::mutex> lock(call->mu);
            call->cv.wait(lock, [&call] { return call->done; });
            return std::make_tuple(call->result, true, call->err);
        }

        // 执行者：在锁外运行 fn，异常一律转成错误值。
        Result result;
        error::ErrorPtr error;
        try {
            if constexpr (std::is_void_v<Return>) {
                std::invoke(std::forward<Fn>(fn));
            } else {
                result = Result::From(std::invoke(std::forward<Fn>(fn)));
            }
        } catch (const std::exception& exception) {
            error = MakeError(std::string("singleflight: ") + exception.what());
        } catch (...) {
            error = MakeError("singleflight: unknown exception");
        }

        // 先发布结果并唤醒等待者，再摘除表项：与 Go 的 defer 顺序一致
        // （wg.Done() 在前、delete(g.m, key) 在后）。
        {
            std::lock_guard<std::mutex> lock(call->mu);
            call->result = result;
            call->err = error;
            call->done = true;
        }
        call->cv.notify_all();
        {
            std::lock_guard<std::mutex> lock(m_mu);
            auto it = m_calls.find(key);
            if (it != m_calls.end() && it->second == call) {
                m_calls.erase(it);
            }
        }
        return std::make_tuple(std::move(result), false, std::move(error));
    }

    /// 从合并表中删除 `key`（不等待在途执行）；对应 Go 的 `Group.Forget`。
    void Forget(std::string_view key) {
        std::lock_guard<std::mutex> lock(m_mu);
        auto it = m_calls.find(key);
        if (it != m_calls.end()) {
            m_calls.erase(it);
        }
    }

    /// 当前在途（尚未完成/尚未摘除）的 key 数量。
    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(m_mu);
        return m_calls.size();
    }

 private:
    /// 一个在途 key 的共享状态；等待者通过 `shared_ptr` 保活。
    struct Call {
        std::mutex mu;                    ///< 保护 done/result/err，并配合 cv
        std::condition_variable cv;       ///< 完成通知
        bool done = false;                ///< 结果是否已发布
        Result result;                    ///< 执行者的返回值（可能为空）
        error::ErrorPtr err;              ///< 执行者捕获的异常（可能为空）
        std::size_t dups = 0;             ///< 被合并的次数（仅用于诊断/Size 语义）
    };

    using CallPtr = std::shared_ptr<Call>;

    /// 透明比较器：允许用 `std::string_view` 直接查找 `std::string` 键，
    /// 命中路径不构造临时字符串。
    struct StringViewLess {
        using is_transparent = void;
        bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
            return lhs < rhs;
        }
    };

    /// 把异常文本转成不可变错误值；`StringError` 是全内联的叶子错误。
    static error::ErrorPtr MakeError(std::string message) {
        return std::make_shared<error::StringError>(std::move(message));
    }

    mutable std::mutex m_mu;  ///< 保护 m_calls
    std::map<std::string, CallPtr, StringViewLess> m_calls;  ///< 在途调用表
};

}  // namespace concurrency
}  // namespace go2cpp
