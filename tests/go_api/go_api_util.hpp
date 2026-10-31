/**
 * @file go_api_util.hpp
 * @brief `tests/go_api` 共用的小工具（端口解析、带超时的通道读取）。
 */

#pragma once

#include "go2cpp/go/go.hpp"
#include "go_api_check.hpp"

#include <cstdint>
#include <optional>
#include <string>

// 测试统一用 Go 风格的短命名空间别名。
namespace go = go2cpp::go;

namespace go_api_test {

/** @brief 从 "127.0.0.1:12345" 里取端口；失败返回 0。 */
inline std::uint16_t ParsePort(const std::string& address) {
    const auto colon = address.rfind(':');
    if (colon == std::string::npos) {
        return 0;
    }
    try {
        return static_cast<std::uint16_t>(std::stoi(address.substr(colon + 1)));
    } catch (...) {
        return 0;
    }
}

/** @brief 在 127.0.0.1:0 上监听 TCP。 */
inline go::net::ListenResult ListenLoopbackTCP() {
    return go::net::Listen("tcp", "127.0.0.1:0");
}

/** @brief 带超时地从通道取一个值；超时返回空。 */
template <typename T>
inline std::optional<T> RecvWithin(go::Chan<T>& channel, go::Duration timeout) {
    auto context = go::WithTimeout(go::Background(), timeout);
    return channel.RecvCtx(context.context);
}

/** @brief 读满 size 字节（循环 Read，处理部分读）。 */
inline bool ReadExact(go::net::Conn& conn, void* buffer, std::size_t size) {
    auto* bytes = static_cast<char*>(buffer);
    std::size_t offset = 0;
    while (offset < size) {
        const auto result = conn.Read(bytes + offset, size - offset);
        if (result.err) {
            return false;
        }
        if (result.n == 0) {
            return false;
        }
        offset += result.n;
    }
    return true;
}

}  // namespace go_api_test
