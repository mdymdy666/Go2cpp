/**
 * @file test_path.cpp
 * @brief `go2cpp::path`（Go `path` 包）的语义、随机属性、压测与 Go 对拍测试。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 *
 * ## 覆盖策略（对应 PROJECT_RULES 第 3 节）
 *
 * 1. **语义表**：逐条移植 Go 1.27.1 `path/path_test.go` 的 `cleantests`/
 *    `jointests`/`splittests`/`exttests`/`basetests`/`dirtests` 与
 *    `match_test.go` 的 `matchTests`（含全部非法模式），共 100+ 条；
 * 2. **随机属性测试**：20 万条随机路径，验证
 *    - 幂等 `Clean(Clean(p)) == Clean(p)`；
 *    - 结果非空，且只有根 `/` 以 `/` 结尾；
 *    - 不含 `//`、`/./`、空组件；前导 `..` 只出现在开头；
 *    - `IsAbs(Clean(p)) == p 以 / 开头`；
 * 3. **与 Go 对拍**：读取 `tests/path/testdata/go_path_clean_10000.txt`
 *    （由 `tests/path/scripts/cross_check.py` 生成、Go `path.Clean` 为 oracle），
 *    逐条比较 10000 条随机/边角路径；
 * 4. **压测**：`Join`/`Clean`/`Match` 各 100 万次，输出 Mops/s。
 *
 * 测试不访问文件系统（除读取对拍数据文件）；`path` 是纯字符串模块。
 *
 * 运行方式：
 *   ./go2cpp_path_test                          # 全部测试
 *   ./go2cpp_path_test --clean-file corpus.txt  # 对拍辅助：逐行输出 Clean
 */

#include "go2cpp/path/path.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef GO2CPP_PATH_TESTDATA_DIR
#define GO2CPP_PATH_TESTDATA_DIR "tests/path/testdata"
#endif

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[path] FAILED: %s\n", what);
        ++g_failures;
    }
}

void CheckEq(const std::string& actual, const std::string& expected,
             const char* what, const std::string& input) {
    if (actual != expected) {
        std::fprintf(stderr,
                     "[path] FAILED: %s input=\"%s\" actual=\"%s\" "
                     "expected=\"%s\"\n",
                     what, input.c_str(), actual.c_str(), expected.c_str());
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// 1. 语义表（逐条对照 Go 1.27.1 的 path/path_test.go、path/match_test.go）
// ---------------------------------------------------------------------------

struct TextCase final {
    const char* input;
    const char* expected;
};

void TestCleanTable() {
    static const TextCase kClean[] = {
        // 已规范
        {"", "."},
        {"abc", "abc"},
        {"abc/def", "abc/def"},
        {"a/b/c", "a/b/c"},
        {".", "."},
        {"..", ".."},
        {"../..", "../.."},
        {"../../abc", "../../abc"},
        {"/abc", "/abc"},
        {"/", "/"},
        // 去尾斜杠
        {"abc/", "abc"},
        {"abc/def/", "abc/def"},
        {"a/b/c/", "a/b/c"},
        {"./", "."},
        {"../", ".."},
        {"../../", "../.."},
        {"/abc/", "/abc"},
        // 合并重复斜杠
        {"abc//def//ghi", "abc/def/ghi"},
        {"//abc", "/abc"},
        {"///abc", "/abc"},
        {"//abc//", "/abc"},
        {"abc//", "abc"},
        // 去 `.`
        {"abc/./def", "abc/def"},
        {"/./abc/def", "/abc/def"},
        {"abc/.", "abc"},
        // 去 `..`
        {"abc/def/ghi/../jkl", "abc/def/jkl"},
        {"abc/def/../ghi/../jkl", "abc/jkl"},
        {"abc/def/..", "abc"},
        {"abc/def/../..", "."},
        {"/abc/def/../..", "/"},
        {"abc/def/../../..", ".."},
        {"/abc/def/../../..", "/"},
        {"abc/def/../../../ghi/jkl/../../../mno", "../../mno"},
        // 组合
        {"abc/./../def", "def"},
        {"abc//./../def", "def"},
        {"abc/../../././../def", "../../def"},
        // 任务书点名的边角 + 非 ASCII / 超长段
        {"a/../b", "b"},
        {"a/./b", "a/b"},
        {"a//b", "a/b"},
        {"../a", "../a"},
        {"..", ".."},
        {"/..", "/"},
        {"/../..", "/"},
        {"...", "..."},
        {".../...", ".../..."},
        {"a/...", "a/..."},
        {"..a/../b", "b"},
        {"a/..b", "a/..b"},
        {"目录/./文件.txt", "目录/文件.txt"},
        {"目录//../文件.txt", "文件.txt"},
        {"日本語/../../naïve", "../naïve"},
        {"a\\b/c", "a\\b/c"},  // 反斜杠是普通字符
        {"a\\b/../c", "c"},    // 但 `..` 仍会消掉整个 `a\b` 组件
        {"a\\b//c", "a\\b/c"},
        {"//", "/"},
        {"///", "/"},
        {"/./", "/"},
        {"/../", "/"},
        {"./", "."},
        {"a/..", "."},
        {"a/../..", ".."},
        {"a/../../..", "../.."},
        {"/a/../..", "/"},
        {"a/b/../../../c", "../c"},
    };
    for (const TextCase& test : kClean) {
        const std::string first = go2cpp::path::Clean(test.input);
        CheckEq(first, test.expected, "Clean", test.input);
        // Go 的 TestClean 还要求对结果幂等。
        const std::string second = go2cpp::path::Clean(first);
        CheckEq(second, test.expected, "Clean(Clean)", test.input);
    }

    // 超长段（10 KiB）：不能截断、不能改变字节。
    std::string long_path(10000, 'x');
    long_path += "/./";
    long_path.append(10000, 'y');
    const std::string expected_long = std::string(10000, 'x') + "/" +
                                      std::string(10000, 'y');
    CheckEq(go2cpp::path::Clean(long_path), expected_long, "Clean 超长段",
            "<10000x/./10000y>");
}

void TestSplitExtBaseDirTable() {
    static const TextCase kSplit[] = {
        {"a/b", "a/|b"},   {"a/b/", "a/b/|"}, {"a/", "a/|"},
        {"a", "|a"},       {"/", "/|"},       {"", "|"},
    };
    for (const TextCase& test : kSplit) {
        const auto [dir, file] = go2cpp::path::Split(test.input);
        CheckEq(dir + "|" + file, test.expected, "Split", test.input);
        Check(std::string(test.input) == dir + file,
              "Split 必须满足 dir+file == 输入");
    }

    static const TextCase kExt[] = {
        {"path.go", ".go"},
        {"path.pb.go", ".go"},
        {"a.dir/b", ""},
        {"a.dir/b.go", ".go"},
        {"a.dir/", ""},
        {"", ""},
        {".go", ".go"},
        {"a/b.c/d", ""},
        {"a/b.", "."},
        {"文件.txt", ".txt"},
        {"目录/文件.tar.gz", ".gz"},
    };
    for (const TextCase& test : kExt) {
        CheckEq(go2cpp::path::Ext(test.input), test.expected, "Ext",
                test.input);
    }

    static const TextCase kBase[] = {
        {"", "."},
        {".", "."},
        {"/.", "."},
        {"/", "/"},
        {"////", "/"},
        {"x/", "x"},
        {"abc", "abc"},
        {"abc/def", "def"},
        {"a/b/.x", ".x"},
        {"a/b/c.", "c."},
        {"a/b/c.x", "c.x"},
        {"/a/b/", "b"},
        {"a/..", ".."},
        {"a//b///", "b"},
        {"目录/文件.txt", "文件.txt"},
    };
    for (const TextCase& test : kBase) {
        CheckEq(go2cpp::path::Base(test.input), test.expected, "Base",
                test.input);
    }

    static const TextCase kDir[] = {
        {"", "."},
        {".", "."},
        {"/.", "/"},
        {"/", "/"},
        {"////", "/"},
        {"/foo", "/"},
        {"x/", "x"},
        {"abc", "."},
        {"abc/def", "abc"},
        {"a/b/", "a/b"},
        {"/a/b", "/a"},
        {"a/..", "a"},
        {"目录/文件.txt", "目录"},
    };
    for (const TextCase& test : kDir) {
        CheckEq(go2cpp::path::Dir(test.input), test.expected, "Dir",
                test.input);
    }
}

void TestJoinTable() {
    using go2cpp::path::Join;
    CheckEq(Join(), "", "Join()", "<empty>");
    CheckEq(Join(""), "", "Join(\"\")", "<empty>");
    CheckEq(Join("a"), "a", "Join(a)", "a");
    CheckEq(Join("a", "b"), "a/b", "Join(a,b)", "a,b");
    CheckEq(Join("a", ""), "a", "Join(a,\"\")", "a,");
    CheckEq(Join("", "b"), "b", "Join(\"\",b)", ",b");
    CheckEq(Join("/", "a"), "/a", "Join(/,a)", "/,a");
    CheckEq(Join("/", ""), "/", "Join(/,\"\")", "/,");
    CheckEq(Join("a/", "b"), "a/b", "Join(a/,b)", "a/,b");
    CheckEq(Join("a/", ""), "a", "Join(a/,\"\")", "a/,");
    CheckEq(Join("", ""), "", "Join(\"\",\"\")", ",");
    CheckEq(Join("a", "", "b"), "a/b", "Join(a,\"\",b)", "a,,b");
    CheckEq(Join("/", "a", "..", "b"), "/b", "Join(/,a,..,b)", "/,a,..,b");
    CheckEq(Join("目录", "文件.txt"), "目录/文件.txt", "Join 非 ASCII",
            "目录,文件.txt");
    CheckEq(Join(std::vector<std::string>{"a", "", "b"}), "a/b",
            "Join(vector)", "a,,b");
}

struct MatchCase final {
    const char* pattern;
    const char* name;
    bool matched;
    bool bad;
};

void TestMatchTable() {
    static const MatchCase kMatch[] = {
        {"abc", "abc", true, false},
        {"*", "abc", true, false},
        {"*c", "abc", true, false},
        {"a*", "a", true, false},
        {"a*", "abc", true, false},
        {"a*", "ab/c", false, false},
        {"a*/b", "abc/b", true, false},
        {"a*/b", "a/c/b", false, false},
        {"a*b*c*d*e*/f", "axbxcxdxe/f", true, false},
        {"a*b*c*d*e*/f", "axbxcxdxexxx/f", true, false},
        {"a*b*c*d*e*/f", "axbxcxdxe/xxx/f", false, false},
        {"a*b*c*d*e*/f", "axbxcxdxexxx/fff", false, false},
        {"a*b?c*x", "abxbbxdbxebxczzx", true, false},
        {"a*b?c*x", "abxbbxdbxebxczzy", false, false},
        {"ab[c]", "abc", true, false},
        {"ab[b-d]", "abc", true, false},
        {"ab[e-g]", "abc", false, false},
        {"ab[^c]", "abc", false, false},
        {"ab[^b-d]", "abc", false, false},
        {"ab[^e-g]", "abc", true, false},
        {"a\\*b", "a*b", true, false},
        {"a\\*b", "ab", false, false},
        {"a?b", "a☺b", true, false},
        {"a[^a]b", "a☺b", true, false},
        {"a???b", "a☺b", false, false},
        {"a[^a][^a][^a]b", "a☺b", false, false},
        {"[a-ζ]*", "α", true, false},
        {"*[a-ζ]", "A", false, false},
        {"a?b", "a/b", false, false},
        {"a*b", "a/b", false, false},
        {"[\\]a]", "]", true, false},
        {"[\\-]", "-", true, false},
        {"[x\\-]", "x", true, false},
        {"[x\\-]", "-", true, false},
        {"[x\\-]", "z", false, false},
        {"[\\-x]", "x", true, false},
        {"[\\-x]", "-", true, false},
        {"[\\-x]", "a", false, false},
        {"[]a]", "]", false, true},
        {"[-]", "-", false, true},
        {"[x-]", "x", false, true},
        {"[x-]", "-", false, true},
        {"[x-]", "z", false, true},
        {"[-x]", "x", false, true},
        {"[-x]", "-", false, true},
        {"[-x]", "a", false, true},
        {"\\", "a", false, true},
        {"[a-b-c]", "a", false, true},
        {"[", "a", false, true},
        {"[^", "a", false, true},
        {"[^bc", "a", false, true},
        {"a[", "a", false, true},
        {"a[", "ab", false, true},
        {"a[", "x", false, true},
        {"a/b[", "x", false, true},
        {"*x", "xxx", true, false},
        // 补充：非 ASCII 字面量、空模式/空名字
        {"", "", true, false},
        {"", "a", false, false},
        {"*", "", true, false},
        {"目录/*.txt", "目录/文件.txt", true, false},
        {"目录/*.txt", "目录/子目录/文件.txt", false, false},
        {"?", "目", true, false},
        {"??", "目", false, false},
        {"[一-龥]", "中", true, false},
    };
    for (const MatchCase& test : kMatch) {
        const auto result = go2cpp::path::MatchChecked(test.pattern, test.name);
        Check(result.matched == test.matched,
              ("Match matched: " + std::string(test.pattern) + " vs " +
               std::string(test.name))
                  .c_str());
        Check(result.bad_pattern == test.bad,
              ("Match bad_pattern: " + std::string(test.pattern) + " vs " +
               std::string(test.name))
                  .c_str());
        Check(go2cpp::path::Match(test.pattern, test.name) == test.matched,
              "Match 便捷版与 MatchChecked 不一致");
    }
}

void TestIsAbs() {
    static const TextCase kAbs[] = {
        {"", "0"}, {"/", "1"}, {"/a", "1"}, {"a", "0"},
        {"a/b", "0"}, {"//a", "1"}, {"./a", "0"}, {"../a", "0"},
    };
    for (const TextCase& test : kAbs) {
        const bool expected = test.expected[0] == '1';
        Check(go2cpp::path::IsAbs(test.input) == expected, "IsAbs");
    }
}

// ---------------------------------------------------------------------------
// 2. 随机属性测试
// ---------------------------------------------------------------------------

/// splitmix64：固定种子，跨平台可重复（不依赖标准库分布实现）。
class Rng final {
  public:
    explicit Rng(std::uint64_t seed) : m_state(seed) {}

    std::uint64_t Next() {
        m_state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = m_state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    std::size_t Below(std::size_t limit) {
        return limit == 0 ? 0 : static_cast<std::size_t>(Next() % limit);
    }

  private:
    std::uint64_t m_state;
};

std::string RandomComponent(Rng* rng) {
    static const char* const kAscii[] = {
        "a", "b", "conf", "app.ini", "x-y_z", "123", "file.cpp",
        ".hidden", "...", "..a", "a..", "with space", "tab\\x", "naïve",
    };
    static const char* const kUnicode[] = {
        "目录", "文件.txt", "日本語", "αβγ", "emoji😀", "naïve",
    };
    switch (rng->Below(10)) {
        case 0:
            return "";
        case 1:
            return ".";
        case 2:
            return "..";
        case 3:
            return kUnicode[rng->Below(sizeof(kUnicode) / sizeof(kUnicode[0]))];
        case 4: {  // 超长段（最长 300 字节）
            const std::size_t length = 150 + rng->Below(151);
            return std::string(length, static_cast<char>('a' + rng->Below(26)));
        }
        default:
            return kAscii[rng->Below(sizeof(kAscii) / sizeof(kAscii[0]))];
    }
}

std::string RandomPath(Rng* rng) {
    std::string path;
    if (rng->Below(100) < 30) {
        path.push_back('/');
        if (rng->Below(100) < 15) {
            path.push_back('/');
        }
    }
    const std::size_t parts = rng->Below(9);
    for (std::size_t i = 0; i < parts; ++i) {
        if (i > 0) {
            const std::size_t extra = rng->Below(10);
            path.append(extra < 6 ? 1 : (extra < 9 ? 2 : 3), '/');
        }
        path += RandomComponent(rng);
    }
    if (rng->Below(100) < 20) {
        path.push_back('/');
    }
    return path;
}

bool HasDoubleSlash(const std::string& value) {
    return value.find("//") != std::string::npos;
}

void TestRandomProperties() {
    Rng rng(0x5EED20261018ULL);
    constexpr int kIterations = 200000;
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        const std::string input = RandomPath(&rng);
        const std::string clean = go2cpp::path::Clean(input);
        const std::string twice = go2cpp::path::Clean(clean);

        Check(!clean.empty(), "Clean 结果不能为空");
        Check(twice == clean, "Clean 必须幂等");
        Check(clean == "/" || clean.back() != '/',
              "Clean 结果只有根可以 / 结尾");
        Check(!HasDoubleSlash(clean), "Clean 结果不能含 //");
        Check(clean == "." || clean.find("/./") == std::string::npos,
              "Clean 结果不能含 . 组件");
        Check(go2cpp::path::IsAbs(clean) ==
                  (!input.empty() && input.front() == '/'),
              "IsAbs 与输入是否绝对一致");

        // 组件检查：无空组件；`..` 只能连续出现在开头。
        // 结果恰好是 "." 时唯一组件就是 "."，属于合法输出，跳过组件检查。
        if (clean != ".") {
            bool seen_non_dotdot = false;
            std::size_t start =
                (!clean.empty() && clean.front() == '/') ? 1 : 0;
            while (start <= clean.size()) {
                const std::size_t slash = clean.find('/', start);
                const std::string component =
                    clean.substr(start, slash == std::string::npos
                                            ? std::string::npos
                                            : slash - start);
                if (component.empty()) {
                    if (slash != std::string::npos) {
                        Check(false, "Clean 结果含空组件");
                        break;
                    }
                } else if (component == "..") {
                    Check(!seen_non_dotdot, ".. 只能出现在开头");
                } else {
                    Check(component != ".", "Clean 结果含 . 组件");
                    seen_non_dotdot = true;
                }
                if (slash == std::string::npos) {
                    break;
                }
                start = slash + 1;
            }
        }

        // Split 的拼接不变量对任意输入都成立。
        const auto [dir, file] = go2cpp::path::Split(input);
        Check(dir + file == input, "Split 拼接不变量");
        // Ext 必须是 Base 的后缀（非空时以 . 开头）。
        const std::string base = go2cpp::path::Base(clean);
        const std::string ext = go2cpp::path::Ext(clean);
        if (!ext.empty()) {
            Check(base.size() >= ext.size() &&
                      base.compare(base.size() - ext.size(), ext.size(),
                                   ext) == 0,
                  "Ext 必须是 Base 的后缀");
        }
        // Base 永远非空。
        Check(!base.empty(), "Base 永远非空");
    }
    std::printf("[path] 随机属性测试: %d 条\n", kIterations);
}

// ---------------------------------------------------------------------------
// 3. 与 Go path.Clean 的 1 万条对拍
// ---------------------------------------------------------------------------

int RunConformance() {
    const std::string file =
        std::string(GO2CPP_PATH_TESTDATA_DIR) + "/go_path_clean_10000.txt";
    std::ifstream input(file);
    if (!input) {
        std::fprintf(stderr,
                     "[path] FAILED: 无法打开对拍数据文件 %s\n"
                     "  重新生成: python3 tests/path/scripts/cross_check.py\n",
                     file.c_str());
        ++g_failures;
        return 0;
    }
    std::size_t total = 0;
    std::size_t mismatches = 0;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            std::fprintf(stderr, "[path] FAILED: 对拍数据行缺少 TAB: %s\n",
                         line.c_str());
            ++g_failures;
            continue;
        }
        const std::string path = line.substr(0, tab);
        const std::string expected = line.substr(tab + 1);
        const std::string actual = go2cpp::path::Clean(path);
        ++total;
        if (actual != expected) {
            ++mismatches;
            if (mismatches <= 20) {
                std::fprintf(stderr,
                             "[path] 对拍不一致: path=\"%s\" ours=\"%s\" "
                             "go=\"%s\"\n",
                             path.c_str(), actual.c_str(), expected.c_str());
            }
        }
    }
    if (total != 10000) {
        std::fprintf(stderr, "[path] FAILED: 对拍数据应为 10000 条，实际 %zu\n",
                     total);
        ++g_failures;
    }
    if (mismatches != 0) {
        std::fprintf(stderr, "[path] FAILED: 与 Go 对拍不一致 %zu/%zu 条\n",
                     mismatches, total);
        ++g_failures;
    }
    std::printf("[path] 与 Go path.Clean 对拍: %zu 条, 不一致 %zu 条\n", total,
                mismatches);
    return static_cast<int>(mismatches);
}

// ---------------------------------------------------------------------------
// 4. 压测：Join / Clean / Match 各 100 万次
// ---------------------------------------------------------------------------

template <typename Fn>
void Bench(const char* name, int iterations, Fn fn) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::size_t sink = 0;
    for (int i = 0; i < iterations; ++i) {
        sink += fn(i);
    }
    const auto end = Clock::now();
    const double seconds =
        std::chrono::duration<double>(end - start).count();
    const double mops = static_cast<double>(iterations) / seconds / 1e6;
    std::printf("[path][bench] %-8s %d ops in %.3f s => %.2f Mops/s (sink=%zu)\n",
                name, iterations, seconds, mops, sink);
}

void RunBenchmarks() {
    constexpr int kIterations = 1000000;

    static const char* const kJoinParts[4][4] = {
        {"usr", "local", "share", "app.ini"},
        {"etc", "go2cpp", "", "config.toml"},
        {"", "var", "log", "app.log"},
        {"opt", "app", "bin", "run"},
    };
    Bench("Join", kIterations, [](int i) {
        const int row = i & 3;
        return go2cpp::path::Join(kJoinParts[row][0], kJoinParts[row][1],
                                  kJoinParts[row][2], kJoinParts[row][3])
            .size();
    });

    static const char* const kCleanPaths[] = {
        "/usr/local/share/app.ini",
        "./config/../conf/app.ini",
        "a/b/c/../../d//./e",
        "../a/./b/../../c",
        "目录/文件.txt/../日志.log",
    };
    Bench("Clean", kIterations, [](int i) {
        return go2cpp::path::Clean(kCleanPaths[i % 5]).size();
    });

    static const char* const kMatchPairs[4][2] = {
        {"*.cpp", "file.cpp"},
        {"src/*.h", "src/a.h"},
        {"a?c", "abc"},
        {"[a-z]*", "hello"},
    };
    Bench("Match", kIterations, [](int i) {
        const int row = i & 3;
        return go2cpp::path::Match(kMatchPairs[row][0], kMatchPairs[row][1])
                   ? std::size_t{1}
                   : std::size_t{0};
    });
}

// ---------------------------------------------------------------------------
// 对拍辅助模式：逐行读入路径，逐行输出 Clean 结果
// ---------------------------------------------------------------------------

int CleanFileMode(const char* path) {
    std::ifstream input(path);
    if (!input) {
        std::fprintf(stderr, "[path] 无法打开 %s\n", path);
        return 1;
    }
    std::string line;
    while (std::getline(input, line)) {
        std::printf("%s\n", go2cpp::path::Clean(line).c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--clean-file") == 0) {
        return CleanFileMode(argv[2]);
    }
    if (argc != 1) {
        std::fprintf(stderr,
                     "用法: %s [--clean-file <路径列表文件>]\n", argv[0]);
        return 2;
    }

    std::printf("=== go2cpp::path 测试 ===\n");
    TestCleanTable();
    TestSplitExtBaseDirTable();
    TestJoinTable();
    TestMatchTable();
    TestIsAbs();
    TestRandomProperties();
    RunConformance();
    RunBenchmarks();

    if (g_failures != 0) {
        std::fprintf(stderr, "[path] failures=%d\n", g_failures);
        return 1;
    }
    std::printf("[path] all checks passed\n");
    return 0;
}
