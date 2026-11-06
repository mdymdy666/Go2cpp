/**
 * @file douyin_proto.hpp
 * @brief 抖音 P0 共享 IDL：六个服务与网关共用的数据结构。
 *
 * @details 只定义结构与最小 JSON 编解码，不含任何业务逻辑、存储或 RPC。
 *          各服务后续通过 RPC 传递这些结构，网关把结构序列化成 REST JSON。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace douyin {

/// @brief 用户（注册 / 资料）。
struct User {
    std::int64_t id{0};
    std::string name;
    std::string email;
    std::int64_t created_at{0};
};

/// @brief 视频（发布 / 详情）。
struct Video {
    std::int64_t id{0};
    std::int64_t user_id{0};
    std::string title;
    std::string url;
    std::int64_t like_count{0};     ///< 点赞数（interaction 服务维护）。
    std::int64_t comment_count{0};  ///< 评论数（interaction 服务维护）。
    std::int64_t created_at{0};
    std::string cover_url;          ///< 封面（`videos/cover_*.jpg`；空=同名 jpg）。
    std::int64_t clip_start_ms{0};  ///< 裁剪起点（毫秒，含）；0=从头播。
    std::int64_t clip_end_ms{0};    ///< 裁剪终点（毫秒，不含）；0=到片尾。
    std::vector<std::string> tags;  ///< 话题标签（由标题提取；空 = 无话题）。
};

/// @brief feed 流中的一项：视频 + 作者名。
struct FeedItem {
    Video video;
    std::string author;
};

/// @brief 评论。
struct Comment {
    std::int64_t id{0};
    std::int64_t video_id{0};
    std::int64_t user_id{0};
    std::string content;
    std::int64_t created_at{0};
    std::int64_t parent_id{0};  ///< 二级回复的父评论 id；0=顶层评论。
    std::int64_t like_count{0}; ///< 评论点赞数（interaction 服务维护）。
    bool liked{false};          ///< 当前查看者是否已赞；匿名恒 false。
    std::int64_t pinned_at{0};  ///< 置顶时间（Unix 秒）；0=未置顶。
};

/// @brief 点赞（video_id + user_id 唯一）。
struct Like {
    std::int64_t video_id{0};
    std::int64_t user_id{0};
};

/// @brief 直播间：status 0=关闭 1=直播中。
struct Room {
    std::int64_t id{0};
    std::int64_t user_id{0};
    std::string title;
    int status{0};
};

/// @brief 搜索结果项：type 取 user/video/live。
struct SearchResult {
    std::string type;
    std::int64_t id{0};
    std::string title;
};

/// @brief 序列化为单行 JSON。
std::string ToJson(const User& value);
std::string ToJson(const Video& value);
std::string ToJson(const FeedItem& value);
std::string ToJson(const Comment& value);
std::string ToJson(const Like& value);
std::string ToJson(const Room& value);
std::string ToJson(const SearchResult& value);

/// @brief 从 JSON 文本解析；缺字段保留默认值，文本非法返回 false。
bool FromJson(const std::string& text, User* out);
bool FromJson(const std::string& text, Video* out);
bool FromJson(const std::string& text, FeedItem* out);
bool FromJson(const std::string& text, Comment* out);
bool FromJson(const std::string& text, Like* out);
bool FromJson(const std::string& text, Room* out);
bool FromJson(const std::string& text, SearchResult* out);

}  // namespace douyin
