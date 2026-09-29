#include "go2cpp/config.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

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
        !ReadSize(ini, "scheduler", "task_affinity_budget", &result.scheduler.task_affinity_budget, error)) return false;
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

}  // namespace go2cpp::config
