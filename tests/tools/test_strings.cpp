/**
 * @file test_strings.cpp
 * @brief `go2cpp/strings.hpp` 的正式测试：Go 语义边界 + 大数据量 + 随机对拍 + 真实场景。
 *
 * 测试设计（按项目规范：大数据量 / 随机 / ≥5 真实场景）：
 *   场景 1 日志行切分与字段清洗   场景 4 HTTP 头名大小写无关比较
 *   场景 2 CSV 字段清洗与还原     场景 5 Builder 拼接（含 UTF-8）与长度语义
 *   场景 3 URL/路径解析（Cut 族） 场景 6 配置项取值（Trim + TrimPrefix）
 *   大数据量：10 万字段 Join/Split 往返 + Count 计数校验
 *   随机：2 万轮随机字节串，对拍 Contains↔Index、Split/Join 往返、TrimSpace 幂等、Replace 幂等
 *   随机：1 万轮随机合法 UTF-8，对拍 RuneCount 与 `Split(s,"")` 的元素数
 *
 * ⚠️ 注释中不得出现连续的斜杠加星号（GCC -Wcomment）。
 */
#include "go2cpp/strings.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

/// 独立参考实现：不做 UTF-8 解码，只按字节寻找子串（用于对拍非空子串路径）。
std::int64_t ReferenceCount(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
        return -1;  // 空子串路径由边界用例单独覆盖，不参与本对拍
    }
    std::int64_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

/// 生成一个随机字节串（长度与内容都由 rng 决定）。
std::string RandomBytes(std::mt19937& rng, std::size_t max_length) {
    std::uniform_int_distribution<std::size_t> length_dist(0, max_length);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    const std::size_t length = length_dist(rng);
    std::string text;
    text.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        text.push_back(static_cast<char>(byte_dist(rng)));
    }
    return text;
}

/// 生成一个随机合法 UTF-8 串（从固定 rune 集合中取，保证 rune 数已知）。
std::string RandomUtf8(std::mt19937& rng, std::size_t max_runes, std::size_t* runes_out) {
    static const char32_t kRunes[] = {U'a', U'Z', U'0', 0x00E9, 0x4E2D, 0x6587, 0x1F600, 0x0416};
    std::uniform_int_distribution<std::size_t> count_dist(0, max_runes);
    std::uniform_int_distribution<std::size_t> rune_dist(0, sizeof(kRunes) / sizeof(kRunes[0]) - 1);
    const std::size_t count = count_dist(rng);
    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
        go2cpp::AppendRune(text, kRunes[rune_dist(rng)]);
    }
    *runes_out = count;
    return text;
}

}  // namespace

/**
 * @brief 运行 strings 模块的全部测试。
 */
void run_strings_tests() {
    go2cpp_tests::announce("strings: Go 语义边界 + 大数据量 + 随机对拍 + 真实场景");
    using namespace go2cpp;

    // ---------------- 场景 1：日志行切分与字段清洗 ----------------
    {
        const std::string line = "  2026-10-15T18:30:00Z | WARN  | disk usage 91%  ";
        const StringSlice fields = Split(line, "|");
        GO2CPP_CHECK(fields.size() == 3);
        const std::string level = TrimSpace(fields[1]);
        GO2CPP_CHECK(level == "WARN");
        GO2CPP_CHECK(HasPrefix(TrimSpace(fields[2]), "disk"));
        const std::string stamp = TrimSpace(fields[0]);
        GO2CPP_CHECK(HasSuffix(stamp, "Z"));
        GO2CPP_CHECK(TrimSuffix(stamp, "Z") == "2026-10-15T18:30:00");
    }

    // ---------------- 场景 2：CSV 字段清洗与还原 ----------------
    {
        const StringSlice cells = Split("alice, 30 , beijing", ",");
        GO2CPP_CHECK(cells.size() == 3);
        StringSlice cleaned;
        for (const auto& cell : cells) {
            cleaned.push_back(TrimSpace(cell));
        }
        GO2CPP_CHECK(Join(cleaned, ",") == "alice,30,beijing");
        GO2CPP_CHECK(cleaned[1] == "30");
    }

    // ---------------- 场景 3：URL / 路径解析（Cut 族） ----------------
    {
        const StringCutResult scheme = Cut("https://example.com/a/b?x=1", "://");
        GO2CPP_CHECK(scheme.found);
        GO2CPP_CHECK(scheme.before == "https");
        GO2CPP_CHECK(HasPrefix(scheme.after, "example.com"));
        const StringCutResult query = Cut(scheme.after, "?");
        GO2CPP_CHECK(query.found && query.before == "example.com/a/b" && query.after == "x=1");
        const auto without_suffix = CutSuffix(query.before, "/b");
        GO2CPP_CHECK(without_suffix.second && without_suffix.first == "example.com/a");
        GO2CPP_CHECK(CutPrefix("/api/v1/users", "/api").first == "/v1/users");
        GO2CPP_CHECK(!CutPrefix("/api", "/other").second);
    }

    // ---------------- 场景 4：HTTP 头名大小写无关比较 ----------------
    {
        GO2CPP_CHECK(EqualFold("Content-Type", "content-type"));
        GO2CPP_CHECK(!EqualFold("Content-Type", "content-encoding"));
        GO2CPP_CHECK(Compare("application/json", "text/plain") < 0);
        GO2CPP_CHECK(ContainsAny("Content-Type", ":-/"));
    }

    // ---------------- 场景 5：Builder 拼接（UTF-8 与长度语义） ----------------
    {
        StringBuilder builder;
        for (int i = 0; i < 1000; ++i) {
            builder.WriteString("你");
        }
        GO2CPP_CHECK(builder.Len() == 3000);          // Len 是字节数（Go 语义）
        const std::string text = builder.String();
        GO2CPP_CHECK(RuneCount(text) == 1000);
        GO2CPP_CHECK(Count(text, "") == 1001);        // rune 数 + 1
        GO2CPP_CHECK(Contains(text, "你你你"));
    }

    // ---------------- 场景 6：配置项取值与引号剥离 ----------------
    {
        GO2CPP_CHECK(TrimPrefix(TrimSpace("  retry=3  "), "retry=") == "3");
        GO2CPP_CHECK(Trim("\"quoted\"", "\"") == "quoted");
        GO2CPP_CHECK(Fields("  a=1\t b=2\n c=3  ").size() == 3);
    }

    // ---------------- 语义边界（Go 的反直觉行为） ----------------
    {
        GO2CPP_CHECK(Count("", "") == 1);
        GO2CPP_CHECK(Count("five", "") == 5);
        GO2CPP_CHECK(Index("abc", "") == 0);
        GO2CPP_CHECK(Contains("abc", ""));
        GO2CPP_CHECK(!ContainsAny("abc", ""));
        GO2CPP_CHECK(IndexAny("abc", "") == -1);
        const StringSlice runes = Split("a,b,c", "");
        GO2CPP_CHECK(runes.size() == 5 && runes[1] == "," && runes[3] == ",");
        GO2CPP_CHECK(SplitN("a,b,c", ",", 0).empty());
        GO2CPP_CHECK(SplitN("a,b,c", ",", 1).size() == 1);
        GO2CPP_CHECK(SplitN("a,b,c", ",", 2)[1] == "b,c");
        GO2CPP_CHECK(Join(SplitAfter("a,b,c", ","), "|") == "a,|b,|c");
        GO2CPP_CHECK(Replace("abc", "", "-", -1) == "-a-b-c-");
        GO2CPP_CHECK(Replace("aaa", "a", "b", 2) == "bba");
        GO2CPP_CHECK(LastIndex("go gopher", "go") == 3);
        GO2CPP_CHECK(IndexRune("chicken", U'k') == 4);
        GO2CPP_CHECK(IndexRune("chicken", 0xFFFDU) == -1);   // 与 Go 一致：U+FFFD 返回 -1
        GO2CPP_CHECK(ToUpper("héllo") == "HÉLLO");
        GO2CPP_CHECK(Clone(std::string_view("abc")) == "abc");
    }

    // ---------------- 大数据量：10 万字段 Join/Split 往返 ----------------
    {
        constexpr std::size_t kFields = 100000;
        StringSlice values;
        values.reserve(kFields);
        for (std::size_t i = 0; i < kFields; ++i) {
            values.push_back(std::to_string(i % 1000));
        }
        const std::string joined = Join(values, ",");
        const StringSlice round_trip = Split(joined, ",");
        GO2CPP_CHECK(round_trip.size() == kFields);
        GO2CPP_CHECK(round_trip.front() == "0");
        GO2CPP_CHECK(round_trip.back() == std::to_string((kFields - 1) % 1000));
        GO2CPP_CHECK(Count(joined, ",") == static_cast<std::int64_t>(kFields - 1));
        GO2CPP_CHECK(Count(joined, "999") > 0);
        // 逐字段抽查 1000 次（避免只验证首尾）
        std::int64_t mismatches = 0;
        for (std::size_t i = 0; i < kFields; i += 100) {
            if (round_trip[i] != std::to_string(i % 1000)) ++mismatches;
        }
        GO2CPP_CHECK(mismatches == 0);
    }

    // ---------------- 随机对拍 1：随机字节串（2 万轮） ----------------
    {
        std::mt19937 rng(20261015U);
        std::int64_t mismatches = 0;
        for (int iteration = 0; iteration < 20000; ++iteration) {
            const std::string text = RandomBytes(rng, 40);
            // 子串：一半取自 text 的随机切片，一半是新的随机串
            std::string needle;
            if (text.size() > 1 && (iteration % 2) == 0) {
                std::uniform_int_distribution<std::size_t> start_dist(0, text.size() - 1);
                const std::size_t start = start_dist(rng);
                const std::size_t length = (text.size() - start) / 2 + 1;
                needle = text.substr(start, length);
            } else {
                needle = RandomBytes(rng, 4);
            }
            if (needle.empty()) continue;

            const bool contains = Contains(text, needle);
            const std::int64_t index = Index(text, needle);
            if (contains != (index >= 0)) ++mismatches;
            if (Count(text, needle) != ReferenceCount(text, needle)) ++mismatches;
            if (HasPrefix(text, needle) != (index == 0)) ++mismatches;
            // 注意：必须用 LastIndex —— 用"首次出现位置"判断是否以 needle 结尾，
            // 在重复子串场景（例如 text="abab"、needle="ab"）会必然误判。
            const std::int64_t last = LastIndex(text, needle);
            if (HasSuffix(text, needle) !=
                (last >= 0 && last + static_cast<std::int64_t>(needle.size()) ==
                                  static_cast<std::int64_t>(text.size()))) {
                ++mismatches;
            }
            // Join/Split 往返必须恒等
            if (Join(Split(text, ","), ",") != text) ++mismatches;
            // TrimSpace 幂等
            if (TrimSpace(TrimSpace(text)) != TrimSpace(text)) ++mismatches;
            // ReplaceAll 把 needle 全部替换成空后，必须不再包含 needle
            if (!needle.empty() && Contains(ReplaceAll(text, needle, ""), needle)) ++mismatches;
            // 长度关系：TrimSpace 结果不长于原文
            if (TrimSpace(text).size() > text.size()) ++mismatches;
        }
        GO2CPP_CHECK(mismatches == 0);
    }

    // ---------------- 随机对拍 2：随机合法 UTF-8（1 万轮） ----------------
    {
        std::mt19937 rng(424242U);
        std::int64_t mismatches = 0;
        for (int iteration = 0; iteration < 10000; ++iteration) {
            std::size_t expected_runes = 0;
            const std::string text = RandomUtf8(rng, 20, &expected_runes);
            if (RuneCount(text) != expected_runes) ++mismatches;
            if (Split(text, "").size() != expected_runes) ++mismatches;
            if (Count(text, "") != static_cast<std::int64_t>(expected_runes) + 1) ++mismatches;
            if (Join(Split(text, ""), "") != text) ++mismatches;   // 按 rune 切分再拼回必须恒等
        }
        GO2CPP_CHECK(mismatches == 0);
    }
}
