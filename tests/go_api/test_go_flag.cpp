/**
 * @file test_go_flag.cpp
 * @brief `go2cpp/go/flag.hpp` 的兼容性测试（对照 Go 标准库 `flag`）。
 *
 * 覆盖：
 *  1. 注册与默认值（String/Int/Bool/Int64/Float64/Duration）；
 *  2. `-x=1` / `-x 1` / `--x=1` / `--x 1` 四种赋值形式；
 *  3. bool 的 `-b` / `-b=false` / `-b true`（后者不消费参数）；
 *  4. Duration 文法（`1s` / `1.5s` / `1h2m3s` / `-5s` / `100ms` / `1µs` / 非法输入）；
 *  5. 错误：未定义 flag、非法值、缺参数、bad syntax；
 *  6. `--` 终止、非 flag 参数终止、`Args/NArg/Arg`；
 *  7. `NFlag` / `Visit` / `VisitAll` / `Lookup` / `Set`；
 *  8. `-h` 触发 Usage（自定义与内置两条路径）；
 *  9. `PrintDefaults` 输出（重定向到临时文件后检查内容）；
 * 10. 重复注册抛 `std::invalid_argument`。
 */

#include "go2cpp/go/flag.hpp"
#include "go_api_check.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace goflag = go2cpp::go::flag;
using goflag::ParseResult;

/// 场景 1：注册与默认值。
void TestRegistration() {
    GO_ANNOUNCE("flag: registration/defaults");
    goflag::FlagSet set("demo");
    auto name = set.String("name", "world", "who to greet");
    auto count = set.Int("count", 3, "repeat count");
    auto debug = set.Bool("debug", false, "verbose");
    auto budget = set.Int64("budget", 1000000000000LL, "budget");
    auto ratio = set.Float64("ratio", 0.5, "ratio");
    auto wait = set.Duration("wait", ::go2cpp::time::Duration::FromNanoseconds(1000000000),
                             "wait time");

    GO_CHECK(*name == "world");
    GO_CHECK(*count == 3);
    GO_CHECK(*debug == false);
    GO_CHECK(*budget == 1000000000000LL);
    GO_CHECK(*ratio == 0.5);
    GO_CHECK(wait->Nanoseconds() == 1000000000LL);
    GO_CHECK(set.Lookup("name") != nullptr);
    GO_CHECK(set.Lookup("missing") == nullptr);
    GO_CHECK(set.Lookup("count")->Type() == "int");
    GO_CHECK(set.Lookup("debug")->Type() == "bool");
    GO_CHECK(set.Lookup("wait")->Type() == "duration");
    // 重复注册：Go 会 panic，这里抛 invalid_argument。
    bool threw = false;
    try {
        set.Int("count", 0, "duplicate");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    GO_CHECK(threw);
}

/// 场景 2：四种赋值形式。
void TestAssignmentForms() {
    GO_ANNOUNCE("flag: -x=1 / -x 1 / --x=1 / --x 1");
    goflag::FlagSet set("forms");
    auto value = set.Int("x", 0, "value");
    GO_CHECK(set.Parse({"-x=1"}) == ParseResult::kOk);
    GO_CHECK(*value == 1);
    GO_CHECK(set.Parse({"-x", "2"}) == ParseResult::kOk);
    GO_CHECK(*value == 2);
    GO_CHECK(set.Parse({"--x=3"}) == ParseResult::kOk);
    GO_CHECK(*value == 3);
    GO_CHECK(set.Parse({"--x", "4"}) == ParseResult::kOk);
    GO_CHECK(*value == 4);
    GO_CHECK(set.Parse({"-x=-5"}) == ParseResult::kOk);
    GO_CHECK(*value == -5);
    // 负值作为独立参数也允许（Go 同样把它当值消费）。
    GO_CHECK(set.Parse({"-x", "-6"}) == ParseResult::kOk);
    GO_CHECK(*value == -6);

    goflag::FlagSet strings("strings");
    auto text = strings.String("s", "", "text");
    GO_CHECK(strings.Parse({"-s=a=b"}) == ParseResult::kOk);
    GO_CHECK(*text == "a=b");  // 只在第一个 '=' 处分割
    GO_CHECK(strings.Parse({"-s", ""}) == ParseResult::kOk);
    GO_CHECK(text->empty());
}

/// 场景 3：bool 的三种形态。
void TestBoolForms() {
    GO_ANNOUNCE("flag: bool forms");
    goflag::FlagSet set("bools");
    auto flag_value = set.Bool("b", false, "switch");
    GO_CHECK(set.Parse({"-b"}) == ParseResult::kOk);
    GO_CHECK(*flag_value == true);
    GO_CHECK(set.Parse({"-b=false"}) == ParseResult::kOk);
    GO_CHECK(*flag_value == false);
    GO_CHECK(set.Parse({"-b", "true"}) == ParseResult::kOk);
    GO_CHECK(*flag_value == true);
    // bool 不消费下一个参数：`true` 留在 Args 里。
    GO_CHECK(set.NArg() == 1);
    GO_CHECK(set.Arg(0) == "true");
    GO_CHECK(set.Parse({"-b=1"}) == ParseResult::kOk);
    GO_CHECK(*flag_value == true);
    GO_CHECK(set.Parse({"-b=0"}) == ParseResult::kOk);
    GO_CHECK(*flag_value == false);
    GO_CHECK(set.Parse({"-b=maybe"}) == ParseResult::kError);
}

/// 场景 4：Duration 文法。
void TestDurationGrammar() {
    GO_ANNOUNCE("flag: Duration grammar");
    goflag::FlagSet set("durations");
    auto wait = set.Duration("wait", ::go2cpp::time::Duration::FromNanoseconds(0), "wait");

    struct Case {
        const char* text;
        std::int64_t nanos;
    };
    const Case valid[] = {
        {"0", 0},          {"1ns", 1},          {"100us", 100000},
        {"1ms", 1000000},  {"1s", 1000000000},  {"1.5s", 1500000000},
        {"2h45m", 9900000000000LL}, {"1h2m3s", 3723000000000LL},
        {"-5s", -5000000000LL},     {"+1.25s", 1250000000LL},
        {"1m30.5s", 90500000000LL},
    };
    for (const Case& item : valid) {
        const ParseResult result = set.Parse({std::string("-wait=") + item.text});
        if (result != ParseResult::kOk || wait->Nanoseconds() != item.nanos) {
            GO_CHECK_MSG(false, std::string("duration ") + item.text + " => " +
                                    std::to_string(wait->Nanoseconds()));
        }
    }
    const char* invalid[] = {"1", "abc", "1x", "--5s", "1..5s", "", "s", "1s1"};
    for (const char* item : invalid) {
        const ParseResult result = set.Parse({std::string("-wait=") + item});
        if (result != ParseResult::kError) {
            GO_CHECK_MSG(false, std::string("duration should fail: ") + item);
        }
    }
    GO_CHECK(true);
}

/// 场景 5：错误路径。
void TestErrors() {
    GO_ANNOUNCE("flag: error paths");
    goflag::FlagSet set("errors");
    auto count = set.Int("count", 0, "count");
    auto name = set.String("name", "", "name");

    GO_CHECK(set.Parse({"-missing"}) == ParseResult::kError);
    GO_CHECK(set.Error() == "flag provided but not defined: -missing");

    GO_CHECK(set.Parse({"-count=abc"}) == ParseResult::kError);
    GO_CHECK(set.Error() == "invalid value \"abc\" for flag -count");

    GO_CHECK(set.Parse({"-count"}) == ParseResult::kError);
    GO_CHECK(set.Error() == "flag needs an argument: -count");

    GO_CHECK(set.Parse({"-count", "42"}) == ParseResult::kOk);
    GO_CHECK(*count == 42);

    GO_CHECK(set.Parse({"-name", "value"}) == ParseResult::kOk);
    GO_CHECK(*name == "value");

    GO_CHECK(set.Parse({"---bad"}) == ParseResult::kError);
    GO_CHECK(set.Error() == "bad flag syntax: ---bad");
    GO_CHECK(set.Parse({"-=x"}) == ParseResult::kError);
    GO_CHECK(set.Parse({"-"}) == ParseResult::kOk);  // 单独 "-" 是非 flag 参数
    GO_CHECK(set.NArg() == 1 && set.Arg(0) == "-");
}

/// 场景 6：`--` 与位置参数。
void TestPositional() {
    GO_ANNOUNCE("flag: -- and positional args");
    goflag::FlagSet set("positional");
    auto level = set.Int("level", 0, "level");
    GO_CHECK(set.Parse({"-level=2", "alpha", "-level=9", "beta"}) == ParseResult::kOk);
    GO_CHECK(*level == 2);
    GO_CHECK(set.NArg() == 3);
    GO_CHECK(set.Arg(0) == "alpha");
    GO_CHECK(set.Arg(1) == "-level=9");
    GO_CHECK(set.Arg(2) == "beta");
    GO_CHECK(set.Arg(7).empty());
    GO_CHECK(set.Arg(-1).empty());

    GO_CHECK(set.Parse({"-level", "3", "--", "-level=4"}) == ParseResult::kOk);
    GO_CHECK(*level == 3);
    GO_CHECK(set.NArg() == 1);
    GO_CHECK(set.Arg(0) == "-level=4");
    GO_CHECK(set.Parse({}) == ParseResult::kOk);
}

/// 场景 7：NFlag / Visit / VisitAll / Set。
void TestIntrospection() {
    GO_ANNOUNCE("flag: NFlag/Visit/VisitAll/Set");
    goflag::FlagSet set("introspection");
    set.String("alpha", "a", "first");
    set.String("beta", "b", "second");
    set.Int("gamma", 0, "third");
    GO_CHECK(set.Parse({"-beta=x", "-gamma=7"}) == ParseResult::kOk);
    GO_CHECK(set.NFlag() == 2);
    std::vector<std::string> visited;
    set.Visit([&](const goflag::Flag& record) { visited.push_back(record.Name()); });
    GO_CHECK(visited.size() == 2);
    GO_CHECK(visited[0] == "beta" && visited[1] == "gamma");  // 字典序
    std::vector<std::string> all;
    set.VisitAll([&](const goflag::Flag& record) { all.push_back(record.Name()); });
    GO_CHECK(all.size() == 3);
    GO_CHECK(all[0] == "alpha" && all[2] == "gamma");
    GO_CHECK(set.Set("alpha", "z"));
    GO_CHECK(set.Lookup("alpha")->String() == "z");
    GO_CHECK(set.Set("alpha", "not-a-string"));  // string 的 Set 永远成功
    GO_CHECK(!set.Set("missing", "x"));
}

/// 场景 8：`-h` 触发 Usage。
void TestHelp() {
    GO_ANNOUNCE("flag: -h triggers Usage");
    goflag::FlagSet set("help");
    set.Int("n", 0, "count");
    bool called = false;
    set.SetUsage([&] { called = true; });
    GO_CHECK(set.Parse({"-h"}) == ParseResult::kHelp);
    GO_CHECK(called);
    called = false;
    GO_CHECK(set.Parse({"-help"}) == ParseResult::kHelp);
    GO_CHECK(called);
    // 内置 Usage 走 "Usage of <name>:" 分支（输出到 stderr，这里只确认不崩溃）。
    goflag::FlagSet another("another");
    another.Int("x", 0, "x");
    GO_CHECK(another.Parse({"-h"}) == ParseResult::kHelp);
}

/// 场景 9：PrintDefaults 输出。
void TestPrintDefaults() {
    GO_ANNOUNCE("flag: PrintDefaults output");
    goflag::FlagSet set("printer");
    set.String("addr", "127.0.0.1:8080", "listen address");
    set.Bool("debug", false, "enable debug");
    std::FILE* temporary = std::tmpfile();
    GO_REQUIRE(temporary != nullptr);
    set.SetOutput(temporary);
    set.PrintDefaults();
    std::rewind(temporary);
    std::string output(4096, '\0');
    const std::size_t read = std::fread(output.data(), 1, output.size() - 1, temporary);
    output.resize(read);
    std::fclose(temporary);
    GO_CHECK(output.find("-addr") != std::string::npos);
    GO_CHECK(output.find("listen address") != std::string::npos);
    GO_CHECK(output.find("127.0.0.1:8080") != std::string::npos);
    GO_CHECK(output.find("-debug") != std::string::npos);
    GO_CHECK(output.find("enable debug") != std::string::npos);
}

/// 场景 10：全局 CommandLine（与 Go 的 flag.Parse 用法一致）。
void TestGlobalCommandLine() {
    GO_ANNOUNCE("flag: global CommandLine");
    auto level = goflag::Int("global-level", 1, "global level");
    auto verbose = goflag::Bool("global-verbose", false, "verbose");
    GO_REQUIRE(goflag::Lookup("global-level") != nullptr);
    GO_CHECK(goflag::Parse({"-global-level=9", "-global-verbose"}) == ParseResult::kOk);
    GO_CHECK(*level == 9);
    GO_CHECK(*verbose);
    GO_CHECK(goflag::NFlag() >= 2);
    GO_CHECK(goflag::Parsed());
}

}  // namespace

int main() {
    TestRegistration();
    TestAssignmentForms();
    TestBoolForms();
    TestDurationGrammar();
    TestErrors();
    TestPositional();
    TestIntrospection();
    TestHelp();
    TestPrintDefaults();
    TestGlobalCommandLine();
    return go_api_test::ExitCode();
}
