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
class IniFile final {
public:
    // 从文件读取并解析配置。path 为 UTF-8 路径；成功返回 true，失败时
    // 将中文诊断写入 error（可为空）。
    bool Load(const std::string& path, std::string* error = nullptr);
    // 解析内存中的 INI 文本。重复调用会先清空旧节和旧键。
    bool Parse(const std::string& text, std::string* error = nullptr);
    // 查询 section/key；键不存在时返回 fallback。
    std::string Get(const std::string& section, const std::string& key,
                    std::string fallback = {}) const;
    // 判断指定 section/key 是否存在。
    bool Has(const std::string& section, const std::string& key) const;

private:
    using Section = std::unordered_map<std::string, std::string>;
    std::unordered_map<std::string, Section> m_sections;
};

template <typename T>
struct ValueCodec {
    static std::string Encode(const T& value) {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    }
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
    static std::string Encode(const std::string& value) { return value; }
    static bool Decode(const std::string& text, std::string* value) { if (!value) return false; *value = text; return true; }
};

template <>
struct ValueCodec<bool> {
    static std::string Encode(bool value) { return value ? "true" : "false"; }
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
    static std::string Encode(log::Level value) { return log::ToString(value); }
    static bool Decode(const std::string& text, log::Level* value) { return log::ParseLevel(text, value); }
};

template <typename T>
struct ValueCodec<std::vector<T>> {
    static std::string Encode(const std::vector<T>& value) {
        std::ostringstream stream;
        for (std::size_t i = 0; i < value.size(); ++i) { if (i) stream << ','; stream << ValueCodec<T>::Encode(value[i]); }
        return stream.str();
    }
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

private:
    std::string m_name;
    std::string m_description;
};

struct RuntimeConfig;

template <typename T>
// 强类型、可监听的配置变量。依赖 ValueCodec<T> 做文本转换，以不可变
// shared_ptr 发布读取快照，以互斥量保护校验器和监听器；向上层提供无锁
//（原子 shared_ptr 读）读取、范围校验和变更通知。
class ConfigVar final : public ConfigVarBase {
public:
    using ptr = std::shared_ptr<ConfigVar<T>>;
    using Listener = std::function<void(const T& old_value, const T& new_value)>;
    using Validator = std::function<bool(const T& value, std::string* error)>;
    ConfigVar(std::string name, T value, std::string description)
        : ConfigVarBase(std::move(name), std::move(description)),
          m_value(std::make_shared<const T>(std::move(value))) {}
    // 读取是配置热路径，值以不可变 shared_ptr 发布，避免普通读取阻塞
    // 配置刷新；SetValue 仍通过互斥锁串行化监听器和写入。
    T GetValue() const {
        const auto value = std::atomic_load_explicit(&m_value, std::memory_order_acquire);
        return value ? *value : T{};
    }
    // 设置配置值。返回 false 表示校验器拒绝了新值；此时旧值和监听器
    // 都不会改变。监听器在配置锁外按注册快照顺序执行。
    bool SetValue(const T& value, std::string* error = nullptr) {
        Validator validator;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            validator = m_validator;
        }
        if (validator && !validator(value, error)) return false;
        T old_value;
        std::map<std::uint64_t, Listener> listeners;
        { std::lock_guard<std::mutex> lock(m_mutex);
          const auto current = std::atomic_load_explicit(&m_value, std::memory_order_acquire);
          if (current && *current == value) return true;
          old_value = current ? *current : T{};
          std::atomic_store_explicit(&m_value, std::make_shared<const T>(value),
                                     std::memory_order_release);
          listeners = m_listeners; }
        for (const auto& [id, listener] : listeners) if (listener) listener(old_value, value);
        return true;
    }
    // 安装强类型配置校验器。校验器不会在配置锁内执行，避免回调再次
    // 读取配置时形成锁反转；调用方应在注册前保证当前值已经合法。
    void SetValidator(Validator validator) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_validator = std::move(validator);
    }
    std::uint64_t AddListener(Listener listener) {
        const auto id = m_next_listener.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_mutex); m_listeners.emplace(id, std::move(listener)); return id;
    }
    void DelListener(std::uint64_t id) { std::lock_guard<std::mutex> lock(m_mutex); m_listeners.erase(id); }
    std::string ToString() const override { return ValueCodec<T>::Encode(GetValue()); }
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
    bool FromString(const std::string& value, std::string* error = nullptr) override {
        T parsed{};
        if (!ValueCodec<T>::Decode(value, &parsed)) { if (error) *error = "配置值转换失败: " + name(); return false; }
        return SetValue(parsed, error);
    }

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<const T> m_value;
    std::map<std::uint64_t, Listener> m_listeners;
    Validator m_validator;
    std::atomic<std::uint64_t> m_next_listener{1};
};

class Config final {
public:
    // 返回进程内唯一配置注册表；注册表本身由调用方在进程退出前使用。
    static Config& Instance();
    // 查找或创建 name 对应的 T 类型变量。name 使用 section.key 形式，
    // default_value 仅在首次创建时生效；类型冲突返回空指针。
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
    ConfigVarBase::ptr LookupBase(const std::string& name) const;
    // 预检所有已注册变量后批量应用 INI 中出现的键。
    bool LoadFromIni(const IniFile& ini, std::string* error = nullptr);
    // 对 path 做稳定快照后调用 LoadFromIni。快照失败或 INI 无效时不会
    // 修改已注册变量；生产热更新应使用 path + ".lock" 编辑锁和原子 rename。
    bool LoadFromFile(const std::string& path, std::string* error = nullptr);
    // 返回当前已注册变量的共享指针快照。
    std::vector<ConfigVarBase::ptr> List() const;
    // 立即加载 path，并按 interval 轮询。监听前先检查 path + ".lock"：锁文件
    // 非空时拒绝启动或暂缓刷新，空文件/不存在时才读取配置；失败不会启动线程。
    bool StartWatcher(const std::string& path, std::chrono::milliseconds interval = std::chrono::milliseconds(1000), std::string* error = nullptr);
    // 停止监听并等待监听线程退出；可重复调用。
    void StopWatcher();
    bool Watching() const noexcept { return m_watching.load(std::memory_order_acquire); }

private:
    Config() = default;
    ~Config();
    void stop_watcher_locked() noexcept;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, ConfigVarBase::ptr> m_vars;
    std::atomic<bool> m_watching{false};
    // 保护 std::thread 对象本身；配置值读取不使用这把生命周期锁。
    mutable std::mutex m_watcher_lifecycle_mutex;
    mutable std::mutex m_watcher_mutex;
    std::condition_variable m_watcher_cv;
    std::thread m_watcher;
};

// 把配置中心的动态变量接到 LoggerManager。config 为注册表；error 可为空。
// 之后修改 ini 并由 watcher 重载，已存在的 Logger 会立即切换级别、格式、
// 文件和 stdout 输出。
bool BindLoggingConfig(Config& config, std::string* error = nullptr);
// 把 scheduler.* 和 log.* 变量绑定到 target。target 生命周期必须覆盖所有
// 监听器；成功返回 true，类型冲突或已有值非法时返回 false。
bool BindRuntimeConfig(Config& config, RuntimeConfig* target,
                       std::string* error = nullptr);

// 新手接口：绑定日志配置并完成“加载 + 热更新”。调度器配置需要先调用
// BindRuntimeConfig，因为 Scheduler 必须在读取结构参数后再创建。
inline bool LoadAndWatch(Config& config, const std::string& path,
                         std::chrono::milliseconds interval = std::chrono::milliseconds(1000),
                         std::string* error = nullptr) {
    if (!BindLoggingConfig(config, error)) return false;
    return config.StartWatcher(path, interval, error);
}

struct RuntimeConfig final {
    scheduler::SchedulerConfig scheduler{};
    log::Level log_level{log::Level::Warn};
    bool log_stdout{false};
    std::string log_directory{"log"};
    std::string log_file{"go2cpp.log"};
    std::string log_format{"{time} [{level}] {logger} ({file}:{line}) {message}\n"};

    // 从 INI 读取并校验运行时配置；失败时不修改 config。
    static bool FromIni(const IniFile& ini, RuntimeConfig* config,
                        std::string* error = nullptr);
    // 校验 P/M、队列、Fiber 栈、时间和日志字段的统一边界。
    bool Validate(std::string* error = nullptr) const;
    // 校验成功后把日志字段应用到全局 LoggerManager。
    bool ApplyLogging(std::string* error = nullptr) const;
};

// 从 path 读取完整 RuntimeConfig；config 必须指向调用方持有的对象。
bool LoadRuntimeConfig(const std::string& path, RuntimeConfig* config,
                      std::string* error = nullptr);

}  // namespace go2cpp::config
