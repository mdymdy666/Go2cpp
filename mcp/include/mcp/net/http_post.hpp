/**
 * @file http_post.hpp
 * @brief 传输层附属：一次性的 HTTP(S) JSON POST。
 *
 * @details
 * LLM 客户端与 embedding 客户端都需要「POST 一段 JSON、拿回响应体」，
 * 但两者都不该各自维护 TLS 分支。这里把这段能力单独放一层：
 *
 * - `http://`：走 Go2Cpp 的 `http::Client`（纯 C++，零外部进程）；
 * - `https://`：Go2Cpp 目前没有可用的 TLS 客户端，委托系统 `curl` 子进程。
 *   这是已知取舍，替换点就在本文件，上层无感。
 *
 * 之所以不放在 `transport/` 的 `ITransport` 里：`ITransport` 是「消息通道」，
 * 而这是一次性请求，生命周期与语义都不同。
 */

#pragma once

#include <chrono>
#include <string>

namespace go2cpp::mcp::net {

/// POST 选项。
struct HttpPostOptions {
    std::string api_key;                                  ///< 非空时加 `Authorization: Bearer`
    std::chrono::milliseconds timeout{60000};             ///< 单次超时
    std::string content_type = "application/json";        ///< Content-Type
};

/**
 * @brief 发送一次 JSON POST。
 * @param url 完整 URL（http:// 或 https://）
 * @param body 请求体（通常是 JSON 文本）
 * @param options 超时与鉴权
 * @param response_body 输出：响应体
 * @param error 失败时写中文诊断
 * @return 成功返回 true。
 */
bool HttpPostJson(const std::string& url, const std::string& body, const HttpPostOptions& options,
                  std::string& response_body, std::string& error);

}  // namespace go2cpp::mcp::net
