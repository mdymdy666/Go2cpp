/**
 * @file douyin_proto.cpp
 * @brief 共享 IDL 的最小 JSON 实现（手写，不引第三方库）。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_proto.hpp"

#include <cstdlib>

namespace douyin {
namespace {

/// @brief 转义 JSON 字符串内容。
std::string EscapeJson(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

/// @brief 取 `"key":value` 的原始值文本（字符串去掉两侧引号）。
bool FindRaw(const std::string& text, const std::string& key, std::string* raw) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
        ++pos;
    }
    if (pos >= text.size()) {
        return false;
    }
    if (text[pos] == '"') {
        const std::size_t end = text.find('"', pos + 1);
        if (end == std::string::npos) {
            return false;
        }
        *raw = text.substr(pos + 1, end - pos - 1);
        return true;
    }
    std::size_t end = pos;
    while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != ']') {
        ++end;
    }
    *raw = text.substr(pos, end - pos);
    return true;
}

/// @brief 取嵌套对象 `"key":{...}` 的子文本（含花括号）。
bool FindObject(const std::string& text, const std::string& key, std::string* out) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos = text.find('{', pos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    int depth = 0;
    for (std::size_t i = pos; i < text.size(); ++i) {
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}') {
            --depth;
            if (depth == 0) {
                *out = text.substr(pos, i - pos + 1);
                return true;
            }
        }
    }
    return false;
}

bool ReadInt(const std::string& text, const std::string& key, std::int64_t* out) {
    std::string raw;
    if (!FindRaw(text, key, &raw) || raw.empty()) {
        return false;
    }
    char* end = nullptr;
    const long long value = std::strtoll(raw.c_str(), &end, 10);
    if (end == raw.c_str()) {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

bool ReadString(const std::string& text, const std::string& key, std::string* out) {
    return FindRaw(text, key, out);
}

std::string Key(const char* name) { return std::string("\"") + name + "\":"; }

}  // namespace

std::string ToJson(const User& value) {
    return std::string("{\"id\":") + std::to_string(value.id) + "," + Key("name") + "\"" +
           EscapeJson(value.name) + "\"," + Key("email") + "\"" + EscapeJson(value.email) + "\"," +
           Key("created_at") + std::to_string(value.created_at) + "," +
           Key("bio") + "\"" + EscapeJson(value.bio) + "\"," +
           Key("avatar_url") + "\"" + EscapeJson(value.avatar_url) + "\"}";
}

std::string ToJson(const Video& value) {
    std::string body = std::string("{\"id\":") + std::to_string(value.id) + "," +
                       Key("user_id") + std::to_string(value.user_id) + "," +
                       Key("title") + "\"" + EscapeJson(value.title) + "\"," +
                       Key("url") + "\"" + EscapeJson(value.url) + "\"," +
                       Key("like_count") + std::to_string(value.like_count) + "," +
                       Key("comment_count") + std::to_string(value.comment_count) + "," +
                       Key("created_at") + std::to_string(value.created_at) + "," +
                       Key("cover_url") + "\"" + EscapeJson(value.cover_url) + "\"," +
                       Key("clip_start_ms") + std::to_string(value.clip_start_ms) + "," +
                       Key("clip_end_ms") + std::to_string(value.clip_end_ms) + ",";
    body += Key("tags") + "[";
    for (std::size_t i = 0; i < value.tags.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "\"" + EscapeJson(value.tags[i]) + "\"";
    }
    body += "]}";
    return body;
}

std::string ToJson(const FeedItem& value) {
    return std::string("{\"video\":") + ToJson(value.video) + "," + Key("author") + "\"" +
           EscapeJson(value.author) + "\"}";
}

std::string ToJson(const Comment& value) {
    return std::string("{\"id\":") + std::to_string(value.id) + "," + Key("video_id") +
           std::to_string(value.video_id) + "," + Key("user_id") +
           std::to_string(value.user_id) + "," + Key("content") + "\"" +
           EscapeJson(value.content) + "\"," + Key("created_at") +
           std::to_string(value.created_at) + "," + Key("parent_id") +
           std::to_string(value.parent_id) + "," + Key("like_count") +
           std::to_string(value.like_count) + "," + Key("liked") +
           (value.liked ? "true" : "false") + "," + Key("pinned_at") +
           std::to_string(value.pinned_at) + "," + Key("author_name") + "\"" +
           EscapeJson(value.author_name) + "\"}";
}

std::string ToJson(const Like& value) {
    return std::string("{\"video_id\":") + std::to_string(value.video_id) + "," +
           Key("user_id") + std::to_string(value.user_id) + "}";
}

std::string ToJson(const Room& value) {
    return std::string("{\"id\":") + std::to_string(value.id) + "," + Key("user_id") +
           std::to_string(value.user_id) + "," + Key("title") + "\"" +
           EscapeJson(value.title) + "\"," + Key("status") + std::to_string(value.status) + "}";
}

std::string ToJson(const SearchResult& value) {
    return std::string("{\"type\":\"") + EscapeJson(value.type) + "\"," + Key("id") +
           std::to_string(value.id) + "," + Key("title") + "\"" + EscapeJson(value.title) + "\"}";
}

bool FromJson(const std::string& text, User* out) {
    if (out == nullptr || text.find('{') == std::string::npos) {
        return false;
    }
    User value;
    ReadInt(text, "id", &value.id);
    ReadString(text, "name", &value.name);
    ReadString(text, "email", &value.email);
    ReadInt(text, "created_at", &value.created_at);
    *out = std::move(value);
    return true;
}

bool FromJson(const std::string& text, Video* out) {
    if (out == nullptr || text.find('{') == std::string::npos) {
        return false;
    }
    Video value;
    ReadInt(text, "id", &value.id);
    ReadInt(text, "user_id", &value.user_id);
    ReadString(text, "title", &value.title);
    ReadString(text, "url", &value.url);
    ReadInt(text, "like_count", &value.like_count);
    ReadInt(text, "comment_count", &value.comment_count);
    ReadInt(text, "created_at", &value.created_at);
    *out = std::move(value);
    return true;
}

bool FromJson(const std::string& text, FeedItem* out) {
    if (out == nullptr || text.find('{') == std::string::npos) {
        return false;
    }
    FeedItem value;
    std::string video_text;
    if (FindObject(text, "video", &video_text)) {
        FromJson(video_text, &value.video);
    }
    ReadString(text, "author", &value.author);
    *out = std::move(value);
    return true;
}

bool FromJson(const std::string& text, Comment* out) {
    if (out == nullptr || text.find('{') == std::string::npos) {
        return false;
    }
    Comment value;
    ReadInt(text, "id", &value.id);
    ReadInt(text, "video_id", &value.video_id);
    ReadInt(text, "user_id", &value.user_id);
    ReadString(text, "content", &value.content);
    ReadInt(text, "created_at", &value.created_at);
    ReadInt(text, "parent_id", &value.parent_id);
    *out = std::move(value);
    return true;
}

bool FromJson(const std::string& text, Like* out) {
    if (out == nullptr || text.find('{') == std::string::npos) {
        return false;
    }
    Like value;
    ReadInt(text, "video_id", &value.video_id);
    ReadInt(text, "user_id", &value.user_id);
    *out = value;
    return true;
}

bool FromJson(const std::string& text, Room* out) {
    if (out == nullptr || text.find('{') == std::string::npos) {
        return false;
    }
    Room value;
    std::int64_t status = 0;
    ReadInt(text, "id", &value.id);
    ReadInt(text, "user_id", &value.user_id);
    ReadString(text, "title", &value.title);
    if (ReadInt(text, "status", &status)) {
        value.status = static_cast<int>(status);
    }
    *out = std::move(value);
    return true;
}

bool FromJson(const std::string& text, SearchResult* out) {
    if (out == nullptr || text.find('{') == std::string::npos) {
        return false;
    }
    SearchResult value;
    ReadString(text, "type", &value.type);
    ReadInt(text, "id", &value.id);
    ReadString(text, "title", &value.title);
    *out = std::move(value);
    return true;
}

}  // namespace douyin
