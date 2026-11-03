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

/**
 * @brief 在开放寻址哈希表里查「服务.方法」全名对应的绑定。
 *
 * @param full_name [in] 形如 `service.method` 的全名，UTF-8；不可为 nullptr；引用在本调用期间有效。
 *
 * @return
 * - 非空 `BoundMethod*`：命中——指针指向注册表内部存储，**不要**释放，也不能在 AddBindings 后长期缓存；
 * - nullptr：未注册——调用方**必须**回 kNotFound，不要重试。
 *
 * ## 定位
 * 分发热路径的唯一查找点。哈希表用线性探测，若删元素时留空槽会截断探测链导致查不到；
 * 本实现只增不删，因此空槽即终点是安全的。
 *
 * ## 步骤
 * step1. 空表直接返回 nullptr；
 * step2. 计算 FNV 哈希并用掩码取起始槽；
 * step3. 线性探测：空槽=未命中，哈希+文本双匹配=命中，否则后移。
 *
 * ## 并发/失败语义
 * - 调用约束：注册表 Freeze 后只读，可并发调用；AddBindings 期间必须由调用方串行化。
 * - 原子性：读路径不加锁（依赖只增不删与 Freeze 语义）。
 * - 失败语义：noexcept，无分配。
 * - 不变量：返回指针在注册表析构前有效，但 Rehash 后仍有效（存的是 m_methods 元素地址）。
 */
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

/**
 * @brief 一次性注册一个服务的全部方法；全有或全无，冻结后拒绝。
 *
 * @param service_name [in] 服务名，UTF-8；长度 1..64，字符集受限——引用在本调用期间有效。
 * @param service [in] 服务对象裸指针，可为任意值但不可为 nullptr；所有权归调用方，注册表只借用。
 * @param bindings [in] 方法绑定数组，不可为 nullptr，count>=1；数组内 name/invoke 均不可为空。
 * @param count [in] 方法个数，>=1。
 *
 * @return
 * - true：全部方法已入表——调用方**必须**保证 service 活到注册表不再使用；
 * - false：名字非法/重复/已冻结/参数为空——注册表**未发生任何变化**，调用方可修正后重试。
 *
 * ## 定位
 * 唯一的注册入口。部分注册会让「注册了一半」的服务对外可见，调用方无从判断哪些方法可用，
 * 因此这里先全量校验再逐条插入。
 *
 * ## 步骤
 * step1. 加锁并检查冻结标志；
 * step2. 校验 service/bindings/count 与服务名；
 * step3. 预生成全部 full_name，逐项校验字符集、长度、重复（表内 + 本批次内）；
 * step4. 任一项失败直接返回 false，不留下半注册；
 * step5. 逐条插入：容量达 70% 先 Rehash，再 push_back + InsertUnlocked；
 * step6. 返回成功。
 *
 * ## 并发/失败语义
 * - 调用约束：注册阶段单线程调用；Freeze 之后调用一律失败。
 * - 原子性：校验阶段无副作用；插入阶段逐条进行，但校验已排除所有失败可能。
 * - 失败语义：返回 false 且不抛异常；失败时表内容不变。
 * - 不变量：成功返回后每个 full_name 都能被 Find 命中且指向同一 service。
 */
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

/**
 * @brief 冻结注册表：此后 AddBindings 一律失败，读路径可无锁并发。
 *
 * @return 无返回值；重复调用幂等。
 *
 * ## 定位
 * 读写分离的唯一开关。漏调 Freeze 会让并发的 Find 与 Rehash 竞争（读线程可能看到搬迁中的表）；
 * 提前 Freeze 则后续服务注册全部失败，表现为「方法找不到」。
 *
 * ## 步骤
 * step1. 持锁保证与正在进行的 AddBindings 互斥；
 * step2. 置冻结标志（release 语义，读侧 acquire 可见）。
 *
 * ## 并发/失败语义
 * - 调用约束：服务端 Listen 之前调用一次。
 * - 原子性：与 AddBindings 互斥，不会冻结到一半。
 * - 失败语义：noexcept。
 * - 不变量：返回后所有 AddBindings 返回 false，Find 无需加锁。
 */
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
