/**
 * @file test_path_filepath.cpp
 * @brief `go2cpp::path::Filepath` 的语义、7 个真实场景与文件系统边界测试。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 *
 * ## 覆盖策略
 *
 * 真实场景（≥5，覆盖全部待验证功能）：
 *  1. 配置文件路径拼接：`Join(base, "conf", "app.ini")` + `Clean`/`Base`/`Dir`/
 *     `Ext`/`Split`/`IsAbs`；
 *  2. 相对路径转绝对：`Abs`（绝对、相对、带 `..`、空串）；
 *  3. 两个路径的相对关系：`Rel`（同路径、子路径、兄弟、跨根、绝对/相对混用、
 *     含前导 `..` 的 base）；
 *  4. 遍历目录树：`Walk`（顺序、深度、`SkipDir`/`SkipAll`、不跟符号链接、
 *     不存在的根、`lstat` 失败）；
 *  5. 通配符展开：`Glob`（单层、跨目录、绝对模式、隐藏文件、无匹配、非法模式）；
 *  6. 静态文件服务器路径解析：`Join`+`Clean`+`Rel`+`Match` 阻断目录穿越；
 *  7. 符号链接解析：`EvalSymlinks`（相对链、绝对链接、悬空链接、自环 255 层上限）。
 *
 * 全部走**真实文件系统**（`/tmp/go2cpp_path_test_<pid>`），不做 mock：
 * 这里验证的正是「和内核语义对不对」。
 */

#include "go2cpp/path/filepath.hpp"
#include "go2cpp/path/path.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[filepath] FAILED: %s\n", what);
        ++g_failures;
    }
}

void CheckEq(const std::string& actual, const std::string& expected,
             const char* what, const std::string& input = {}) {
    if (actual != expected) {
        std::fprintf(stderr,
                     "[filepath] FAILED: %s input=\"%s\" actual=\"%s\" "
                     "expected=\"%s\"\n",
                     what, input.c_str(), actual.c_str(), expected.c_str());
        ++g_failures;
    }
}

std::string Root() {
    return "/tmp/go2cpp_path_test_" + std::to_string(::getpid());
}

std::string Under(const std::string& relative) {
    return Root() + "/" + relative;
}

void MakeDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    Check(!ec, ("创建目录失败: " + path).c_str());
}

void WriteFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    Check(out.good(), ("写文件失败: " + path).c_str());
}

void MakeSymlink(const std::string& target, const std::string& link) {
    ::unlink(link.c_str());
    Check(::symlink(target.c_str(), link.c_str()) == 0,
          ("创建符号链接失败: " + link).c_str());
}

/// 测试根目录的规范形式（/tmp 本身可能是符号链接，比较时要先解析）。
std::string CanonicalRoot() {
    std::string out;
    std::string error;
    if (!go2cpp::path::Filepath::EvalSymlinks(Root(), &out, &error)) {
        std::fprintf(stderr, "[filepath] FAILED: EvalSymlinks(root): %s\n",
                     error.c_str());
        ++g_failures;
        return Root();
    }
    return out;
}

// ---------------------------------------------------------------------------
// SplitList / VolumeName / ToSlash / FromSlash / IsLocal / Localize
// ---------------------------------------------------------------------------
void TestSplitListVolumeSlashLocalize() {
    namespace fp = go2cpp::path::Filepath;

    // SplitList：空串 -> 空列表；连续/首尾分隔符产生空段。
    {
        const std::vector<std::string> list = fp::SplitList("/bin:/usr/bin::/opt/x");
        Check(list.size() == 4, "SplitList 段数");
        CheckEq(list[0], "/bin", "SplitList[0]");
        CheckEq(list[1], "/usr/bin", "SplitList[1]");
        CheckEq(list[2], "", "SplitList[2] 空段");
        CheckEq(list[3], "/opt/x", "SplitList[3]");
        Check(fp::SplitList("").empty(), "SplitList 空串应为空列表");
        const std::vector<std::string> single = fp::SplitList("/only");
        Check(single.size() == 1 && single[0] == "/only", "SplitList 单段");
        const std::vector<std::string> two_empty = fp::SplitList(":");
        Check(two_empty.size() == 2 && two_empty[0].empty() &&
                  two_empty[1].empty(),
              "SplitList(':') 应为两个空段");
    }

    // VolumeName：Unix 恒为空。
    CheckEq(fp::VolumeName("/a/b"), "", "VolumeName 绝对路径");
    CheckEq(fp::VolumeName("C:/x"), "", "VolumeName 盘符（Unix 不认）");
    CheckEq(fp::VolumeName(""), "", "VolumeName 空串");

    // ToSlash / FromSlash：Unix 是恒等变换（反斜杠是普通字符）。
    CheckEq(fp::ToSlash("a\\b/c"), "a\\b/c", "ToSlash Unix 恒等");
    CheckEq(fp::FromSlash("a\\b/c"), "a\\b/c", "FromSlash Unix 恒等");
    CheckEq(fp::Localize("/a/b"), "/a/b", "Localize Unix 恒等");
    CheckEq(fp::Localize(""), "", "Localize 空串");

    // IsLocal：与 Go unixIsLocal 的判据逐项对齐。
    Check(!fp::IsLocal(""), "IsLocal 空串为 false");
    Check(!fp::IsLocal("/a"), "IsLocal 绝对路径为 false");
    Check(!fp::IsLocal(".."), "IsLocal('..') 为 false");
    Check(!fp::IsLocal("../a"), "IsLocal('../a') 为 false");
    Check(!fp::IsLocal("a/../../b"), "IsLocal('a/../../b') 为 false");
    Check(fp::IsLocal("."), "IsLocal('.') 为 true");
    Check(fp::IsLocal("a"), "IsLocal('a') 为 true");
    Check(fp::IsLocal("a/b"), "IsLocal('a/b') 为 true");
    Check(fp::IsLocal("./a"), "IsLocal('./a') 为 true");
    Check(fp::IsLocal("a/../b"), "IsLocal('a/../b') 为 true");
    Check(fp::IsLocal("a/b/.."), "IsLocal('a/b/..') 为 true");

    // 真实场景：遍历 PATH 段并判断每个相对项是否可安全 Join 到安装根。
    {
        const std::vector<std::string> entries =
            fp::SplitList("/usr/local/bin:/usr/bin:relative/bin:../escape");
        Check(entries.size() == 4, "PATH 解析段数");
        static const bool kExpected[] = {false, false, true, false};
        for (std::size_t i = 0; i < entries.size(); ++i) {
            Check(fp::IsLocal(entries[i]) == kExpected[i],
                  "PATH 段的 IsLocal 判定");
        }
        CheckEq(fp::Join("/install", "relative/bin"), "/install/relative/bin",
                "IsLocal 的 Join 场景");
    }
}

// ---------------------------------------------------------------------------
// 词法函数：Linux 上与 path 完全一致
// ---------------------------------------------------------------------------

void TestLexicalDelegation() {
    Check(go2cpp::path::Filepath::Separator == '/', "Linux 分隔符应为 /");
    Check(go2cpp::path::Filepath::ListSeparator == ':', "Linux 列表分隔符应为 :");

    static const char* const kPaths[] = {
        "", "/", "a/b", "a//b/./c/../d", "../a", "/a/../../b",
        "目录/文件.txt", "a\\b/c",
    };
    for (const char* p : kPaths) {
        CheckEq(go2cpp::path::Filepath::Clean(p), go2cpp::path::Clean(p),
                "Filepath::Clean 与 path::Clean 不一致", p);
        CheckEq(go2cpp::path::Filepath::Base(p), go2cpp::path::Base(p),
                "Filepath::Base", p);
        CheckEq(go2cpp::path::Filepath::Dir(p), go2cpp::path::Dir(p),
                "Filepath::Dir", p);
        CheckEq(go2cpp::path::Filepath::Ext(p), go2cpp::path::Ext(p),
                "Filepath::Ext", p);
        const auto [fd, ff] = go2cpp::path::Filepath::Split(p);
        const auto [pd, pf] = go2cpp::path::Split(p);
        Check(fd == pd && ff == pf, "Filepath::Split");
        Check(go2cpp::path::Filepath::IsAbs(p) == go2cpp::path::IsAbs(p),
              "Filepath::IsAbs");
    }

    CheckEq(go2cpp::path::Filepath::Join("a", "", "b"), "a/b",
            "Filepath::Join");
    CheckEq(go2cpp::path::Filepath::Join(), "", "Filepath::Join 空参数");
    CheckEq(go2cpp::path::Filepath::Join("", ""), "", "Filepath::Join 全空");
    CheckEq(go2cpp::path::Filepath::Join(std::vector<std::string>{"a", "b"}),
            "a/b", "Filepath::Join vector");

    Check(go2cpp::path::Filepath::Match("*.cpp", "a.cpp"), "Filepath::Match");
    Check(!go2cpp::path::Filepath::Match("a*b", "a/b"),
          "Filepath::Match 不跨分隔符");
    Check(go2cpp::path::Filepath::MatchChecked("[", "a").bad_pattern,
          "Filepath::MatchChecked 非法模式");
}

// ---------------------------------------------------------------------------
// 场景 1：配置文件路径拼接
// ---------------------------------------------------------------------------

void ScenarioConfigPath() {
    namespace fp = go2cpp::path::Filepath;
    const std::string base = "/etc/go2cpp";
    const std::string full = fp::Join(base, "conf", "app.ini");
    CheckEq(full, "/etc/go2cpp/conf/app.ini", "场景1 Join 配置路径");
    CheckEq(fp::Clean(full + "/./"), "/etc/go2cpp/conf/app.ini",
            "场景1 Clean 配置路径");
    CheckEq(fp::Base(full), "app.ini", "场景1 Base");
    CheckEq(fp::Dir(full), "/etc/go2cpp/conf", "场景1 Dir");
    CheckEq(fp::Ext(full), ".ini", "场景1 Ext");
    const auto [dir, file] = fp::Split(full);
    CheckEq(dir, "/etc/go2cpp/conf/", "场景1 Split.dir");
    CheckEq(file, "app.ini", "场景1 Split.file");
    Check(fp::IsAbs(full), "场景1 配置路径应为绝对路径");

    // 相对 base + 空元素 + 重复斜杠 + `..` 回退。
    CheckEq(fp::Join("config", "", "conf//", "../conf/app.ini"),
            "config/conf/app.ini", "场景1 相对 base 拼接");
    CheckEq(fp::Join(base, ""), base, "场景1 空元素被忽略");
    CheckEq(fp::Join("", ""), "", "场景1 全空仍为空串");
}

// ---------------------------------------------------------------------------
// 场景 2：相对路径转绝对
// ---------------------------------------------------------------------------

void ScenarioAbs() {
    namespace fp = go2cpp::path::Filepath;
    std::string out;
    std::string error;

    Check(fp::Abs("/a/./b/../c", &out, &error), "场景2 Abs 绝对路径应成功");
    CheckEq(out, "/a/c", "场景2 Abs 绝对路径 Clean");

    Check(fp::Abs("rel/../x", &out, &error), "场景2 Abs 相对路径应成功");
    std::string cwd;
    {
        std::error_code ec;
        cwd = std::filesystem::current_path(ec).string();
        Check(!ec, "场景2 读取 cwd");
    }
    CheckEq(out, fp::Join(cwd, "x"), "场景2 Abs = Join(cwd, Clean)");

    Check(fp::Abs("", &out, &error), "场景2 Abs 空串应成功");
    CheckEq(out, fp::Clean(cwd), "场景2 Abs 空串 = Clean(cwd)");

    Check(fp::Abs("../a", &out, &error), "场景2 Abs .. 前缀");
    CheckEq(out, fp::Join(cwd, "../a"), "场景2 Abs .. 前缀拼接");
    Check(out.find("/../") == std::string::npos,
          "场景2 Abs 结果必须是规范化的");
}

// ---------------------------------------------------------------------------
// 场景 3：两路径的相对关系
// ---------------------------------------------------------------------------

void ScenarioRel() {
    namespace fp = go2cpp::path::Filepath;
    struct RelCase final {
        const char* base;
        const char* target;
        const char* expected;  // nullptr 表示应失败
    };
    static const RelCase kCases[] = {
        {"/a/b", "/a/b", "."},
        {"/a/b", "/a/b/c", "c"},
        {"/a/b", "/a/c", "../c"},
        {"/a/b", "/a", ".."},
        {"/a/b", "/x/y", "../../x/y"},
        {"/", "/a/b", "a/b"},
        {"a/b", "a/b/c", "c"},
        {"a/b", "a/c", "../c"},
        {"a", "a/b", "b"},
        {".", "a/b", "a/b"},
        {"../a", "../a/b", "b"},
        {"/a/b", "/a/b/../c", "../c"},
        {"a/b", "/a/b", nullptr},
        {"../a", "a/b", nullptr},
        {"/a/b", "../a", nullptr},
    };
    for (const RelCase& test : kCases) {
        std::string out;
        std::string error;
        const bool ok = fp::Rel(test.base, test.target, &out, &error);
        if (test.expected == nullptr) {
            Check(!ok, "场景3 Rel 应失败");
            Check(!error.empty(), "场景3 Rel 失败时必须写诊断");
            continue;
        }
        Check(ok, "场景3 Rel 应成功");
        if (!ok) {
            std::fprintf(stderr, "  base=%s target=%s error=%s\n", test.base,
                         test.target, error.c_str());
            continue;
        }
        CheckEq(out, test.expected, "场景3 Rel 结果",
                std::string(test.base) + " -> " + test.target);
        // 核心不变量：Join(base, Rel(base,target)) == Clean(target)
        CheckEq(fp::Join(test.base, out), fp::Clean(test.target),
                "场景3 Rel 拼接不变量");
    }
}

// ---------------------------------------------------------------------------
// 场景 4：遍历目录树
// ---------------------------------------------------------------------------

void ScenarioWalk() {
    namespace fp = go2cpp::path::Filepath;
    const std::string root = Under("walk");
    MakeDir(root + "/a_dir/nested");
    MakeDir(root + "/b_dir");
    WriteFile(root + "/a_dir/a1.txt", "1");
    WriteFile(root + "/a_dir/a2.txt", "22");
    WriteFile(root + "/a_dir/nested/n1.txt", "333");
    WriteFile(root + "/b_dir/b1.txt", "4444");
    WriteFile(root + "/c.txt", "55555");
    MakeSymlink("a_dir", root + "/link_to_a");

    struct Visit final {
        std::string path;
        int depth;
        bool is_dir;
        bool is_link;
        bool is_regular;
        std::uintmax_t size;
        int error_code;
    };
    std::vector<Visit> visits;
    const auto callback = [&visits, &root](const std::string& path,
                                           const fp::WalkInfo& info,
                                           int error_code) {
        int depth = 0;
        if (path.size() > root.size()) {
            for (std::size_t i = root.size(); i < path.size(); ++i) {
                if (path[i] == '/') {
                    ++depth;
                }
            }
        }
        visits.push_back(Visit{path, depth, info.is_dir, info.is_symlink,
                               info.is_regular, info.size, error_code});
        return fp::WalkAction::Continue;
    };

    std::string error;
    Check(fp::Walk(root, callback, &error), "场景4 Walk 应成功");
    Check(visits.size() == 10, "场景4 Walk 条目数应为 10（含根）");

    static const char* const kExpected[] = {
        "",
        "/a_dir",
        "/a_dir/a1.txt",
        "/a_dir/a2.txt",
        "/a_dir/nested",
        "/a_dir/nested/n1.txt",
        "/b_dir",
        "/b_dir/b1.txt",
        "/c.txt",
        "/link_to_a",
    };
    for (std::size_t i = 0; i < visits.size() && i < 10; ++i) {
        CheckEq(visits[i].path, root + kExpected[i], "场景4 Walk 顺序");
        Check(visits[i].error_code == 0, "场景4 Walk 不应有错误");
    }
    if (visits.size() == 10) {
        static const int kDepths[] = {0, 1, 2, 2, 2, 3, 1, 2, 1, 1};
        for (std::size_t i = 0; i < 10; ++i) {
            Check(visits[i].depth == kDepths[i], "场景4 Walk 深度");
        }
        Check(visits[0].is_dir, "场景4 根应为目录");
        Check(visits[2].is_regular && visits[2].size == 1,
              "场景4 a1.txt 应为普通文件且 size=1");
        Check(visits[9].is_link && !visits[9].is_dir,
              "场景4 符号链接应作为链接条目回调、不跟随");
        Check(!visits[6].is_link, "场景4 b_dir 不是链接");
    }
    for (const Visit& visit : visits) {
        Check(visit.path.find("link_to_a/") == std::string::npos,
              "场景4 Walk 不能跟进符号链接");
    }

    // SkipDir：跳过 a_dir 子树。
    std::vector<std::string> skipped;
    fp::Walk(root, [&skipped](const std::string& path, const fp::WalkInfo& info,
                              int) {
        skipped.push_back(path);
        return info.is_dir && path.size() > 0 &&
                       path.substr(path.size() - 5) == "a_dir"
                   ? fp::WalkAction::SkipDir
                   : fp::WalkAction::Continue;
    });
    Check(skipped.size() == 6, "场景4 SkipDir 后条目数应为 6");
    Check(std::find(skipped.begin(), skipped.end(),
                    root + "/a_dir/nested") == skipped.end(),
          "场景4 SkipDir 必须跳过子树");

    // SkipAll：只访问前 3 个条目。
    std::vector<std::string> stopped;
    fp::Walk(root, [&stopped](const std::string& path, const fp::WalkInfo&,
                              int) {
        stopped.push_back(path);
        return stopped.size() >= 3 ? fp::WalkAction::SkipAll
                                   : fp::WalkAction::Continue;
    });
    Check(stopped.size() == 3, "场景4 SkipAll 应立即结束");

    // 不存在的根：回调收到 ENOENT，Walk 本身仍返回 true（Go 语义）。
    int root_error = 0;
    std::size_t calls = 0;
    const bool walk_ok = fp::Walk(Under("no_such_dir"),
                                  [&root_error, &calls](const std::string&,
                                                        const fp::WalkInfo&,
                                                        int code) {
                                      ++calls;
                                      root_error = code;
                                      return fp::WalkAction::Continue;
                                  });
    Check(walk_ok, "场景4 根不存在时 Walk 仍返回 true");
    Check(calls == 1 && root_error == ENOENT,
          "场景4 根不存在时回调应收到 ENOENT");
}

// ---------------------------------------------------------------------------
// 场景 5：Glob 通配符展开
// ---------------------------------------------------------------------------

void ScenarioGlob() {
    namespace fp = go2cpp::path::Filepath;
    const std::string root = Under("glob");
    MakeDir(root + "/sub");
    WriteFile(root + "/main.cpp", "m");
    WriteFile(root + "/util.cpp", "u");
    WriteFile(root + "/notes.txt", "n");
    WriteFile(root + "/.hidden.cpp", "h");
    WriteFile(root + "/sub/impl.cpp", "i");
    WriteFile(root + "/sub/impl.h", "j");

    std::vector<std::string> matches;
    std::string error;
    Check(fp::Glob(root + "/*.cpp", &matches, &error), "场景5 Glob 单层应成功");
    const std::vector<std::string> expected_cpp = {
        root + "/.hidden.cpp", root + "/main.cpp", root + "/util.cpp"};
    Check(matches == expected_cpp, "场景5 Glob 单层结果/顺序");
    Check(matches.empty() || matches.front().find(root) == 0,
          "场景5 Glob 结果是完整路径");

    Check(fp::Glob(root + "/*/*.cpp", &matches, &error),
          "场景5 Glob 跨目录应成功");
    Check(matches == std::vector<std::string>{root + "/sub/impl.cpp"},
          "场景5 Glob 跨目录结果");

    Check(fp::Glob(root + "/sub/*.h", &matches, &error),
          "场景5 Glob 子目录头文件");
    Check(matches == std::vector<std::string>{root + "/sub/impl.h"},
          "场景5 Glob 子目录结果");

    Check(fp::Glob(root + "/nomatch*", &matches, &error),
          "场景5 Glob 无匹配应成功");
    Check(matches.empty(), "场景5 Glob 无匹配返回空列表");

    Check(fp::Glob(root + "/*", &matches, &error), "场景5 Glob *");
    Check(matches.size() == 5, "场景5 Glob * 应有 5 个条目");
    Check(std::is_sorted(matches.begin(), matches.end()),
          "场景5 Glob 结果按词典序");

    Check(!fp::Glob(root + "/[", &matches, &error),
          "场景5 Glob 非法模式应失败");
    Check(!error.empty(), "场景5 Glob 非法模式应写诊断");
    Check(matches.empty(), "场景5 Glob 失败时结果为空");
}

// ---------------------------------------------------------------------------
// 场景 6：静态文件服务器路径解析（目录穿越防护）
// ---------------------------------------------------------------------------

void ScenarioStaticServer() {
    namespace fp = go2cpp::path::Filepath;
    const std::string doc_root = "/srv/www";

    struct Request final {
        const char* raw_path;
        bool expected_allowed;
        const char* expected_path;
    };
    static const Request kRequests[] = {
        {"/index.html", true, "/srv/www/index.html"},
        {"/static/app.js", true, "/srv/www/static/app.js"},
        {"/static/../secret.txt", false, "/srv/www/secret.txt"},
        {"../etc/passwd", false, "/srv/etc/passwd"},
        {"/static//./app.js", true, "/srv/www/static/app.js"},
    };
    for (const Request& request : kRequests) {
        const std::string cleaned = fp::Clean(request.raw_path);
        const std::string joined = fp::Join(doc_root, cleaned);
        std::string relative;
        std::string error;
        bool inside = false;
        if (fp::Rel(doc_root, joined, &relative, &error)) {
            inside = fp::Clean(relative) != ".." &&
                     fp::Clean(relative).rfind("../", 0) != 0;
        }
        // 只允许 .html/.js/.css 等静态资源（对文件名匹配，不是整条路径）。
        const std::string base = fp::Base(joined);
        const bool extension_ok = fp::Match("*.html", base) ||
                                  fp::Match("*.js", base) ||
                                  fp::Match("*.css", base);
        const bool allowed = inside && extension_ok;
        Check(allowed == request.expected_allowed, "场景6 静态资源放行判断");
        CheckEq(joined, request.expected_path, "场景6 解析后的文件路径");
    }

    // 典型穿越：不带前导 / 的 `../etc/passwd` 会把根拼到 /srv/etc/passwd，
    // Rel 给出以 `..` 开头的相对路径，调用方据此拒绝。
    std::string relative;
    std::string error;
    const std::string escaped = fp::Join(doc_root, "../etc/passwd");
    CheckEq(escaped, "/srv/etc/passwd", "场景6 穿越拼接结果");
    Check(fp::Rel(doc_root, escaped, &relative, &error),
          "场景6 Rel 跨根仍可给出相对路径");
    Check(relative == "../etc/passwd", "场景6 跨根相对路径应以 .. 开头");
    Check(relative.rfind("../", 0) == 0, "场景6 穿越必须可被识别");
}

// ---------------------------------------------------------------------------
// 场景 7：EvalSymlinks
// ---------------------------------------------------------------------------

void ScenarioEvalSymlinks() {
    namespace fp = go2cpp::path::Filepath;
    const std::string root = Under("links");
    MakeDir(root + "/real");
    WriteFile(root + "/real/file.txt", "payload");
    MakeSymlink("real", root + "/link1");
    MakeSymlink("link1", root + "/link2");
    MakeSymlink(root + "/real", root + "/abs_link");
    MakeSymlink("missing_target", root + "/dangling");
    MakeSymlink("self_loop", root + "/self_loop");

    const std::string canonical_root = CanonicalRoot() + "/links";
    std::string out;
    std::string error;

    Check(fp::EvalSymlinks(root + "/link2/file.txt", &out, &error),
          "场景7 相对链接链应成功");
    CheckEq(out, canonical_root + "/real/file.txt", "场景7 相对链接链结果");

    Check(fp::EvalSymlinks(root + "/abs_link/file.txt", &out, &error),
          "场景7 绝对链接应成功");
    CheckEq(out, canonical_root + "/real/file.txt", "场景7 绝对链接结果");

    Check(fp::EvalSymlinks(root + "/real/file.txt", &out, &error),
          "场景7 普通路径应成功");
    CheckEq(out, canonical_root + "/real/file.txt", "场景7 普通路径结果");

    Check(!fp::EvalSymlinks(root + "/dangling", &out, &error),
          "场景7 悬空链接应失败");
    Check(!error.empty(), "场景7 悬空链接应写诊断");

    Check(!fp::EvalSymlinks(root + "/self_loop", &out, &error),
          "场景7 自环链接应因超过 255 层失败");
    Check(error.find("255") != std::string::npos,
          "场景7 自环错误信息应提到 255");

    Check(!fp::EvalSymlinks(Under("no/such/file"), &out, &error),
          "场景7 不存在路径应失败");
}

}  // namespace

int main() {
    std::printf("=== go2cpp::path::Filepath 测试 ===\n");

    std::error_code ec;
    std::filesystem::remove_all(Root(), ec);  // 清掉上次异常退出残留
    MakeDir(Root());

    TestLexicalDelegation();
    TestSplitListVolumeSlashLocalize();
    ScenarioConfigPath();
    ScenarioAbs();
    ScenarioRel();
    ScenarioWalk();
    ScenarioGlob();
    ScenarioStaticServer();
    ScenarioEvalSymlinks();

    std::filesystem::remove_all(Root(), ec);

    if (g_failures != 0) {
        std::fprintf(stderr, "[filepath] failures=%d\n", g_failures);
        return 1;
    }
    std::printf("[filepath] all checks passed\n");
    return 0;
}
