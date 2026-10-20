/**
 * @file include/go2cpp/config/config.hpp
 * @brief 配置中心：INI 解析、类型安全的 ConfigVar、事务式热加载与运行时快照。
 *
 * ## 模块定位
 *
 * 位于**基础设施服务层**，向下只依赖 C++17 标准库与 `log`/`scheduler` 的类型；
 * 向上被这些地方使用：
 * - `BindRuntimeConfig` 把 `scheduler.*`、`timer.*` 接到 `RuntimeConfig`
 *   （见 `example/config/runtime_config_full.cpp`）；
 * - `BindLoggingConfig`/`LoadAndWatch` 把 `log.*` 接到 `log::LoggerManager`；
 * - `src/network/hook.cpp` 的 `RegisterHookConfig` 用 `ConfigVar<bool>` 注册
 *   `hook.*` 开关；
 * - `src/os/config.cpp` 在静态初始化期注册 `os.*` 两项。
 *
 * ## 本文件定义什么
 *
 * - `IniFile`：section/key/value 解析与查询，不做类型转换、范围校验或同步；
 * - `ValueCodec`：字符串与 bool/Level/vector 等类型的编解码；
 * - `ConfigVarBase`/`ConfigVar<T>`：强类型、可校验、可监听、支持事务暂存的配置项；
 * - `Config`：进程级注册表 + 文件监听 + 两阶段提交（预检→写入→统一通知）；
 * - `RuntimeConfig`/`LoadRuntimeConfig`：调度器/日志/定时器阈值的只读快照；
 * - `AutoConfigRegistrar`/`AutoConfigCanceller` 与注册/注销宏。
 *
 * ## 阅读建议
 *
 * 先看 `ConfigVar::ApplyString`/`NotifyPending`/`RollbackPending`，理解「为什么
 * 热加载必须分两阶段」；再看 `Config::LoadFromIni` 的预检→写入→广播；最后看
 * `StartWatcher` 的稳定快照与 `.lock` 协议。状态转化表见
 * `docs/state-machines/Config.md`。
 *
 * ## 边界与陷阱
 *
 * - 配置文件是**外部输入**：范围上限集中在 `config.cpp` 的 `kMax*`，各模块不要放宽；
 * - 监听器在配置锁外执行，允许回调再次读/提交配置；单个监听器异常会被吞掉；
 * - `DumpIni` 把值中的反斜杠、`#`/`;`、控制字符与首尾空格编码为 `\xHH`，
 *   `IniFile::Parse` 对称还原 —— 导出再读回逐字节一致；
 * - 静态注册发生在 `main` 之前，跨 TU 注销依赖初始化顺序，不可靠。
 */
#pragma once

// 简单、可审查的 ini 配置。解析器只处理 section、key=value 和注释，避免
// 引入 YAML 等重量依赖；运行时配置在加载后统一校验，再交给各模块应用。

#include "go2cpp/log.hpp"
#include "go2cpp/scheduler/scheduler.hpp"

#include <cstddef>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace go2cpp::config {

// 轻量 INI 解析器。依赖标准文件流和字符串容器，为上层配置中心提供
// section/key/value 查询；它不负责类型转换、范围校验或线程同步。
/** @brief 简单 INI 文件解析器，负责 section/key/value 的读取与查询。 */
/**
 * ## 定位
 * `Config::LoadFromIni` 与 `RuntimeConfig::FromIni` 的输入载体；也被 examples
 * 用来从内存文本构造配置。它只负责**语法**，类型转换/范围校验由 ConfigVar 与
 * RuntimeConfig 完成。
 *
 * ## 不变量
 * - `Parse` 成功前不修改 `m_sections`：先解析到临时表，成功才 `swap`；
 * - 节名缺 `]`、键名缺 `=`、空键名都直接失败并带行号；
 * - 值在第一个**未转义**的 `#`/`;` 处截断；字面量 `\n` 还原为换行，
 *   `\xHH` 还原为原始字节（`DumpIni` 的输出依赖这一约定往返）。
 */
class IniFile final {
public:
    // 从文件读取并解析配置。path 为 UTF-8 路径；成功返回 true，失败时
    // 将中文诊断写入 error（可为空）。
    /** @brief 从文件读取并解析 INI。@param path UTF-8 文件路径。 */
    bool Load(const std::string& path, std::string* error = nullptr);
    // 解析内存中的 INI 文本。重复调用会先清空旧节和旧键。
    /** @brief 解析内存中的 INI 文本。 */
    bool Parse(const std::string& text, std::string* error = nullptr);
    // 查询 section/key；键不存在时返回 fallback。
    /** @brief 查询 section/key，不存在时返回 fallback。 */
    std::string Get(const std::string& section, const std::string& key,
                    std::string fallback = {}) const;
    // 判断指定 section/key 是否存在。
    /** @brief 判断指定 section/key 是否存在。 */
    bool Has(const std::string& section, const std::string& key) const;

private:
    using Section = std::unordered_map<std::string, std::string>;
    std::unordered_map<std::string, Section> m_sections;  ///< 节/键/值表；Load/Parse 整体替换（swap），Get/Has 只读；无锁，调用方需自行同步
};

/** @brief 默认配置值与字符串之间的流式转换器。 */
/**
 * ## 定位
 * `ConfigVar<T>` 的文本编解码策略：`ToString`/`ValidateString`/`FromString` 都经
 * 由它。主模板面向可流式输入输出的标量，`std::string`/`bool`/`log::Level`/
 * `std::vector<T>` 各有特化。
 *
 * ## 不变量
 * - `Decode` 必须消费**整个**文本（`stream >> ws` 后要求 eof），否则 `"1x"` 会
 *   被误读成 `1`；
 * - `Decode` 失败不得修改输出参数；`Encode` 的结果必须能被同一 `Decode` 读回。
 */
template <typename T>
struct ValueCodec {
    /** @brief 将配置值编码为文本。 */
    static std::string Encode(const T& value) {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    }
    /** @brief 从文本解码配置值。@return 成功返回 true。 */
    static bool Decode(const std::string& text, T* value) {
        if (!value) return false;
        std::istringstream stream(text);
        T parsed{};
        stream >> parsed;
        if (stream.fail()) return false;
        stream >> std::ws;
        if (!stream.eof()) return false;
        *value = std::move(parsed);
        return true;
    }
};

template <>
struct ValueCodec<std::string> {
    /** @brief 原样编码字符串。 */
    static std::string Encode(const std::string& value) { return value; }
    /** @brief 将文本写入字符串。 */
    static bool Decode(const std::string& text, std::string* value) { if (!value) return false; *value = text; return true; }
};

template <>
struct ValueCodec<bool> {
    /** @brief 编码布尔值为 true/false。 */
    static std::string Encode(bool value) { return value ? "true" : "false"; }
    /** @brief 解析常见布尔文本。 */
    static bool Decode(const std::string& text, bool* value) {
        if (!value) return false;
        std::string clean = text;
        for (auto& character : clean) if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
        if (clean == "1" || clean == "true" || clean == "yes" || clean == "on") *value = true;
        else if (clean == "0" || clean == "false" || clean == "no" || clean == "off") *value = false;
        else return false;
        return true;
    }
};

template <>
struct ValueCodec<log::Level> {
    /** @brief 编码日志级别。 */
    static std::string Encode(log::Level value) { return log::ToString(value); }
    /** @brief 解析日志级别文本。 */
    static bool Decode(const std::string& text, log::Level* value) { return log::ParseLevel(text, value); }
};

template <typename T>
struct ValueCodec<std::vector<T>> {
    /** @brief 以逗号分隔编码列表。 */
    static std::string Encode(const std::vector<T>& value) {
        std::ostringstream stream;
        for (std::size_t i = 0; i < value.size(); ++i) { if (i) stream << ','; stream << ValueCodec<T>::Encode(value[i]); }
        return stream.str();
    }
    /** @brief 解析逗号分隔列表。 */
    static bool Decode(const std::string& text, std::vector<T>* value) {
        if (!value) return false;
        std::vector<T> parsed;
        std::istringstream stream(text); std::string item;
        while (std::getline(stream, item, ',')) { T element{}; if (!ValueCodec<T>::Decode(item, &element)) return false; parsed.push_back(std::move(element)); }
        *value = std::move(parsed); return true;
    }
};

// 配置变量的非模板接口。Config 中保存它的智能指针，热加载器通过本类
// 完成枚举、字符串校验和写入，而具体类型由 ConfigVar<T> 提供。
/** @brief 配置变量的非模板基类，提供统一字符串校验和加载接口。 */
/**
 * ## 定位
 * `Config` 注册表用基类指针保存所有类型的变量，从而在不知道 `T` 的情况下完成
 * 枚举、文本校验和事务提交。`LoadFromIni` 只通过这个接口工作。
 *
 * ## 不变量
 * - 构造后 `name`/`description` 不再变化；
 * - `HasPending`/`NotifyPending`/`RollbackPending` 必须成对使用：只有
 *   `ApplyString` 成功且 `HasPending()` 为真的变量才能进入通知/回滚列表；
 * - 事务接口只允许 `Config::LoadFromIni` 调用，普通调用方应使用 `FromString`。
 */
class ConfigVarBase {
public:
    using ptr = std::shared_ptr<ConfigVarBase>;
    ConfigVarBase(std::string name, std::string description) : m_name(std::move(name)), m_description(std::move(description)) {}
    virtual ~ConfigVarBase() = default;
    const std::string& name() const noexcept { return m_name; }
    const std::string& description() const noexcept { return m_description; }
    virtual std::string ToString() const = 0;
    virtual bool ValidateString(const std::string& value, std::string* error = nullptr) const = 0;
    virtual bool FromString(const std::string& value, std::string* error = nullptr) = 0;
    /**
     * @brief 在配置事务中应用文本值，但暂不调用监听器。
     * @details Config::LoadFromIni 先对全部配置项做预检，再调用本接口完成
     *          第一阶段提交；所有变量成功后由 NotifyPending 统一发布通知。
     *          普通调用方应继续使用 FromString，不需要自行管理事务。
     * @param value 待转换的文本值。
     * @param error 失败时写入中文诊断，可为空。
     * @return 转换、校验和写入均成功返回 true。
     */
    virtual bool ApplyString(const std::string& value, std::string* error = nullptr) = 0;
    /** @brief 判断事务阶段是否产生了待发布变更。 */
    virtual bool HasPending() const noexcept = 0;
    /**
     * @brief 发布本次事务中暂存的变更通知。
     * @details 没有暂存变更时为空操作；调用顺序由 Config 事务管理器保证。
     */
    virtual void NotifyPending() noexcept = 0;
    /**
     * @brief 回滚本次事务中已经写入但尚未通知的值。
     * @details 仅用于提交阶段发生异常或失败时恢复内存中的旧值。
     */
    virtual void RollbackPending() noexcept = 0;

private:
    std::string m_name;  ///< 完整名 module.key；构造后不变，name() 读取
    std::string m_description;  ///< 人类可读说明；构造后不变，DumpIni 把它写成 # 注释
};

struct RuntimeConfig;

template <typename T>
// 强类型、可监听的配置变量。依赖 ValueCodec<T> 做文本转换，以不可变
// shared_ptr 发布读取快照，以互斥量保护校验器和监听器；向上层提供无锁
//（原子 shared_ptr 读）读取、范围校验和变更通知。
/**
 * @brief 线程安全的强类型配置变量。
 * @details Validator 在提交前校验，Listener 在成功变更后通知；值以共享
 *          指针原子发布，读操作不长时间持有配置锁。
 */
/**
 * ## 定位
 * 被 `Scheduler`（通过 `BindRuntimeConfig` 的监听器）、`log::LoggerManager`
 * （`BindLoggingConfig`）、`src/network/hook.cpp` 的 `RegisterHookConfig` 与
 * `src/os/config.cpp` 静态注册项使用；读取方通过 `GetValue()` 拿不可变快照。
 *
 * ## 生命周期与线程模型
 * 通常由 `Config::Lookup<T>` 创建并持有，`Config` 析构时统一释放；外部可持有
 * `shared_ptr`（`Remove` 是软注销）。`GetValue` 走原子 `shared_ptr` 读，不拿锁；
 * `SetValue`/`ApplyString`/`NotifyPending` 用 `m_mutex` 串行化写入与通知快照。
 *
 * ## 状态机
 * 单变量事务有 Idle / Pending 两个稳定状态与 Notified/RolledBack 两个出口，
 * 完整转化表见 `docs/state-machines/Config.md`（触发函数：`ApplyString`、
 * `NotifyPending`、`RollbackPending`、`SetValue`）。
 *
 * ## 不变量
 * - `m_pending` 存在期间 `SetValue`/第二次 `ApplyString` 一律拒绝，避免两个事务
 *   交错覆盖同一变量；
 * - `m_value` 永远指向不可变对象，读方无需加锁；
 * - 监听器在 `m_mutex` 释放后执行，且单个监听器异常不影响其余监听器。
 */
class ConfigVar final : public ConfigVarBase {
public:
    using ptr = std::shared_ptr<ConfigVar<T>>;
    using Listener = std::function<void(const T& old_value, const T& new_value)>;
    using Validator = std::function<bool(const T& value, std::string* error)>;
    /** @brief 创建配置变量。 */
    ConfigVar(std::string name, T value, std::string description)
        : ConfigVarBase(std::move(name), std::move(description)),
          m_value(std::make_shared<const T>(std::move(value))) {}
    // 读取是配置热路径，值以不可变 shared_ptr 发布，避免普通读取阻塞
    // 配置刷新；SetValue 仍通过互斥锁串行化监听器和写入。
    /** @brief 读取当前值的线程安全副本。 */
    T GetValue() const {
        const auto value = std::atomic_load_explicit(&m_value, std::memory_order_acquire);
        return value ? *value : T{};
    }
    // 设置配置值。返回 false 表示校验器拒绝了新值；此时旧值和监听器
    // 都不会改变。监听器在配置锁外按注册快照顺序执行。
    /** @brief 校验并发布新值，失败时保持旧值不变。 */
    bool SetValue(const T& value, std::string* error = nullptr) {
        Validator validator;
        Validator direct_validator;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            validator = m_validator;
            direct_validator = m_direct_validator;
        }
        if (validator && !validator(value, error)) return false;
        if (direct_validator && !direct_validator(value, error)) return false;
        T old_value;
        std::map<std::uint64_t, Listener> listeners;
        { std::lock_guard<std::mutex> lock(m_mutex);
          if (m_pending) {
              if (error) *error = "配置变量正在等待上一事务通知: " + name();
              return false;
          }
          const auto current = std::atomic_load_explicit(&m_value, std::memory_order_acquire);
          if (current && *current == value) return true;
          old_value = current ? *current : T{};
          std::atomic_store_explicit(&m_value, std::make_shared<const T>(value),
                                     std::memory_order_release);
          listeners = m_listeners; }
        for (const auto& [id, listener] : listeners) {
            (void)id;
            if (!listener) continue;
            try {
                listener(old_value, value);
            } catch (...) {
                // 用户监听器属于插件边界；单个监听器异常不应阻断其余
                // 监听器，也不应把异常传播到配置调用方。
            }
        }
        return true;
    }
    // 安装强类型配置校验器。校验器不会在配置锁内执行，避免回调再次
    // 读取配置时形成锁反转；调用方应在注册前保证当前值已经合法。
    /** @brief 设置新值校验器。 */
    void SetValidator(Validator validator) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_validator = std::move(validator);
    }
    /**
     * @brief 设置仅用于单值直接更新的校验器。
     * @details 配置文件事务使用完整候选快照校验器处理跨字段约束；直接调用
     *          SetValue/FromString 时则用本校验器对照当前关联字段校验。
     */
    void SetDirectValidator(Validator validator) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_direct_validator = std::move(validator);
    }
    /** @brief 添加变更监听器并返回监听 ID。 */
    std::uint64_t AddListener(Listener listener) {
        const auto id = m_next_listener.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_mutex); m_listeners.emplace(id, std::move(listener)); return id;
    }
    /** @brief 删除指定监听器。 */
    void DelListener(std::uint64_t id) { std::lock_guard<std::mutex> lock(m_mutex); m_listeners.erase(id); }
    /** @brief 将当前值编码为字符串。 */
    std::string ToString() const override { return ValueCodec<T>::Encode(GetValue()); }
    /** @brief 校验文本而不修改当前值。 */
    bool ValidateString(const std::string& value, std::string* error = nullptr) const override {
        T parsed{};
        if (!ValueCodec<T>::Decode(value, &parsed)) {
            if (error) *error = "配置值转换失败: " + name();
            return false;
        }
        Validator validator;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            validator = m_validator;
        }
        if (validator && !validator(parsed, error)) return false;
        return true;
    }
    /** @brief 校验并从文本应用新值。 */
    bool FromString(const std::string& value, std::string* error = nullptr) override {
        T parsed{};
        if (!ValueCodec<T>::Decode(value, &parsed)) { if (error) *error = "配置值转换失败: " + name(); return false; }
        return SetValue(parsed, error);
    }

    /**
     * @brief 在事务第一阶段写入值，不触发监听器。
     * @param value 待转换的文本值。
     * @param error 失败时写入中文诊断，可为空。
     * @return 成功写入返回 true。
     */
    bool ApplyString(const std::string& value, std::string* error = nullptr) override {
        T parsed{};
        if (!ValueCodec<T>::Decode(value, &parsed)) {
            if (error) *error = "配置值转换失败: " + name();
            return false;
        }
        Validator validator;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            validator = m_validator;
        }
        if (validator && !validator(parsed, error)) return false;

        // 先完成可能抛出 bad_alloc 的共享值构造，避免写入后才发现内存
        // 分配失败而无法把本变量加入事务回滚列表。
        auto next_value = std::make_shared<const T>(parsed);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending) {
            if (error) *error = "配置变量正在等待上一事务通知: " + name();
            return false;
        }
        const auto current = std::atomic_load_explicit(&m_value, std::memory_order_acquire);
        if (current && *current == parsed) {
            m_pending.reset();
            return true;
        }
        PendingChange change;
        change.old_value = current;
        change.new_value = parsed;
        m_pending = std::move(change);
        std::atomic_store_explicit(&m_value, std::move(next_value),
                                   std::memory_order_release);
        return true;
    }

    /**
     * @brief 发布事务阶段暂存的监听通知。
     * @details 回调在释放内部互斥锁和 Config 事务锁后执行，允许回调再次读取
     *          或提交配置；同一变量在一次事务中只发布最终值，避免中间值触发
     *          模块重配置。
     */
    void NotifyPending() noexcept override {
        std::optional<PendingChange> change;
        std::map<std::uint64_t, Listener> listeners;
        try {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pending) return;
            listeners = m_listeners;
            change = std::move(m_pending);
            m_pending.reset();
        } catch (...) {
            // 监听器快照分配失败时仍清理暂存状态，配置值已经完整提交；
            // 本次仅跳过回调，不能让热加载线程因为扩展点异常退出。
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.reset();
            return;
        }
        for (const auto& [id, listener] : listeners) {
            (void)id;
            if (!listener) continue;
            try {
                const T old_value = change->old_value ? *change->old_value : T{};
                listener(old_value, change->new_value);
            } catch (...) {
                // 监听器属于扩展点，单个插件异常不能阻断同一变量的其余
                // 监听器，也不能回滚已经完整提交的配置值。
            }
        }
    }

    /** @brief 返回当前变量是否存在尚未广播的事务变更。 */
    bool HasPending() const noexcept override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pending.has_value();
    }

    /**
     * @brief 回滚事务阶段暂存的值。
     * @details 回滚只恢复值，不调用监听器；因为事务尚未对外发布。
     */
    void RollbackPending() noexcept override {
        try {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pending) return;
            std::atomic_store_explicit(&m_value, m_pending->old_value,
                                       std::memory_order_release);
            m_pending.reset();
        } catch (...) {
            // 回滚路径不能向监听线程传播异常；若分配失败，保留当前值并
            // 交由进程级错误处理记录，避免在 noexcept 路径中 terminate。
        }
    }

private:
    struct PendingChange {
        // 保存旧快照而不是旧值副本，回滚时无需再次分配 T，能够在
        // bad_alloc 等异常路径下保持 noexcept 回滚的确定性。
        std::shared_ptr<const T> old_value;  ///< 变更前值的不可变快照；ApplyString 写入，RollbackPending 原子恢复，NotifyPending 读取
        T new_value{};  ///< 事务中待广播的新值；ApplyString 拷贝，NotifyPending 传给监听器
    };
    mutable std::mutex m_mutex;  ///< 保护 m_value/m_listeners/m_validator/m_direct_validator/m_pending；SetValue/ApplyString/NotifyPending 使用
    std::shared_ptr<const T> m_value;  ///< 当前值的不可变快照；atomic_load/store 读写，GetValue 读路径不加锁
    std::map<std::uint64_t, Listener> m_listeners;  ///< 监听器表（按 ID 升序）；AddListener/DelListener 与 SetValue/NotifyPending 快照使用
    Validator m_validator;  ///< 事务/常规校验器；SetValidator 写，SetValue/ValidateString/ApplyString 读
    Validator m_direct_validator;  ///< 仅单值直接更新使用的校验器；SetDirectValidator 写，SetValue 读
    std::atomic<std::uint64_t> m_next_listener{1};  ///< 监听器 ID 分配器；AddListener fetch_add，ID 不复用
    std::optional<PendingChange> m_pending;  ///< 事务暂存变更；ApplyString 置位，HasPending/NotifyPending/RollbackPending 读或清
};

/**
 * @brief 全局配置变量注册表和动态文件监听器。
 * @details 文件加载使用先校验后提交和 lock 文件协议，避免编辑中的半文件
 *          影响运行时配置。
 */
/**
 * ## 定位
 * 进程级配置注册表与热加载入口：`BindLoggingConfig`/`BindRuntimeConfig` 往这里
 * 注册变量与监听器，examples/tests 用 `StartWatcher` 验证热更新，
 * `src/network/hook.cpp` 通过 `Config::Instance()` 取得注册表。
 *
 * ## 生命周期与线程模型
 * `Instance()` 是函数内静态对象，析构时 `StopWatcher()`；注册表读写由 `m_mutex`
 * 保护，整批文件事务由 `m_transaction_mutex` 串行化。热加载线程只在本类内部创建，
 * 监听器与提交回调都在**释放事务锁之后**执行。
 *
 * ## 状态机
 * 文件事务有 Idle / Validating / Applying / Pending / Committed / RolledBack
 * 六个状态，状态转化表见 `docs/state-machines/Config.md`（触发函数：
 * `LoadFromIni`、`ApplyString`、`NotifyPending`、`RollbackPending`、`StartWatcher`）。
 *
 * ## 不变量
 * - 一次 `LoadFromIni` 中，所有变量要么全部写入并广播，要么全部回滚；
 * - 事务锁只保护预检+写入，**绝不**包住监听器回调；
 * - `m_watching == true` 时 `m_watcher` 一定是可 join 的线程（除非在自身线程内停止）。
 */
class Config final {
public:
    /**
     * @brief 一次配置事务完成后的扩展回调。
     * @details 回调执行时，本次文件提交涉及的全部 ConfigVar 已经写入；
     *          适合 scheduler、logger 等需要整体重建快照的插件。回调在事务锁
     *          释放后执行，可以查询或再次提交配置；回调异常会被配置中心隔离，
     *          不会撤销已经提交的配置值。
     */
    using CommitListener = std::function<void(const std::vector<ConfigVarBase::ptr>& changed)>;
    /**
     * @brief 配置文件事务候选快照校验器。
     * @details 参数包含所有已注册变量的完整候选快照；未出现在本次 INI 中的
     *          键使用当前值。校验器在任何变量写入前执行，用于检查跨字段约束。
     */
    using TransactionValidator =
        std::function<bool(const std::map<std::string, std::string>& candidates,
                           std::string* error)>;
    // 返回进程内唯一配置注册表；注册表本身由调用方在进程退出前使用。
    /** @brief 返回进程内唯一配置注册表。 */
    static Config& Instance();
    // 查找或创建 name 对应的 T 类型变量。name 使用 section.key 形式，
    // default_value 仅在首次创建时生效；类型冲突返回空指针。
    /** @brief 查找或创建指定类型配置变量。 */
    template <typename T>
    std::shared_ptr<ConfigVar<T>> Lookup(const std::string& name, const T& default_value,
                                         const std::string& description = {}) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_vars.find(name);
        if (it != m_vars.end()) return std::dynamic_pointer_cast<ConfigVar<T>>(it->second);
        auto variable = std::make_shared<ConfigVar<T>>(name, default_value, description);
        m_vars.emplace(name, variable);
        return variable;
    }
    // 按名称返回非模板变量；不存在时返回空指针。
    /** @brief 以非模板基类指针查找配置变量。 */
    ConfigVarBase::ptr LookupBase(const std::string& name) const;
    // 预检所有已注册变量后批量应用 INI 中出现的键。
    /** @brief 先整体校验再应用 INI 配置。 */
    bool LoadFromIni(const IniFile& ini, std::string* error = nullptr);
    // 对 path 做稳定快照后调用 LoadFromIni。快照失败或 INI 无效时不会
    // 修改已注册变量；生产热更新应使用 path + ".lock" 编辑锁和原子 rename。
    /** @brief 以稳定快照读取文件并应用配置。 */
    bool LoadFromFile(const std::string& path, std::string* error = nullptr);
    // 返回当前已注册变量的共享指针快照。
    /** @brief 返回当前已注册变量快照。 */
    std::vector<ConfigVarBase::ptr> List() const;
    /**
     * @brief 运行时注销一个配置变量。
     * @details 返回 false 表示不存在。注销后该变量不再出现在 `List()`、
     *          `DumpIni()` 与 INI 加载范围内；已持有其 `shared_ptr` 的调用方
     *          仍可读写它，但它的值不再被配置文件同步 —— 这是「软注销」语义，
     *          避免让外部持有的指针悬空。
     * @param name 完整名（`module.key`）。
     */
    bool Remove(const std::string& name);

    /**
     * @brief 按 `module` 分组返回配置变量。
     * @details 完整名以第一个 `.` 切分为 `module` 与 `key`；没有 `.` 的归入
     *          空字符串分组。组内按 key 字典序排列，保证输出稳定可 diff。
     */
    std::map<std::string, std::vector<ConfigVarBase::ptr>> Groups() const;

    /**
     * @brief 生成带注释的 INI 文本。
     * @details 只读取当前值，不修改任何状态。输出按 `[module]` 分段、段内按
     *          key 字典序排列，描述作为 `#` 注释。值中的反斜杠、`#`/`;`、
     *          控制字符与首尾空格被编码为 `\xHH`，保证生成的文本可以被
     *          `IniFile::Parse` 逐字节读回。
     */
    std::string DumpIni() const;

    /**
     * @brief 原子地把当前配置写成 INI 文件。
     * @details 先写入 `path + ".tmp"` 再 `rename` 覆盖，避免读者看到半个文件。
     *          这是与 `LoadFromIni` 对称的「导出」方向。
     * @param path 目标路径。
     * @param error 失败时写入中文诊断，可为空。
     */
    bool SaveToFile(const std::string& path, std::string* error = nullptr) const;

    /**
     * @brief 注册一次文件事务完成回调。
     * @param listener 提交完成后接收变更变量快照的回调。
     * @return 可用于 DelCommitListener 的监听器 ID。
     */
    std::uint64_t AddCommitListener(CommitListener listener);
    /** @brief 删除指定的事务完成回调。 */
    void DelCommitListener(std::uint64_t id);
    /** @brief 注册配置文件事务级候选快照校验器。 */
    std::uint64_t AddTransactionValidator(TransactionValidator validator);
    /** @brief 删除指定的配置文件事务级校验器。 */
    void DelTransactionValidator(std::uint64_t id);
    // 立即加载 path，并按 interval 轮询。监听前先检查 path + ".lock"：锁文件
    // 非空时拒绝启动或暂缓刷新，空文件/不存在时才读取配置；失败不会启动线程。
    /** @brief 启动后台监听线程。 */
    bool StartWatcher(const std::string& path, std::chrono::milliseconds interval = std::chrono::milliseconds(1000), std::string* error = nullptr);
    // 停止监听并等待监听线程退出；可重复调用。
    /** @brief 停止监听并等待线程退出。 */
    void StopWatcher();
    /** @brief 返回监听线程是否运行。 */
    bool Watching() const noexcept { return m_watching.load(std::memory_order_acquire); }

private:
    Config() = default;
    ~Config();
    void stop_watcher_locked() noexcept;
    mutable std::mutex m_mutex;  ///< 保护 m_vars/m_commit_listeners/m_transaction_validators；Lookup/List/Groups/Add* 使用
    // 文件事务锁串行化多个 LoadFromIni/LoadFromFile 调用，保证不同线程
    // 不会交错写入同一批 ConfigVar 并互相覆盖暂存通知。
    mutable std::mutex m_transaction_mutex;  ///< 串行化整批 LoadFromIni/LoadFromFile；保证预检与提交不被其他事务交错
    std::unordered_map<std::string, ConfigVarBase::ptr> m_vars;  ///< 完整名到变量的注册表；Lookup/LookupBase/List/Remove/Groups 使用
    std::map<std::uint64_t, CommitListener> m_commit_listeners;  ///< 事务提交后的扩展回调；Add/DelCommitListener 与 LoadFromIni 的快照使用
    std::atomic<std::uint64_t> m_next_commit_listener{1};  ///< 提交监听器 ID 分配器；AddCommitListener fetch_add
    std::map<std::uint64_t, TransactionValidator> m_transaction_validators;  ///< 跨字段候选快照校验器；Add/DelTransactionValidator 与 LoadFromIni 使用
    std::atomic<std::uint64_t> m_next_transaction_validator{1};  ///< 事务校验器 ID 分配器；AddTransactionValidator fetch_add
    std::atomic<bool> m_watching{false};  ///< 监听线程运行标志；StartWatcher 置 true，stop_watcher_locked 置 false，Watching 读取
    // 保护 std::thread 对象本身；配置值读取不使用这把生命周期锁。
    mutable std::mutex m_watcher_lifecycle_mutex;  ///< 保护 m_watcher 的创建/停止；StartWatcher/StopWatcher/析构使用
    mutable std::mutex m_watcher_mutex;  ///< 配合 m_watcher_cv 等待刷新周期；监听线程与 stop_watcher_locked 使用
    std::condition_variable m_watcher_cv;  ///< 停止监听时立即唤醒等待中的监听线程；只在启停路径 notify_all
    std::thread m_watcher;  ///< 后台监听线程；StartWatcher 创建，stop_watcher_locked join 或 detach
};

// 把配置中心的动态变量接到 LoggerManager。config 为注册表；error 可为空。
// 之后修改 ini 并由 watcher 重载，已存在的 Logger 会立即切换级别、格式、
// 文件和 stdout 输出。
/** @brief 将日志字段注册到配置系统。 */
bool BindLoggingConfig(Config& config, std::string* error = nullptr);
// 把 scheduler.* 和 log.* 变量绑定到 target。target 生命周期必须覆盖所有
// 监听器；成功返回 true，类型冲突或已有值非法时返回 false。
/** @brief 将调度器和日志字段绑定到运行时目标。 */
bool BindRuntimeConfig(Config& config, RuntimeConfig* target,
                       std::string* error = nullptr);

// 新手接口：绑定日志配置并完成“加载 + 热更新”。调度器配置需要先调用
// BindRuntimeConfig，因为 Scheduler 必须在读取结构参数后再创建。
/** @brief 注册配置后加载文件并启动动态监听。 */
inline bool LoadAndWatch(Config& config, const std::string& path,
                         std::chrono::milliseconds interval = std::chrono::milliseconds(1000),
                         std::string* error = nullptr) {
    if (!BindLoggingConfig(config, error)) return false;
    return config.StartWatcher(path, interval, error);
}

/** @brief 调度器、栈和日志的统一运行时配置快照。 */
/**
 * ## 定位
 * 启动路径与热更新共用的**只读参数快照**：`LoadRuntimeConfig`/`FromIni` 负责
 * 构造，`Validate` 负责范围检查，`ApplyLogging` 把日志字段推给
 * `log::LoggerManager`。`BindRuntimeConfig` 则把其中字段绑定为可变监听目标。
 *
 * ## 不变量
 * - `Validate()` 返回 true 后，调度器/日志/定时器字段都处于文档允许的范围；
 * - `FromIni` 在副本上构造，失败不改动传入对象；
 * - `ApplyLogging` 只改全局 LoggerManager，不触碰调度器。
 */
struct RuntimeConfig final {
    scheduler::SchedulerConfig scheduler{};  ///< 调度器参数子结构；FromIni/Validate 读写，BindRuntimeConfig 把监听器接到各字段
    log::Level log_level{log::Level::Warn};  ///< 日志最低级别快照；FromIni 解析，ApplyLogging 应用
    bool log_stdout{false};  ///< 是否同时输出 stdout；FromIni 解析，ApplyLogging 应用
    std::string log_directory{"log"};  ///< 日志目录；FromIni 覆盖，ApplyLogging 传给 LoggerManager
    std::string log_file{"go2cpp.log"};  ///< 日志文件名；FromIni 覆盖，ApplyLogging 传给 LoggerManager
    std::string log_format{"{time} [{level}] {logger} ({file}:{line}) {message}\n"};  ///< PatternFormatter 模式；FromIni 覆盖，ApplyLogging 传给 LoggerManager
    /** @brief 定时器时长告警阈值，单位为天；0 关闭对应级别。 */
    std::size_t timer_warn_after_days{30};  ///< 定时器 WARN 阈值（天，0 关闭）；FromIni/Validate 与 BindRuntimeConfig 的 timer 监听器使用
    /** @brief 定时器时长错误阈值，单位为天；0 关闭对应级别。 */
    std::size_t timer_error_after_days{365};  ///< 定时器 ERROR 阈值（天，0 关闭）；FromIni/Validate 与 BindRuntimeConfig 的 timer 监听器使用

    // 从 INI 读取并校验运行时配置；失败时不修改 config。
    /** @brief 从 INI 构造配置快照但不修改外部对象。 */
    static bool FromIni(const IniFile& ini, RuntimeConfig* config,
                        std::string* error = nullptr);
    // 校验 P/M、队列、Fiber 栈、时间和日志字段的统一边界。
    /** @brief 校验全部调度器、栈和日志参数边界。 */
    bool Validate(std::string* error = nullptr) const;
    // 校验成功后把日志字段应用到全局 LoggerManager。
    /** @brief 将日志配置应用到 LoggerManager。 */
    bool ApplyLogging(std::string* error = nullptr) const;
};

// 从 path 读取完整 RuntimeConfig；config 必须指向调用方持有的对象。
/** @brief 从文件加载并校验 RuntimeConfig。 */
bool LoadRuntimeConfig(const std::string& path, RuntimeConfig* config,
                      std::string* error = nullptr);
// ---------------------------------------------------------------------------
// 名字工具：完整名形如 "module.key"
// ---------------------------------------------------------------------------

/** @brief 以第一个 `.` 切分完整名；没有 `.` 时 module 为空。 */
std::pair<std::string, std::string> SplitConfigName(const std::string& full_name);

/** @brief 拼接 "module.key"；module 为空时只返回 key。 */
std::string JoinConfigName(const std::string& module, const std::string& key);

// ---------------------------------------------------------------------------
// 静态注册 / 注销
// ---------------------------------------------------------------------------

/**
 * @brief 把一项配置在**静态初始化期**注册进 `Config::Instance()`。
 * @details 一般通过 `GO2CPP_CONFIG_REGISTER` 宏使用。构造时调用 `Lookup<T>`；
 *          若同名已存在且类型不同，`Lookup` 返回空，此时本对象不持有任何东西。
 */
template <typename T>
/**
 * ## 定位
 * 静态注册宏 `GO2CPP_CONFIG_REGISTER` 的载体：在 `main` 之前调用
 * `Config::Lookup<T>` 把配置项放进注册表。
 *
 * ## 不变量
 * - 构造期间绝不抛异常（静态初始化期异常会直接 terminate）；
 * - `m_var` 为空表示类型冲突或注册异常，`registered()` 可查询。
 */
class AutoConfigRegistrar final {
public:
    AutoConfigRegistrar(const char* module, const char* key, T default_value,
                        const char* description)
        : m_name(JoinConfigName(module ? module : "", key ? key : "")) {
        try {
            m_var = Config::Instance().Lookup<T>(m_name, std::move(default_value),
                                                 description ? description : "");
        } catch (...) {
            // 静态初始化期不允许异常穿透到 main 之前：注册失败就放弃该项。
            m_var = nullptr;
        }
    }

    AutoConfigRegistrar(const AutoConfigRegistrar&) = delete;
    AutoConfigRegistrar& operator=(const AutoConfigRegistrar&) = delete;

    /** @brief 完整名（便于诊断）。 */
    const std::string& name() const noexcept { return m_name; }

    /** @brief 是否成功拿到配置变量（类型冲突或异常时为 false）。 */
    bool registered() const noexcept { return m_var != nullptr; }

private:
    std::string m_name;  ///< 完整名；构造时 JoinConfigName 生成，name() 读取
    std::shared_ptr<ConfigVar<T>> m_var;  ///< 注册得到的变量；类型冲突或异常时为空，registered() 判断
};

/**
 * @brief 在**静态初始化期**注销一项配置（软注销，语义见 `Config::Remove`）。
 */
/**
 * ## 定位
 * 静态注销宏 `GO2CPP_CONFIG_CANCEL` 的载体，语义是 `Config::Remove` 的软注销。
 *
 * ## 不变量
 * - 只影响当前翻译单元已注册的项；跨 TU 顺序不可靠；
 * - 构造期间吞掉异常，`removed()` 表示是否真的删除。
 */
class AutoConfigCanceller final {
public:
    AutoConfigCanceller(const char* module, const char* key) : m_removed(false) {
        try {
            m_removed = Config::Instance().Remove(
                JoinConfigName(module ? module : "", key ? key : ""));
        } catch (...) {
            m_removed = false;
        }
    }

    AutoConfigCanceller(const AutoConfigCanceller&) = delete;
    AutoConfigCanceller& operator=(const AutoConfigCanceller&) = delete;

    /** @brief 是否确实移除了某项。 */
    bool removed() const noexcept { return m_removed; }

private:
    bool m_removed;  ///< 是否确实移除；构造时 Config::Remove 的结果，removed() 读取
};

// 用 __LINE__ 保证同一 `key` 在多处注册时不重名。
#define GO2CPP_CONFIG_REGISTER_IMPL(module, type, key, default_value, description, line) \
    static ::go2cpp::config::AutoConfigRegistrar<type>                                  \
        go2cpp_config_registrar_##key##_##line(module, #key, default_value, description)

/**
 * @brief 静态注册一项配置。
 * @param module 分组名（会与 key 拼成 `module.key`）。
 * @param type 值类型；**不能含未加括号的逗号**（如 `std::map<int, int>`），
 *        否则宏参数会被逗号截断，请先 `using` 一个别名。
 * @param key 键名。
 * @param default_value 首次注册时的默认值。
 * @param description 说明文本，会写进生成的 INI 注释。
 */
#define GO2CPP_CONFIG_REGISTER(module, type, key, default_value, description) \
    GO2CPP_CONFIG_REGISTER_IMPL(module, type, key, default_value, description, __LINE__)

#define GO2CPP_CONFIG_CANCEL_IMPL(module, key, line) \
    static ::go2cpp::config::AutoConfigCanceller      \
        go2cpp_config_canceller_##key##_##line(module, #key)

/**
 * @brief 静态注销一项配置。
 * @details 只能注销**本翻译单元里已经注册过**的项；跨 TU 的注销依赖静态
 *          初始化顺序，不可靠 —— 跨模块注销请在运行时调用 `Config::Remove`。
 */
#define GO2CPP_CONFIG_CANCEL(module, key) GO2CPP_CONFIG_CANCEL_IMPL(module, key, __LINE__)


}  // namespace go2cpp::config
