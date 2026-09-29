#pragma once

// 简单、可审查的 ini 配置。解析器只处理 section、key=value 和注释，避免
// 引入 YAML 等重量依赖；运行时配置在加载后统一校验，再交给各模块应用。

#include "go2cpp/log.hpp"
#include "go2cpp/scheduler/scheduler.hpp"

#include <cstddef>
#include <atomic>
#include <chrono>
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

class IniFile final {
public:
    bool Load(const std::string& path, std::string* error = nullptr);
    bool Parse(const std::string& text, std::string* error = nullptr);
    std::string Get(const std::string& section, const std::string& key,
                    std::string fallback = {}) const;
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
class ConfigVar final : public ConfigVarBase {
public:
    using ptr = std::shared_ptr<ConfigVar<T>>;
    using Listener = std::function<void(const T& old_value, const T& new_value)>;
    ConfigVar(std::string name, T value, std::string description)
        : ConfigVarBase(std::move(name), std::move(description)), m_value(std::move(value)) {}
    T GetValue() const { std::shared_lock<std::shared_mutex> lock(m_mutex); return m_value; }
    void SetValue(const T& value) {
        T old_value;
        std::map<std::uint64_t, Listener> listeners;
        { std::unique_lock<std::shared_mutex> lock(m_mutex); if (m_value == value) return; old_value = m_value; m_value = value; listeners = m_listeners; }
        for (const auto& [id, listener] : listeners) if (listener) listener(old_value, value);
    }
    std::uint64_t AddListener(Listener listener) {
        const auto id = m_next_listener.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock<std::shared_mutex> lock(m_mutex); m_listeners.emplace(id, std::move(listener)); return id;
    }
    void DelListener(std::uint64_t id) { std::unique_lock<std::shared_mutex> lock(m_mutex); m_listeners.erase(id); }
    std::string ToString() const override { return ValueCodec<T>::Encode(GetValue()); }
    bool ValidateString(const std::string& value, std::string* error = nullptr) const override {
        T parsed{};
        if (ValueCodec<T>::Decode(value, &parsed)) return true;
        if (error) *error = "配置值转换失败: " + name();
        return false;
    }
    bool FromString(const std::string& value, std::string* error = nullptr) override {
        T parsed{};
        if (!ValueCodec<T>::Decode(value, &parsed)) { if (error) *error = "配置值转换失败: " + name(); return false; }
        SetValue(parsed); return true;
    }

private:
    mutable std::shared_mutex m_mutex;
    T m_value;
    std::map<std::uint64_t, Listener> m_listeners;
    std::atomic<std::uint64_t> m_next_listener{1};
};

class Config final {
public:
    static Config& Instance();
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
    ConfigVarBase::ptr LookupBase(const std::string& name) const;
    bool LoadFromIni(const IniFile& ini, std::string* error = nullptr);
    bool LoadFromFile(const std::string& path, std::string* error = nullptr);
    std::vector<ConfigVarBase::ptr> List() const;
    bool StartWatcher(const std::string& path, std::chrono::milliseconds interval = std::chrono::milliseconds(1000));
    void StopWatcher();
    bool Watching() const noexcept { return m_watching.load(std::memory_order_acquire); }

private:
    Config() = default;
    ~Config();
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, ConfigVarBase::ptr> m_vars;
    std::atomic<bool> m_watching{false};
    std::thread m_watcher;
};

// 把配置中心的动态变量接到 LoggerManager。之后修改 ini 并由 watcher 重载，
// 已存在的 Logger 会立即切换级别、格式、文件和 stdout 输出。
bool BindLoggingConfig(Config& config, std::string* error = nullptr);
bool BindRuntimeConfig(Config& config, RuntimeConfig* target,
                       std::string* error = nullptr);

struct RuntimeConfig final {
    scheduler::SchedulerConfig scheduler{};
    log::Level log_level{log::Level::Warn};
    bool log_stdout{false};
    std::string log_directory{"log"};
    std::string log_file{"go2cpp.log"};
    std::string log_format{"{time} [{level}] {logger} ({file}:{line}) {message}\n"};

    static bool FromIni(const IniFile& ini, RuntimeConfig* config,
                        std::string* error = nullptr);
    bool Validate(std::string* error = nullptr) const;
    bool ApplyLogging(std::string* error = nullptr) const;
};

bool LoadRuntimeConfig(const std::string& path, RuntimeConfig* config,
                      std::string* error = nullptr);

}  // namespace go2cpp::config
