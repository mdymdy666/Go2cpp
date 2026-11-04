/**
 * @file config.cpp
 * @brief Config 的 INI 解析实现。
 * @details 解析失败时保留调用方原值（先在临时对象上组装，全部成功才提交），
 *          避免「读到一半失败」留下半更新配置。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "config.h"

#include "go2cpp/config.hpp"

#include <exception>
#include <sstream>
#include <utility>

namespace app {
namespace {

/// @brief 读取无符号整数；键不存在时保留默认值，格式/范围错误返回 false。
bool ReadUint(const go2cpp::config::IniFile& ini, const char* section, const char* key,
              unsigned long long minimum, unsigned long long maximum, unsigned long long* out,
              std::string* error) {
    const std::string text = ini.Get(section, key, "");
    if (text.empty()) {
        return true;
    }
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed);
        if (consumed != text.size() || value < minimum || value > maximum) {
            throw std::invalid_argument("out of range");
        }
        *out = value;
        return true;
    } catch (const std::exception&) {
        if (error != nullptr) {
            *error = std::string("配置 ") + section + "." + key + " 非法: " + text;
        }
        return false;
    }
}

}  // namespace

Config Config::Default() { return Config{}; }

bool Config::LoadFromFile(const std::string& path, std::string* error) {
    go2cpp::config::IniFile ini;
    if (!ini.Load(path, error)) {
        return false;
    }

    Config candidate = Config::Default();
    candidate.host = ini.Get("server", "host", candidate.host);
    candidate.web_root = ini.Get("server", "web_root", candidate.web_root);
    candidate.log_file = ini.Get("logging", "file", candidate.log_file);
    candidate.log_level = ini.Get("logging", "level", candidate.log_level);

    unsigned long long number = candidate.port;
    if (!ReadUint(ini, "server", "port", 1, 65535, &number, error)) {
        return false;
    }
    candidate.port = static_cast<std::uint16_t>(number);

    number = candidate.worker_threads;
    if (!ReadUint(ini, "server", "worker_threads", 1, 64, &number, error)) {
        return false;
    }
    candidate.worker_threads = static_cast<std::size_t>(number);

    number = candidate.sse_interval_ms;
    if (!ReadUint(ini, "server", "sse_interval_ms", 100, 60000, &number, error)) {
        return false;
    }
    candidate.sse_interval_ms = static_cast<std::uint32_t>(number);

    *this = std::move(candidate);
    return true;
}

std::string Config::Describe() const {
    std::ostringstream out;
    out << "host=" << host << " port=" << port << " workers=" << worker_threads
        << " web_root=" << web_root << " sse_interval_ms=" << sse_interval_ms
        << " log_file=" << log_file << " log_level=" << log_level;
    return out.str();
}

}  // namespace app
