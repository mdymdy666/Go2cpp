/**
 * @file config.cpp
 * @brief Config 的 INI 解析：先组装临时对象，全部成功才提交。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "config.h"

#include "go2cpp/config.hpp"

#include <exception>
#include <sstream>
#include <utility>

namespace chat {
namespace {

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
            throw std::invalid_argument("range");
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
    candidate.host = ini.Get("chat", "host", candidate.host);
    candidate.history_file = ini.Get("chat", "history_file", candidate.history_file);
    candidate.log_file = ini.Get("logging", "file", candidate.log_file);
    candidate.log_level = ini.Get("logging", "level", candidate.log_level);

    unsigned long long number = candidate.port;
    if (!ReadUint(ini, "chat", "port", 1, 65535, &number, error)) return false;
    candidate.port = static_cast<std::uint16_t>(number);

    number = candidate.max_peers;
    if (!ReadUint(ini, "chat", "max_peers", 1, 4096, &number, error)) return false;
    candidate.max_peers = static_cast<std::size_t>(number);

    number = candidate.heartbeat_ms;
    if (!ReadUint(ini, "chat", "heartbeat_ms", 50, 60000, &number, error)) return false;
    candidate.heartbeat_ms = static_cast<std::uint32_t>(number);

    number = candidate.peer_timeout_ms;
    if (!ReadUint(ini, "chat", "peer_timeout_ms", 500, 600000, &number, error)) return false;
    candidate.peer_timeout_ms = static_cast<std::uint32_t>(number);

    number = candidate.app_ping_interval_ms;
    if (!ReadUint(ini, "chat", "app_ping_interval_ms", 500, 600000, &number, error)) return false;
    candidate.app_ping_interval_ms = static_cast<std::uint32_t>(number);

    number = candidate.app_session_timeout_ms;
    if (!ReadUint(ini, "chat", "app_session_timeout_ms", 1000, 3600000, &number, error)) {
        return false;
    }
    candidate.app_session_timeout_ms = static_cast<std::uint32_t>(number);

    number = candidate.reconnect_grace_ms;
    if (!ReadUint(ini, "chat", "reconnect_grace_ms", 1000, 3600000, &number, error)) return false;
    candidate.reconnect_grace_ms = static_cast<std::uint32_t>(number);

    number = candidate.max_message_bytes;
    if (!ReadUint(ini, "chat", "max_message_bytes", 64, 65536, &number, error)) return false;
    candidate.max_message_bytes = static_cast<std::size_t>(number);

    number = candidate.replay_lines;
    if (!ReadUint(ini, "chat", "replay_lines", 0, 200, &number, error)) return false;
    candidate.replay_lines = static_cast<std::size_t>(number);

    *this = std::move(candidate);
    return true;
}

std::string Config::Describe() const {
    std::ostringstream out;
    out << "host=" << host << " port=" << port << " max_peers=" << max_peers
        << " peer_timeout_ms=" << peer_timeout_ms << " ping_ms=" << app_ping_interval_ms
        << " grace_ms=" << reconnect_grace_ms << " history=" << history_file;
    return out.str();
}

}  // namespace chat
