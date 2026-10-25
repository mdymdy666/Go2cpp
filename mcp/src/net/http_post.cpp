/**
 * @file http_post.cpp
 * @brief HTTP(S) JSON POST 实现。
 */

#include "mcp/net/http_post.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "go2cpp/network/http.hpp"
#include "util/process.hpp"

namespace go2cpp::mcp::net {

namespace {

/// 解析 http:// URL 为 host/port/target；失败返回 false。
bool SplitHttpUrl(const std::string& url, std::string& host, std::uint16_t& port,
                  std::string& target) {
    const std::string prefix = "http://";
    if (url.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    const std::size_t host_start = prefix.size();
    const std::size_t slash = url.find('/', host_start);
    const std::string authority =
        (slash == std::string::npos) ? url.substr(host_start) : url.substr(host_start, slash - host_start);
    target = (slash == std::string::npos) ? "/" : url.substr(slash);

    const std::size_t colon = authority.find(':');
    port = 80;
    if (colon == std::string::npos) {
        host = authority;
    } else {
        host = authority.substr(0, colon);
        try {
            port = static_cast<std::uint16_t>(std::stoi(authority.substr(colon + 1)));
        } catch (const std::exception&) {
            return false;
        }
    }
    return !host.empty();
}

}  // namespace

bool HttpPostJson(const std::string& url, const std::string& body, const HttpPostOptions& options,
                  std::string& response_body, std::string& error) {
    if (url.compare(0, 8, "https://") == 0) {
        // HTTPS：curl 子进程（Go2Cpp 暂无可用 TLS 客户端）。
        std::vector<std::string> argv = {
            "curl", "-sS", "--fail-with-body",
            "-m", std::to_string(options.timeout.count() / 1000 + 1),
            "-X", "POST",
            "-H", "Content-Type: " + options.content_type,
        };
        if (!options.api_key.empty()) {
            argv.push_back("-H");
            argv.push_back("Authorization: Bearer " + options.api_key);
        }
        argv.push_back("--data-binary");
        argv.push_back("@-");
        argv.push_back(url);

        const auto result = util::RunProcess(argv, options.timeout.count() + 5000, body);
        if (result.timed_out) {
            error = "HTTPS 请求超时";
            return false;
        }
        if (result.exit_code != 0) {
            error = "curl 退出码 " + std::to_string(result.exit_code) + ": " + result.output;
            return false;
        }
        response_body = result.output;
        return true;
    }

    std::string host;
    std::uint16_t port = 80;
    std::string target;
    if (!SplitHttpUrl(url, host, port, target)) {
        error = "不支持的 URL: " + url;
        return false;
    }

    go2cpp::http::Request request;
    request.method = go2cpp::http::Method::Post;
    request.target = target;
    request.header.set("Content-Type", options.content_type);
    if (!options.api_key.empty()) {
        request.header.set("Authorization", "Bearer " + options.api_key);
    }
    request.body = body;

    go2cpp::http::Client client(options.timeout);
    const auto result = client.do_request(request, host, port);
    if (!result.ok()) {
        error = "HTTP 请求失败: " + result.message;
        return false;
    }
    response_body = result.response->body;
    return true;
}

}  // namespace go2cpp::mcp::net
