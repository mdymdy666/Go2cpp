/**
 * @file store.cpp
 * @brief MessageStore 的实现：追加、清洗字段、尾部读取。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "store.h"

#include <deque>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

namespace chat {
namespace {

/// @brief 把用户文本清洗成单行、无分隔符冲突的形式。
std::string Sanitize(std::string_view text) {
    std::string output;
    output.reserve(text.size());
    for (const char ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '|') {
            output.push_back(' ');
            continue;
        }
        output.push_back(ch);
    }
    return output;
}

std::vector<std::string> Split(const std::string& line, char separator, std::size_t limit) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (parts.size() + 1 < limit) {
        const std::size_t end = line.find(separator, begin);
        if (end == std::string::npos) {
            break;
        }
        parts.push_back(line.substr(begin, end - begin));
        begin = end + 1;
    }
    parts.push_back(line.substr(begin));
    return parts;
}

}  // namespace

MessageStore::MessageStore(std::string path) : m_path(std::move(path)) {}

bool MessageStore::Append(const StoredMessage& message, std::string* error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ofstream output(m_path, std::ios::out | std::ios::app);
    if (!output) {
        if (error != nullptr) *error = "无法打开持久化文件: " + m_path;
        return false;
    }
    output << message.epoch_ms << '|' << Sanitize(message.room) << '|' << Sanitize(message.from)
           << '|' << Sanitize(message.text) << '\n';
    if (!output.good()) {
        if (error != nullptr) *error = "写入持久化文件失败: " + m_path;
        return false;
    }
    return true;
}

std::vector<StoredMessage> MessageStore::Recent(std::size_t max_lines) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ifstream input(m_path);
    if (!input) {
        return {};
    }
    std::deque<StoredMessage> tail;
    std::string line;
    while (std::getline(input, line)) {
        const auto parts = Split(line, '|', 4);
        if (parts.size() != 4) {
            continue;
        }
        StoredMessage message;
        try {
            message.epoch_ms = std::stoull(parts[0]);
        } catch (const std::exception&) {
            continue;
        }
        message.room = parts[1];
        message.from = parts[2];
        message.text = parts[3];
        tail.push_back(std::move(message));
        if (tail.size() > max_lines) {
            tail.pop_front();
        }
    }
    return std::vector<StoredMessage>(tail.begin(), tail.end());
}

}  // namespace chat
