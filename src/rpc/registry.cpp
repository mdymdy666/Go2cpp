/**
 * @file registry.cpp
 * @brief 服务注册表实现：开放寻址哈希表 + 名字校验 + 冻结语义。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/rpc/registry.hpp"

#include <algorithm>
#include <utility>

#include "go2cpp/rpc/protocol.hpp"

namespace go2cpp::rpc {
namespace {

/// @brief 服务名/方法名各自的字节上限。
constexpr std::size_t kMaxNamePartBytes = 64;

/// @brief 名字字符集：字母数字 + `_`/`-`/`.`。
bool NamePartValid(std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaxNamePartBytes) {
        return false;
    }
    for (const char character : text) {
        const bool alpha = (character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!alpha && !digit && character != '_' && character != '-' && character != '.') {
            return false;
        }
    }
    return true;
}

}  // namespace

std::uint64_t ServiceRegistry::Hash(std::string_view text) noexcept {
    // FNV-1a 64：8-32 字节的名字约 1 cycle/byte，短方法名足够快，且无表。
    std::uint64_t value = 1469598103934665603ull;
    for (const char character : text) {
        value ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        value *= 1099511628211ull;
    }
    return value;
}

bool ServiceRegistry::ValidName(std::string_view text) noexcept {
    return NamePartValid(text);
}

const BoundMethod* ServiceRegistry::FindUnlocked(std::string_view full_name) const noexcept {
    if (m_slots.empty()) {
        return nullptr;
    }
    const std::uint64_t hash = Hash(full_name);
    const std::size_t mask = m_slots.size() - 1;
    std::size_t index = static_cast<std::size_t>(hash) & mask;
    for (;;) {
        const Slot& slot = m_slots[index];
        if (slot.method == nullptr) {
            return nullptr;
        }
        if (slot.hash == hash && slot.method->full_name == full_name) {
            return slot.method;
        }
        index = (index + 1) & mask;
    }
}

const BoundMethod* ServiceRegistry::Find(std::string_view full_name) const noexcept {
    return FindUnlocked(full_name);
}

void ServiceRegistry::InsertUnlocked(const BoundMethod* method) {
    const std::uint64_t hash = Hash(method->full_name);
    const std::size_t mask = m_slots.size() - 1;
    std::size_t index = static_cast<std::size_t>(hash) & mask;
    while (m_slots[index].method != nullptr) {
        index = (index + 1) & mask;
    }
    m_slots[index].hash = hash;
    m_slots[index].method = method;
}

void ServiceRegistry::Rehash(std::size_t capacity) {
    std::size_t size = 16;
    while (size < capacity) {
        size <<= 1;
    }
    m_slots.assign(size, Slot{});
    for (const BoundMethod& method : m_methods) {
        InsertUnlocked(&method);
    }
}

bool ServiceRegistry::AddBindings(std::string_view service_name, void* service,
                                  const MethodBinding* bindings, std::size_t count) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_frozen.load(std::memory_order_acquire)) {
        return false;
    }
    if (service == nullptr || bindings == nullptr || count == 0) {
        return false;
    }
    if (!ValidName(service_name)) {
        return false;
    }

    // 先做完整校验：任何一项不合法都不产生部分注册。
    std::vector<std::string> full_names;
    full_names.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const MethodBinding& binding = bindings[index];
        if (binding.name == nullptr || binding.invoke == nullptr) {
            return false;
        }
        const std::string_view method_name(binding.name);
        if (!ValidName(method_name)) {
            return false;
        }
        std::string full_name;
        full_name.reserve(service_name.size() + 1 + method_name.size());
        full_name.append(service_name);
        full_name.push_back('.');
        full_name.append(method_name);
        if (full_name.size() > kMaxMethodBytes) {
            return false;
        }
        if (FindUnlocked(full_name) != nullptr) {
            return false;
        }
        if (std::find(full_names.begin(), full_names.end(), full_name) != full_names.end()) {
            return false;
        }
        full_names.push_back(std::move(full_name));
    }

    for (std::size_t index = 0; index < count; ++index) {
        // 扩容必须在 push_back 之前：Rehash 只重插「已入表」的条目。
        if ((m_count + 1) * 10 >= m_slots.size() * 7) {
            Rehash(m_slots.empty() ? 16 : m_slots.size() * 2);
        }
        BoundMethod method;
        method.full_name = std::move(full_names[index]);
        method.service = service;
        method.binding = bindings[index];
        m_methods.push_back(std::move(method));
        InsertUnlocked(&m_methods.back());
        ++m_count;
    }
    return true;
}

void ServiceRegistry::Freeze() noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frozen.store(true, std::memory_order_release);
}

std::vector<std::string> ServiceRegistry::MethodNames() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> names;
    names.reserve(m_methods.size());
    for (const BoundMethod& method : m_methods) {
        names.push_back(method.full_name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace go2cpp::rpc
