#include "go2cpp/config.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iostream>

namespace go2cpp::config {
namespace {

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ParseSize(const std::string& text, std::size_t* value) {
    if (!value) return false;
    const auto clean = Trim(text);
    if (clean.empty()) return false;
    std::size_t parsed = 0;
    const auto result = std::from_chars(clean.data(), clean.data() + clean.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != clean.data() + clean.size()) return false;
    *value = parsed;
    return true;
}

bool ParseBool(const std::string& text, bool* value) {
    if (!value) return false;
    auto clean = Trim(text);
    std::transform(clean.begin(), clean.end(), clean.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    if (clean == "1" || clean == "true" || clean == "yes" || clean == "on") *value = true;
    else if (clean == "0" || clean == "false" || clean == "no" || clean == "off") *value = false;
    else return false;
    return true;
}

bool ReadSize(const IniFile& ini, const char* section, const char* key,
              std::size_t* target, std::string* error) {
    if (!ini.Has(section, key)) return true;
    if (ParseSize(ini.Get(section, key), target)) return true;
    if (error) *error = std::string("配置项不是非负整数: ") + section + "." + key;
    return false;
}

}  // namespace

bool IniFile::Load(const std::string& path, std::string* error) {
    std::ifstream stream(path);
    if (!stream) { if (error) *error = "无法打开配置文件: " + path; return false; }
    std::ostringstream contents; contents << stream.rdbuf();
    return Parse(contents.str(), error);
}

bool IniFile::Parse(const std::string& text, std::string* error) {
    m_sections.clear();
    std::string current = "global";
    std::istringstream stream(text);
    std::string line; std::size_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        auto clean = Trim(line);
        if (clean.empty() || clean.front() == '#' || clean.front() == ';') continue;
        if (clean.front() == '[') {
            if (clean.back() != ']') { if (error) *error = "节标题缺少 ]，行 " + std::to_string(line_number); return false; }
            current = Trim(clean.substr(1, clean.size() - 2));
            if (current.empty()) { if (error) *error = "节标题为空，行 " + std::to_string(line_number); return false; }
            continue;
        }
        const auto equal = clean.find('=');
        if (equal == std::string::npos) { if (error) *error = "配置项缺少 =，行 " + std::to_string(line_number); return false; }
        const auto key = Trim(clean.substr(0, equal));
        auto value = Trim(clean.substr(equal + 1));
        const auto comment = value.find_first_of("#;");
        if (comment != std::string::npos) value = Trim(value.substr(0, comment));
        for (std::size_t position = 0; (position = value.find("\\n", position)) != std::string::npos;) {
            value.replace(position, 2, "\n");
            ++position;
        }
        if (key.empty()) { if (error) *error = "配置键为空，行 " + std::to_string(line_number); return false; }
        m_sections[current][key] = value;
    }
    return true;
}

std::string IniFile::Get(const std::string& section, const std::string& key,
                         std::string fallback) const {
    const auto section_it = m_sections.find(section);
    if (section_it == m_sections.end()) return fallback;
    const auto value_it = section_it->second.find(key);
    return value_it == section_it->second.end() ? fallback : value_it->second;
}

bool IniFile::Has(const std::string& section, const std::string& key) const {
    const auto section_it = m_sections.find(section);
    return section_it != m_sections.end() && section_it->second.find(key) != section_it->second.end();
}

bool RuntimeConfig::FromIni(const IniFile& ini, RuntimeConfig* config, std::string* error) {
    if (!config) { if (error) *error = "RuntimeConfig 输出指针为空"; return false; }
    RuntimeConfig result = *config;
    if (!ReadSize(ini, "scheduler", "processor_count", &result.scheduler.processor_count, error) ||
        !ReadSize(ini, "scheduler", "max_workers", &result.scheduler.max_workers, error) ||
        !ReadSize(ini, "scheduler", "min_workers", &result.scheduler.min_workers, error) ||
        !ReadSize(ini, "scheduler", "local_queue_limit", &result.scheduler.local_queue_limit, error) ||
        !ReadSize(ini, "scheduler", "fiber_stack_size", &result.scheduler.fiber_stack_size, error) ||
        !ReadSize(ini, "scheduler", "task_affinity_budget", &result.scheduler.task_affinity_budget, error) ||
        !ReadSize(ini, "scheduler", "fiber_bin_capacity", &result.scheduler.fiber_bin_capacity, error)) return false;
    std::size_t value = 0;
    if (ini.Has("scheduler", "idle_wait_ms")) { if (!ReadSize(ini, "scheduler", "idle_wait_ms", &value, error)) return false; result.scheduler.idle_wait = std::chrono::milliseconds(value); }
    if (ini.Has("scheduler", "idle_worker_timeout_ms")) { if (!ReadSize(ini, "scheduler", "idle_worker_timeout_ms", &value, error)) return false; result.scheduler.idle_worker_timeout = std::chrono::milliseconds(value); }
    if (ini.Has("scheduler", "sysmon_interval_ms")) { if (!ReadSize(ini, "scheduler", "sysmon_interval_ms", &value, error)) return false; result.scheduler.sysmon_interval = std::chrono::milliseconds(value); }
    if (ini.Has("scheduler", "long_syscall_threshold_ms")) { if (!ReadSize(ini, "scheduler", "long_syscall_threshold_ms", &value, error)) return false; result.scheduler.long_syscall_threshold = std::chrono::milliseconds(value); }
    if (ini.Has("scheduler", "allow_worker_oversubscription") && !ParseBool(ini.Get("scheduler", "allow_worker_oversubscription"), &result.scheduler.allow_worker_oversubscription)) { if (error) *error = "allow_worker_oversubscription 不是布尔值"; return false; }
    if (ini.Has("scheduler", "enable_sysmon") && !ParseBool(ini.Get("scheduler", "enable_sysmon"), &result.scheduler.enable_sysmon)) { if (error) *error = "enable_sysmon 不是布尔值"; return false; }
    if (ini.Has("scheduler", "pin_workers_to_cpu") && !ParseBool(ini.Get("scheduler", "pin_workers_to_cpu"), &result.scheduler.pin_workers_to_cpu)) { if (error) *error = "pin_workers_to_cpu 不是布尔值"; return false; }
    if (ini.Has("log", "level") && !log::ParseLevel(ini.Get("log", "level"), &result.log_level)) { if (error) *error = "log.level 不是有效级别"; return false; }
    if (ini.Has("log", "stdout") && !ParseBool(ini.Get("log", "stdout"), &result.log_stdout)) { if (error) *error = "log.stdout 不是布尔值"; return false; }
    result.log_directory = ini.Get("log", "directory", result.log_directory);
    result.log_file = ini.Get("log", "file", result.log_file);
    result.log_format = ini.Get("log", "format", result.log_format);
    if (!result.Validate(error)) return false;
    *config = std::move(result);
    return true;
}

bool RuntimeConfig::Validate(std::string* error) const {
    const auto fail = [error](const std::string& message) { if (error) *error = message; return false; };
    const auto& value = scheduler;
    if (value.min_workers != 0 && value.min_workers < 1) return fail("scheduler.min_workers 必须至少为 1");
    if (value.max_workers > 32) return fail("scheduler.max_workers 不能超过 32");
    if (value.max_workers != 0 && value.min_workers != 0 && value.max_workers < value.min_workers) return fail("scheduler.max_workers 不能小于 min_workers");
    if (value.processor_count > 32) return fail("scheduler.processor_count 不能超过 32");
    if (value.local_queue_limit == 0) return fail("scheduler.local_queue_limit 必须大于 0");
    if (value.fiber_bin_capacity > 4096) return fail("scheduler.fiber_bin_capacity 不能超过 4096");
    if (log_directory.empty() || log_file.empty()) return fail("日志目录和文件名不能为空");
    return true;
}

bool RuntimeConfig::ApplyLogging(std::string* error) const {
    if (!Validate(error)) return false;
    log::LoggerManager::Instance().Configure(log_level, log_stdout, log_directory, log_file, log_format);
    return true;
}

bool LoadRuntimeConfig(const std::string& path, RuntimeConfig* config, std::string* error) {
    IniFile ini;
    return ini.Load(path, error) && RuntimeConfig::FromIni(ini, config, error);
}

Config& Config::Instance() {
    static Config instance;
    return instance;
}

Config::~Config() { StopWatcher(); }

ConfigVarBase::ptr Config::LookupBase(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_vars.find(name);
    return it == m_vars.end() ? nullptr : it->second;
}

bool Config::LoadFromIni(const IniFile& ini, std::string* error) {
    const auto variables = List();
    // 先检查所有已注册项对应的文本能否转换；实际写入仍按变量顺序完成，
    // 这样错误能指出具体变量，同时不要求所有类型都暴露内部临时值。
    std::vector<std::pair<ConfigVarBase::ptr, std::string>> pending;
    for (const auto& variable : variables) {
        if (!variable) continue;
        const auto separator = variable->name().find('.');
        const auto section = separator == std::string::npos ? "global" : variable->name().substr(0, separator);
        const auto key = separator == std::string::npos ? variable->name() : variable->name().substr(separator + 1);
        if (!ini.Has(section, key)) continue;
        const auto text = ini.Get(section, key);
        if (!variable->ValidateString(text, error)) return false;
        pending.emplace_back(variable, text);
    }
    for (const auto& [variable, text] : pending) if (!variable->FromString(text, error)) return false;
    return true;
}

bool Config::LoadFromFile(const std::string& path, std::string* error) {
    IniFile ini;
    return ini.Load(path, error) && LoadFromIni(ini, error);
}

std::vector<ConfigVarBase::ptr> Config::List() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ConfigVarBase::ptr> result;
    result.reserve(m_vars.size());
    for (const auto& [name, variable] : m_vars) result.push_back(variable);
    return result;
}

bool Config::StartWatcher(const std::string& path, std::chrono::milliseconds interval, std::string* error) {
    if (path.empty() || interval <= std::chrono::milliseconds::zero()) {
        if (error) *error = "config watcher path or interval is invalid";
        return false;
    }
    std::lock_guard<std::mutex> lifecycle_lock(m_watcher_lifecycle_mutex);
    stop_watcher_locked();
    std::error_code read_error;
    auto stamp = std::filesystem::last_write_time(path, read_error);
    if (read_error) {
        if (error) *error = "cannot read config timestamp: " + path;
        return false;
    }
    if (!LoadFromFile(path, error)) return false;
    m_watching.store(true, std::memory_order_release);
    m_watcher = std::thread([this, path, interval, stamp]() mutable {
        while (m_watching.load(std::memory_order_acquire)) {
            // 条件变量让 StopWatcher 立即唤醒，不必等待一个完整刷新周期。
            std::unique_lock<std::mutex> wait_lock(m_watcher_mutex);
            m_watcher_cv.wait_for(wait_lock, interval, [this] {
                return !m_watching.load(std::memory_order_acquire);
            });
            wait_lock.unlock();
            if (!m_watching.load(std::memory_order_acquire)) break;
            std::error_code read_error;
            const auto current = std::filesystem::last_write_time(path, read_error);
            if (read_error || current == stamp) continue;
            std::string error;
            try {
                if (LoadFromFile(path, &error)) {
                    stamp = current;
                } else {
                    std::cerr << "Go2Cpp 配置热加载失败: " << error << '\n';
                    stamp = current;
                }
            } catch (const std::exception& exception) {
                // 监听器属于用户扩展点，异常不能逃出 watcher 线程；
                // 记录本次失败后继续监视后续文件变化。
                std::cerr << "Go2Cpp 配置监听器异常: " << exception.what()
                          << '\n';
                stamp = current;
            } catch (...) {
                std::cerr << "Go2Cpp 配置监听器抛出未知异常\n";
                stamp = current;
            }
        }
    });
    return true;
}

void Config::stop_watcher_locked() noexcept {
    m_watching.store(false, std::memory_order_release);
    m_watcher_cv.notify_all();
    if (m_watcher.joinable()) {
        if (m_watcher.get_id() == std::this_thread::get_id()) m_watcher.detach();
        else m_watcher.join();
    }
}

void Config::StopWatcher() {
    std::lock_guard<std::mutex> lifecycle_lock(m_watcher_lifecycle_mutex);
    stop_watcher_locked();
}

bool BindLoggingConfig(Config& config, std::string* error) {
    auto level = config.Lookup<log::Level>("log.level", log::Level::Warn, "日志最低级别");
    auto stdout_enabled = config.Lookup<bool>("log.stdout", false, "是否输出到 stdout");
    auto directory = config.Lookup<std::string>("log.directory", "log", "日志目录");
    auto file = config.Lookup<std::string>("log.file", "go2cpp.log", "日志文件名");
    auto format = config.Lookup<std::string>("log.format", "{time} [{level}] {logger} ({file}:{line}) {message}\n", "日志格式");
    if (!level || !stdout_enabled || !directory || !file || !format) {
        if (error) *error = "日志配置变量类型冲突";
        return false;
    }
    const std::weak_ptr<ConfigVar<log::Level>> weak_level = level;
    const std::weak_ptr<ConfigVar<bool>> weak_stdout = stdout_enabled;
    const std::weak_ptr<ConfigVar<std::string>> weak_directory = directory;
    const std::weak_ptr<ConfigVar<std::string>> weak_file = file;
    const std::weak_ptr<ConfigVar<std::string>> weak_format = format;
    const auto apply = [weak_level, weak_stdout, weak_directory, weak_file, weak_format]() {
        const auto current_level = weak_level.lock();
        const auto current_stdout = weak_stdout.lock();
        const auto current_directory = weak_directory.lock();
        const auto current_file = weak_file.lock();
        const auto current_format = weak_format.lock();
        if (!current_level || !current_stdout || !current_directory || !current_file || !current_format) return;
        log::LoggerManager::Instance().Configure(current_level->GetValue(), current_stdout->GetValue(),
                                                  current_directory->GetValue(), current_file->GetValue(), current_format->GetValue());
    };
    level->AddListener([apply](const log::Level&, const log::Level&) { apply(); });
    stdout_enabled->AddListener([apply](const bool&, const bool&) { apply(); });
    directory->AddListener([apply](const std::string&, const std::string&) { apply(); });
    file->AddListener([apply](const std::string&, const std::string&) { apply(); });
    format->AddListener([apply](const std::string&, const std::string&) { apply(); });
    apply();
    return true;
}

bool BindRuntimeConfig(Config& config, RuntimeConfig* target, std::string* error) {
    if (!target) { if (error) *error = "RuntimeConfig 绑定目标为空"; return false; }
    if (!BindLoggingConfig(config, error)) return false;
    bool valid = true;
    const auto bind_size = [&config, target, &valid](const std::string& name, std::size_t* field, const char* description) {
        auto variable = config.Lookup<std::size_t>(name, *field, description);
        if (!variable) { valid = false; return; }
        variable->AddListener([field](const std::size_t&, const std::size_t& value) { *field = value; });
    };
    bind_size("scheduler.processor_count", &target->scheduler.processor_count, "P 数量，0 为自动");
    bind_size("scheduler.min_workers", &target->scheduler.min_workers, "最小 M 数量");
    bind_size("scheduler.max_workers", &target->scheduler.max_workers, "最大 M 数量");
    bind_size("scheduler.local_queue_limit", &target->scheduler.local_queue_limit, "P 本地队列容量");
    bind_size("scheduler.fiber_stack_size", &target->scheduler.fiber_stack_size, "Fiber 初始栈大小");
    bind_size("scheduler.task_affinity_budget", &target->scheduler.task_affinity_budget, "任务亲和预算");
    auto fiber_bin_capacity = config.Lookup<std::size_t>(
        "scheduler.fiber_bin_capacity", target->scheduler.fiber_bin_capacity,
        "每个 M 的 FiberBin 容量，最多 4096");
    if (!fiber_bin_capacity) {
        valid = false;
    } else {
        fiber_bin_capacity->AddListener(
            [target](const std::size_t&, const std::size_t& value) {
                target->scheduler.fiber_bin_capacity =
                    std::min<std::size_t>(value, 4096U);
            });
    }
    auto allow_oversubscription = config.Lookup<bool>("scheduler.allow_worker_oversubscription", target->scheduler.allow_worker_oversubscription, "允许 M 超过 P");
    auto enable_sysmon = config.Lookup<bool>("scheduler.enable_sysmon", target->scheduler.enable_sysmon, "启用 sysmon");
    auto pin_workers = config.Lookup<bool>("scheduler.pin_workers_to_cpu", target->scheduler.pin_workers_to_cpu, "绑定 CPU");
    if (!allow_oversubscription || !enable_sysmon || !pin_workers) valid = false;
    if (allow_oversubscription) {
        allow_oversubscription->AddListener([target](const bool&, const bool& value) {
            target->scheduler.allow_worker_oversubscription = value;
        });
    }
    if (enable_sysmon) {
        enable_sysmon->AddListener([target](const bool&, const bool& value) {
            target->scheduler.enable_sysmon = value;
        });
    }
    if (pin_workers) {
        pin_workers->AddListener([target](const bool&, const bool& value) {
            target->scheduler.pin_workers_to_cpu = value;
        });
    }
    auto idle_wait = config.Lookup<std::size_t>("scheduler.idle_wait_ms", target->scheduler.idle_wait.count(), "空闲等待毫秒");
    auto idle_timeout = config.Lookup<std::size_t>("scheduler.idle_worker_timeout_ms", target->scheduler.idle_worker_timeout.count(), "空闲 M 回收毫秒");
    auto sysmon_interval = config.Lookup<std::size_t>("scheduler.sysmon_interval_ms", target->scheduler.sysmon_interval.count(), "sysmon 周期毫秒");
    auto syscall_threshold = config.Lookup<std::size_t>("scheduler.long_syscall_threshold_ms", target->scheduler.long_syscall_threshold.count(), "长系统调用阈值毫秒");
    if (!idle_wait || !idle_timeout || !sysmon_interval || !syscall_threshold) valid = false;
    if (idle_wait) {
        idle_wait->AddListener([target](const std::size_t&, const std::size_t& value) {
            target->scheduler.idle_wait = std::chrono::milliseconds(value);
        });
    }
    if (idle_timeout) {
        idle_timeout->AddListener([target](const std::size_t&, const std::size_t& value) {
            target->scheduler.idle_worker_timeout = std::chrono::milliseconds(value);
        });
    }
    if (sysmon_interval) {
        sysmon_interval->AddListener([target](const std::size_t&, const std::size_t& value) {
            target->scheduler.sysmon_interval = std::chrono::milliseconds(value);
        });
    }
    if (syscall_threshold) {
        syscall_threshold->AddListener([target](const std::size_t&, const std::size_t& value) {
            target->scheduler.long_syscall_threshold = std::chrono::milliseconds(value);
        });
    }
    if (!valid && error) *error = "运行时配置变量类型冲突";
    return valid;
}

}  // namespace go2cpp::config
