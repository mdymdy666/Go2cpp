/**
 * @file type.cpp
 * @brief 类型注册表实现。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/reflect/type.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp::reflect {

TypeRegistry& TypeRegistry::Instance() {
    // C++11 起函数内静态变量的初始化是线程安全的（magic static），
    // 多个线程首次并发调用也只会构造一次。
    static TypeRegistry registry;
    return registry;
}

TypeRegistry::TypeRegistry() {
    AddBuiltin(kBoolType, "bool", sizeof(bool));
    AddBuiltin(kInt32Type, "int32", sizeof(std::int32_t));
    AddBuiltin(kInt64Type, "int64", sizeof(std::int64_t));
    AddBuiltin(kUint32Type, "uint32", sizeof(std::uint32_t));
    AddBuiltin(kUint64Type, "uint64", sizeof(std::uint64_t));
    AddBuiltin(kDoubleType, "double", sizeof(double));
    AddBuiltin(kStringType, "string", sizeof(std::string));
    AddBuiltin(kBytesType, "bytes", sizeof(std::vector<std::uint8_t>));
}

void TypeRegistry::AddBuiltin(TypeId id, const char* name, std::size_t size) {
    Entry entry;
    entry.name = name;
    entry.size = size;
    entry.builtin = true;
    m_by_id.emplace(id, std::move(entry));
    m_by_name.emplace(name, id);
}

TypeId TypeRegistry::Register(std::string_view name, std::size_t size) {
    if (name.empty() || size == 0 || name.size() > kMaxTypeNameBytes) {
        return kUnknownType;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string key(name);
    const auto existing = m_by_name.find(key);
    if (existing != m_by_name.end()) {
        // 同名幂等：保留首次分配的 id，调用方可以安全地重复注册。
        return existing->second;
    }
    TypeId candidate = m_next;
    while (m_by_id.find(candidate) != m_by_id.end()) {
        if (candidate == std::numeric_limits<TypeId>::max()) {
            return kUnknownType;
        }
        ++candidate;
    }
    if (candidate != std::numeric_limits<TypeId>::max()) {
        m_next = candidate + 1;
    }
    Entry entry;
    entry.name = key;
    entry.size = size;
    entry.builtin = false;
    m_by_id.emplace(candidate, std::move(entry));
    m_by_name.emplace(key, candidate);
    return candidate;
}

TypeId TypeRegistry::RegisterWithId(TypeId id, std::string_view name, std::size_t size) {
    if (id < kFirstDynamicType || name.empty() || size == 0 || name.size() > kMaxTypeNameBytes) {
        return kUnknownType;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string key(name);
    const auto name_it = m_by_name.find(key);
    if (name_it != m_by_name.end()) {
        // 同名幂等：返回首次分配的 id，调用方以返回值为准。
        return name_it->second;
    }
    if (m_by_id.find(id) != m_by_id.end()) {
        return kUnknownType;
    }
    Entry entry;
    entry.name = key;
    entry.size = size;
    entry.builtin = false;
    m_by_id.emplace(id, std::move(entry));
    m_by_name.emplace(key, id);
    return id;
}

bool TypeRegistry::Find(std::string_view name, TypeId& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_by_name.find(std::string(name));
    if (found == m_by_name.end()) {
        return false;
    }
    out = found->second;
    return true;
}

bool TypeRegistry::Name(TypeId id, std::string_view& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_by_id.find(id);
    if (found == m_by_id.end()) {
        return false;
    }
    // unordered_map 的 rehash 不移动节点，且本类不删除条目，因此该视图在
    // 进程生命周期内有效（类注释已说明）。
    out = found->second.name;
    return true;
}

bool TypeRegistry::Describe(TypeId id, TypeInfo& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_by_id.find(id);
    if (found == m_by_id.end()) {
        return false;
    }
    out.name = found->second.name;
    out.size = found->second.size;
    out.builtin = found->second.builtin;
    return true;
}

bool TypeRegistry::Contains(TypeId id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_by_id.find(id) != m_by_id.end();
}

std::size_t TypeRegistry::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_by_id.size();
}

}  // namespace go2cpp::reflect
