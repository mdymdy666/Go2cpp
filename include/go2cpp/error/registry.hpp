#pragma once

/**
 * @file registry.hpp
 * @brief 错误类型反射表 + 动态错误码表。
 *
 * @details 参考 csvp 的 `CsvErrorTypeRegistry` / `ErrorCodeRegistry`，把两套
 *          注册能力合并进一个 `Registry`，解决两个不同问题：
 *
 * - **类型反射**：`类型名 -> 工厂 / 父类型 / 继承深度`。用于日志、配置、
 *   反序列化与测试里“按名字创建错误”。父类型/深度只存一份，实例上的
 *   三个虚函数由 `GO2CPP_ERROR_TYPE` 生成。
 * - **动态错误码**：把“手写枚举值”换成运行期分配的稳定整数码，码从
 *   `kFirstCustomCode`（1000）起递增，0 保留给 `kOk`；同名重复注册幂等。
 *   `Name()` 返回的 `string_view` 指向内部 `deque`，后续注册不会让已有
 *   视图失效。
 *
 * 线程模型：一把互斥量保护全部状态，可从多线程并发注册/查询。`Create()`
 * 在锁外调用用户工厂，避免工厂重入注册表时死锁。
 *
 * 失败诊断：`LastErrorCode()` / `LastErrorDetail()` 记录最近一次失败
 * （0 表示没有错误）；成功不会自动清零，调用方可在关键操作前
 * `ClearLastError()`。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/error/error.hpp"

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
#include <utility>
#include <vector>

namespace go2cpp::error {

/// @brief 错误工厂：返回不可变错误指针。
using ErrorFactory = std::function<ErrorPtr()>;

/// @brief “未绑定错误码”的哨兵值。
inline constexpr std::int32_t kNoCode = -1;

/// @brief 类型反射快照（`Registry::Types()` 的返回值）。
struct TypeInfo {
    std::string type_name;   ///< 反射类型名。
    std::string parent_name;  ///< 父类型名；根类型为空。
    std::uint8_t depth = 0;  ///< 继承深度。
    std::int32_t code = kNoCode; ///< 绑定的错误码；未绑定为 `kNoCode`。
    bool has_factory = false;    ///< 是否有工厂（根类型没有）。
};

/**
 * @brief 错误类型与错误码注册表（进程级单例）。
 *
 * @details 首次 `Instance()` 时预注册根类型与全部内置错误类型；业务模块
 *          可随时 `Register("demo.MyError", "go2cpp.Error", 1, factory)` 扩展。
 *
 *          注册规则（失败时 `LastErrorCode()` 给出原因）：
 *          - 类型名非空；
 *          - `depth == 0` 表示根类型，父类型必须为空；
 *          - `depth > 0` 时父类型必须已注册，且 `depth == 父深度 + 1`；
 *          - `depth <= kMaxErrorDepth`；
 *          - 非根类型的工厂必须非空；
 *          - 同名重复注册幂等：元数据一致返回 true，不一致返回 false。
 */
class Registry {
 public:
    /// @brief 进程级单例；首次调用时注册内置类型与内置码绑定。
    static Registry& Instance() {
        static Registry instance;
        return instance;
    }

    // ------------------------------------------------------------------
    // 类型反射
    // ------------------------------------------------------------------

    /**
     * @brief 注册根类型（`depth == 0`，无工厂）。
     * @param type_name 根类型名；空名返回 false。
     */
    bool RegisterRoot(std::string_view type_name) {
        return Register(type_name, std::string_view(), 0, ErrorFactory());
    }

    /**
     * @brief 注册一个错误类型。
     * @param type_name 非空反射类型名。
     * @param parent_name 父类型名；`depth > 0` 时必须已注册。
     * @param depth 继承深度，必须 `<= kMaxErrorDepth` 且等于父深度 + 1。
     * @param factory 非空工厂（根类型可为空）。
     * @return 成功或同名幂等返回 true；参数非法返回 false 并记录诊断。
     */
    bool Register(std::string_view type_name, std::string_view parent_name, std::uint8_t depth,
                  ErrorFactory factory) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (type_name.empty()) {
            SetLastErrorLocked(ErrorCode::kInvalidArgument, "类型名不能为空");
            return false;
        }
        const auto existing = m_types.find(type_name);
        if (existing != m_types.end()) {
            const bool same = existing->second.depth == depth &&
                              std::string_view(existing->second.parent_name) == parent_name;
            if (!same) {
                SetLastErrorLocked(ErrorCode::kAlreadyExists,
                                   "同名类型已注册且元数据不一致：" + std::string(type_name));
                return false;
            }
            return true;
        }
        if (depth > kMaxErrorDepth) {
            SetLastErrorLocked(ErrorCode::kOutOfRange,
                               "继承深度超过上限 " + std::to_string(kMaxErrorDepth) + "：" +
                                   std::string(type_name));
            return false;
        }
        if (depth == 0) {
            if (!parent_name.empty()) {
                SetLastErrorLocked(ErrorCode::kInvalidArgument,
                                   "根类型的父类型必须为空：" + std::string(type_name));
                return false;
            }
        } else {
            if (parent_name.empty()) {
                SetLastErrorLocked(ErrorCode::kInvalidArgument,
                                   "非根类型必须声明父类型：" + std::string(type_name));
                return false;
            }
            if (!factory) {
                SetLastErrorLocked(ErrorCode::kInvalidArgument,
                                   "非根类型必须提供工厂：" + std::string(type_name));
                return false;
            }
            const auto parent = m_types.find(parent_name);
            if (parent == m_types.end()) {
                SetLastErrorLocked(ErrorCode::kFailedPrecondition,
                                   "父类型未注册：" + std::string(parent_name));
                return false;
            }
            if (static_cast<std::uint8_t>(parent->second.depth + 1U) != depth) {
                SetLastErrorLocked(ErrorCode::kFailedPrecondition,
                                   "继承深度与父类型不连续：" + std::string(type_name));
                return false;
            }
        }
        Entry entry;
        entry.parent_name = std::string(parent_name);
        entry.depth = depth;
        entry.factory = std::move(factory);
        m_types.emplace(std::string(type_name), std::move(entry));
        return true;
    }

    /**
     * @brief 从类型自身的反射元数据注册（推荐入口）。
     * @tparam T 派生自 `Error` 且可默认构造的错误类型。
     * @return 成功返回 true；构造探测实例或注册失败返回 false。
     * @note 使用 `GO2CPP_ERROR_TYPE` 的类型自动具备一致的父类型/深度；
     *       手写类型也可用，只要它实现了三个反射虚函数。
     */
    template <typename T>
    bool Register() {
        static_assert(std::is_base_of<Error, T>::value, "T 必须派生自 go2cpp::error::Error");
        static_assert(std::is_default_constructible<T>::value, "T 必须可默认构造");
        try {
            const T probe{};
            return Register(probe.TypeName(), probe.ParentTypeName(), probe.InheritanceDepth(),
                            []() -> ErrorPtr { return MakeError<T>(); });
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_mutex);
            SetLastErrorLocked(ErrorCode::kInternal, "构造类型探测实例失败");
            return false;
        }
    }

    /// @brief 按类型名创建实例；未注册或工厂抛异常返回空指针。
    ErrorPtr Create(std::string_view type_name) const {
        ErrorFactory factory;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_types.find(type_name);
            if (found == m_types.end()) {
                SetLastErrorLocked(ErrorCode::kNotFound, "类型未注册：" + std::string(type_name));
                return {};
            }
            if (!found->second.factory) {
                SetLastErrorLocked(ErrorCode::kFailedPrecondition,
                                   "类型没有工厂：" + std::string(type_name));
                return {};
            }
            factory = found->second.factory;
        }
        // 锁外调用：用户工厂可能再次访问注册表，锁内调用会死锁。
        try {
            ErrorPtr created = factory();
            if (!created) {
                SetLastError(ErrorCode::kInternal, "类型工厂返回空指针：" + std::string(type_name));
            }
            return created;
        } catch (...) {
            SetLastError(ErrorCode::kInternal, "类型工厂抛出异常：" + std::string(type_name));
            return {};
        }
    }

    /// @brief 类型名是否已注册。
    bool Has(std::string_view type_name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_types.find(type_name) != m_types.end();
    }

    /// @brief 父类型名；未注册的根类型/未知类型返回空。
    std::string_view ParentName(std::string_view type_name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_types.find(type_name);
        if (found == m_types.end()) {
            return {};
        }
        return found->second.parent_name;
    }

    /// @brief 继承深度；未注册返回 0。
    std::uint8_t Depth(std::string_view type_name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_types.find(type_name);
        return found == m_types.end() ? std::uint8_t{0} : found->second.depth;
    }

    /**
     * @brief 类型名 → 父类型链（自身在前，根在后）。
     * @return 已注册名字的链；遇到未注册祖先时停止。最多 `kMaxErrorDepth + 1` 项。
     */
    std::vector<std::string_view> Ancestors(std::string_view type_name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string_view> chain;
        std::string_view current = type_name;
        for (std::size_t guard = 0; guard <= kMaxErrorDepth && !current.empty(); ++guard) {
            const auto found = m_types.find(current);
            if (found == m_types.end()) {
                break;
            }
            chain.push_back(found->first);
            current = found->second.parent_name;
        }
        return chain;
    }

    /// @brief `type_name` 的类型链是否包含 `ancestor_name`（自身也算）。
    bool IsA(std::string_view type_name, std::string_view ancestor_name) const {
        const std::vector<std::string_view> chain = Ancestors(type_name);
        for (const std::string_view node : chain) {
            if (node == ancestor_name) {
                return true;
            }
        }
        return false;
    }

    /// @brief 已注册类型数量（含根类型）。
    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_types.size();
    }

    /// @brief 全部类型快照，按类型名排序。
    std::vector<TypeInfo> Types() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<TypeInfo> result;
        result.reserve(m_types.size());
        for (const auto& item : m_types) {
            TypeInfo info;
            info.type_name = item.first;
            info.parent_name = item.second.parent_name;
            info.depth = item.second.depth;
            info.code = item.second.code;
            info.has_factory = static_cast<bool>(item.second.factory);
            result.push_back(std::move(info));
        }
        return result;
    }

    // ------------------------------------------------------------------
    // 错误码
    // ------------------------------------------------------------------

    /**
     * @brief 注册一个动态错误码名。
     * @param name 非空错误码名。
     * @return 稳定码（从 `kFirstCustomCode` 起）；空名返回 0，同名幂等返回首次码。
     */
    std::uint32_t RegisterCode(std::string_view name) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (name.empty()) {
            SetLastErrorLocked(ErrorCode::kInvalidArgument, "错误码名不能为空");
            return 0;
        }
        const auto existing = m_code_lookup.find(name);
        if (existing != m_code_lookup.end()) {
            return existing->second;
        }
        const std::uint32_t code = m_next_code;
        ++m_next_code;
        m_code_names.emplace_back(name);
        const std::string_view stored = m_code_names.back();
        m_code_lookup.emplace(stored, code);
        m_code_by_number.emplace(code, stored);
        return code;
    }

    /**
     * @brief 码对应的名字。
     * @return 内置码返回 `ErrorCodeName()`（如 `"NOT_FOUND"`）；动态码返回注册名；
     *         未注册返回 `"<unknown>"`。
     */
    std::string_view CodeName(std::uint32_t code) const {
        // 只有 0–15 是枚举的合法取值；16–999 是保留空洞，直接归为 UNKNOWN，
        // 避免把越界值 static_cast 成枚举（那是未定义行为）。
        if (code <= static_cast<std::uint32_t>(ErrorCode::kWrappedError)) {
            return ErrorCodeName(static_cast<ErrorCode>(code));
        }
        if (code < static_cast<std::uint32_t>(kFirstCustomCode)) {
            return "UNKNOWN";
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_code_by_number.find(code);
        return found == m_code_by_number.end() ? std::string_view("<unknown>") : found->second;
    }

    /// @brief 按名字查动态码；未注册返回 0（`kOk` 也是 0，调用方需自行区分语义）。
    std::uint32_t LookupCode(std::string_view name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_code_lookup.find(name);
        return found == m_code_lookup.end() ? 0U : found->second;
    }

    /// @brief 已注册动态码数量（不含内置码与保留的 0）。
    std::size_t CodeCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_code_names.size();
    }

    /**
     * @brief 把类型名绑定到一个错误码，供“按码找类型”使用。
     * @param type_name 已注册的类型名。
     * @param code 内置码或动态码。
     * @return 成功或幂等返回 true；类型未注册、码已被其他类型占用返回 false。
     */
    bool BindCode(std::string_view type_name, std::int32_t code) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto type = m_types.find(type_name);
        if (type == m_types.end()) {
            SetLastErrorLocked(ErrorCode::kNotFound, "类型未注册：" + std::string(type_name));
            return false;
        }
        const auto occupied = m_code_to_type.find(code);
        if (occupied != m_code_to_type.end()) {
            if (std::string_view(occupied->second) == type_name) {
                return true;
            }
            SetLastErrorLocked(ErrorCode::kAlreadyExists,
                               "错误码已被其他类型占用：" + std::to_string(code));
            return false;
        }
        if (type->second.code != kNoCode && type->second.code != code) {
            SetLastErrorLocked(ErrorCode::kFailedPrecondition,
                               "类型已绑定其他错误码：" + std::string(type_name));
            return false;
        }
        type->second.code = code;
        m_code_to_type.emplace(code, type->first);
        return true;
    }

    /// @brief 按码查类型名；未绑定返回空。
    std::string_view TypeNameOf(std::int32_t code) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_code_to_type.find(code);
        return found == m_code_to_type.end() ? std::string_view() : std::string_view(found->second);
    }

    /// @brief 类型绑定的错误码；未绑定返回 `kNoCode`。
    std::int32_t CodeOf(std::string_view type_name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_types.find(type_name);
        return found == m_types.end() ? kNoCode : found->second.code;
    }

    // ------------------------------------------------------------------
    // 诊断
    // ------------------------------------------------------------------

    /// @brief 最近一次失败的错误码；0 表示没有错误。
    std::uint32_t LastErrorCode() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last_error;
    }

    /// @brief 最近一次失败的说明文字。
    std::string LastErrorDetail() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last_error_detail;
    }

    /// @brief 清空诊断状态。
    void ClearLastError() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_error = 0;
        m_last_error_detail.clear();
    }

    /// @brief 注册内置类型与内置码绑定；可重复调用，幂等。
    void RegisterBuiltins() {
        RegisterRoot(kErrorRootTypeName);
        Register<StringError>();
        Register<ArgumentError>();
        Register<IOError>();
        Register<TimeoutError>();
        Register<NotFoundError>();
        Register<FileNotFoundError>();
        Register<PermissionError>();
        Register<WrappedError>();
        Register<JoinError>();

        // 按码找类型只对“代表类型”有意义，同一码不允许多个类型抢占。
        BindCode("go2cpp.StringError", static_cast<std::int32_t>(ErrorCode::kUnknown));
        BindCode("go2cpp.ArgumentError", static_cast<std::int32_t>(ErrorCode::kInvalidArgument));
        BindCode("go2cpp.NotFoundError", static_cast<std::int32_t>(ErrorCode::kNotFound));
        BindCode("go2cpp.PermissionError", static_cast<std::int32_t>(ErrorCode::kPermissionDenied));
        BindCode("go2cpp.IOError", static_cast<std::int32_t>(ErrorCode::kUnavailable));
        BindCode("go2cpp.TimeoutError", static_cast<std::int32_t>(ErrorCode::kDeadlineExceeded));
        BindCode("go2cpp.WrappedError", static_cast<std::int32_t>(ErrorCode::kWrappedError));
    }

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) = delete;
    Registry& operator=(Registry&&) = delete;

 private:
    Registry() {
        // 构造期无并发，加锁只是复用同一路径；RegisterBuiltins 本身幂等。
        RegisterBuiltins();
    }

    struct Entry {
        std::string parent_name; ///< 父类型名；根类型为空。
        std::uint8_t depth = 0;  ///< 继承深度。
        std::int32_t code = kNoCode;  ///< 绑定的错误码。
        ErrorFactory factory;         ///< 工厂；根类型为空。
    };

    /// @brief 记录失败诊断；调用方必须已持有 `m_mutex`。
    void SetLastErrorLocked(ErrorCode code, std::string detail) const {
        m_last_error = static_cast<std::uint32_t>(code);
        m_last_error_detail = std::move(detail);
    }

    /// @brief 记录失败诊断；自行加锁（供锁外路径使用）。
    void SetLastError(ErrorCode code, std::string detail) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        SetLastErrorLocked(code, std::move(detail));
    }

    mutable std::mutex m_mutex; ///< 保护下列全部状态。
    /// @note 用 `std::map` + 透明比较器：C++17 下可直接用 `std::string_view`
    ///       查找，避免每次查询构造临时 `std::string`。
    std::map<std::string, Entry, std::less<>> m_types;
    std::map<std::int32_t, std::string> m_code_to_type; ///< 码 → 类型名。
    /// @note `deque` 而不是 `vector`：新增名字时已有元素不移动，
    ///       `CodeName()` 返回的 `string_view` 在并发注册期间仍然有效。
    std::deque<std::string> m_code_names;
    std::unordered_map<std::string_view, std::uint32_t> m_code_lookup; ///< 名字 → 动态码。
    std::unordered_map<std::uint32_t, std::string_view> m_code_by_number; ///< 动态码 → 名字。
    std::uint32_t m_next_code = static_cast<std::uint32_t>(kFirstCustomCode);
    mutable std::uint32_t m_last_error = 0;   ///< 最近失败码；0 = 无错误。
    mutable std::string m_last_error_detail;  ///< 最近失败说明。
};

} // namespace go2cpp::error
