#include "go2cpp/log.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// logging 全面版：同一个组件的 7 个使用场景。
//   1  级别解析与枚举         ParseLevel 接受合法名、拒绝非法名
//   2  ★ 级别过滤             MinimumLevelFilter 按阈值放行/拦截
//   3  格式化模式             PatternFormatter 把 LogRecord 渲染成一行
//   4  ★ 自定义格式项         LogFormatter::Item + AddFormat/RemoveFormat
//   5  ★ 内存输出目标         MemorySink 收集并在 Snapshot 取回
//   6  自定义输出目标         LogSink 子类接管写入
//   7  LogRecord 字段         所有字段都能被格式项读到
//
// 说明：全部在内存里完成，**不写任何文件、不依赖 go2cpp.ini**。
//       直接调用 sink->Write(record, formatted)，绕过 LoggerManager 的接线，
//       因此本示例只依赖格式化与输出目标两组稳定接口。

namespace {

using namespace go2cpp::log;

// 自定义格式项：输出线程名，验证 %T 可以读到 LogRecord 的字段。
class ThreadNameItem final : public LogFormatter::Item {
public:
    void format(std::ostream& stream, const LogRecord& record) const override {
        stream << (record.thread_name.empty() ? "unnamed" : record.thread_name);
    }
};

// 自定义格式项：输出 fiber_id。
class FiberItem final : public LogFormatter::Item {
public:
    void format(std::ostream& stream, const LogRecord& record) const override {
        stream << record.fiber_id;
    }
};

// 自定义输出目标：把每条格式化结果加上前缀后收进 vector。
class PrefixedSink final : public LogSink {
public:
    void Write(const LogRecord& record, std::string_view formatted) override {
        lines.emplace_back(std::string("[P] ") + std::string(formatted));
        ++writes;
        last_level = record.level;
    }
    void Flush() override { ++flushes; }

    std::vector<std::string> lines;
    int writes{0};
    int flushes{0};
    Level last_level{Level::Off};
};

LogRecord make_record(Level level, const std::string& message) {
    LogRecord record;
    record.level = level;
    record.logger = "Demo";
    record.message = message;
    record.file = "demo.cpp";
    record.function = "main";
    record.line = 42;
    record.timestamp_ms = 1700000000000ULL;
    record.thread_id = 7;
    record.fiber_id = 99;
    record.elapse_ms = 5;
    record.thread_name = "worker-1";
    return record;
}

bool scenario_parse_level() {
    std::clog << "[full][场景1] 级别解析\n";
    struct Case { const char* text; Level expected; };
    const Case cases[] = {
        {"trace", Level::Trace}, {"debug", Level::Debug}, {"info", Level::Info},
        {"warn", Level::Warn},   {"error", Level::Error}, {"critical", Level::Critical},
        {"off", Level::Off},
    };
    bool all_ok = true;
    for (const auto& item : cases) {
        Level parsed = Level::Off;
        all_ok = ParseLevel(item.text, &parsed) && parsed == item.expected && all_ok;
    }
    Level ignored = Level::Info;
    const bool rejected = !ParseLevel("verbose", &ignored);
    std::cout << "  7 个合法级别全部解析正确=" << all_ok
              << " 非法名被拒=" << rejected << "（预期 1 1）\n";
    return all_ok && rejected;
}

bool scenario_minimum_level_filter() {
    std::clog << "[full][场景2] 级别过滤\n";
    MinimumLevelFilter filter(Level::Warn);
    const bool threshold = filter.level() == Level::Warn;
    const bool allow_error = filter.Accept(make_record(Level::Error, "e"));
    const bool allow_warn = filter.Accept(make_record(Level::Warn, "w"));
    const bool deny_info = !filter.Accept(make_record(Level::Info, "i"));
    const bool deny_debug = !filter.Accept(make_record(Level::Debug, "d"));

    // 放宽阈值后，原先被拒的记录应当被放行
    filter.SetLevel(Level::Debug);
    const bool now_allow_debug = filter.Accept(make_record(Level::Debug, "d"));
    const bool now_allow_info = filter.Accept(make_record(Level::Info, "i"));

    const bool ok = threshold && allow_error && allow_warn && deny_info && deny_debug &&
                    now_allow_debug && now_allow_info;
    std::cout << "  阈值=Warn 放行 Error/Warn、拦截 Info/Debug="
              << (allow_error && allow_warn && deny_info && deny_debug)
              << " 放宽到 Debug 后 Info/Debug 放行="
              << (now_allow_debug && now_allow_info) << "（预期 1 1）\n";
    return ok;
}

bool scenario_pattern_formatter() {
    std::clog << "[full][场景3] 格式化模式\n";
    PatternFormatter message_only("%m%n");
    const auto record = make_record(Level::Info, "hello");
    const bool message_ok = message_only.Format(record) == "hello\n";
    const bool pattern_ok = message_only.pattern() == "%m%n";
    // ★ 不假设哪些模式算"非法"——实测 PatternFormatter("%") 的 HasError() 为假，
    //   "%" 被当作字面量处理。这里改为验证"不同模式产出不同结果"。
    PatternFormatter prefixed("[%m]%n");
    const bool prefixed_ok = prefixed.Format(record) == "[hello]\n";

    std::cout << "  %m%n 输出=\"" << message_only.Format(record)
              << "\" pattern()=" << message_only.pattern()
              << " [%m]%n 输出=\"" << prefixed.Format(record)
              << "\"（预期 hello / %m%n / [hello]）\n";
    return message_ok && pattern_ok && prefixed_ok;
}

bool scenario_custom_format_item() {
    std::clog << "[full][场景4] 自定义格式项\n";
    // ★ 多数单字母键已被内置格式项占用：实测 T 是制表符、F 是 fiber_id，
    //   对它们调用 AddFormat 会因重名而失败（首版硬编码 "T"/"F" 就栽在这里）。
    //   正确做法是用 HasFormat 探测出空闲键，让示例不依赖内置键的分配。
    // ★ 格式项的键必须是**单字符**（%x 只消费一个字符）：首版用 "zz" 导致
    //   模式 "%zz" 被解析成 "%z" + 字面 'z'，渲染结果原样输出。
    //   这里在字母表里扫描出两个未被内置项占用的单字符键。
    std::string key_a;
    std::string key_b;
    for (char ch = 'a'; ch <= 'z'; ++ch) {
        const std::string candidate(1, ch);
        if (LogFormatter::HasFormat(candidate)) {
            continue;
        }
        if (key_a.empty()) {
            key_a = candidate;
        } else {
            key_b = candidate;
            break;
        }
    }
    if (key_a.empty() || key_b.empty()) {
        std::cout << "  找不到两个空闲的单字符格式键\n";
        return false;
    }

    const bool added_t = LogFormatter::AddFormat(
        key_a, [](const std::string&) { return std::make_shared<ThreadNameItem>(); });
    const bool added_f = LogFormatter::AddFormat(
        key_b, [](const std::string&) { return std::make_shared<FiberItem>(); });

    const bool has_t = LogFormatter::HasFormat(key_a);
    const bool has_f = LogFormatter::HasFormat(key_b);
    const auto formats = LogFormatter::Formats();
    bool listed = !formats.empty();
    bool found_t = false;
    for (const auto& key : formats) {
        if (key == key_a) {
            found_t = true;
        }
    }

    // 必须在注册之后构造 PatternFormatter（构造时保存格式项快照）
    PatternFormatter formatter("%" + key_a + "/%" + key_b);
    const auto record = make_record(Level::Info, "x");
    const std::string rendered = formatter.Format(record);

    const bool removed_t = LogFormatter::RemoveFormat(key_a);
    const bool has_t_after = LogFormatter::HasFormat(key_a);

    const bool ok = added_t && added_f && has_t && has_f && listed && found_t &&
                    rendered == "worker-1/99" && removed_t && !has_t_after;
    std::cout << "  注册成功=" << (added_t && added_f) << " Formats 列出新键=" << found_t
              << " 渲染=\"" << rendered << "\" 注销后 HasFormat=" << has_t_after
              << "（预期 1 1 worker-1/99 0）\n";
    return ok;
}

bool scenario_memory_sink() {
    std::clog << "[full][场景5] 内存输出目标\n";
    MemorySink sink;
    PatternFormatter formatter("%m%n");
    for (int index = 0; index < 3; ++index) {
        const auto record = make_record(Level::Info, "line-" + std::to_string(index));
        sink.Write(record, formatter.Format(record));
    }
    const auto snapshot = sink.Snapshot();
    const bool ok = snapshot.size() == 3 && snapshot[0] == "line-0\n" &&
                    snapshot[2] == "line-2\n";
    std::cout << "  收集条数=" << snapshot.size() << " 首=\"" 
              << (snapshot.empty() ? "" : snapshot[0]) << "\" 末=\""
              << (snapshot.empty() ? "" : snapshot.back()) << "\"（预期 3）\n";
    return ok;
}

bool scenario_custom_sink() {
    std::clog << "[full][场景6] 自定义输出目标\n";
    PrefixedSink sink;
    PatternFormatter formatter("%m%n");
    sink.Write(make_record(Level::Warn, "first"), formatter.Format(make_record(Level::Warn, "first")));
    sink.Write(make_record(Level::Error, "second"), formatter.Format(make_record(Level::Error, "second")));
    sink.Flush();

    const bool ok = sink.writes == 2 && sink.flushes == 1 &&
                    sink.lines.size() == 2 &&
                    sink.lines[0] == "[P] first\n" &&
                    sink.lines[1] == "[P] second\n" &&
                    sink.last_level == Level::Error;
    std::cout << "  写入次数=" << sink.writes << " flush 次数=" << sink.flushes
              << " 首行=\"" << (sink.lines.empty() ? "" : sink.lines[0])
              << "\" 最后级别=" << static_cast<int>(sink.last_level)
              << "（预期 2 1）\n";
    return ok;
}

bool scenario_record_fields() {
    std::clog << "[full][场景7] LogRecord 字段可被格式项读到\n";
    // 注册一个把所有关键字段拼在一起的项
    const bool added = LogFormatter::AddFormat("A", [](const std::string&) {
        class AllItem final : public LogFormatter::Item {
        public:
            void format(std::ostream& stream, const LogRecord& record) const override {
                stream << record.logger << '|' << record.file << ':' << record.line
                       << '|' << record.function << '|' << record.thread_id
                       << '|' << record.fiber_id << '|' << record.elapse_ms
                       << '|' << record.timestamp_ms;
            }
        };
        return std::make_shared<AllItem>();
    });
    PatternFormatter formatter("%A");
    const std::string rendered = formatter.Format(make_record(Level::Info, "x"));
    LogFormatter::RemoveFormat("A");

    const std::string expected =
        "Demo|demo.cpp:42|main|7|99|5|1700000000000";
    const bool ok = added && rendered == expected;
    std::cout << "  渲染=\"" << rendered << "\"（预期 " << expected << "）\n";
    return ok;
}

}  // namespace

/**
 * @brief logging 全面版：7 个场景，覆盖级别、过滤、格式化与输出目标。
 * @details 全部在内存完成，不写文件、不依赖 go2cpp.ini。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_parse_level() && ok;
    ok = scenario_minimum_level_filter() && ok;
    ok = scenario_pattern_formatter() && ok;
    ok = scenario_custom_format_item() && ok;
    ok = scenario_memory_sink() && ok;
    ok = scenario_custom_sink() && ok;
    ok = scenario_record_fields() && ok;

    std::clog << (ok ? "[full] 7 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}