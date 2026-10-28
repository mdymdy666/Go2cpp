/**
 * @file registry.cpp
 * @brief 错误码注册表与错误类型反射表的实现。
 *
 * @details 注册表内部用互斥量保护容器；`LastError` 的写入也走同一把锁，
 *          但所有失败路径都先释放锁再记录，避免“持锁调用用户代码/重入”。
 *          内置码与内置类型在单例构造时预注册，保证 `codes::*` 常量的数值
 *          与初始化顺序无关。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/error/registry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp::bento::error {

namespace {

/// 内置错误码名；顺序只影响首次分配的数值，调用方不应依赖具体数字。
constexpr std::string_view kBuiltinCodeNames[] = {
    "bento.protocol", "bento.frame",  "bento.checksum", "bento.session", "bento.peer",
    "bento.timeout",  "bento.crypto", "bento.config",   "bento.internal", "bento.constraint",
};

/// 诊断用码：内部错误。直接 Register 而不是读 codes::kInternal，
/// 避免 inline 变量的动态初始化顺序影响注册表构造期的行为。
std::uint32_t InternalCode() {
    return ErrorCodeRegistry::Instance().Register("bento.internal");
}

/// 诊断用码：不变量/前置条件违反。
std::uint32_t ConstraintCode() {
    return ErrorCodeRegistry::Instance().Register("bento.constraint");
}

} // namespace

ErrorCodeRegistry& ErrorCodeRegistry::Instance() {
    static ErrorCodeRegistry instance;
    return instance;
}

ErrorCodeRegistry::ErrorCodeRegistry() {
    for (const std::string_view name : kBuiltinCodeNames) {
        Register(name);
    }
}

std::uint32_t ErrorCodeRegistry::Register(std::string_view name) {
    if (name.empty()) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = m_codes.find(name);
    if (existing != m_codes.end()) {
        return existing->second;
    }

    m_names.emplace_back(name);
    const std::string& stored_name = m_names.back();
    const std::string_view stored_view(stored_name.data(), stored_name.size());
    const std::uint32_t code = m_next.fetch_add(1, std::memory_order_relaxed);
    try {
        m_codes.emplace(stored_view, code);
    } catch (...) {
        // 插入失败时回滚，保持“码 = deque 下标 + 1”的不变量。
        m_names.pop_back();
        m_next.fetch_sub(1, std::memory_order_relaxed);
        throw;
    }
    return code;
}

std::string_view ErrorCodeRegistry::Name(std::uint32_t code) const {
    if (code == 0) {
        return "kOk";
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t index = static_cast<std::size_t>(code - 1U);
    if (index >= m_names.size()) {
        return "<unknown>";
    }
    const std::string& name = m_names[index];
    return std::string_view(name.data(), name.size());
}

std::size_t ErrorCodeRegistry::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_names.size();
}

bool ErrorCodeRegistry::Contains(std::string_view name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_codes.find(name) != m_codes.end();
}

std::uint32_t ErrorCodeRegistry::Lookup(std::string_view name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_codes.find(name);
    return it == m_codes.end() ? 0 : it->second;
}

ErrorTypeRegistry& ErrorTypeRegistry::Instance() {
    static ErrorTypeRegistry instance;
    return instance;
}

ErrorTypeRegistry::ErrorTypeRegistry() {
    RegisterBuiltinErrorTypes();
}

bool ErrorTypeRegistry::Register(std::string_view type_name, std::string_view parent_name,
                                 std::uint8_t depth, ErrorFactory factory) {
    if (type_name.empty()) {
        SetLastError(InternalCode(), "错误类型名为空");
        return false;
    }
    if (!factory) {
        SetLastError(InternalCode(), "错误工厂为空: " + std::string(type_name));
        return false;
    }
    if (depth > kMaxErrorDepth) {
        SetLastError(kDepthLimitErrorCode,
                     "错误类型继承深度超限: " + std::string(type_name) +
                         " depth=" + std::to_string(static_cast<unsigned>(depth)) +
                         " max=" + std::to_string(static_cast<unsigned>(kMaxErrorDepth)));
        return false;
    }
    if (depth > 0 && parent_name.empty()) {
        SetLastError(ConstraintCode(), "非根错误类型缺少父类型名: " + std::string(type_name));
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_types.find(std::string(type_name)) != m_types.end()) {
        return true;
    }
    m_types.emplace(std::string(type_name),
                    TypeInfo{std::string(parent_name), depth, std::move(factory)});
    return true;
}

ErrorPtr ErrorTypeRegistry::Create(std::string_view type_name) const {
    ErrorFactory factory;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_types.find(std::string(type_name));
        if (it != m_types.end()) {
            found = true;
            try {
                factory = it->second.factory;
            } catch (...) {
                // 复制失败在锁外统一记录，避免在锁内再次加锁。
            }
        }
    }

    if (!found) {
        SetLastError(InternalCode(), "未注册的错误类型: " + std::string(type_name));
        return nullptr;
    }
    if (!factory) {
        SetLastError(InternalCode(), "错误工厂复制失败: " + std::string(type_name));
        return nullptr;
    }
    try {
        ErrorPtr result = factory();
        if (!result) {
            SetLastError(InternalCode(), "错误工厂返回空实例: " + std::string(type_name));
        }
        return result;
    } catch (...) {
        SetLastError(InternalCode(), "错误工厂抛出异常: " + std::string(type_name));
        return nullptr;
    }
}

bool ErrorTypeRegistry::Has(std::string_view type_name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_types.find(std::string(type_name)) != m_types.end();
}

std::string_view ErrorTypeRegistry::ParentName(std::string_view type_name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_types.find(std::string(type_name));
    if (it == m_types.end()) {
        return {};
    }
    return it->second.parent_name;
}

std::uint8_t ErrorTypeRegistry::Depth(std::string_view type_name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_types.find(std::string(type_name));
    if (it == m_types.end()) {
        return 0;
    }
    return it->second.depth;
}

std::size_t ErrorTypeRegistry::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_types.size();
}

void ErrorTypeRegistry::RegisterBuiltinErrorTypes() {
    Register(type_names::kStringError, type_names::kError, 1,
             []() -> ErrorPtr { return NewError(""); });
    Register(type_names::kWrappedError, type_names::kError, 1,
             []() -> ErrorPtr { return Wrap(NewError(""), ""); });
    Register(type_names::kJoinError, type_names::kError, 1, []() -> ErrorPtr {
        return Join(std::initializer_list<ErrorPtr>{NewError("")});
    });
}

std::uint32_t ErrorTypeRegistry::LastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_error;
}

std::string ErrorTypeRegistry::LastErrorDetail() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_error_detail;
}

void ErrorTypeRegistry::ClearLastError() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_last_error = 0;
    m_last_error_detail.clear();
}

void ErrorTypeRegistry::SetLastError(std::uint32_t code, std::string detail) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_last_error = code;
    m_last_error_detail = std::move(detail);
}

} // namespace go2cpp::bento::error
