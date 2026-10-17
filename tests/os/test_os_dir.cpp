/**
 * @file test_os_dir.cpp
 * @brief `os` 模块的目录 / 符号链接 / 时间戳 / Sync / Truncate 测试。
 *
 * ## 覆盖策略
 *
 * 全部走**真实文件系统**（`/tmp/osdir_test_<pid>`），不做 mock —— 因为这里要
 * 验证的正是「和内核语义对不对」：`mkdir -p` 的 EEXIST 分支、`readdir` 的
 * `d_type`、`readlink` 的截断重试、`fsync` 后重开能否读到、`ftruncate` 扩展出的
 * 空洞是否读作 0。这些都没法用 mock 验。
 *
 * 8 个场景里含**压测**（1000 文件建/列/删）与**随机树**（随机深度与宽度）。
 */
#include "go2cpp/os/os.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[os-dir] FAILED: %s\n", what);
        ++g_failures;
    }
}

/// 测试根目录（按 pid 隔离，避免并发跑测试时互相踩）。
std::string Root() {
    return "/tmp/osdir_test_" + std::to_string(::getpid());
}

std::string Under(const std::string& relative) { return Root() + "/" + relative; }

/// 写一个小文件（用 os::WriteFile，顺带覆盖它）。
void MakeFile(const std::string& path, const std::string& content = "x") {
    // 不静默：写失败通常意味着父目录不存在，早期版本忽略返回值导致
    // 场景 4/5/6 集体失败却只看到下游断言报错，排查变慢。
    const auto status = go2cpp::os::WriteFile(path, content);
    Check(status.ok(), ("MakeFile 失败: " + path).c_str());
}

bool PathExists(const std::string& path) {
    struct ::stat info {};
    return ::lstat(path.c_str(), &info) == 0;
}

/// 按名字排序，便于比较（ReadDir 的顺序不保证）。
void SortByName(std::vector<go2cpp::os::DirEntry>* entries) {
    std::sort(entries->begin(), entries->end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
}

// ---------------------------------------------------------------------------
// 场景 1：MkdirAll + ReadDir 基础
// ---------------------------------------------------------------------------
void ScenarioMkdirAndRead() {
    const auto base = Under("s1/a/b/c");
    auto status = go2cpp::os::MkdirAll(base, 0755);
    Check(status.ok(), "MkdirAll 多级创建应成功");
    Check(PathExists(base), "多级目录应真的存在");

    // 在被列出的目录里放 3 个文件 + 1 个子目录
    MakeFile(base + "/f1.txt", "1");
    MakeFile(base + "/f2.txt", "22");
    MakeFile(base + "/f3.txt", "333");
    Check(go2cpp::os::MkdirAll(base + "/subdir", 0755).ok(), "子目录创建应成功");

    std::vector<go2cpp::os::DirEntry> entries;
    std::string error;
    status = go2cpp::os::ReadDir(base, &entries, &error);
    Check(status.ok(), "ReadDir 应成功");
    if (!status.ok()) {
        std::fprintf(stderr, "[os-dir] ReadDir 错误: %s\n", error.c_str());
        return;
    }
    Check(entries.size() == 4, "ReadDir 应返回 4 个条目（不含 . 与 ..）");
    SortByName(&entries);
    if (entries.size() == 4) {
        Check(entries[0].name == "f1.txt" && entries[0].is_regular, "f1.txt 应为普通文件");
        Check(entries[1].name == "f2.txt" && entries[1].is_regular, "f2.txt 应为普通文件");
        Check(entries[2].name == "f3.txt" && entries[2].is_regular, "f3.txt 应为普通文件");
        Check(entries[3].name == "subdir" && entries[3].is_dir, "subdir 应为目录");
    }

    // 不存在的目录
    std::vector<go2cpp::os::DirEntry> none;
    status = go2cpp::os::ReadDir(Under("s1/not-exist"), &none, &error);
    Check(!status.ok() && status.system_error == ENOENT,
          "ReadDir 不存在的目录应返回 kOpenFailed + ENOENT");
}

// ---------------------------------------------------------------------------
// 场景 2：MkdirAll 边界
// ---------------------------------------------------------------------------
void ScenarioMkdirEdges() {
    // 已存在且是目录 -> 成功（幂等）
    Check(go2cpp::os::MkdirAll(Root() + "/s2", 0755).ok(), "首次创建应成功");
    Check(go2cpp::os::MkdirAll(Root() + "/s2", 0755).ok(), "重复创建同一目录应成功（幂等）");

    // 已存在但是普通文件 -> 失败 EEXIST
    MakeFile(Root() + "/s2/plain.txt");
    auto status = go2cpp::os::MkdirAll(Root() + "/s2/plain.txt", 0755);
    Check(!status.ok() && status.system_error == EEXIST,
          "在普通文件上 MkdirAll 应返回 kIoFailed + EEXIST");

    // 空路径
    status = go2cpp::os::MkdirAll("", 0755);
    Check(!status.ok() && status.code == go2cpp::os::OsError::kInvalidArgument,
          "空路径应返回 kInvalidArgument");

    // 深路径（10 级）
    std::string deep = Root() + "/s2";
    for (int i = 0; i < 10; ++i) {
        deep += "/d" + std::to_string(i);
    }
    Check(go2cpp::os::MkdirAll(deep, 0755).ok(), "10 级深路径创建应成功");
    Check(PathExists(deep), "10 级深路径应存在");
}

// ---------------------------------------------------------------------------
// 场景 3：RemoveAll
// ---------------------------------------------------------------------------
void ScenarioRemoveAll() {
    Check(go2cpp::os::MkdirAll(Root() + "/s3", 0755).ok(),
          "创建 s3 应成功");
    // 单个文件
    MakeFile(Root() + "/s3/file.txt");
    Check(go2cpp::os::RemoveAll(Root() + "/s3/file.txt").ok(), "删除单个文件应成功");
    Check(!PathExists(Root() + "/s3/file.txt"), "文件应已被删除");

    // 不存在的路径 -> 视为成功（对齐 Go）
    Check(go2cpp::os::RemoveAll(Root() + "/s3/never-existed").ok(),
          "删除不存在的路径应成功（Go 语义）");

    // 递归非空目录
    go2cpp::os::MkdirAll(Root() + "/s3/tree/a/b", 0755);
    MakeFile(Root() + "/s3/tree/x.txt");
    MakeFile(Root() + "/s3/tree/a/y.txt");
    MakeFile(Root() + "/s3/tree/a/b/z.txt");
    Check(go2cpp::os::RemoveAll(Root() + "/s3/tree").ok(), "递归删除应成功");
    Check(!PathExists(Root() + "/s3/tree"), "整棵树应已被删除");

    // 符号链接：只删链接，不删目标
    const auto target = Root() + "/s3/target.txt";
    const auto link = Root() + "/s3/link.txt";
    MakeFile(target, "keep-me");
    Check(go2cpp::os::Symlink(target, link).ok(), "创建符号链接应成功");
    Check(go2cpp::os::RemoveAll(link).ok(), "删除符号链接应成功");
    Check(!PathExists(link), "链接应已被删除");
    Check(PathExists(target), "链接目标**不应**被删除（lstat 语义）");
}

// ---------------------------------------------------------------------------
// 场景 4：符号链接
// ---------------------------------------------------------------------------
void ScenarioSymlink() {
    Check(go2cpp::os::MkdirAll(Root() + "/s4", 0755).ok(),
          "创建 s4 应成功");
    const auto target = Root() + "/s4/real.txt";
    const auto link = Root() + "/s4/link.txt";
    MakeFile(target, "real-content");

    auto status = go2cpp::os::Symlink(target, link);
    Check(status.ok(), "Symlink 应成功");

    // 重复创建同一链接 -> EEXIST
    status = go2cpp::os::Symlink(target, link);
    Check(!status.ok() && status.system_error == EEXIST, "重复 Symlink 应返回 EEXIST");

    std::string read_back;
    std::string error;
    status = go2cpp::os::Readlink(link, &read_back, &error);
    Check(status.ok(), "Readlink 应成功");
    Check(read_back == target, "Readlink 应读回原目标");

    // 悬空链接：目标不存在也能读
    const auto dangling = Root() + "/s4/dangling";
    Check(go2cpp::os::Symlink("/no/such/file", dangling).ok(), "悬空链接应能创建");
    status = go2cpp::os::Readlink(dangling, &read_back, &error);
    Check(status.ok() && read_back == "/no/such/file", "悬空链接的 Readlink 应成功");

    // StatPath: follow=true 跟随到目标；follow=false 看到链接本身
    struct ::stat info {};
    Check(go2cpp::os::StatPath(link, &info, true).ok(), "StatPath(follow) 应成功");
    Check(S_ISREG(info.st_mode), "跟随链接后应是普通文件");
    Check(go2cpp::os::StatPath(link, &info, false).ok(), "StatPath(nofollow) 应成功");
    Check(S_ISLNK(info.st_mode), "不跟随时应是符号链接");

    // 长目标（>256 字节）触发 Readlink 的扩容重试
    std::string long_target = "/tmp/";
    while (long_target.size() < 600) {
        long_target += "very-long-segment/";
    }
    const auto long_link = Root() + "/s4/long-link";
    Check(go2cpp::os::Symlink(long_target, long_link).ok(), "长目标符号链接应能创建");
    status = go2cpp::os::Readlink(long_link, &read_back, &error);
    Check(status.ok() && read_back == long_target,
          "Readlink 长目标应触发扩容并读回完整内容");

    // 不是符号链接的普通文件 -> EINVAL
    status = go2cpp::os::Readlink(target, &read_back, &error);
    Check(!status.ok() && status.system_error == EINVAL,
          "对普通文件 Readlink 应返回 EINVAL");
}

// ---------------------------------------------------------------------------
// 场景 5：SetTimes
// ---------------------------------------------------------------------------
void ScenarioSetTimes() {
    Check(go2cpp::os::MkdirAll(Root() + "/s5", 0755).ok(),
          "创建 s5 应成功");
    const auto path = Root() + "/s5/timed.txt";
    MakeFile(path);

    struct timespec times[2];
    times[0].tv_sec = 1600000000;   // atime
    times[0].tv_nsec = 123456789;
    times[1].tv_sec = 1700000000;   // mtime
    times[1].tv_nsec = 987654321;

    auto status = go2cpp::os::SetTimes(path, times[0], times[1]);
    Check(status.ok(), "SetTimes 应成功");

    struct ::stat info {};
    Check(go2cpp::os::StatPath(path, &info, true).ok(), "StatPath 应成功");
    Check(info.st_mtim.tv_sec == times[1].tv_sec,
          "mtime 的秒应与设置值一致");
    // 纳秒精度取决于文件系统（有些只支持秒级），所以只在秒匹配时校验纳秒
    if (info.st_mtim.tv_sec == times[1].tv_sec && info.st_mtim.tv_nsec != 0) {
        Check(info.st_mtim.tv_nsec == times[1].tv_nsec,
              "mtime 的纳秒应与设置值一致（文件系统支持时）");
    }

    // 不存在的路径
    status = go2cpp::os::SetTimes(Root() + "/s5/nope", times[0], times[1]);
    Check(!status.ok() && status.system_error == ENOENT,
          "对不存在路径 SetTimes 应返回 ENOENT");
}

// ---------------------------------------------------------------------------
// 场景 6：File::Sync + File::Truncate
// ---------------------------------------------------------------------------
void ScenarioSyncAndTruncate() {
    using go2cpp::os::File;
    using go2cpp::os::OpenFlag;

    Check(go2cpp::os::MkdirAll(Root() + "/s6", 0755).ok(), "创建 s6 应成功");
    const auto path = Root() + "/s6/durable.txt";
    const std::string payload = "durable-payload-0123456789";

    // 写 + Sync
    {
        File file;
        auto status = File::Create(path, 0644, &file);
        Check(status.ok(), "Create 应成功");
        if (!status.ok()) return;
        Check(file.WriteAll(payload.data(), payload.size()) == payload.size(),
              "WriteAll 应写入完整长度");
        Check(file.Sync(), "Sync（fsync）应成功");
    }

    // 重新打开验证持久化
    {
        File file;
        // **必须是 kReadWrite**：在只读句柄上 ftruncate 会失败（EINVAL），
        // 第一版这里写的是 kReadOnly，导致后面的 Truncate 断言全挂。
        auto status = File::Open(path, OpenFlag::kReadWrite, 0, &file);
        Check(status.ok(), "重新 Open 应成功");
        if (!status.ok()) return;
        std::string read_back(static_cast<std::size_t>(file.Size()), '\0');
        const auto got = file.Read(read_back.data(), read_back.size());
        Check(got == static_cast<ssize_t>(payload.size()), "重新读取的长度应一致");
        Check(read_back == payload, "Sync 后重新读取的内容应一致");

        // Truncate 到 0
        status = file.Truncate(0);
        Check(status.ok(), "Truncate(0) 应成功");
        Check(file.Size() == 0, "Truncate(0) 后大小应为 0");

        // 负值
        status = file.Truncate(-1);
        Check(!status.ok() && status.code == go2cpp::os::OsError::kInvalidArgument,
              "Truncate(-1) 应返回 kInvalidArgument");
    }

    // Truncate 扩展 -> 新增区域读作 0
    {
        File file;
        auto status = File::Create(path, 0644, &file);
        Check(status.ok(), "再次 Create 应成功");
        if (!status.ok()) return;
        Check(file.Truncate(1024).ok(), "Truncate(1024) 应成功");
        Check(file.Size() == 1024, "扩展后大小应为 1024");
        file.Seek(0, SEEK_SET);
        std::string zeros(1024, 'X');
        const auto got = file.Read(zeros.data(), zeros.size());
        Check(got == 1024, "扩展后应能读满 1024 字节");
        bool all_zero = true;
        for (const char c : zeros) {
            if (c != '\0') { all_zero = false; break; }
        }
        Check(all_zero, "ftruncate 扩展出的区域应读出全 0");
    }

    // 无效句柄
    {
        File invalid;
        Check(!invalid.Sync(), "对无效句柄 Sync 应返回 false");
        Check(!invalid.Truncate(0).ok(), "对无效句柄 Truncate 应失败");
    }
}

// ---------------------------------------------------------------------------
// 场景 7：压测 —— 1000 个文件的建 / 列 / 删
// ---------------------------------------------------------------------------
void ScenarioStress() {
    constexpr int kFileCount = 1000;
    const auto dir = Root() + "/s7/many";
    Check(go2cpp::os::MkdirAll(dir, 0755).ok(), "压测目录创建应成功");

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kFileCount; ++i) {
        MakeFile(dir + "/file_" + std::to_string(i) + ".txt", "x");
    }
    const auto after_create = std::chrono::steady_clock::now();

    std::vector<go2cpp::os::DirEntry> entries;
    Check(go2cpp::os::ReadDir(dir, &entries).ok(), "压测 ReadDir 应成功");
    Check(static_cast<int>(entries.size()) == kFileCount,
          "ReadDir 应返回 1000 个条目");
    const auto after_read = std::chrono::steady_clock::now();

    Check(go2cpp::os::RemoveAll(dir).ok(), "压测 RemoveAll 应成功");
    Check(!PathExists(dir), "压测目录应已被删除");
    const auto after_remove = std::chrono::steady_clock::now();

    const auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::printf("[os-dir] 压测 1000 文件: 创建=%.1fms 遍历=%.1fms 删除=%.1fms\n",
                ms(start, after_create), ms(after_create, after_read),
                ms(after_read, after_remove));
    Check(ms(start, after_remove) < 30000.0, "1000 文件的建/列/删应在 30 秒内");
}

// ---------------------------------------------------------------------------
// 场景 8：随机目录树
// ---------------------------------------------------------------------------
void ScenarioRandomTree() {
    std::mt19937 rng(20261017);
    std::uniform_int_distribution<int> depth_dist(1, 4);
    std::uniform_int_distribution<int> width_dist(1, 4);
    std::uniform_int_distribution<int> file_dist(0, 3);

    const auto root = Root() + "/s8/random";
    Check(go2cpp::os::MkdirAll(root, 0755).ok(), "随机树根创建应成功");

    int created_dirs = 1;
    int created_files = 0;

    // 递归建树（用显式栈避免递归深度问题）
    std::vector<std::pair<std::string, int>> stack;
    stack.emplace_back(root, 0);
    while (!stack.empty()) {
        auto [path, depth] = stack.back();
        stack.pop_back();
        if (depth >= depth_dist(rng)) {
            continue;
        }
        const int width = width_dist(rng);
        for (int i = 0; i < width; ++i) {
            const auto child = path + "/d" + std::to_string(depth) + "_" + std::to_string(i);
            if (!go2cpp::os::MkdirAll(child, 0755).ok()) {
                Check(false, "随机树 MkdirAll 应成功");
                continue;
            }
            ++created_dirs;
            stack.emplace_back(child, depth + 1);
        }
        const int files = file_dist(rng);
        for (int i = 0; i < files; ++i) {
            MakeFile(path + "/f" + std::to_string(i) + ".txt", "data");
            ++created_files;
        }
    }

    // 递归统计（用 ReadDir 遍历全树）
    int counted_dirs = 0;
    int counted_files = 0;
    std::vector<std::string> to_visit{root};
    while (!to_visit.empty()) {
        const auto current = to_visit.back();
        to_visit.pop_back();
        ++counted_dirs;
        std::vector<go2cpp::os::DirEntry> entries;
        if (!go2cpp::os::ReadDir(current, &entries).ok()) {
            Check(false, "随机树 ReadDir 应成功");
            continue;
        }
        for (const auto& entry : entries) {
            if (entry.is_dir) {
                to_visit.push_back(current + "/" + entry.name);
            } else if (entry.is_regular) {
                ++counted_files;
            }
        }
    }

    std::printf("[os-dir] 随机树: 建 %d 目录 / %d 文件；统计到 %d 目录 / %d 文件\n",
                created_dirs, created_files, counted_dirs, counted_files);
    Check(counted_dirs == created_dirs, "递归统计的目录数应与创建数一致");
    Check(counted_files == created_files, "递归统计的文件数应与创建数一致");

    // 整棵删掉
    Check(go2cpp::os::RemoveAll(root).ok(), "随机树 RemoveAll 应成功");
    Check(!PathExists(root), "随机树应已被完全删除");
}

}  // namespace

int main() {
    std::printf("=== os 目录 / 符号链接 / 时间戳 / Sync / Truncate 测试 ===\n");

    // 准备一个干净根目录
    go2cpp::os::RemoveAll(Root());
    if (!go2cpp::os::MkdirAll(Root(), 0755).ok()) {
        std::fprintf(stderr, "[os-dir] 无法创建测试根目录: %s\n", Root().c_str());
        return 1;
    }

    std::printf("[os-dir][场景1] MkdirAll + ReadDir\n");
    ScenarioMkdirAndRead();
    std::printf("[os-dir][场景2] MkdirAll 边界\n");
    ScenarioMkdirEdges();
    std::printf("[os-dir][场景3] RemoveAll\n");
    ScenarioRemoveAll();
    std::printf("[os-dir][场景4] 符号链接\n");
    ScenarioSymlink();
    std::printf("[os-dir][场景5] SetTimes\n");
    ScenarioSetTimes();
    std::printf("[os-dir][场景6] File::Sync + Truncate\n");
    ScenarioSyncAndTruncate();
    std::printf("[os-dir][场景7] 压测 1000 文件\n");
    ScenarioStress();
    std::printf("[os-dir][场景8] 随机目录树\n");
    ScenarioRandomTree();

    // 清理
    go2cpp::os::RemoveAll(Root());

    if (g_failures == 0) {
        std::printf("\n[os-dir] failures=0\n");
    } else {
        std::printf("\n[os-dir] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
