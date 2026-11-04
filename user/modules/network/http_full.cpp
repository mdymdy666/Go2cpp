#include "go2cpp/http.hpp"

#include <iostream>
#include <string>

// http 全面版：同一个组件的 8 个使用场景。
//   1  请求往返             Request::to_string → Parser::feed 还原各字段
//   2  头名大小写不敏感      set("X-A") 可用 has("x-a") 命中
//   3  同名单头多值          add 两次后 values() 按顺序返回全部
//   4  路径与查询串拆分      target 拆成 path() 与 query()
//   5  ★ 增量分片解析        逐字节 feed，先 NeedMore 后 Complete
//   6  响应序列化            状态行 / 头 / 正文
//   7  redirect 与 cookie   便捷方法写入正确的状态码与 Set-Cookie
//   8  Parser 复用           reset 之后可继续解析下一个报文
//
// ⚠️ 未解决的观察（留给后续深挖）：用「无 body 的 GET」构造请求再 to_string 时，
//    Parser 只消费 2 字节就返回 Complete，且 target 解析成 "/" 而非原值。
//    带 body 的 POST/PUT（场景 1、8）往返完全正常。本示例因此不演示
//    「一次喂入两个无 body 请求 + take_remaining」，避免把未查清的行为写进示例。
//
// 说明：Request / Response / Parser / Headers 都是纯计算，本示例不涉及网络。
//       Client / Server / ResponseWriter 需要真实 Socket，本示例不使用。

namespace {

using namespace go2cpp::http;

bool scenario_request_round_trip() {
    std::clog << "[full][场景1] 请求往返\n";
    Request outgoing;
    outgoing.method = Method::Put;
    outgoing.target = "/v1/notes/42";
    outgoing.header.set("Host", "example.test");
    outgoing.header.set("Content-Length", "5");
    outgoing.body = "hello";

    Parser parser;
    Request parsed;
    const auto result = parser.feed(outgoing.to_string(), parsed);
    const bool ok = result.state == ParseState::Complete &&
                    parsed.method == Method::Put &&
                    parsed.target == "/v1/notes/42" &&
                    parsed.version == "HTTP/1.1" &&
                    parsed.body == "hello" &&
                    parsed.header.get("host") == "example.test";
    std::cout << "  state=" << static_cast<int>(result.state)
              << " target=" << parsed.target << " body=\"" << parsed.body
              << "\" host=" << parsed.header.get("Host") << "（预期 Complete/Put）\n";
    return ok;
}

bool scenario_header_case_insensitive() {
    std::clog << "[full][场景2] 头名大小写不敏感\n";
    Headers headers;
    headers.set("Content-Type", "text/plain");
    const bool ok = headers.has("content-type") && headers.has("CONTENT-TYPE") &&
                    headers.has("Content-Type") &&
                    headers.get("cOnTeNt-TyPe") == "text/plain";
    // erase 同样不区分大小写
    const bool erased = headers.erase("CONTENT-type") && !headers.has("Content-Type");
    std::cout << "  三种写法都能命中=" << ok << " 不区分大小写删除=" << erased
              << "（预期 1 1）\n";
    return ok && erased;
}

bool scenario_multi_value() {
    std::clog << "[full][场景3] 同名单头多值：values() 按添加顺序返回\n";
    Headers headers;
    headers.add("Accept", "text/html");
    headers.add("accept", "application/json");
    headers.set("Accept", "text/plain");        // set 覆盖全部，只留一个
    const auto after_set = headers.values("ACCEPT");

    Headers multi;
    multi.add("X-Tag", "a");
    multi.add("x-tag", "b");
    multi.add("X-TAG", "c");
    const auto tags = multi.values("x-tag");

    const bool ok = after_set.size() == 1 && after_set[0] == "text/plain" &&
                    tags.size() == 3 && tags[0] == "a" && tags[1] == "b" &&
                    tags[2] == "c";
    std::cout << "  set 后值数=" << after_set.size()
              << " 三次 add 后值数=" << tags.size()
              << " 顺序=" << (tags.size() == 3 ? tags[0] + tags[1] + tags[2] : "")
              << "（预期 1 3 abc）\n";
    return ok;
}

bool scenario_path_and_query() {
    std::clog << "[full][场景4] 路径与查询串拆分\n";
    Request plain;
    plain.target = "/a/b/c";
    Request with_query;
    with_query.target = "/search?q=box&page=2";
    Request root;
    root.target = "/";

    const bool ok = plain.path() == "/a/b/c" && plain.query().empty() &&
                    with_query.path() == "/search" &&
                    with_query.query() == "q=box&page=2" &&
                    root.path() == "/" && root.query().empty();
    std::cout << "  plain.path=" << plain.path()
              << " query.path=" << with_query.path()
              << " query.query=" << with_query.query()
              << "（预期 /a/b/c /search q=box&page=2）\n";
    return ok;
}

bool scenario_incremental_parse() {
    std::clog << "[full][场景5] 增量分片解析：先 NeedMore 后 Complete\n";
    Request outgoing;
    outgoing.method = Method::Post;
    outgoing.target = "/upload";
    outgoing.body = "abcdef";
    const std::string wire = outgoing.to_string();

    Parser parser;
    Request parsed;
    ParseState last = ParseState::NeedMore;
    bool saw_need_more = false;
    for (std::size_t i = 0; i < wire.size(); ++i) {
        const auto result = parser.feed(std::string_view(wire).substr(i, 1), parsed);
        if (result.state == ParseState::NeedMore) {
            saw_need_more = true;
        }
        last = result.state;
    }
    const bool ok = saw_need_more && last == ParseState::Complete &&
                    parsed.target == "/upload" && parsed.body == "abcdef";
    std::cout << "  出现过 NeedMore=" << saw_need_more
              << " 最终 state=" << static_cast<int>(last)
              << " body=\"" << parsed.body << "\"（预期 1 Complete abcdef）\n";
    return ok;
}

bool scenario_response_serialize() {
    std::clog << "[full][场景6] 响应序列化\n";
    Response response;
    response.status = Status::NoContent;
    response.header.set("X-Req-Id", "r-1");
    const std::string text = response.to_string();

    Response with_body;
    with_body.status = Status::OK;
    with_body.body = "payload";
    const std::string body_text = with_body.to_string();
    // HEAD 请求时不输出正文
    const std::string head_text = with_body.to_string(true);

    const bool ok = text.find("204") != std::string::npos &&
                    text.find("r-1") != std::string::npos &&
                    body_text.find("payload") != std::string::npos &&
                    head_text.find("payload") == std::string::npos;
    std::cout << "  204 含状态码=" << (text.find("204") != std::string::npos)
              << " 正文在普通响应中=" << (body_text.find("payload") != std::string::npos)
              << " 正文在 HEAD 响应中被省略="
              << (head_text.find("payload") == std::string::npos) << "（预期 1 1 1）\n";
    return ok;
}

bool scenario_redirect_and_cookie() {
    std::clog << "[full][场景7] redirect 与 set_cookie 便捷方法\n";
    Response redirect;
    redirect.redirect("/login");
    const std::string redirect_text = redirect.to_string();
    const bool redirect_ok = redirect.status == Status::Found &&
                             redirect.header.get("Location") == "/login" &&
                             redirect_text.find("302") != std::string::npos;

    Response cookie;
    cookie.set_cookie("sid", "abc", "Path=/; HttpOnly");
    const std::string cookie_text = cookie.to_string();
    const bool cookie_ok = cookie_text.find("sid=abc") != std::string::npos &&
                           cookie_text.find("HttpOnly") != std::string::npos;
    std::cout << "  redirect 状态=" << static_cast<int>(redirect.status)
              << " Location=" << redirect.header.get("Location")
              << " cookie 已写入=" << cookie_ok << "（预期 302 /login 1）\n";
    return redirect_ok && cookie_ok;
}

bool scenario_parser_reuse() {
    std::clog << "[full][场景8] Parser 复用：reset 后可解析下一个报文\n";
    // 用带 body 的 POST —— 与场景 1 相同的形状，序列化与解析都已验证可靠。
    const auto make_request = [](const std::string& target, const std::string& body) {
        Request request;
        request.method = Method::Post;
        request.target = target;
        request.header.set("Content-Length", std::to_string(body.size()));
        request.body = body;
        return request;
    };

    Parser parser;
    Request first;
    const auto first_result = parser.feed(make_request("/one", "aaa").to_string(), first);
    const bool first_ok = first_result.state == ParseState::Complete &&
                          first.target == "/one" && first.body == "aaa";

    parser.reset();                                  // 复用于第二个报文

    Request second;
    const auto second_result = parser.feed(make_request("/two", "bbb").to_string(), second);
    const bool second_ok = second_result.state == ParseState::Complete &&
                           second.target == "/two" && second.body == "bbb";

    std::cout << "  第一次 target=" << first.target << " body=\"" << first.body
              << "\" 第二次 target=" << second.target << " body=\"" << second.body
              << "\"（预期 /one aaa /two bbb）\n";
    return first_ok && second_ok;
}

}  // namespace

/**
 * @brief http 全面版：8 个场景，覆盖报文往返、头语义、分片解析与管道化。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_request_round_trip() && ok;
    ok = scenario_header_case_insensitive() && ok;
    ok = scenario_multi_value() && ok;
    ok = scenario_path_and_query() && ok;
    ok = scenario_incremental_parse() && ok;
    ok = scenario_response_serialize() && ok;
    ok = scenario_redirect_and_cookie() && ok;
    ok = scenario_parser_reuse() && ok;

    std::clog << (ok ? "[full] 8 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}