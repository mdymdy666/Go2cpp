#pragma once

/**
 * @file registry.hpp
 * @brief 错误码注册表与错误类型反射表。
 *
 * @details 两套注册表解决两个不同问题：
 *
 * - `ErrorCodeRegistry`：把“手写枚举值”换成运行期分配的稳定整数码。码从 1
 *   递增，0 永久保留给 `kOk`；同名重复注册幂等。`Name()` 返回的
 *   `string_view` 指向内部 `deque`，后续注册不会让已有视图失效。
 * - `ErrorTypeRegistry`：`类型名 -> 工厂/父类型/深度`，用于日志、配置、
 *   反序列化与测试里按名字创建错误。反射信息（父类型/深度）只在此表存一份，
 *   实例上的三个虚函数由 `BENTO_ERROR_CLASS` 生成。
 *
 * 线程模型：两个注册表都用互斥量保护，可从多线程并发注册/查询；
 * `LastError()` 是诊断快照，并发失败时只保证“最后一次写入可见”，
 * 不保证因果顺序。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/error/error.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

namespace go2cpp::bento::error {

/**
 * @brief 全局错误码分配器。
 *
 * @details 内置码在 `Instance()` 构造时按固定顺序预注册，因此
 *          `codes::kProtocol` 等 `inline` 常量无论初始化顺序如何都拿到同一个
 *          数值。业务模块也可以随时 `Register("module.error")` 扩展。
 */
class ErrorCodeRegistry {
 public:
    /// @brief 进程级单例；首次调用时预注册内置码。
    static ErrorCodeRegistry& Instance();

    /**
     * @brief 注册一个错误码名。
     * @return 稳定码；名字为空返回 0，同名幂等返回首次分配的码。
     */
    std::uint32_t Register(std::string_view name);

    /// @brief 码对应的名字；0 返回 `"kOk"`，未注册返回 `"<unknown>"`。
    std::string_view Name(std::uint32_t code) const;

    /// @brief 已注册码数量（不含保留的 0）。
    std::size_t Size() const;

    /// @brief 名字是否已注册。
    bool Contains(std::string_view name) const;

    /// @brief 按名字查码；未注册返回 0。
    std::uint32_t Lookup(std::string_view name) const;

    ErrorCodeRegistry(const ErrorCodeRegistry&) = delete;
    ErrorCodeRegistry& operator=(const ErrorCodeRegistry&) = delete;

 private:
    ErrorCodeRegistry();

    mutable std::mutex m_mutex;
    /// 用 deque 而不是 vector：注册新名字时已有元素不移动，
    /// `Name()` 返回的 string_view 在并发注册期间仍然有效。
    std::deque<std::string> m_names;
    std::unordered_map<std::string_view, std::uint32_t> m_codes;
    std::atomic<std::uint32_t> m_next{1};
};

/// @brief Bento 内置错误码句柄；数值由注册表分配，不要硬编码。
namespace codes {

inline const std::uint32_t kOk = 0;  ///< 保留：无错误。
inline const std::uint32_t kProtocol =
    ErrorCodeRegistry::Instance().Register("bento.protocol");  ///< 协议状态机错误。
inline const std::uint32_t kFrame =
    ErrorCodeRegistry::Instance().Register("bento.frame");  ///< 帧编解码错误。
inline const std::uint32_t kChecksum =
    ErrorCodeRegistry::Instance().Register("bento.checksum");  ///< 校验和不匹配。
inline const std::uint32_t kSession =
    ErrorCodeRegistry::Instance().Register("bento.session");  ///< 会话/连接不存在或失效。
inline const std::uint32_t kPeer =
    ErrorCodeRegistry::Instance().Register("bento.peer");  ///< 对端不可达或身份错误。
inline const std::uint32_t kTimeout =
    ErrorCodeRegistry::Instance().Register("bento.timeout");  ///< 超时。
inline const std::uint32_t kCrypto =
    ErrorCodeRegistry::Instance().Register("bento.crypto");  ///< 加解密/认证失败。
inline const std::uint32_t kConfig =
    ErrorCodeRegistry::Instance().Register("bento.config");  ///< 配置非法。
inline const std::uint32_t kInternal =
    ErrorCodeRegistry::Instance().Register("bento.internal");  ///< 内部错误。
inline const std::uint32_t kConstraint =
    ErrorCodeRegistry::Instance().Register("bento.constraint");  ///< 不变量/前置条件违反。

} // namespace codes

/// @brief 允许的最大继承层数：根 `bento.Error` 为 0，直接子类为 1。
inline constexpr std::uint8_t kMaxErrorDepth = 3;

/// @brief 深度超限时写入 `LastError()` 的码；沿用包装码表示“拒绝注册”。
inline constexpr std::uint32_t kDepthLimitErrorCode =
    static_cast<std::uint32_t>(ErrorCode::kWrappedError);

/// @brief 错误工厂：返回不可变错误指针。
using ErrorFactory = std::function<ErrorPtr()>;

/// @brief 内置错误类型名常量，避免调用方散落字符串字面量。
namespace type_names {

inline constexpr std::string_view kError = "bento.Error";                ///< 根类型。
inline constexpr std::string_view kStringError = "bento.StringError";    ///< 叶子错误。
inline constexpr std::string_view kWrappedError = "bento.WrappedError";  ///< 单原因包装。
inline constexpr std::string_view kJoinError = "bento.JoinError";        ///< 多原因聚合。

} // namespace type_names

/**
 * @brief 错误类型反射表：类型名到工厂、父类型与继承深度。
 *
 * @details 注册是幂等的：同名只保留首次注册的元数据与工厂。`Create()` 会
 *          复制工厂后在锁外调用，避免用户工厂重入注册表导致死锁。
 *
 *          失败原因记录在 `LastError()`（一个 `ErrorCodeRegistry` 码）与
 *          `LastErrorDetail()` 中；成功不会自动清零，调用方可在关键操作前
 *          `ClearLastError()`。
 */
class ErrorTypeRegistry {
 public:
    /// @brief 进程级单例；首次调用时注册内置类型。
    static ErrorTypeRegistry& Instance();

    /**
     * @brief 注册一个错误类型。
     * @param type_name 非空反射类型名。
     * @param parent_name 父类型名；`depth > 0` 时必须非空。
     * @param depth 继承深度，必须 <= `kMaxErrorDepth`。
     * @param factory 非空工厂。
     * @return 成功或同名幂等返回 true；参数非法/深度超限返回 false。
     * @note 深度超限只返回 false 并记录 `LastError()`，**不 assert**，
     *       避免库在 Debug 构建里直接终止进程；调用方应检查返回值。
     */
    bool Register(std::string_view type_name, std::string_view parent_name, std::uint8_t depth,
                  ErrorFactory factory);

    /// @brief 从 `BENTO_ERROR_CLASS` 生成的静态元数据注册类型。
    template <typename T>
    bool Register() {
        static_assert(std::is_base_of<Error, T>::value, "T 必须派生自 bento::error::Error");
        static_assert(std::is_default_constructible<T>::value, "T 必须可默认构造");
        return Register(T::BentoErrorTypeName(), T::BentoErrorParentTypeName(),
                        T::BentoErrorDepth(), []() -> ErrorPtr { return std::make_shared<T>(); });
    }

    /// @brief 按类型名创建实例；未注册或工厂失败/抛异常返回空。
    ErrorPtr Create(std::string_view type_name) const;

    /// @brief 类型名是否已注册。
    bool Has(std::string_view type_name) const;

    /// @brief 父类型名；未注册返回空。
    std::string_view ParentName(std::string_view type_name) const;

    /// @brief 继承深度；未注册返回 0。
    std::uint8_t Depth(std::string_view type_name) const;

    /// @brief 已注册类型数量。
    std::size_t Size() const;

    /// @brief 注册内置类型（String/Wrapped/Join）；可重复调用，幂等。
    void RegisterBuiltinErrorTypes();

    /// @brief 最近一次注册/创建失败的错误码；0 表示没有错误。
    std::uint32_t LastError() const;

    /// @brief 最近一次失败的说明文字。
    std::string LastErrorDetail() const;

    /// @brief 清空诊断状态。
    void ClearLastError();

    ErrorTypeRegistry(const ErrorTypeRegistry&) = delete;
    ErrorTypeRegistry& operator=(const ErrorTypeRegistry&) = delete;

 private:
    ErrorTypeRegistry();

    struct TypeInfo {
        std::string parent_name;
        std::uint8_t depth = 0;
        ErrorFactory factory;
    };

    void SetLastError(std::uint32_t code, std::string detail) const;

    mutable std::mutex m_mutex;
    // 用 `std::map` + 透明比较器：C++17 下可直接用 `std::string_view` 查找，
    // 避免每次 Create/Has/Depth 构造临时 `std::string`（一次堆分配）。
    // 类型表规模很小（内置 3 个 + 业务若干个），O(log n) 足够。
    std::map<std::string, TypeInfo, std::less<>> m_types;
    mutable std::uint32_t m_last_error = 0;
    mutable std::string m_last_error_detail;
};

} // namespace go2cpp::bento::error

/**
 * @brief 为派生自 `bento::error::Error` 的类型生成三个反射虚函数。
 * @param TypeNameValue 反射类型名（字符串字面量），例如 `"demo.TimeoutError"`。
 * @param ParentNameValue 父类型名，例如 `"bento.Error"`。
 * @param DepthValue 继承深度；超过 3 时注册会被拒绝。
 * @note 只生成 `TypeName`/`ParentTypeName`/`InheritanceDepth`；`Message()` 与
 *       `Code()` 仍由派生类实现。宏应放在类的 `public:` 区域。
 */
#define BENTO_ERROR_CLASS(TypeNameValue, ParentNameValue, DepthValue)                              \
    static constexpr std::string_view BentoErrorTypeName() noexcept {                              \
        return (TypeNameValue);                                                                    \
    }                                                                                              \
    static constexpr std::string_view BentoErrorParentTypeName() noexcept {                        \
        return (ParentNameValue);                                                                  \
    }                                                                                              \
    static constexpr std::uint8_t BentoErrorDepth() noexcept {                                     \
        return static_cast<std::uint8_t>(DepthValue);                                              \
    }                                                                                              \
    std::string_view TypeName() const noexcept override { return BentoErrorTypeName(); }            \
    std::string_view ParentTypeName() const noexcept override {                                    \
        return BentoErrorParentTypeName();                                                         \
    }                                                                                              \
    std::uint8_t InheritanceDepth() const noexcept override { return BentoErrorDepth(); }
