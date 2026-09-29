#pragma once

// 简单、可审查的 ini 配置。解析器只处理 section、key=value 和注释，避免
// 引入 YAML 等重量依赖；运行时配置在加载后统一校验，再交给各模块应用。

#include "go2cpp/log.hpp"
#include "go2cpp/scheduler/scheduler.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>

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
