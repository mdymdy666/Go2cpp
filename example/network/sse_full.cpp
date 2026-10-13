#include "go2cpp/sse.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

// sse 全面版：同一个组件的 7 个使用场景。
//   1  基本事件              data 字段 + 空行结束
//   2  重复 data 字段        多个 data 行按规范合并为多行数据
//   3  event / id / retry   三个附加字段分别落到 Event 与解析器状态
//   4  ★ 任意字节分片        逐字节喂入与整段喂入结果一致
//   5  换行变体与 BOM        LF / CRLF / CR 与 UTF-8 BOM 都能识别
//   6  注释行                以冒号开头的行被忽略，不产生事件
//   7  ★ 半个事件不派发      没有空行结束时 EOF 不强行派发，避免截断误报
//   8  encode 往返           编码后再解析得到等价事件
//
// 说明：sse::Parser 是纯解析器，不涉及网络；sse::Stream 才需要 Socket，本示例不使用。

namespace {

using go2cpp::sse::Event;
using go2cpp::sse::Parser;

// 便捷：一次性喂入并返回事件列表
std::vector<Event> parse_all(Parser& parser, const std::string& text) {
    std::vector<Event> events;
    (void)parser.feed(text, events);
    return events;
}

bool scenario_basic() {
    std::clog << "[full][场景1] 基本事件\n";
    Parser parser;
    const auto events = parse_all(parser, "data: hello\n\n");
    const bool ok = events.size() == 1 && events[0].data == "hello" &&
                    events[0].event.empty() && events[0].id.empty();
    std::cout << "  事件数=" << events.size() << " data=\""
              << (events.empty() ? "" : events[0].data) << "\"（预期 1 hello）\n";
    return ok;
}

bool scenario_multiple_data_lines() {
    std::clog << "[full][场景2] 重复 data 字段合并为多行\n";
    Parser parser;
    const auto events = parse_all(parser, "data: line1\ndata: line2\ndata: line3\n\n");
    const bool ok = events.size() == 1 &&
                    events[0].data == "line1\nline2\nline3";
    std::cout << "  事件数=" << events.size() << " data=\""
              << (events.empty() ? "" : events[0].data)
              << "\"（预期合并为三行）\n";
    return ok;
}

bool scenario_extra_fields() {
    std::clog << "[full][场景3] event / id / retry 字段\n";
    Parser parser;
    const auto events = parse_all(
        parser, "event: update\nid: 77\nretry: 3000\ndata: payload\n\n");
    const bool fields_ok = events.size() == 1 && events[0].event == "update" &&
                           events[0].id == "77" &&
                           events[0].retry_ms.has_value() &&
                           *events[0].retry_ms == 3000U;
    // 解析器还要记住 last_event_id 与 retry_after，供重连时使用
    const bool state_ok = parser.last_event_id() == "77" &&
                          parser.retry_after().has_value() &&
                          *parser.retry_after() == 3000U;
    std::cout << "  event=" << (events.empty() ? "" : events[0].event)
              << " id=" << (events.empty() ? "" : events[0].id)
              << " retry=" << (events.empty() || !events[0].retry_ms
                                   ? -1 : static_cast<long long>(*events[0].retry_ms))
              << " last_event_id=" << parser.last_event_id()
              << " retry_after=" << (parser.retry_after() ? "有" : "无")
              << "（预期 update 77 3000 77 有）\n";
    return fields_ok && state_ok;
}

bool scenario_byte_by_byte() {
    std::clog << "[full][场景4] 任意字节分片：逐字节喂入结果一致\n";
    const std::string text =
        "event: dotted\nid: 9\ndata: alpha\ndata: beta\nretry: 500\n\n";

    Parser whole;
    const auto whole_events = parse_all(whole, text);

    Parser split;
    std::vector<Event> split_events;
    bool all_ok = true;
    for (const char ch : text) {
        const auto one = split.feed(std::string_view(&ch, 1), split_events);
        all_ok = all_ok && one.ok();
    }

    const bool ok = all_ok && whole_events.size() == 1 &&
                    split_events.size() == 1 &&
                    whole_events[0].data == split_events[0].data &&
                    whole_events[0].event == split_events[0].event &&
                    whole_events[0].id == split_events[0].id &&
                    split_events[0].data == "alpha\nbeta" &&
                    split.last_event_id() == "9";
    std::cout << "  整段事件数=" << whole_events.size()
              << " 逐字节事件数=" << split_events.size()
              << " data=\"" << (split_events.empty() ? "" : split_events[0].data)
              << "\"（预期 1 1 alpha\\nbeta）\n";
    return ok;
}

bool scenario_line_endings_and_bom() {
    std::clog << "[full][场景5] 换行变体与 UTF-8 BOM\n";
    Parser lf;
    const auto lf_events = parse_all(lf, "data: lf\n\n");

    Parser crlf;
    const auto crlf_events = parse_all(crlf, "data: crlf\r\n\r\n");

    Parser cr;
    const auto cr_events = parse_all(cr, "data: cr\r\r");

    Parser bom;
    // ★ 注意："\xBFdata" 会被当成一个超范围的十六进制转义（\x 会尽量多吞十六进制字符，
    //   而 'd' 正好是十六进制数字）。必须用相邻字符串字面量把 BOM 与正文切开。
    const auto bom_events = parse_all(bom, "\xEF\xBB\xBF" "data: bom\n\n");

    const bool ok = lf_events.size() == 1 && lf_events[0].data == "lf" &&
                    crlf_events.size() == 1 && crlf_events[0].data == "crlf" &&
                    cr_events.size() == 1 && cr_events[0].data == "cr" &&
                    bom_events.size() == 1 && bom_events[0].data == "bom";
    std::cout << "  LF=\"" << (lf_events.empty() ? "" : lf_events[0].data)
              << "\" CRLF=\"" << (crlf_events.empty() ? "" : crlf_events[0].data)
              << "\" CR=\"" << (cr_events.empty() ? "" : cr_events[0].data)
              << "\" BOM=\"" << (bom_events.empty() ? "" : bom_events[0].data)
              << "\"（预期 lf crlf cr bom）\n";
    return ok;
}

bool scenario_comments() {
    std::clog << "[full][场景6] 注释行不产生事件\n";
    Parser parser;
    const auto only_comment = parse_all(parser, ": this is a comment\n\n");
    const auto mixed = parse_all(
        parser, ": leading comment\ndata: real\n: trailing comment\n\n");
    const bool ok = only_comment.empty() && mixed.size() == 1 &&
                    mixed[0].data == "real";
    std::cout << "  纯注释事件数=" << only_comment.size()
              << " 混合事件数=" << mixed.size()
              << " data=\"" << (mixed.empty() ? "" : mixed[0].data)
              << "\"（预期 0 1 real）\n";
    return ok;
}

bool scenario_partial_not_dispatched() {
    std::clog << "[full][场景7] 半个事件不派发（避免把截断误报为完整消息）\n";
    Parser parser;
    // 缺少结尾空行：不应派发
    const auto partial = parse_all(parser, "data: incomplete\n");
    // 补上空行后才派发
    const auto completed = parse_all(parser, "\n");
    const bool ok = partial.empty() && completed.size() == 1 &&
                    completed[0].data == "incomplete";
    std::cout << "  未补空行事件数=" << partial.size()
              << " 补空行后事件数=" << completed.size()
              << " data=\"" << (completed.empty() ? "" : completed[0].data)
              << "\"（预期 0 1 incomplete）\n";
    return ok;
}

bool scenario_encode_round_trip() {
    std::clog << "[full][场景8] encode 往返\n";
    Event outgoing;
    outgoing.event = "message";
    outgoing.id = "1001";
    outgoing.data = "first\nsecond";
    outgoing.retry_ms = std::uint64_t{1500};

    std::string encoded;
    const auto encode_result = outgoing.encode(encoded);

    Parser parser;
    const auto events = parse_all(parser, encoded);
    const bool ok = encode_result.ok() && events.size() == 1 &&
                    events[0].event == "message" && events[0].id == "1001" &&
                    events[0].data == "first\nsecond" &&
                    events[0].retry_ms.has_value() &&
                    *events[0].retry_ms == 1500U;
    std::cout << "  编码成功=" << encode_result.ok() << " 往返事件数=" << events.size()
              << " data=\"" << (events.empty() ? "" : events[0].data)
              << "\"（预期 1 1 first\\nsecond）\n";
    return ok;
}

}  // namespace

/**
 * @brief sse 全面版：8 个场景，覆盖分帧、字段、分片、换行变体与截断保护。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_basic() && ok;
    ok = scenario_multiple_data_lines() && ok;
    ok = scenario_extra_fields() && ok;
    ok = scenario_byte_by_byte() && ok;
    ok = scenario_line_endings_and_bom() && ok;
    ok = scenario_comments() && ok;
    ok = scenario_partial_not_dispatched() && ok;
    ok = scenario_encode_round_trip() && ok;

    std::clog << (ok ? "[full] 8 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}