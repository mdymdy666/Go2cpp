#include "go2cpp/network/framing.hpp"
#include "go2cpp/network/http.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

namespace {

/**
 * @brief 记录单个场景的检查结果。
 * @param[in] scenario 场景名称。
 * @param[in] condition 需要验证的条件。
 * @return 条件满足返回 true。
 */
bool check(const char* scenario, bool condition) {
    std::cout << "[network] " << scenario << " => "
              << (condition ? "通过" : "失败") << '\n';
    return condition;
}

/**
 * @brief 场景一（成功路径）：一次输入包含多条完整消息，即粘包。
 * @details TCP 是字节流，一次 read 可能带回多条消息；分帧器必须一次性
 *          交付全部完整帧，不能只交付第一条。
 * @return 检查通过返回 true。
 */
bool demo_coalesced_frames() {
    auto framer = go2cpp::Framer::delimiter("\r\n");
    framer.feed("first\r\nsecond\r\nthird\r\n");
    const auto frames = framer.take_frames();
    return check("场景一 粘包：一次输入交付全部完整帧",
                 frames.size() == 3 && frames[0] == "first" &&
                     frames[1] == "second" && frames[2] == "third");
}

/**
 * @brief 场景二（等待路径）：分隔符被拆到两次输入，未完成时只能等待。
 * @details 半包时 take_frames() 为空但 buffered_size() 不为零；
 *          调用方不能把空结果当成连接结束，必须继续读取直到分隔符到齐。
 * @return 检查通过返回 true。
 */
bool demo_split_delimiter() {
    auto framer = go2cpp::Framer::delimiter("\r\n");
    framer.feed("first\r");
    const auto partial = framer.take_frames();
    const bool waiting = partial.empty() && framer.buffered_size() == 6;
    framer.feed("\nsecond\r\n");
    const auto frames = framer.take_frames();
    return check("场景二 半包：分隔符跨两次输入时先等待再一次性交付",
                 waiting && frames.size() == 2 && frames[0] == "first" &&
                     frames[1] == "second");
}

/**
 * @brief 场景三（错误路径）：超过 max_frame 后进入失败态并丢弃缓冲。
 * @details 失败时会清空已缓存帧，避免调用方把部分结果误当成完整批次；
 *          失败态会拒绝后续输入，复用前必须 reset()。
 * @return 检查通过返回 true。
 */
bool demo_oversized_frame() {
    auto framer = go2cpp::Framer::delimiter("\r\n", 4);
    const auto oversized = framer.try_feed("12345");
    const bool rejected =
        oversized == go2cpp::net::Framer::FeedError::FrameTooLarge;
    const auto after_failure = framer.try_feed("ok\r\n");
    const bool failed_state =
        after_failure == go2cpp::net::Framer::FeedError::Failed &&
        framer.buffered_size() == 0;
    framer.reset();
    // max_frame 仍为 4，复用后同样不能越过限制，这里用一个合法短帧。
    framer.feed("ok\r\n");
    const bool reusable = framer.take_frames().size() == 1;
    return check("场景三 超长帧：拒绝并丢弃缓冲，reset 后可复用",
                 rejected && failed_state && reusable);
}

/**
 * @brief 场景四（分片路径）：HTTP 请求逐字节到达，累积后一次完成。
 * @details 解析器按字节流工作，数据不足时返回 NeedMore，绝不会把半条
 *          请求当成完成；这对应真实连接上请求头与 body 分批到达的情况。
 * @return 检查通过返回 true。
 */
bool demo_incremental_http() {
    const std::string wire =
        "POST /submit?from=demo HTTP/1.1\r\n"
        "Host: example\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    go2cpp::http::Parser parser;
    go2cpp::http::Request request;
    go2cpp::http::ParseResult result;
    std::size_t need_more = 0;
    for (std::size_t index = 0; index < wire.size(); ++index) {
        result = parser.feed(std::string_view(wire).substr(index, 1), request);
        if (result.state == go2cpp::http::ParseState::Error) break;
        if (result.state == go2cpp::http::ParseState::NeedMore) ++need_more;
    }
    const bool completed = result.state == go2cpp::http::ParseState::Complete;
    const bool ok =
        completed && need_more + 1 == wire.size() &&
        go2cpp::http::method_name(request.method) == "POST" &&
        request.path() == "/submit" && request.query() == "from=demo" &&
        request.header.get("Host") == "example" && request.body == "hello";
    return check("场景四 分片解析：逐字节 NeedMore 后一次 Complete", ok);
}

/**
 * @brief 场景五（错误路径）：非法请求行明确报错，且必须 reset 才能恢复。
 * @details 解析器返回错误后不会自动丢弃缓冲区，未 reset() 时同样的坏数据
 *          会继续导致错误；调用方应关闭连接或显式 reset() 后复用。
 * @return 检查通过返回 true。
 */
bool demo_invalid_http() {
    const std::string valid =
        "GET /ok HTTP/1.1\r\nHost: example\r\nContent-Length: 0\r\n\r\n";
    go2cpp::http::Parser parser;
    go2cpp::http::Request request;
    const auto bad = parser.feed("BOGUS / HTTP/1.1\r\nHost: example\r\n\r\n",
                                 request);
    const bool reported = bad.state == go2cpp::http::ParseState::Error &&
                          !bad.error.empty();
    const auto blocked = parser.feed(valid, request);
    const bool still_blocked = blocked.state == go2cpp::http::ParseState::Error;
    parser.reset();
    const auto good = parser.feed(valid, request);
    const bool recovered = good.state == go2cpp::http::ParseState::Complete &&
                           request.path() == "/ok";
    return check("场景五 非法请求：明确报错，reset 后才能继续解析",
                 reported && still_blocked && recovered);
}

}  // namespace

/**
 * @brief network 模块入门示例：Framer 分帧与 HTTP 增量解析。
 * @details 五个场景覆盖成功路径、等待路径（半包、分片）和错误路径
 *          （超长帧、非法请求）。需要 epoll/Fiber 上的 socket 等待、超时与
 *          取消时请看 example/network/io_wait_many_demo.cpp；SSE 与 WebSocket
 *          分别见 sse_demo.cpp 与 websocket_demo.cpp。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    std::cout << "[network] Framer + HttpParser 入门示例\n";
    const bool coalesced = demo_coalesced_frames();
    const bool split = demo_split_delimiter();
    const bool oversized = demo_oversized_frame();
    const bool incremental = demo_incremental_http();
    const bool invalid = demo_invalid_http();
    const bool all = coalesced && split && oversized && incremental && invalid;
    std::cout << "[network] 汇总: coalesced=" << std::boolalpha << coalesced
              << " split=" << split << " oversized=" << oversized
              << " incremental=" << incremental << " invalid=" << invalid
              << " all=" << all << '\n';
    return all ? 0 : 1;
}