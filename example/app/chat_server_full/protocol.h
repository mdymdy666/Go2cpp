#pragma once

/**
 * @file protocol.h
 * @brief chat_server_full 的文本协议：命令解析与下行消息编码。
 * @details 每条消息是一行 UTF-8 文本，命令大小写不敏感。选择文本协议是为了
 *          让 README 里的示例与 tcpdump/日志都能直接读懂；真实产品应换成
 *          带长度前缀的二进制协议，避免转义与解析歧义。
 *
 *          客户端 -> 服务端：JOIN/HELLO/ROOM/SAY/DM/WHO/ROOMS/PING/PONG/QUIT
 *          服务端 -> 客户端：OK/ERROR/TOKEN/SYS/MSG/DM/WHO/ROOMS/HIST/PING/PONG
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace chat {

/// @brief 解析后的命令：verb 已转大写，rest 保留原始参数串。
struct Command {
    std::string verb;
    std::string rest;
};

/// @brief 去掉首尾空白（空格、制表、CR、LF）。
std::string Trim(std::string_view text);

/// @brief 解析一行命令；空行返回 false，verb 非法（非字母或超长）也返回 false。
bool ParseCommand(std::string_view line, Command* out);

/// @brief 从参数串拆出第一个 token 与其余文本（可能为空）。
bool SplitFirst(std::string_view text, std::string* first, std::string* remaining);

/// @brief 昵称校验：1..32 字节、不含空白与控制字符、不含 '|'。
bool IsSafeName(const std::string& name);

/// @brief 房间名校验：1..24 字节，仅 [A-Za-z0-9_-]。
bool IsSafeRoom(const std::string& room);

std::string FormatRoomMessage(const std::string& room, const std::string& from,
                              const std::string& text);
std::string FormatDirectMessage(const std::string& from, const std::string& text);
std::string FormatHistory(const std::string& room, const std::string& from,
                          const std::string& text);
std::string FormatSystem(const std::string& text);
std::string FormatError(const std::string& text);
std::string FormatOk(const std::string& text);
std::string FormatToken(const std::string& token);
std::string FormatOnline(const std::vector<std::string>& names);
std::string FormatRooms(const std::vector<std::string>& rooms);

/// @brief 用空格连接列表（空列表返回空串）。
std::string JoinList(const std::vector<std::string>& items);

}  // namespace chat
