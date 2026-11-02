/**
 * @file protocol.cpp
 * @brief protocol.h 的实现：解析、校验与下行消息拼装。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "protocol.h"

#include <cctype>
#include <utility>

namespace chat {
namespace {

bool IsSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

}  // namespace

std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && IsSpace(text[begin])) ++begin;
    while (end > begin && IsSpace(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

bool ParseCommand(std::string_view line, Command* out) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || out == nullptr) {
        return false;
    }
    const std::size_t space = trimmed.find_first_of(" \t");
    const std::string verb_text = trimmed.substr(0, space);
    if (verb_text.empty() || verb_text.size() > 16) {
        return false;
    }
    std::string verb;
    verb.reserve(verb_text.size());
    for (const char ch : verb_text) {
        if (!std::isalpha(static_cast<unsigned char>(ch))) {
            return false;
        }
        verb.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    out->verb = std::move(verb);
    out->rest = (space == std::string::npos) ? std::string{} : Trim(trimmed.substr(space + 1));
    return true;
}

bool SplitFirst(std::string_view text, std::string* first, std::string* remaining) {
    const std::string trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }
    const std::size_t space = trimmed.find_first_of(" \t");
    if (first != nullptr) {
        *first = trimmed.substr(0, space);
    }
    if (remaining != nullptr) {
        *remaining = (space == std::string::npos) ? std::string{} : Trim(trimmed.substr(space + 1));
    }
    return true;
}

bool IsSafeName(const std::string& name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    for (const char ch : name) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte == 0x7f || ch == '|') {
            return false;
        }
    }
    return true;
}

bool IsSafeRoom(const std::string& room) {
    if (room.empty() || room.size() > 24) {
        return false;
    }
    for (const char ch : room) {
        const bool allowed = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                             (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

std::string FormatRoomMessage(const std::string& room, const std::string& from,
                              const std::string& text) {
    return "MSG " + room + " " + from + " " + text;
}

std::string FormatDirectMessage(const std::string& from, const std::string& text) {
    return "DM " + from + " " + text;
}

std::string FormatHistory(const std::string& room, const std::string& from,
                          const std::string& text) {
    return "HIST " + room + " " + from + " " + text;
}

std::string FormatSystem(const std::string& text) { return "SYS " + text; }
std::string FormatError(const std::string& text) { return "ERROR " + text; }
std::string FormatOk(const std::string& text) { return "OK " + text; }
std::string FormatToken(const std::string& token) { return "TOKEN " + token; }

std::string JoinList(const std::vector<std::string>& items) {
    std::string output;
    for (const auto& item : items) {
        if (!output.empty()) output.push_back(' ');
        output += item;
    }
    return output;
}

std::string FormatOnline(const std::vector<std::string>& names) {
    return "WHO " + JoinList(names);
}

std::string FormatRooms(const std::vector<std::string>& rooms) {
    return "ROOMS " + JoinList(rooms);
}

}  // namespace chat
