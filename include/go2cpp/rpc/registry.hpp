#pragma once

/**
 * @file registry.hpp
 * @brief 服务注册表 + 方法绑定：把 C++ 成员函数变成可按名字调用的 RPC 方法。
 *
 * @details
 * 本文件解决「Go `net/rpc` 的 `Register(receiver)` 在 C++ 里怎么落」这个问题。
 * C++ 没有运行期方法枚举，所以方法表必须显式列出；为了让注册仍然是一行，
 * 用 **ADL 重载 + 宏**：
 *
 * ```cpp
 * struct UserService {
 *     User Get(const UserId& id);
 *     Status Create(const CreateRequest& req, User* out);
 *     Status Subscribe(const Topic& topic, ServerStream* stream);  // 流式
 * };
 *
 * GO2CPP_RPC_SERVICE(UserService, Get, Create, Subscribe)   // 在 UserService 所在命名空间
 *
 * rpc::Server server;
 * server.Register("User", &service);   // 名字 "User"，方法名 "Get"/"Create"/"Subscribe"
 * ```
 *
 * 宏展开成一个 `RpcMethods(UserService*)` 重载，`AddService` 用**无限定调用**
 * 找到它（ADL）。因此宏可以写在服务类型自己的命名空间里，不需要全局特化，
 * 也不会和别的服务冲突。
 *
 * ## 支持的 handler 签名
 *
 * | 形态 | 示例 | 线上行为 |
 * |---|---|---|
 * | 值返回 | `User Get(const UserId&)` | 返回值序列化为 result |
 * | 输出参数 | `Status Create(const Req&, User* out)` | `out` 序列化为 result |
 * | 仅状态 | `Status Remove(const Id&)` | result 为空 |
 * | 仅动作 | `void Touch(const Id&)` | result 为空 |
 * | 服务端流 | `Status Watch(const Req&, ServerStream*)` | 逐条 `StreamData`，结束发 `StreamEnd` |
 *
 * `const` 成员函数同样支持（`std::invoke` 统一处理）。输入参数按注册时的
 * 类型依次从请求字节流反序列化；参数个数由签名决定（`MethodBinding::argc`）。
 *
 * ## 线程模型
 *
 * - `AddService` / `AddBindings` 只在 `Listen` 之前调用（`Server::Listen`
 *   会 `Freeze()`，之后注册返回 false）；
 * - `Find` **无锁**：注册完成后表只读，多连接线程并发查找安全。这是刻意的：
 *   每次 RPC 都要查一次表，加锁会把注册表变成串行点。
 *
 * ## 名字规则
 *
 * 完整方法名是 `"服务名.方法名"`，例如 `"User.Get"`。服务名 1..64 字节，
 * 方法名 1..64 字节，只允许 `[A-Za-z0-9_.-]` 之外的字符时返回 false（拒绝
 * 空格/斜杠，避免日志与路由歧义）。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "go2cpp/reflect/buffer.hpp"
#include "go2cpp/reflect/serializer_impl.hpp"
#include "go2cpp/rpc/status.hpp"

namespace go2cpp::rpc {

class ServerStream;

/**
 * @brief 单次调用的上下文：调用 id、方法名、取消标志。
 *
 * @details 由服务端在调用 handler 前构造，**只在本次 handler 执行期间有效**；
 *          handler 不能把它存起来跨调用使用（内部全是裸指针）。
 *          `cancelled()` 读取的是连接读线程置位的原子标志，可安全跨线程轮询。
 */
class CallContext {
   public:
    /// @brief 空上下文（仅测试/默认构造用）。
    CallContext() = default;

    /**
     * @brief 构造上下文。
     * @param call_id 调用 id（同一条连接内唯一）。
     * @param method 完整方法名指针（生命周期覆盖 handler）。
     * @param cancelled 取消标志指针（生命周期覆盖 handler）。
     */
    CallContext(std::uint64_t call_id, const std::string* method,
                const std::atomic<bool>* cancelled)
        : m_id(call_id), m_method(method), m_cancelled(cancelled) {}

    /// @brief 调用 id。
    std::uint64_t id() const noexcept { return m_id; }

    /// @brief 完整方法名（`"User.Get"`）；空上下文返回空视图。
    std::string_view method() const noexcept {
        return m_method != nullptr ? std::string_view(*m_method) : std::string_view();
    }

    /// @brief 对端是否已请求取消（流式 handler 应周期性检查）。
    bool cancelled() const noexcept {
        return m_cancelled != nullptr && m_cancelled->load(std::memory_order_relaxed);
    }

   private:
    std::uint64_t m_id = 0;                        ///< 调用 id。
    const std::string* m_method = nullptr;         ///< 方法名（不拥有）。
    const std::atomic<bool>* m_cancelled = nullptr; ///< 取消标志（不拥有）。
};

namespace detail {

/// @brief 依赖模板参数的恒假值（static_assert 用，避免非依赖断言提前触发）。
template <typename T>
inline constexpr bool kDependentFalse = false;

/// @brief 尾参数是否为 `ServerStream*` / `ServerStream&`。
template <typename T>
struct IsServerStreamArg : std::false_type {};
template <>
struct IsServerStreamArg<ServerStream*> : std::true_type {};
template <>
struct IsServerStreamArg<ServerStream&> : std::true_type {};
/// @brief 尾参数是否为 `ServerStream&`（决定传引用还是指针）。
template <typename T>
struct IsServerStreamRef : std::false_type {};
template <>
struct IsServerStreamRef<ServerStream&> : std::true_type {};

/**
 * @brief 输出参数识别：`T*` 且 `T` 非 const、非 `ServerStream`。
 * @tparam T 待检查类型。
 */
template <typename T>
struct OutPointer {
    static constexpr bool kValue = false;  ///< 是否输出参数。
    using Value = void;                    ///< 输出值类型。
};
template <typename T>
struct OutPointer<T*> {
    static constexpr bool kValue = !std::is_const_v<T> && !std::is_same_v<T, ServerStream>;
    using Value = std::remove_cv_t<T>;
};

/**
 * @brief 成员函数指针的签名分解。
 * @tparam T 成员函数指针类型。
 */
template <typename T>
struct MethodSignature;

template <typename R, typename C, typename... A>
struct MethodSignature<R (C::*)(A...)> {
    using Class = C;                              ///< 服务类型。
    using Return = R;                             ///< 返回类型。
    using Args = std::tuple<A...>;                ///< 参数类型列表。
    static constexpr std::size_t kArity = sizeof...(A);  ///< 参数个数。
};

template <typename R, typename C, typename... A>
struct MethodSignature<R (C::*)(A...) const> : MethodSignature<R (C::*)(A...)> {};

/// @brief 取 tuple 前 N 个类型。
template <typename Tuple, typename IndexSequence>
struct TuplePrefixImpl;
template <typename Tuple, std::size_t... I>
struct TuplePrefixImpl<Tuple, std::index_sequence<I...>> {
    using Type = std::tuple<std::tuple_element_t<I, Tuple>...>;
};
template <typename Tuple, std::size_t N>
using TuplePrefix = typename TuplePrefixImpl<Tuple, std::make_index_sequence<N>>::Type;

/**
 * @brief 把 tuple 里每个类型退化为值类型。
 *
 * @details handler 参数常写 `const UserId&`，但 `std::tuple<const UserId&>`
 *          不可默认构造、也不能承载反序列化结果。这里统一去掉引用/const，
 *          调用时把左值绑定回 handler 的 `const T&` 形参。
 */
template <typename Tuple, typename IndexSequence>
struct TupleDecayImpl;
template <typename Tuple, std::size_t... I>
struct TupleDecayImpl<Tuple, std::index_sequence<I...>> {
    using Type = std::tuple<std::decay_t<std::tuple_element_t<I, Tuple>>...>;
};
template <typename Tuple>
using TupleDecay = typename TupleDecayImpl<
    Tuple, std::make_index_sequence<std::tuple_size<Tuple>::value>>::Type;

/**
 * @brief 取 tuple 第 N 个类型（从 1 开始数）；N 为 0 时给 `void`。
 * @tparam Tuple 类型列表。
 * @tparam N 位置（1..size）。
 */
template <typename Tuple, std::size_t N, bool = (N > 0)>
struct TupleElementAt {
    using Type = void;  ///< 越界时的占位。
};
template <typename Tuple, std::size_t N>
struct TupleElementAt<Tuple, N, true> {
    using Type = std::tuple_element_t<N - 1, Tuple>;
};

/**
 * @brief 解码一个带 varint 长度前缀的参数。
 *
 * @details 客户端按「varint 长度 + 序列化字节」逐个打包参数（见
 *          `detail::PackArgument`），服务端必须按同一格式拆开，否则会把
 *          长度前缀当成第一个字段读。子 Buffer 独立解码后要求恰好读完，
 *          这样参数内部多写/少写字节都会被拒绝。
 *
 * @param in 参数字节流。
 * @param value 目标值。
 * @return 成功 true；长度非法/解码失败/尾随字节返回 false（`in` 被标记失败）。
 */
template <typename T>
bool DecodeArgument(reflect::Buffer& in, T& value) {
    std::uint64_t length = 0;
    if (!in.GetVarint(length)) {
        return false;
    }
    if (length > in.Remaining()) {
        in.Fail();
        return false;
    }
    reflect::Buffer sub;
    if (length > 0 && !sub.PutBytes(in.Cursor(), static_cast<std::size_t>(length))) {
        in.Fail();
        return false;
    }
    if (!reflect::Deserialize(sub, value) || sub.Remaining() != 0) {
        in.Fail();
        return false;
    }
    return in.Skip(static_cast<std::size_t>(length));
}

/**
 * @brief 把参数区依次反序列化到 tuple。
 * @param in 参数字节流。
 * @param values 目标 tuple。
 * @param indices 展开用的下标序列。
 * @return 全部成功返回 true；任一失败返回 false（`in` 已被标记失败）。
 */
template <typename Tuple, std::size_t... I>
bool DecodeArguments(reflect::Buffer& in, Tuple& values, std::index_sequence<I...>) {
    bool ok = true;
    ((ok = ok && DecodeArgument(in, std::get<I>(values))), ...);
    return ok;
}

/**
 * @brief 单个 handler 的类型擦除绑定器。
 *
 * @details 这个类模板是 RPC 的核心适配层：它从成员函数指针推导参数/返回类型，
 *          生成一个签名固定的 `Invoke` 函数指针，服务端只按这个签名调用。
 *          所有分支都在编译期用 `if constexpr` 展开，运行期没有额外分支。
 *
 * @tparam M 成员函数指针（`&Service::Method`）。
 */
template <auto M>
struct MethodBinder {
    using Signature = MethodSignature<decltype(M)>;
    using Class = typename Signature::Class;
    using Return = typename Signature::Return;
    using Args = typename Signature::Args;
    static constexpr std::size_t kArity = Signature::kArity;

    /// 尾参数是否为流式参数。
    using LastArg = typename TupleElementAt<Args, kArity>::Type;
    static constexpr bool kStreaming = IsServerStreamArg<LastArg>::value;
    /// 去掉流式参数后的参数个数。
    static constexpr std::size_t kTailAdjusted = kArity - (kStreaming ? 1u : 0u);
    /// 去掉流式参数后的末位参数（可能是输出指针）。
    using AdjustedLast = typename TupleElementAt<Args, kTailAdjusted>::Type;
    /// 是否有输出参数（`T*`，且不是流式方法）。
    static constexpr bool kHasOut = !kStreaming && OutPointer<AdjustedLast>::kValue;
    /// 输入参数个数（去掉尾部的流式/输出参数）。
    static constexpr std::size_t kInputCount = kTailAdjusted - (kHasOut ? 1u : 0u);
    /// 输入参数类型 tuple（引用/const 已退化为值类型）。
    using InputArgs = TupleDecay<TuplePrefix<Args, kInputCount>>;
    /// 输出值类型（无输出参数时为 void）。
    using OutValue = typename OutPointer<AdjustedLast>::Value;
    /// 线上参数个数（u8）。
    static constexpr std::size_t kArgc = kInputCount;

    /**
     * @brief 类型擦除的调用入口。
     * @param service 服务对象指针（`Class*`）。
     * @param context 调用上下文（取消标志等）。
     * @param args 参数区（按注册签名依次反序列化）。
     * @param result 结果输出缓冲（handler 返回值/输出参数写这里）。
     * @param stream 流式发送器（`ServerStream*`，非流式方法为 nullptr）。
     * @return 执行状态；handler 返回的 `Status` 原样透传。
     */
    static Status Invoke(void* service, CallContext& context, reflect::Buffer& args,
                         reflect::Buffer& result, void* stream) {
        (void)context;
        auto* object = static_cast<Class*>(service);
        if (object == nullptr) {
            return Status(StatusCode::kInternal, "null service instance");
        }
        InputArgs inputs{};
        if (!DecodeArguments(args, inputs, std::make_index_sequence<kInputCount>{})) {
            return Status(StatusCode::kInvalidArgument, "request argument decode failed");
        }
        if (args.Remaining() != 0) {
            return Status(StatusCode::kInvalidArgument,
                          "trailing bytes after request arguments");
        }
        if constexpr (kStreaming) {
            if (stream == nullptr) {
                return Status(StatusCode::kInternal, "streaming handler without stream");
            }
            return InvokeStreaming(object, std::move(inputs), static_cast<ServerStream*>(stream));
        } else if constexpr (kHasOut) {
            return InvokeWithOutput(object, std::move(inputs), result);
        } else {
            return InvokePlain(object, std::move(inputs), result);
        }
    }

   private:
    /// @brief 按尾参数形态构造流式 tail（指针或引用）。
    template <typename StreamTail>
    static StreamTail MakeStreamTail(ServerStream* stream) {
        if constexpr (IsServerStreamRef<LastArg>::value) {
            return StreamTail(*stream);
        } else {
            return StreamTail(stream);
        }
    }

    /// @brief 无输出参数：返回值直接序列化。
    template <typename Inputs>
    static Status InvokePlain(Class* object, Inputs inputs, reflect::Buffer& result) {
        if constexpr (std::is_void_v<Return>) {
            std::apply(
                [object](auto&&... xs) {
                    std::invoke(M, object, std::forward<decltype(xs)>(xs)...);
                },
                inputs);
            return Status::Ok();
        } else if constexpr (std::is_same_v<std::decay_t<Return>, Status>) {
            return std::apply(
                [object](auto&&... xs) -> Status {
                    return std::invoke(M, object, std::forward<decltype(xs)>(xs)...);
                },
                inputs);
        } else {
            std::decay_t<Return> value = std::apply(
                [object](auto&&... xs) -> std::decay_t<Return> {
                    return std::invoke(M, object, std::forward<decltype(xs)>(xs)...);
                },
                inputs);
            if (!reflect::Serialize(value, result)) {
                return Status(StatusCode::kInternal, "failed to serialize handler result");
            }
            return Status::Ok();
        }
    }

    /// @brief 有输出参数：handler 写 `out`，成功后序列化 `out`。
    template <typename Inputs>
    static Status InvokeWithOutput(Class* object, Inputs inputs, reflect::Buffer& result) {
        static_assert(std::is_void_v<Return> || std::is_same_v<std::decay_t<Return>, Status>,
                      "handler with output parameter must return Status or void");
        OutValue out{};
        auto full = std::tuple_cat(std::move(inputs), std::tuple<OutValue*>{&out});
        Status status;
        if constexpr (std::is_void_v<Return>) {
            std::apply(
                [object](auto&&... xs) {
                    std::invoke(M, object, std::forward<decltype(xs)>(xs)...);
                },
                full);
            status = Status::Ok();
        } else {
            status = std::apply(
                [object](auto&&... xs) -> Status {
                    return std::invoke(M, object, std::forward<decltype(xs)>(xs)...);
                },
                full);
        }
        if (!status.ok()) {
            return status;
        }
        if (!reflect::Serialize(out, result)) {
            return Status(StatusCode::kInternal, "failed to serialize handler output");
        }
        return Status::Ok();
    }

    /// @brief 流式：handler 通过 `ServerStream` 逐条发送，最终状态进 StreamEnd。
    template <typename Inputs>
    static Status InvokeStreaming(Class* object, Inputs inputs, ServerStream* stream) {
        static_assert(std::is_void_v<Return> || std::is_same_v<std::decay_t<Return>, Status>,
                      "streaming handler must return Status or void");
        using StreamTail = std::conditional_t<IsServerStreamRef<LastArg>::value,
                                              std::tuple<ServerStream&>,
                                              std::tuple<ServerStream*>>;
        StreamTail tail = MakeStreamTail<StreamTail>(stream);
        auto full = std::tuple_cat(std::move(inputs), std::move(tail));
        if constexpr (std::is_void_v<Return>) {
            std::apply(
                [object](auto&&... xs) {
                    std::invoke(M, object, std::forward<decltype(xs)>(xs)...);
                },
                full);
            return Status::Ok();
        } else {
            return std::apply(
                [object](auto&&... xs) -> Status {
                    return std::invoke(M, object, std::forward<decltype(xs)>(xs)...);
                },
                full);
        }
    }
};

/// @brief 方法表缺失时的占位类型（让 `if constexpr` 能给出清晰静态断言）。
struct NoMethods {};

}  // namespace detail

/**
 * @brief 一个 RPC 方法的类型擦除描述。
 *
 * @details `name` 必须指向静态存储的字符串字面量（`Method<M>("Get")` 的
 *          `"Get"`），注册表只保存指针、不拷贝短名。
 */
struct MethodBinding {
    const char* name = nullptr;  ///< 短方法名（静态存储）。
    std::uint8_t argc = 0;       ///< 输入参数个数。
    bool streaming = false;      ///< 是否服务端流式方法。
    /// 调用入口；`stream` 仅在 `streaming` 时非空。
    Status (*invoke)(void* service, CallContext& context, reflect::Buffer& args,
                     reflect::Buffer& result, void* stream) = nullptr;
};

/**
 * @brief 构造方法绑定。
 * @tparam M 成员函数指针，例如 `&UserService::Get`。
 * @param name 短方法名（字符串字面量）。
 * @return 类型擦除的方法描述。
 */
template <auto M>
MethodBinding Method(const char* name) {
    static_assert(detail::MethodBinder<M>::kArgc <= 255, "too many rpc input arguments");
    MethodBinding binding;
    binding.name = name;
    binding.argc = static_cast<std::uint8_t>(detail::MethodBinder<M>::kArgc);
    binding.streaming = detail::MethodBinder<M>::kStreaming;
    binding.invoke = &detail::MethodBinder<M>::Invoke;
    return binding;
}

/**
 * @brief 方法表兜底：没有 `RpcMethods(Service*)` 重载时返回 `NoMethods`。
 *
 * @details 用 C 可变参数是为了让 ADL 找到的精确重载永远优先；这个兜底只
 *          负责把「忘记写 `GO2CPP_RPC_SERVICE`」变成一条清晰的静态断言。
 */
inline detail::NoMethods RpcMethods(...) { return {}; }

/**
 * @brief 已注册方法：服务对象 + 方法描述 + 完整名。
 */
struct BoundMethod {
    std::string full_name;   ///< `"User.Get"`。
    void* service = nullptr; ///< 服务对象指针（用户保证生命周期覆盖 Server）。
    MethodBinding binding;   ///< 方法描述（`name` 仍指向短名字面量）。
};

/**
 * @brief 服务注册表：完整方法名 → 已绑定方法。
 *
 * @details 开放寻址哈希表（2 的幂容量、线性探测、FNV-1a 64），
 *          查找路径**无锁、无分配**；注册路径加锁并禁止 `Freeze()` 后写入。
 *          表项存在 `std::deque` 里保证指针稳定，哈希表只存指针。
 */
class ServiceRegistry {
   public:
    ServiceRegistry() = default;
    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;
    ServiceRegistry(ServiceRegistry&&) = delete;
    ServiceRegistry& operator=(ServiceRegistry&&) = delete;

    /**
     * @brief 注册一个服务（模板版，方法表由 `GO2CPP_RPC_SERVICE` 提供）。
     * @tparam Service 服务类型（非 const）。
     * @param service_name 服务名（`"User"`）。
     * @param service 服务对象指针（生命周期必须覆盖 Server）。
     * @return 成功 true；重名/非法名/表为空 false。
     */
    template <typename Service>
    bool AddService(std::string_view service_name, Service* service) {
        static_assert(!std::is_const_v<Service>, "rpc service must be non-const");
        auto table = RpcMethods(service);
        if constexpr (std::is_same_v<std::decay_t<decltype(table)>, detail::NoMethods>) {
            static_assert(detail::kDependentFalse<Service>,
                          "no rpc method table: use GO2CPP_RPC_SERVICE(Service, Method...) "
                          "in the service namespace");
            return false;
        } else {
            return AddMethods(service_name, static_cast<void*>(service), table,
                              std::make_index_sequence<
                                  std::tuple_size<std::decay_t<decltype(table)>>::value>{});
        }
    }

    /**
     * @brief 注册一组显式方法（不依赖宏）。
     * @param service_name 服务名。
     * @param service 服务对象指针。
     * @param bindings 方法描述数组。
     * @param count 数组长度（必须 > 0）。
     * @return 成功 true；任一名字非法/重复返回 false（不产生部分注册）。
     */
    bool AddBindings(std::string_view service_name, void* service,
                     const MethodBinding* bindings, std::size_t count);

    /**
     * @brief 按完整名查找（无锁）。
     * @param full_name `"User.Get"`。
     * @return 找到返回稳定指针；未注册返回 nullptr。
     */
    const BoundMethod* Find(std::string_view full_name) const noexcept;

    /// @brief 冻结注册表：之后所有注册失败（`Server::Listen` 调用）。
    void Freeze() noexcept;

    /// @brief 是否已冻结。
    bool frozen() const noexcept { return m_frozen.load(std::memory_order_acquire); }

    /// @brief 已注册方法数。
    std::size_t size() const noexcept { return m_count; }

    /**
     * @brief 全部完整方法名（字典序，诊断/测试用）。
     * @return 名字列表。
     */
    std::vector<std::string> MethodNames() const;

   private:
    /// 哈希表槽位；`method == nullptr` 表示空槽。
    struct Slot {
        std::uint64_t hash = 0;           ///< 预计算的完整名哈希。
        const BoundMethod* method = nullptr;  ///< 指向 `m_methods` 中的条目。
    };

    /// 展开 tuple 形式的方法表，转成数组后走 `AddBindings`。
    template <typename Tuple, std::size_t... I>
    bool AddMethods(std::string_view service_name, void* service, const Tuple& table,
                    std::index_sequence<I...>) {
        std::array<MethodBinding, sizeof...(I)> bindings{std::get<I>(table)...};
        return AddBindings(service_name, service, bindings.data(), bindings.size());
    }

    /// FNV-1a 64 位哈希（查找路径热点，见 docs/modules/rpc.md §6）。
    static std::uint64_t Hash(std::string_view text) noexcept;
    /// 名字是否合法（非空、长度上限、字符集）。
    static bool ValidName(std::string_view text) noexcept;
    /// 在无锁状态下查一个完整名（注册时查重）。
    const BoundMethod* FindUnlocked(std::string_view full_name) const noexcept;
    /// 扩容并重插全部槽位。
    void Rehash(std::size_t capacity);
    /// 插入一个已构造的条目（调用方保证不重复且容量足够）。
    void InsertUnlocked(const BoundMethod* method);

    mutable std::mutex m_mutex;          ///< 保护注册路径（查找不加锁）。
    std::deque<BoundMethod> m_methods;   ///< 方法条目（指针稳定）。
    std::vector<Slot> m_slots;           ///< 开放寻址哈希表。
    std::size_t m_count = 0;             ///< 已注册条目数。
    std::atomic<bool> m_frozen{false};   ///< 冻结标志（查找路径不读它）。
};

/**
 * @brief 定义服务的方法表（ADL 重载）。
 *
 * 用法：在服务类型所在命名空间（或全局）展开一次，最多 8 个方法：
 * @code
 * GO2CPP_RPC_SERVICE(UserService, Get, Create, Remove)
 * @endcode
 *
 * 宏为每个方法生成 `Method<&Service::方法名>("方法名")`，因此方法名必须
 * 是合法标识符；需要不同线上名字时改用 `Method<&Service::M>("Other")` +
 * `server.Register(name, service, { ... })` 的显式形式。
 */
#define GO2CPP_RPC_DETAIL_ENTRY(ServiceType, MethodName) \
    ::go2cpp::rpc::Method<&ServiceType::MethodName>(#MethodName)

#define GO2CPP_RPC_DETAIL_ENTRY_1(S, a) GO2CPP_RPC_DETAIL_ENTRY(S, a)
#define GO2CPP_RPC_DETAIL_ENTRY_2(S, a, b) \
    GO2CPP_RPC_DETAIL_ENTRY(S, a), GO2CPP_RPC_DETAIL_ENTRY(S, b)
#define GO2CPP_RPC_DETAIL_ENTRY_3(S, a, b, c) \
    GO2CPP_RPC_DETAIL_ENTRY(S, a), GO2CPP_RPC_DETAIL_ENTRY(S, b), GO2CPP_RPC_DETAIL_ENTRY(S, c)
#define GO2CPP_RPC_DETAIL_ENTRY_4(S, a, b, c, d)                     \
    GO2CPP_RPC_DETAIL_ENTRY(S, a), GO2CPP_RPC_DETAIL_ENTRY(S, b),    \
        GO2CPP_RPC_DETAIL_ENTRY(S, c), GO2CPP_RPC_DETAIL_ENTRY(S, d)
#define GO2CPP_RPC_DETAIL_ENTRY_5(S, a, b, c, d, e)                  \
    GO2CPP_RPC_DETAIL_ENTRY(S, a), GO2CPP_RPC_DETAIL_ENTRY(S, b),    \
        GO2CPP_RPC_DETAIL_ENTRY(S, c), GO2CPP_RPC_DETAIL_ENTRY(S, d), \
        GO2CPP_RPC_DETAIL_ENTRY(S, e)
#define GO2CPP_RPC_DETAIL_ENTRY_6(S, a, b, c, d, e, f)               \
    GO2CPP_RPC_DETAIL_ENTRY(S, a), GO2CPP_RPC_DETAIL_ENTRY(S, b),    \
        GO2CPP_RPC_DETAIL_ENTRY(S, c), GO2CPP_RPC_DETAIL_ENTRY(S, d), \
        GO2CPP_RPC_DETAIL_ENTRY(S, e), GO2CPP_RPC_DETAIL_ENTRY(S, f)
#define GO2CPP_RPC_DETAIL_ENTRY_7(S, a, b, c, d, e, f, g)            \
    GO2CPP_RPC_DETAIL_ENTRY(S, a), GO2CPP_RPC_DETAIL_ENTRY(S, b),    \
        GO2CPP_RPC_DETAIL_ENTRY(S, c), GO2CPP_RPC_DETAIL_ENTRY(S, d), \
        GO2CPP_RPC_DETAIL_ENTRY(S, e), GO2CPP_RPC_DETAIL_ENTRY(S, f), \
        GO2CPP_RPC_DETAIL_ENTRY(S, g)
#define GO2CPP_RPC_DETAIL_ENTRY_8(S, a, b, c, d, e, f, g, h)         \
    GO2CPP_RPC_DETAIL_ENTRY(S, a), GO2CPP_RPC_DETAIL_ENTRY(S, b),    \
        GO2CPP_RPC_DETAIL_ENTRY(S, c), GO2CPP_RPC_DETAIL_ENTRY(S, d), \
        GO2CPP_RPC_DETAIL_ENTRY(S, e), GO2CPP_RPC_DETAIL_ENTRY(S, f), \
        GO2CPP_RPC_DETAIL_ENTRY(S, g), GO2CPP_RPC_DETAIL_ENTRY(S, h)
#define GO2CPP_RPC_DETAIL_ENTRY_9(S, a, b, c, d, e, f, g, h, i)      \
    GO2CPP_RPC_DETAIL_ENTRY(S, a), GO2CPP_RPC_DETAIL_ENTRY(S, b),    \
        GO2CPP_RPC_DETAIL_ENTRY(S, c), GO2CPP_RPC_DETAIL_ENTRY(S, d), \
        GO2CPP_RPC_DETAIL_ENTRY(S, e), GO2CPP_RPC_DETAIL_ENTRY(S, f), \
        GO2CPP_RPC_DETAIL_ENTRY(S, g), GO2CPP_RPC_DETAIL_ENTRY(S, h), \
        GO2CPP_RPC_DETAIL_ENTRY(S, i)
#define GO2CPP_RPC_DETAIL_ENTRY_10(S, a, b, c, d, e, f, g, h, i, j)  \
    GO2CPP_RPC_DETAIL_ENTRY(S, a), GO2CPP_RPC_DETAIL_ENTRY(S, b),    \
        GO2CPP_RPC_DETAIL_ENTRY(S, c), GO2CPP_RPC_DETAIL_ENTRY(S, d), \
        GO2CPP_RPC_DETAIL_ENTRY(S, e), GO2CPP_RPC_DETAIL_ENTRY(S, f), \
        GO2CPP_RPC_DETAIL_ENTRY(S, g), GO2CPP_RPC_DETAIL_ENTRY(S, h), \
        GO2CPP_RPC_DETAIL_ENTRY(S, i), GO2CPP_RPC_DETAIL_ENTRY(S, j)
#define GO2CPP_RPC_DETAIL_ENTRY_11(S, a, b, c, d, e, f, g, h, i, j, k)  \
    GO2CPP_RPC_DETAIL_ENTRY(S, a), GO2CPP_RPC_DETAIL_ENTRY(S, b),        \
        GO2CPP_RPC_DETAIL_ENTRY(S, c), GO2CPP_RPC_DETAIL_ENTRY(S, d),    \
        GO2CPP_RPC_DETAIL_ENTRY(S, e), GO2CPP_RPC_DETAIL_ENTRY(S, f),    \
        GO2CPP_RPC_DETAIL_ENTRY(S, g), GO2CPP_RPC_DETAIL_ENTRY(S, h),    \
        GO2CPP_RPC_DETAIL_ENTRY(S, i), GO2CPP_RPC_DETAIL_ENTRY(S, j),    \
        GO2CPP_RPC_DETAIL_ENTRY(S, k)
#define GO2CPP_RPC_DETAIL_ENTRY_12(S, a, b, c, d, e, f, g, h, i, j, k, l)  \
    GO2CPP_RPC_DETAIL_ENTRY(S, a), GO2CPP_RPC_DETAIL_ENTRY(S, b),           \
        GO2CPP_RPC_DETAIL_ENTRY(S, c), GO2CPP_RPC_DETAIL_ENTRY(S, d),       \
        GO2CPP_RPC_DETAIL_ENTRY(S, e), GO2CPP_RPC_DETAIL_ENTRY(S, f),       \
        GO2CPP_RPC_DETAIL_ENTRY(S, g), GO2CPP_RPC_DETAIL_ENTRY(S, h),       \
        GO2CPP_RPC_DETAIL_ENTRY(S, i), GO2CPP_RPC_DETAIL_ENTRY(S, j),       \
        GO2CPP_RPC_DETAIL_ENTRY(S, k), GO2CPP_RPC_DETAIL_ENTRY(S, l)

#define GO2CPP_RPC_DETAIL_PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, NAME, ...) NAME

#define GO2CPP_RPC_DETAIL_ENTRIES(ServiceType, ...)                                   \
    GO2CPP_RPC_DETAIL_PICK(__VA_ARGS__, GO2CPP_RPC_DETAIL_ENTRY_12,                   \
                           GO2CPP_RPC_DETAIL_ENTRY_11, GO2CPP_RPC_DETAIL_ENTRY_10,    \
                           GO2CPP_RPC_DETAIL_ENTRY_9, GO2CPP_RPC_DETAIL_ENTRY_8,      \
                           GO2CPP_RPC_DETAIL_ENTRY_7, GO2CPP_RPC_DETAIL_ENTRY_6,      \
                           GO2CPP_RPC_DETAIL_ENTRY_5, GO2CPP_RPC_DETAIL_ENTRY_4,      \
                           GO2CPP_RPC_DETAIL_ENTRY_3, GO2CPP_RPC_DETAIL_ENTRY_2,      \
                           GO2CPP_RPC_DETAIL_ENTRY_1)                                 \
    (ServiceType, __VA_ARGS__)

#define GO2CPP_RPC_SERVICE(ServiceType, ...)                                          \
    inline auto RpcMethods(ServiceType*) {                                             \
        return std::make_tuple(GO2CPP_RPC_DETAIL_ENTRIES(ServiceType, __VA_ARGS__));   \
    }

}  // namespace go2cpp::rpc
