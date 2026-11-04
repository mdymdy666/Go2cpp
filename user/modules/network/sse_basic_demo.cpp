#include "go2cpp/sse.hpp"

#include <iostream>
#include <string>
#include <vector>

/**
 * @brief SSE 简单版：解析事件、按字节分片喂入、编码回写。
 * @details sse::Parser 是纯解析器，不涉及任何网络连接：
 *          ① feed 一段完整事件文本，得到 Event；
 *          ② 同一段文本逐字节喂入，结果必须一致（增量解析能力）；
 *          ③ Event::encode 把它编回 SSE 文本，可再次解析。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;
    bool ok = true;

    // ① 解析一个完整事件
    std::clog << "[sse][场景1] 解析完整事件\n";
    sse::Parser parser;
    std::vector<sse::Event> events;
    const auto result = parser.feed("data: hello\n\n", events);
    const bool parsed = result.ok() && events.size() == 1 &&
                        events[0].data == "hello";
    std::cout << "  ok=" << result.ok() << " 事件数=" << events.size()
              << " data=\"" << (events.empty() ? "" : events[0].data)
              << "\"（预期 1 1 hello）\n";
    ok = ok && parsed;

    // ② 逐字节分片喂入同一段文本
    std::clog << "[sse][场景2] 逐字节分片喂入\n";
    sse::Parser split_parser;
    std::vector<sse::Event> split_events;
    const std::string text = "data: chunked\n\n";
    bool feed_ok = true;
    for (const char ch : text) {
        const auto one = split_parser.feed(std::string_view(&ch, 1), split_events);
        feed_ok = feed_ok && one.ok();
    }
    const bool same = split_events.size() == 1 && split_events[0].data == "chunked";
    std::cout << "  分片喂入全部成功=" << feed_ok << " 事件数=" << split_events.size()
              << " data=\"" << (split_events.empty() ? "" : split_events[0].data)
              << "\"（预期 1 1 chunked）\n";
    ok = ok && feed_ok && same;

    // ③ 编码往返
    std::clog << "[sse][场景3] encode 往返\n";
    sse::Event outgoing;
    outgoing.event = "ping";
    outgoing.id = "42";
    outgoing.data = "payload";
    std::string encoded;
    const auto encode_result = outgoing.encode(encoded);
    sse::Parser round_parser;
    std::vector<sse::Event> round_events;
    const auto round_result = round_parser.feed(encoded, round_events);
    const bool round_ok = encode_result.ok() && round_result.ok() &&
                          round_events.size() == 1 &&
                          round_events[0].event == "ping" &&
                          round_events[0].id == "42" &&
                          round_events[0].data == "payload";
    std::cout << "  编码=\"" << encoded.substr(0, 20) << "...\" 往返一致="
              << round_ok << "（预期 1）\n";
    ok = ok && round_ok;

    std::clog << (ok ? "[sse] 全部通过\n" : "[sse] 有失败\n");
    return ok ? 0 : 1;
}
