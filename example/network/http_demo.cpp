#include "go2cpp/http.hpp"

#include <iostream>
#include <string>

/**
 * @brief http 简单版：构造请求、解析回来、构造响应。
 * @details http 的 Request/Response/Parser 都是纯计算，不涉及任何网络连接：
 *          ① Request::to_string 生成报文，Parser::feed 解析回结构体；
 *          ② Headers 的名字大小写不敏感；
 *          ③ Response::to_string 生成响应报文。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp::http;
    bool ok = true;

    // ① 请求往返
    std::clog << "[http][场景1] 请求 to_string / Parser 往返\n";
    Request outgoing;
    outgoing.method = Method::Post;
    outgoing.target = "/api/items?id=7";
    outgoing.header.set("Content-Type", "application/json");
    outgoing.body = R"({"name":"box"})";

    Parser parser;
    Request parsed;
    const auto result = parser.feed(outgoing.to_string(), parsed);
    const bool round_ok = result.state == ParseState::Complete &&
                          parsed.method == Method::Post &&
                          parsed.target == "/api/items?id=7" &&
                          parsed.body == R"({"name":"box"})" &&
                          parsed.header.get("content-type") == "application/json";
    std::cout << "  state=" << static_cast<int>(result.state)
              << " method_is_post=" << (parsed.method == Method::Post)
              << " target=" << parsed.target << "（预期 Complete Post）\n";
    ok = ok && round_ok;

    // ② Headers 名字大小写不敏感
    std::clog << "[http][场景2] Headers 大小写不敏感\n";
    Headers headers;
    headers.set("X-Trace-Id", "abc123");
    const bool has_any_case = headers.has("x-trace-id") &&
                              headers.has("X-TRACE-ID") &&
                              headers.get("X-Trace-ID") == "abc123";
    std::cout << "  小写查询=" << headers.has("x-trace-id")
              << " 大写查询=" << headers.has("X-TRACE-ID")
              << " 取值=" << headers.get("X-Trace-Id") << "（预期 1 1 abc123）\n";
    ok = ok && has_any_case;

    // ③ 响应构造
    std::clog << "[http][场景3] Response::to_string\n";
    Response response;
    response.status = Status::Created;
    response.header.set("Content-Type", "text/plain");
    response.body = "created";
    const std::string text = response.to_string();
    const bool response_ok = text.find("201") != std::string::npos &&
                             text.find("created") != std::string::npos;
    std::cout << "  首行=\"" << text.substr(0, text.find('\r'))
              << "\"（预期含 201）\n";
    ok = ok && response_ok;

    std::clog << (ok ? "[http] 全部通过\n" : "[http] 有失败\n");
    return ok ? 0 : 1;
}
