#pragma once

/**
 * @file debug_fiber.hpp
 * @brief Go2Cpp 调试模块：带调试元数据的 Fiber 与元数据注册表。
 *
 * 设计依据：DebugTooling/design/09-debug-class-conventions.md
 *   - 使用者要求：Debug 模块的类尽量**继承原类**并加 Debug 前缀，避免重复实现；
 *   - 实测结论：`Fiber` 无 `final`（可继承），但**无虚析构**。
 *
 * 硬约束（必须遵守，否则未定义行为）：
 *   1. **绝不可**通过 `Fiber*` 或 `std::unique_ptr<Fiber>` 销毁 `DebugFiber`
 *      （`~Fiber()` 非虚）。因此 `DebugFiber` **不得**放入 `Task::m_fiber`。
 *   2. 本文件**不修改** `Fiber` 的任何行为：只**增加**字段与查询方法。
 *
 * 与框架的关系：`DebugFiber` 与 `DebugRegistry` 全部位于 `go2cpp::debug` 命名空间，
 * 原框架（Fiber/Task/Scheduler）对本文件**零依赖**。
 *
 * ## 模块定位
 *
 * `debug` 的 Fiber 元数据层：给执行体附加任务名/描述/源码位置，并通过进程内
 * 注册表按 Fiber id 查询。上游是业务任务创建与调试 CLI，下游是 `Fiber`。
 *
 * ## 本文件定义什么
 *
 * - `FiberDebugInfo`：可拷贝的元数据（含 `present` 区分「未注册」与「空值」）；
 * - `DebugFiber`：继承 `Fiber` 并附加元数据的执行体；
 * - `DebugRegistry`：Fiber id → 元数据的线程安全注册表（单例）。
 *
 * ## 阅读建议
 *
 * 先读文件头「绝不可通过 Fiber* 销毁 DebugFiber」的生命周期硬约束，再看
 * `FiberDebugInfo::present` 的两种“查不到”，最后看注册表的快照语义。
 *
 * ## 边界与陷阱
 *
 * `Fiber` 无虚析构：`DebugFiber` 只能由 `unique_ptr<DebugFiber>` 或值语义
 * 持有，绝不能放进 `Task::m_fiber`。注册表不拥有 Fiber，条目需要调用方注销。
 */

#include "go2cpp/fiber.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace go2cpp::debug {

/**
 * @brief Fiber 的调试元数据（纯数据，可自由拷贝）。
 * @details `present` 用于区分两种"查不到"：
 *          - `present == false`：该 Fiber **从未注册**过元数据；
 *          - `present == true` 但字段为空：**已注册**，只是该项没有来源。
 *          调试输出必须区分这两者，不得都用 unknown 表示。
 *
 * ## 定位
 * `DebugFiber` 与 `DebugRegistry` 共用的元数据值类型；调试 CLI 输出与测试
 * 断言都读取它。
 *
 * ## 不变量
 * - `present` 由注册/构造路径置真，不会自动变假；
 * - `line == 0` 表示无源码行号；空字符串表示该项无来源。
 */
struct FiberDebugInfo {
    std::string name;         ///< 任务名，例如 s_main_task。
    std::string description;  ///< 任务描述（使用者要求的"任务描述字段"）。
    std::string file;         ///< 源文件路径。
    std::uint32_t line{0};    ///< 源码行号；0 表示无来源。
    bool present{false};      ///< 是否已注册。

    bool operator==(const FiberDebugInfo& other) const noexcept {
        return name == other.name && description == other.description &&
               file == other.file && line == other.line &&
               present == other.present;
    }
};

/**
 * @brief 带调试元数据的 Fiber。
 *
 * 依赖：`Fiber` 提供用户栈与挂起/恢复；本类只附加元数据。
 * 对上层提供：`Fiber` 的全部既有能力 + 元数据读写。
 *
 * @note **生命周期约束**：本类没有（也不能有）虚析构，
 *       因此只能由 `std::unique_ptr<DebugFiber>` 或值语义持有；
 *       传给任何接受 `Fiber&`/`Fiber*` 的接口时**必须保证不会被其删除**。
 *
 * ## 定位
 * 带调试元数据的执行体：业务创建任务时用 `DebugFiber` 记录任务名/描述/
 * 源码位置，调试工具再通过 `DebugRegistry` 按 id 查询。
 *
 * ## 生命周期与线程模型
 * 构造即注册元数据（`present=true`），`m_id` 记录 `Fiber::id()`；不可拷贝/
 * 移动。对象析构不会自动注销注册表条目（墓碑信息是刻意保留的）。
 *
 * ## 不变量
 * - `m_info.present` 在构造后恒为真；
 * - `m_id` 在构造时固定，等于 `Fiber::id()`；
 * - 元数据修改只影响本对象，注册表需要显式 `Register` 才看得到。
 */
class DebugFiber : public Fiber {
public:
    using Ptr = std::unique_ptr<DebugFiber>;
    using Function = Fiber::Function;

    /** @brief 构造带元数据的 Fiber。@param function 入口函数。@param info 调试元数据。@param stack_size 栈大小。 */
    explicit DebugFiber(Function function, FiberDebugInfo info = {},
                        std::size_t stack_size = DefaultStackSize())
        : Fiber(std::move(function), stack_size), m_info(std::move(info)) {
        if (!m_info.present) {
            m_info.present = true;  // 构造即视为已注册（即使字段为空）
        }
        m_id = Fiber::id();
    }

    DebugFiber(const DebugFiber&) = delete;
    DebugFiber& operator=(const DebugFiber&) = delete;
    DebugFiber(DebugFiber&&) = delete;
    DebugFiber& operator=(DebugFiber&&) = delete;

    /** @brief 返回调试元数据。 */
    const FiberDebugInfo& info() const noexcept { return m_info; }
    const FiberDebugInfo& Info() const noexcept { return m_info; }
    /** @brief 返回任务描述。 */
    const std::string& description() const noexcept { return m_info.description; }
    /** @brief 返回任务名。 */
    const std::string& name() const noexcept { return m_info.name; }
    /** @brief 设置任务描述。 */
    void set_description(std::string value) { m_info.description = std::move(value); }
    /** @brief 设置任务名。 */
    void set_name(std::string value) { m_info.name = std::move(value); }
    /** @brief 设置源码位置。 */
    void set_source_location(std::string file, std::uint32_t line) {
        m_info.file = std::move(file);
        m_info.line = line;
    }
    /** @brief 返回构造时记录的 Fiber 编号（= Fiber::id()）。 */
    std::uint64_t debug_id() const noexcept { return m_id; }

private:
    FiberDebugInfo m_info;      ///< 调试元数据；构造时设置，set_* 修改，info()/description()/name() 读取
    std::uint64_t m_id{0};      ///< 构造时记录的 Fiber::id()；debug_id() 读取
};

/**
 * @brief Fiber 调试元数据注册表（`FiberId` → 元数据）。
 *
 * 依赖：仅标准库（`std::mutex` + `std::unordered_map`）。**刻意不使用**
 *       `go2cpp::sync::Mutex` —— 后者是协程感知锁、需要调度器上下文，
 *       而调试注册表必须能在任意线程（含非受管线程）安全访问。
 * 对上层提供：注册、注销、查询、快照、并发安全。
 *
 * @note 注册表**不拥有** `DebugFiber`，只保存元数据副本，
 *       因此 `DebugFiber` 析构后条目仍在（需调用方注销），这是刻意的：
 *       观测"已结束的 Fiber"（墓碑信息）是调试的常见需求。
 *
 * ## 定位
 * 进程内 Fiber 元数据注册表：业务注册、调试 CLI/测试按 id 查询；不依赖
 * 调度器，可在任意线程访问。
 *
 * ## 生命周期与线程模型
 * `Instance()` 返回静态单例；`std::mutex` 保护全部条目。`Snapshot()`/
 * `Lookup()` 返回副本，调用后不再持锁。
 *
 * ## 不变量
 * - `Register` 写入的条目的 `present` 恒为真；
 * - `Unregister`/`Clear` 只影响注册表，不影响 `DebugFiber` 对象；
 * - 注册表只存值，不持有任何 Fiber 指针。
 */
class DebugRegistry final {
public:
    /** @brief 返回进程内唯一实例（线程安全的静态初始化）。 */
    static DebugRegistry& Instance() noexcept {
        static DebugRegistry registry;
        return registry;
    }

    /** @brief 注册/覆盖某 Fiber 的元数据。 */
    void Register(std::uint64_t fiber_id, FiberDebugInfo info) {
        info.present = true;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries[fiber_id] = std::move(info);
    }

    /** @brief 注销某 Fiber。@return 是否确实存在并已移除。 */
    bool Unregister(std::uint64_t fiber_id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.erase(fiber_id) > 0;
    }

    /** @brief 查询元数据；未注册返回 nullopt。 */
    std::optional<FiberDebugInfo> Lookup(std::uint64_t fiber_id) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_entries.find(fiber_id);
        if (it == m_entries.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /** @brief 当前条目数。 */
    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

    /** @brief 清空全部条目（测试与收尾用）。 */
    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }

    /** @brief 一次性导出全部条目（快照语义：调用后不再持锁）。 */
    std::vector<std::pair<std::uint64_t, FiberDebugInfo>> Snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::pair<std::uint64_t, FiberDebugInfo>> out;
        out.reserve(m_entries.size());
        for (const auto& entry : m_entries) {
            out.emplace_back(entry.first, entry.second);
        }
        return out;
    }

private:
    DebugRegistry() = default;
    ~DebugRegistry() = default;
    DebugRegistry(const DebugRegistry&) = delete;
    DebugRegistry& operator=(const DebugRegistry&) = delete;

    mutable std::mutex m_mutex; ///< 保护 m_entries；Register/Unregister/Lookup/Snapshot 都持锁
    std::unordered_map<std::uint64_t, FiberDebugInfo> m_entries; ///< Fiber id → 元数据副本；Register 写，Unregister/Clear 删
};

}  // namespace go2cpp::debug
