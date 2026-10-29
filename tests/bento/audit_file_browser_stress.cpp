/**
 * @file audit_file_browser_stress.cpp
 * @brief MOD-30 `FileBrowser` 独立审计程序：压测 + 随机路径 + 5 类复杂场景。
 *
 * @details 本文件**不包含** `test_util.hpp`（因而不引入 `link.hpp`），只链接
 *          `file_browser.cpp` 与系统库，用来同时证明模块解耦与头文件自包含。
 *
 *          章节：
 *          1. 1 GiB 流式 Get/Put（吞吐 / RSS / fd / 磁盘 / 内容校验）；
 *          2. 10 万次 List/Stat 与万次小文件 Get/Put（吞吐、fd 泄漏）；
 *          3. 2500+ 随机路径（`..`/绝对/符号链接/空段/反斜杠/盘符/NUL/超长/
 *             Unicode/控制字符），断言拒绝、不逃逸、不崩、无残留；
 *          4. 5 类复杂场景：路径逃逸、1 GiB 大文件、8 线程并发、权限拒绝
 *             （root 时降权到 nobody）、沙箱根不存在/只读/被替换；
 *          5. 排序与目录竞态探针。
 *
 *          用法：`./audit_file_browser_stress [--quick]`
 *
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 */

#include <fcntl.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/app/file_browser.hpp"

namespace {

namespace fs = std::filesystem;

using go2cpp::bento::app::FileBrowser;
using go2cpp::bento::app::FileBrowserOptions;
using go2cpp::bento::app::FileBrowserStatus;
using go2cpp::bento::app::FileEntry;
using go2cpp::bento::app::FileEntryType;

// ---------------------------------------------------------------------------
// 断言与输出
// ---------------------------------------------------------------------------

int g_checks = 0;
int g_failures = 0;

void CheckImpl(bool ok, const char* expression, int line) {
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, line, expression);
        ++g_failures;
    }
}

const char* StatusName(FileBrowserStatus status) {
    return go2cpp::bento::app::FileBrowserStatusToString(status);
}

void CheckStatusImpl(FileBrowserStatus actual, FileBrowserStatus expected,
                     const char* expression, int line) {
    ++g_checks;
    if (actual != expected) {
        std::fprintf(stderr, "[FAIL] %s:%d: %s -> got %s, want %s\n", __FILE__, line,
                     expression, StatusName(actual), StatusName(expected));
        ++g_failures;
    }
}

#define A_CHECK(expression) CheckImpl((expression), #expression, __LINE__)
#define A_CHECK_EQ(actual, expected) \
    CheckImpl(((actual) == (expected)), #actual " == " #expected, __LINE__)
#define A_CHECK_STATUS(actual, expected) \
    CheckStatusImpl((actual), (expected), #actual, __LINE__)

void Note(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

void Section(const char* title) {
    Note("\n=== %s ===\n", title);
}

// ---------------------------------------------------------------------------
// 环境采样
// ---------------------------------------------------------------------------

std::uint64_t NowUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::uint64_t ProcStatusKb(const char* key) {
    std::ifstream input("/proc/self/status");
    std::string line;
    const std::size_t key_length = std::strlen(key);
    while (std::getline(input, line)) {
        if (line.size() > key_length && line.compare(0, key_length, key) == 0 &&
            line[key_length] == ':') {
            unsigned long long value = 0;
            if (std::sscanf(line.c_str() + key_length + 1, "%llu", &value) == 1) {
                return static_cast<std::uint64_t>(value);
            }
        }
    }
    return 0;
}

std::uint64_t RssBytes() { return ProcStatusKb("VmRSS") * 1024ull; }
std::uint64_t PeakRssBytes() { return ProcStatusKb("VmHWM") * 1024ull; }

std::size_t CountFds() {
    std::error_code ec;
    std::size_t count = 0;
    fs::directory_iterator iterator("/proc/self/fd", ec);
    const fs::directory_iterator end;
    while (!ec && iterator != end) {
        ++count;
        iterator.increment(ec);
    }
    return count;
}

// ---------------------------------------------------------------------------
// 文件系统辅助
// ---------------------------------------------------------------------------

/// 创建唯一临时基目录（/tmp）；失败返回空路径。
fs::path MakeTempBase(const char* tag) {
    const fs::path base = fs::temp_directory_path() /
                          (std::string("bento_fb_audit_") + tag + "_" +
                           std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    if (ec) {
        return {};
    }
    return base;
}

void RemoveTree(const fs::path& dir) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec;
         it.increment(ec)) {
        std::error_code chmod_ec;
        fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, chmod_ec);
    }
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(dir, ec);
}

bool WriteText(const fs::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        return false;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

bool ReadText(const fs::path& path, std::string* text) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    text->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

std::uint64_t FileSize(const fs::path& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(info.st_size);
}

std::size_t CountPartFiles(const fs::path& dir) {
    std::error_code ec;
    std::size_t count = 0;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (it->path().filename().string().find("go2cpp-part") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

std::uint64_t TreeBytes(const fs::path& dir) {
    std::error_code ec;
    std::uint64_t total = 0;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec;
         it.increment(ec)) {
        std::error_code size_ec;
        if (it->is_regular_file(size_ec) && !size_ec) {
            total += it->file_size(size_ec);
        }
    }
    return total;
}

std::vector<std::string> SnapshotTree(const fs::path& dir) {
    std::vector<std::string> snapshot;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec;
         it.increment(ec)) {
        std::error_code relative_ec;
        std::string item = fs::relative(it->path(), dir, relative_ec).string();
        item += ":";
        item += std::to_string(FileSize(it->path()));
        snapshot.push_back(std::move(item));
    }
    std::sort(snapshot.begin(), snapshot.end());
    return snapshot;
}

// ---------------------------------------------------------------------------
// 大文件读写（1 MiB 块，按块号派生内容）
// ---------------------------------------------------------------------------

constexpr std::size_t kBlockBytes = 1u << 20;

void FillBlock(std::uint8_t* buffer, std::uint64_t block_index) {
    const std::uint64_t seed = block_index * 0x9e3779b97f4a7c15ull + 0x123456789abcdefull;
    std::uint64_t state = seed | 1ull;
    for (std::size_t index = 0; index < kBlockBytes; index += 8) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        std::memcpy(buffer + index, &state, sizeof(state));
    }
    std::memcpy(buffer, &block_index, sizeof(block_index));
}

bool WriteAll(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t result = ::write(fd, data + written, size - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool WritePattern(const fs::path& path, std::uint64_t bytes) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    std::vector<std::uint8_t> buffer(kBlockBytes);
    bool ok = true;
    for (std::uint64_t offset = 0; offset < bytes; offset += kBlockBytes) {
        FillBlock(buffer.data(), offset / kBlockBytes);
        const std::size_t chunk =
            static_cast<std::size_t>(std::min<std::uint64_t>(kBlockBytes, bytes - offset));
        if (!WriteAll(fd, buffer.data(), chunk)) {
            ok = false;
            break;
        }
    }
    ok = (::close(fd) == 0) && ok;
    return ok;
}

bool VerifyPattern(const fs::path& path, std::uint64_t bytes, std::string* reason) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *reason = "open 失败";
        return false;
    }
    std::vector<std::uint8_t> expected(kBlockBytes);
    std::vector<std::uint8_t> actual(kBlockBytes);
    std::uint64_t total = 0;
    bool ok = true;
    for (;;) {
        const ssize_t got = ::read(fd, actual.data(), actual.size());
        if (got == 0) {
            break;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            *reason = "read 失败";
            break;
        }
        const std::uint64_t block = total / kBlockBytes;
        const std::size_t within = static_cast<std::size_t>(total % kBlockBytes);
        if (within == 0) {
            FillBlock(expected.data(), block);
        }
        if (std::memcmp(expected.data() + within, actual.data(),
                        static_cast<std::size_t>(got)) != 0) {
            ok = false;
            *reason = "块内容不匹配，偏移 " + std::to_string(total);
            break;
        }
        total += static_cast<std::uint64_t>(got);
        if (total > bytes) {
            ok = false;
            *reason = "文件长度超过预期";
            break;
        }
    }
    ::close(fd);
    if (ok && total != bytes) {
        ok = false;
        *reason = "长度不符：期望 " + std::to_string(bytes) + "，实际 " +
                  std::to_string(total);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 1. 1 GiB 大文件传输
// ---------------------------------------------------------------------------

void RunLargeFileScenario(const fs::path& base, bool quick) {
    Section("场景 A：1 GiB 流式 Get / Put");
    const std::uint64_t size = quick ? (256ull << 20) : (1ull << 30);
    const fs::path root = base / "root";
    std::error_code ec;
    fs::create_directories(root, ec);
    A_CHECK(!ec);

    const fs::path source = root / "big.bin";
    const std::uint64_t write_start = NowUs();
    A_CHECK(WritePattern(source, size));
    const std::uint64_t write_us = NowUs() - write_start;
    Note("源文件生成：%.1f MiB，%.1f ms（%.0f MiB/s）\n", static_cast<double>(size) / 1048576.0,
         static_cast<double>(write_us) / 1000.0,
         static_cast<double>(size) / 1048576.0 / (static_cast<double>(write_us) / 1e6));

    const FileBrowser browser(root.string());
    A_CHECK(browser.valid());

    const std::uint64_t rss_before = RssBytes();
    const std::size_t fd_before = CountFds();
    const std::uint64_t tree_before = TreeBytes(root);

    const fs::path download = base / "download.bin";
    const std::uint64_t get_start = NowUs();
    const FileBrowserStatus get_status = browser.Get("big.bin", download.string());
    const std::uint64_t get_us = NowUs() - get_start;
    const std::size_t fd_after_get = CountFds();
    A_CHECK_STATUS(get_status, FileBrowserStatus::kOk);
    std::string verify_reason;
    A_CHECK(VerifyPattern(download, size, &verify_reason));
    if (!verify_reason.empty()) {
        Note("Get 校验失败：%s\n", verify_reason.c_str());
    }
    Note("Get：%.1f MiB，%.1f ms，%.0f MiB/s，RSS %.1f→峰值 %.1f MiB，fd %zu→%zu\n",
         static_cast<double>(size) / 1048576.0, static_cast<double>(get_us) / 1000.0,
         static_cast<double>(size) / 1048576.0 / (static_cast<double>(get_us) / 1e6),
         static_cast<double>(rss_before) / 1048576.0,
         static_cast<double>(PeakRssBytes()) / 1048576.0, fd_before, fd_after_get);

    const fs::path upload_target = root / "upload" / "big-copy.bin";
    const std::size_t fd_before_put = CountFds();
    const std::uint64_t put_start = NowUs();
    const FileBrowserStatus put_status = browser.Put("upload/big-copy.bin", download.string());
    const std::uint64_t put_us = NowUs() - put_start;
    const std::size_t fd_after_put = CountFds();
    A_CHECK_STATUS(put_status, FileBrowserStatus::kOk);
    verify_reason.clear();
    A_CHECK(VerifyPattern(upload_target, size, &verify_reason));
    if (!verify_reason.empty()) {
        Note("Put 校验失败：%s\n", verify_reason.c_str());
    }
    Note("Put：%.1f MiB，%.1f ms，%.0f MiB/s，fd %zu→%zu，根内临时文件残留 %zu\n",
         static_cast<double>(size) / 1048576.0, static_cast<double>(put_us) / 1000.0,
         static_cast<double>(size) / 1048576.0 / (static_cast<double>(put_us) / 1e6),
         fd_before_put, fd_after_put, CountPartFiles(root));
    A_CHECK_EQ(CountPartFiles(root), static_cast<std::size_t>(0));
    A_CHECK_EQ(CountPartFiles(base), static_cast<std::size_t>(0));

    // 上限判定：1 GiB 恰等于默认上限（允许），1 GiB + 1 字节必须快速拒绝。
    const fs::path huge = root / "huge.bin";
    const int huge_fd = ::open(huge.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    A_CHECK(huge_fd >= 0);
    if (huge_fd >= 0) {
        A_CHECK(::ftruncate(huge_fd, static_cast<off_t>(size + 1)) == 0);
        ::close(huge_fd);
    }
    FileBrowserOptions limit_options;
    limit_options.max_file_bytes = size;
    const FileBrowser limit_browser(root.string(), limit_options);
    const std::uint64_t too_large_start = NowUs();
    const FileBrowserStatus too_large =
        limit_browser.Get("huge.bin", (base / "too-large.bin").string());
    const std::uint64_t too_large_us = NowUs() - too_large_start;
    A_CHECK_STATUS(too_large, FileBrowserStatus::kTooLarge);
    A_CHECK(!fs::exists(base / "too-large.bin"));
    Note("超限拒绝：kTooLarge，%.3f ms（稀疏文件，不发生复制）\n",
         static_cast<double>(too_large_us) / 1000.0);

    Note("磁盘：源 %.1f MiB，下载副本 %.1f MiB，上传副本 %.1f MiB，根内合计 %.1f MiB\n",
         static_cast<double>(size) / 1048576.0,
         static_cast<double>(FileSize(download)) / 1048576.0,
         static_cast<double>(FileSize(upload_target)) / 1048576.0,
         static_cast<double>(TreeBytes(root)) / 1048576.0);

    std::error_code remove_ec;
    fs::remove(source, remove_ec);
    fs::remove(download, remove_ec);
    fs::remove(upload_target, remove_ec);
    fs::remove(huge, remove_ec);
    fs::remove(root / "upload", remove_ec);
    A_CHECK_EQ(CountPartFiles(root), static_cast<std::size_t>(0));
    const std::uint64_t tree_after = TreeBytes(root);
    Note("清理后根目录逻辑大小：%.1f MiB（清理前源 + 上传副本约 %.1f MiB）\n",
         static_cast<double>(tree_after) / 1048576.0,
         static_cast<double>(tree_before) / 1048576.0);
}

// ---------------------------------------------------------------------------
// 2. 10 万次 List/Stat 与小文件吞吐
// ---------------------------------------------------------------------------

struct BenchRow {
    const char* name;
    std::uint64_t operations;
    std::uint64_t microseconds;
};

void PrintBench(const BenchRow& row) {
    const double seconds = static_cast<double>(row.microseconds) / 1e6;
    Note("%-30s %8llu ops  %9.1f ms  %12.0f op/s\n", row.name,
         static_cast<unsigned long long>(row.operations),
         static_cast<double>(row.microseconds) / 1000.0,
         static_cast<double>(row.operations) / (seconds > 0.0 ? seconds : 1.0));
}

void RunMicroBench(const fs::path& base, bool quick) {
    Section("压测 B：大规模 List/Stat 与小文件 Get/Put");
    const fs::path root = base / "root";
    std::error_code ec;
    fs::create_directories(root / "d0" / "d1" / "d2", ec);
    A_CHECK(!ec);
    for (int index = 0; index < 32; ++index) {
        char name[32];
        std::snprintf(name, sizeof(name), "f%02d.bin", index);
        A_CHECK(WriteText(root / name, "0123456789abcdef"));
    }
    A_CHECK(WriteText(root / "d0" / "d1" / "d2" / "deep.bin", "deep"));
    A_CHECK(WriteText(root / "d0" / "d1" / "mid.bin", "mid"));

    const FileBrowser browser(root.string());
    A_CHECK(browser.valid());

    const std::size_t fd_before = CountFds();
    const std::uint64_t rss_before = RssBytes();
    const std::uint64_t iterations = quick ? 5000 : 100000;
    std::vector<FileEntry> entries;
    FileEntry entry;

    std::uint64_t start = NowUs();
    for (std::uint64_t index = 0; index < iterations; ++index) {
        if (browser.List("", &entries) != FileBrowserStatus::kOk) {
            A_CHECK(false);
            break;
        }
    }
    const std::uint64_t list_us = NowUs() - start;
    A_CHECK_EQ(entries.size(), static_cast<std::size_t>(33));
    PrintBench({"List(根, 33 项)", iterations, list_us});

    start = NowUs();
    for (std::uint64_t index = 0; index < iterations; ++index) {
        if (browser.Stat("f17.bin", &entry) != FileBrowserStatus::kOk) {
            A_CHECK(false);
            break;
        }
    }
    const std::uint64_t stat_us = NowUs() - start;
    PrintBench({"Stat(单段)", iterations, stat_us});

    const std::uint64_t deep_iterations = iterations / 2;
    start = NowUs();
    for (std::uint64_t index = 0; index < deep_iterations; ++index) {
        if (browser.Stat("d0/d1/d2/deep.bin", &entry) != FileBrowserStatus::kOk) {
            A_CHECK(false);
            break;
        }
    }
    const std::uint64_t deep_us = NowUs() - start;
    PrintBench({"Stat(4 段嵌套)", deep_iterations, deep_us});

    // 小文件 Get/Put（4 KiB，覆盖已有目标）——测路径解析 + 临时文件 + 发布。
    const fs::path local = base / "small.bin";
    A_CHECK(WriteText(local, std::string(4096, 'q')));
    fs::create_directories(root / "small", ec);
    A_CHECK(!ec);
    FileBrowserOptions overwrite_options;
    overwrite_options.overwrite = true;
    const FileBrowser overwrite_browser(root.string(), overwrite_options);
    const std::uint64_t small_iterations = quick ? 500 : 3000;
    start = NowUs();
    for (std::uint64_t index = 0; index < small_iterations; ++index) {
        if (overwrite_browser.Put("small/t.bin", local.string()) != FileBrowserStatus::kOk) {
            A_CHECK(false);
            break;
        }
    }
    const std::uint64_t put_us = NowUs() - start;
    PrintBench({"Put(4 KiB, 覆盖)", small_iterations, put_us});

    const fs::path out = base / "small-out.bin";
    start = NowUs();
    for (std::uint64_t index = 0; index < small_iterations; ++index) {
        if (overwrite_browser.Get("small/t.bin", out.string()) != FileBrowserStatus::kOk) {
            A_CHECK(false);
            break;
        }
    }
    const std::uint64_t get_us = NowUs() - start;
    PrintBench({"Get(4 KiB, 覆盖)", small_iterations, get_us});

    const std::size_t fd_after = CountFds();
    Note("fd：%zu→%zu（增量 %lld）；RSS：%.1f→%.1f MiB（峰值 %.1f MiB）\n", fd_before,
         fd_after, static_cast<long long>(fd_after) - static_cast<long long>(fd_before),
         static_cast<double>(rss_before) / 1048576.0,
         static_cast<double>(RssBytes()) / 1048576.0,
         static_cast<double>(PeakRssBytes()) / 1048576.0);
    A_CHECK(fd_after <= fd_before + 2);
    A_CHECK_EQ(CountPartFiles(root), static_cast<std::size_t>(0));
    A_CHECK_EQ(CountPartFiles(base), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// 3. 随机路径
// ---------------------------------------------------------------------------

bool ContainsControlByte(const std::string& path) {
    for (char ch : path) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f) {
            return true;
        }
    }
    return false;
}

/// 与模块文档一致的“必须拒绝”判定（用于随机用例的强断言）。
bool MustReject(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (path.size() > go2cpp::bento::app::kFileBrowserDefaultMaxPathBytes) {
        return true;
    }
    if (path.front() == '/' || path.back() == '/') {
        return true;
    }
    if (path.find('\0') != std::string::npos || ContainsControlByte(path)) {
        return true;
    }
    if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos) {
        return true;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string::npos ? path.size() : slash;
        if (end == start) {
            return true;
        }
        const std::string_view segment(path.data() + start, end - start);
        if (segment == "." || segment == "..") {
            return true;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return false;
}

std::string JoinParts(const std::vector<std::string>& parts, std::size_t count) {
    std::string joined;
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            joined.push_back('/');
        }
        joined += parts[index];
    }
    return joined;
}

void RunRandomPaths(const fs::path& base) {
    Section("随机 C：2500+ 随机路径（拒绝 / 不逃逸 / 不崩 / 无残留）");
    const fs::path outside = base / "outside";
    const fs::path root = base / "root";
    const fs::path local_dir = base / "local";
    std::error_code ec;
    fs::create_directories(root / "d0" / "d1", ec);
    fs::create_directories(outside, ec);
    fs::create_directories(local_dir, ec);
    A_CHECK(!ec);
    A_CHECK(WriteText(root / "f000.txt", "zero"));
    A_CHECK(WriteText(root / "f001.txt", "one"));
    A_CHECK(WriteText(root / "d0" / "f002.txt", "two"));
    A_CHECK(WriteText(outside / "secret.txt", "outside-secret"));
    A_CHECK(::symlink("f000.txt", (root / "link_in").c_str()) == 0);
    A_CHECK(::symlink((outside / "secret.txt").c_str(), (root / "link_out").c_str()) == 0);
    A_CHECK(::symlink(outside.c_str(), (root / "link_dir").c_str()) == 0);
    A_CHECK(::symlink((outside / "secret.txt").c_str(), (root / "chain_a").c_str()) == 0);
    A_CHECK(::symlink("chain_a", (root / "chain_b").c_str()) == 0);
    A_CHECK(::symlink("loop_b", (root / "loop_a").c_str()) == 0);
    A_CHECK(::symlink("loop_a", (root / "loop_b").c_str()) == 0);
    A_CHECK(::mkfifo((root / "pipe.fifo").c_str(), 0644) == 0);

    const std::vector<std::string> outside_before = SnapshotTree(outside);
    const std::vector<std::string> root_before = SnapshotTree(root);

    const FileBrowser browser(root.string());
    A_CHECK(browser.valid());
    const std::string local = (local_dir / "out.bin").string();
    A_CHECK(WriteText(local_dir / "upload.bin", "upload-source"));

    std::mt19937 rng(20261029u);
    const std::vector<std::string> tokens = {
        "f000.txt", "f001.txt", "d0", "f002.txt", "link_in", "link_out", "link_dir",
        "chain_b",  "loop_a",   "nope", "..",       ".",      "",         "x"};
    const std::string long_component(300, 'x');
    const std::string unicode = "数据-ä.txt";
    std::vector<std::string> cases;
    cases.reserve(2600);
    const std::vector<std::string> fixed = {
        "..",
        "../outside/secret.txt",
        "../../etc/passwd",
        "/etc/passwd",
        "//etc/passwd",
        "./f000.txt",
        "d0/../f000.txt",
        "d0/./f001.txt",
        "d0//f002.txt",
        "d0/",
        "/",
        "",
        ".",
        "..\\..\\etc\\passwd",
        "f000.txt\\",
        "C:/windows/win.ini",
        "C:\\windows\\win.ini",
        "\\\\?\\C:\\windows",
        "\\\\server\\share",
        "f000.txt:stream",
        "f000.txt\n",
        "f000.txt\r",
        "f000.txt\t",
        std::string("f000.txt\0hidden", 16),
        std::string(1024, 'a'),
        std::string(1025, 'a'),
        long_component,
        std::string(255, 'b'),
        std::string(256, 'b'),
        unicode,
        " ",
        "   ",
        "\t",
        "~",
        "$HOME",
        "${HOME}",
        "*",
        "?",
        "[a-z]",
        "%2e%2e",
        "%2e%2e%2fetc%2fpasswd",
        "....//....//etc/passwd",
        "..../",
        ".. /",
        "../",
        "link_dir/secret.txt",
        "link_dir/",
        "link_out",
        "link_in",
        "chain_b",
        "loop_a",
        "loop_b",
        "pipe.fifo",
        "d0/d1",
        "d0/d1/",
    };
    for (const std::string& item : fixed) {
        cases.push_back(item);
        cases.push_back(item);  // 覆盖“首次”和“重复调用”两种状态
    }
    for (int iteration = 0; iteration < 2400; ++iteration) {
        std::string path;
        switch (rng() % 10) {
            case 0:
                path = "/" + JoinParts(tokens, 1 + rng() % 3);
                break;
            case 1:
                path = JoinParts(tokens, 1 + rng() % 3) + "/../" +
                       JoinParts(tokens, 1 + rng() % 2);
                break;
            case 2: {
                const std::size_t count = 1 + rng() % 3;
                for (std::size_t index = 0; index < count; ++index) {
                    if (index != 0) {
                        path.push_back('/');
                    }
                    path += tokens[rng() % tokens.size()];
                    if (rng() % 3 == 0) {
                        path.push_back('/');
                    }
                }
                break;
            }
            case 3:
                path = JoinParts(tokens, 1 + rng() % 3) + "\\" +
                       JoinParts(tokens, 1 + rng() % 2);
                break;
            case 4:
                path = JoinParts(tokens, 1 + rng() % 3) + ":x";
                break;
            case 5:
                path = JoinParts(tokens, 1 + rng() % 3);
                path.append(rng() % 2 == 0 ? "\n" : "\x01");
                break;
            case 6: {
                const std::size_t length = 1000 + rng() % 80;
                path.assign(length, static_cast<char>('a' + rng() % 26));
                break;
            }
            case 7: {
                path = JoinParts(tokens, 1 + rng() % 2) + "/";
                path += std::string(1 + rng() % 300, static_cast<char>('a' + rng() % 26));
                break;
            }
            case 8:
                path = JoinParts(tokens, 1 + rng() % 3);
                path += rng() % 2 == 0 ? "//" : "/./";
                path += JoinParts(tokens, 1 + rng() % 2);
                break;
            default:
                path = std::string(rng() % 4, '/') + JoinParts(tokens, 1 + rng() % 3);
                break;
        }
        cases.push_back(std::move(path));
    }

    std::array<std::uint64_t, 10> status_counts{};
    std::uint64_t rejected = 0;
    std::uint64_t get_ok = 0;
    std::uint64_t put_ok = 0;
    std::uint64_t list_ok = 0;
    std::uint64_t stat_ok = 0;
    std::vector<FileEntry> entries;
    FileEntry entry;

    for (const std::string& path : cases) {
        const bool must_reject = MustReject(path);
        std::error_code remove_ec;
        fs::remove(fs::path(local), remove_ec);

        const FileBrowserStatus list_status = browser.List(path, &entries);
        const FileBrowserStatus stat_status = browser.Stat(path, &entry);
        const FileBrowserStatus get_status = browser.Get(path, local);
        const FileBrowserStatus put_status =
            browser.Put(path, (local_dir / "upload.bin").string());

        const FileBrowserStatus statuses[] = {list_status, stat_status, get_status, put_status};
        for (FileBrowserStatus status : statuses) {
            ++status_counts[static_cast<std::size_t>(status)];
        }
        if (must_reject) {
            ++rejected;
            A_CHECK_STATUS(list_status, FileBrowserStatus::kPathRejected);
            A_CHECK_STATUS(stat_status, FileBrowserStatus::kPathRejected);
            A_CHECK_STATUS(get_status, FileBrowserStatus::kPathRejected);
            A_CHECK_STATUS(put_status, FileBrowserStatus::kPathRejected);
        }
        if (list_status == FileBrowserStatus::kOk) {
            ++list_ok;
        }
        if (stat_status == FileBrowserStatus::kOk) {
            ++stat_ok;
        }
        if (get_status == FileBrowserStatus::kOk) {
            ++get_ok;
            const std::uint64_t source_size = FileSize(root / path);
            A_CHECK_EQ(FileSize(fs::path(local)), source_size);
        } else {
            A_CHECK(!fs::exists(fs::path(local)));
        }
        if (put_status == FileBrowserStatus::kOk) {
            ++put_ok;
            const fs::path uploaded = root / path;
            std::error_code canonical_ec;
            const fs::path canonical = fs::weakly_canonical(uploaded, canonical_ec);
            A_CHECK(!canonical_ec);
            const std::string canonical_text = canonical.string();
            A_CHECK(canonical_text.rfind(root.string(), 0) == 0);
            A_CHECK_EQ(FileSize(uploaded), static_cast<std::uint64_t>(13));
        }
    }

    std::error_code remove_ec;
    fs::remove(fs::path(local), remove_ec);

    Note("用例 %zu：强制拒绝 %llu；List ok %llu，Stat ok %llu，Get ok %llu，Put ok %llu\n",
         cases.size(), static_cast<unsigned long long>(rejected),
         static_cast<unsigned long long>(list_ok),
         static_cast<unsigned long long>(stat_ok),
         static_cast<unsigned long long>(get_ok),
         static_cast<unsigned long long>(put_ok));
    Note("状态分布：ok=%llu rejected=%llu not_found=%llu not_regular=%llu too_large=%llu "
         "io_error=%llu perm=%llu exists=%llu too_many=%llu invalid=%llu\n",
         static_cast<unsigned long long>(status_counts[0]),
         static_cast<unsigned long long>(status_counts[2]),
         static_cast<unsigned long long>(status_counts[3]),
         static_cast<unsigned long long>(status_counts[4]),
         static_cast<unsigned long long>(status_counts[5]),
         static_cast<unsigned long long>(status_counts[6]),
         static_cast<unsigned long long>(status_counts[7]),
         static_cast<unsigned long long>(status_counts[8]),
         static_cast<unsigned long long>(status_counts[9]),
         static_cast<unsigned long long>(status_counts[1]));

    const std::vector<std::string> outside_after = SnapshotTree(outside);
    A_CHECK(outside_after == outside_before);
    const std::vector<std::string> root_after = SnapshotTree(root);
    for (const std::string& preserved : root_before) {
        A_CHECK(std::find(root_after.begin(), root_after.end(), preserved) !=
                root_after.end());
    }
    A_CHECK_EQ(CountPartFiles(root), static_cast<std::size_t>(0));
    A_CHECK_EQ(CountPartFiles(local_dir), static_cast<std::size_t>(0));

    // NUL 截断：旧实现会把 "<base>/nul-target\0evil" 截断成 "<base>/nul-target" 并静默写盘。
    const std::string nul_local = (base / "nul-target").string() + std::string("\0evil", 5);
    const FileBrowserStatus nul_get = browser.Get("f000.txt", nul_local);
    const FileBrowserStatus nul_put = browser.Put("nul.bin", nul_local);
    A_CHECK_STATUS(nul_get, FileBrowserStatus::kInvalidArgument);
    A_CHECK_STATUS(nul_put, FileBrowserStatus::kInvalidArgument);
    A_CHECK(!fs::exists(base / "nul-target"));
    A_CHECK(!fs::exists(base / "nul.bin"));
    Note("local_path 含 NUL：Get=%s，Put=%s\n", StatusName(nul_get), StatusName(nul_put));
    A_CHECK_EQ(CountPartFiles(root), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// 4a. 路径逃逸 / 符号链接场景
// ---------------------------------------------------------------------------

void RunEscapeScenario(const fs::path& base) {
    Section("场景 D：路径逃逸（../、绝对、符号链接链、覆盖语义、根替换）");
    const fs::path root = base / "root";
    const fs::path outside = base / "outside";
    const fs::path local_dir = base / "local";
    std::error_code ec;
    fs::create_directories(root, ec);
    fs::create_directories(outside, ec);
    fs::create_directories(local_dir, ec);
    A_CHECK(!ec);
    A_CHECK(WriteText(root / "a.txt", "alpha"));
    A_CHECK(WriteText(outside / "secret.txt", "outside-secret"));
    A_CHECK(::symlink("a.txt", (root / "link_in").c_str()) == 0);
    A_CHECK(::symlink((outside / "secret.txt").c_str(), (root / "link_out").c_str()) == 0);
    A_CHECK(::symlink(outside.c_str(), (root / "link_dir").c_str()) == 0);
    A_CHECK(::symlink("link_out", (root / "link_chain").c_str()) == 0);
    A_CHECK(::symlink("loop_b", (root / "loop_a").c_str()) == 0);
    A_CHECK(::symlink("loop_a", (root / "loop_b").c_str()) == 0);

    const FileBrowser browser(root.string());
    FileBrowserOptions follow_options;
    follow_options.follow_symlinks = true;
    const FileBrowser follower(root.string(), follow_options);
    const std::string local = (local_dir / "out.bin").string();
    std::vector<FileEntry> entries;
    FileEntry entry;

    A_CHECK(WriteText(local_dir / "up.bin", "up"));
    const std::vector<std::string> escapes = {
        "../outside/secret.txt", "/etc/passwd", "link_out", "link_dir/secret.txt",
        "link_chain",            "loop_a",       "a/../../outside/secret.txt"};
    for (const std::string& path : escapes) {
        A_CHECK_STATUS(browser.List(path, &entries), FileBrowserStatus::kPathRejected);
        A_CHECK_STATUS(browser.Get(path, local), FileBrowserStatus::kPathRejected);
        A_CHECK_STATUS(browser.Put(path, (local_dir / "up.bin").string()),
                       FileBrowserStatus::kPathRejected);
        // 单段符号链接的 Stat 需要能报告链接本身（文档语义）；多段路径拒绝。
        if (path == "link_out" || path == "link_chain" || path == "loop_a") {
            A_CHECK_STATUS(browser.Stat(path, &entry), FileBrowserStatus::kOk);
            A_CHECK_EQ(entry.type, FileEntryType::kSymlink);
        } else {
            A_CHECK_STATUS(browser.Stat(path, &entry), FileBrowserStatus::kPathRejected);
        }
    }
    A_CHECK_STATUS(browser.List("link_in", &entries), FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(browser.Get("link_in", local), FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(browser.Stat("link_in", &entry), FileBrowserStatus::kOk);
    A_CHECK_EQ(entry.type, FileEntryType::kSymlink);

    // follow=true：仍以真实路径为判据。
    A_CHECK_STATUS(follower.Get("link_out", local), FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(follower.List("link_dir", &entries), FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(follower.List("link_in", &entries), FileBrowserStatus::kNotRegular);
    A_CHECK_STATUS(follower.Stat("link_out", &entry), FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(follower.Stat("link_in", &entry), FileBrowserStatus::kOk);
    A_CHECK_EQ(entry.type, FileEntryType::kFile);
    A_CHECK_STATUS(follower.Get("link_in", local), FileBrowserStatus::kOk);
    std::string text;
    A_CHECK(ReadText(local, &text));
    A_CHECK_EQ(text, std::string("alpha"));
    fs::remove(local, ec);
    A_CHECK_STATUS(follower.Get("loop_a", local), FileBrowserStatus::kPathRejected);

    // overwrite=true 时 Put 到“指向外部”的符号链接：沙箱规则拒绝链接路径本身。
    A_CHECK(WriteText(local_dir / "upload.bin", "pwn-content"));
    A_CHECK(::symlink((outside / "secret.txt").c_str(), (root / "evil_link").c_str()) == 0);
    std::string secret_before;
    A_CHECK(ReadText(outside / "secret.txt", &secret_before));
    FileBrowserOptions overwrite_options;
    overwrite_options.overwrite = true;
    const FileBrowser overwriter(root.string(), overwrite_options);
    A_CHECK_STATUS(overwriter.Put("evil_link", (local_dir / "upload.bin").string()),
                   FileBrowserStatus::kPathRejected);
    std::string outside_after;
    A_CHECK(ReadText(outside / "secret.txt", &outside_after));
    A_CHECK_EQ(outside_after, secret_before);
    A_CHECK(fs::is_symlink(root / "evil_link"));

    // overwrite=true 时 Get 到本地符号链接：同样替换链接而不是写穿。
    const fs::path victim = base / "victim.txt";
    A_CHECK(WriteText(victim, "victim"));
    A_CHECK(::symlink(victim.c_str(), (local_dir / "local_link").c_str()) == 0);
    A_CHECK_STATUS(overwriter.Get("a.txt", (local_dir / "local_link").string()),
                   FileBrowserStatus::kOk);
    A_CHECK(ReadText(victim, &text));
    A_CHECK_EQ(text, std::string("victim"));
    A_CHECK(!fs::is_symlink(local_dir / "local_link"));
    A_CHECK(ReadText(local_dir / "local_link", &text));
    A_CHECK_EQ(text, std::string("alpha"));

    const std::vector<std::string> outside_after_snapshot = SnapshotTree(outside);
    Note("逃逸用例：全部拒绝；follow=true 仅放行根内链接；覆盖语义未写穿（外部快照 %zu 项）\n",
         outside_after_snapshot.size());

    // 根目录被替换成符号链接（构造后的 TOCTOU）：List("") 曾直接返回 m_root。
    fs::remove_all(root, ec);
    A_CHECK(::symlink(outside.c_str(), root.c_str()) == 0);
    const FileBrowserStatus list_root = browser.List("", &entries);
    A_CHECK_STATUS(list_root, FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(browser.List("secret.txt", &entries), FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(browser.Get("secret.txt", local), FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(browser.Put("pwned.txt", (local_dir / "upload.bin").string()),
                   FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(browser.Stat("", &entry), FileBrowserStatus::kPathRejected);
    A_CHECK(!fs::exists(outside / "pwned.txt"));
    Note("根被替换为符号链接：List(\"\")=%s\n", StatusName(list_root));

    // 根目录被删除：不得复活，也不得把写入落到别处。
    fs::remove(root, ec);
    A_CHECK_STATUS(browser.List("", &entries), FileBrowserStatus::kNotFound);
    A_CHECK_STATUS(browser.List("a.txt", &entries), FileBrowserStatus::kNotFound);
    A_CHECK_STATUS(browser.Stat("a.txt", &entry), FileBrowserStatus::kNotFound);
    A_CHECK_STATUS(browser.Get("a.txt", local), FileBrowserStatus::kNotFound);
    A_CHECK_STATUS(browser.Put("resurrect.txt", (local_dir / "upload.bin").string()),
                   FileBrowserStatus::kNotFound);
    A_CHECK(!fs::exists(root));
    A_CHECK(!fs::exists(local));
    Note("根被删除：List/Stat/Get/Put 均 kNotFound，根未被 Put 重建\n");
}

// ---------------------------------------------------------------------------
// 4b. 8 线程并发
// ---------------------------------------------------------------------------

void RunConcurrencyScenario(const fs::path& base) {
    Section("场景 E：8 线程同目录并发 Get/Put");
    const fs::path root = base / "root";
    const fs::path local_dir = base / "local";
    std::error_code ec;
    fs::create_directories(root / "conc", ec);
    fs::create_directories(local_dir, ec);
    A_CHECK(!ec);

    const FileBrowser browser(root.string());
    A_CHECK(browser.valid());

    constexpr int kThreads = 8;
    constexpr int kIterations = 200;
    std::atomic<int> failures{0};
    std::atomic<int> operations{0};
    const std::size_t fd_before = CountFds();
    const std::uint64_t start = NowUs();

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&, thread_index] {
            const std::string payload =
                "thread-" + std::to_string(thread_index) + "-" + std::string(2048, 'p');
            const fs::path source = local_dir / ("src_" + std::to_string(thread_index));
            if (!WriteText(source, payload)) {
                ++failures;
                return;
            }
            const fs::path download = local_dir / ("dst_" + std::to_string(thread_index));
            FileBrowserOptions overwrite_options;
            overwrite_options.overwrite = true;
            const FileBrowser overwrite_browser(root.string(), overwrite_options);
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                const std::string name = "conc/t" + std::to_string(thread_index) + "_" +
                                         std::to_string(iteration) + ".bin";
                if (overwrite_browser.Put(name, source.string()) != FileBrowserStatus::kOk) {
                    ++failures;
                    continue;
                }
                if (overwrite_browser.Get(name, download.string()) != FileBrowserStatus::kOk) {
                    ++failures;
                    continue;
                }
                std::string actual;
                if (!ReadText(download, &actual) || actual != payload) {
                    ++failures;
                }
                ++operations;
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const std::uint64_t elapsed_us = NowUs() - start;
    const std::size_t fd_after = CountFds();
    Note("独立文件：%d 线程 × %d 轮，%d 次 Put+Get，%.1f ms，%.0f op/s，失败 %d\n", kThreads,
         kIterations, operations.load(), static_cast<double>(elapsed_us) / 1000.0,
         static_cast<double>(operations.load()) / (static_cast<double>(elapsed_us) / 1e6),
         failures.load());
    A_CHECK_EQ(failures.load(), 0);
    A_CHECK(fd_after <= fd_before + 2);
    A_CHECK_EQ(CountPartFiles(root), static_cast<std::size_t>(0));

    // 同一目标 + overwrite=false：只允许一个线程发布成功。
    const fs::path same_local = local_dir / "same_src.bin";
    A_CHECK(WriteText(same_local, "same-target"));
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> ok_count{0};
    std::atomic<int> exists_count{0};
    std::atomic<int> other_count{0};
    workers.clear();
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&] {
            ++ready;
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int round = 0; round < 30; ++round) {
                const FileBrowserStatus status = browser.Put(
                    "conc/same_" + std::to_string(round) + ".bin", same_local.string());
                if (status == FileBrowserStatus::kOk) {
                    ++ok_count;
                } else if (status == FileBrowserStatus::kAlreadyExists) {
                    ++exists_count;
                } else {
                    ++other_count;
                }
            }
        });
    }
    while (ready.load() < kThreads) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }
    Note("同一目标：%d 线程 × 30 轮 overwrite=false，ok=%d，exists=%d，其它=%d\n", kThreads,
         ok_count.load(), exists_count.load(), other_count.load());
    A_CHECK_EQ(ok_count.load(), 30);
    A_CHECK_EQ(exists_count.load(), 30 * (kThreads - 1));
    A_CHECK_EQ(other_count.load(), 0);
    A_CHECK_EQ(FileSize(root / "conc" / "same_29.bin"), static_cast<std::uint64_t>(11));
    A_CHECK_EQ(CountPartFiles(root), static_cast<std::size_t>(0));

    // 目录内容变化时的 List：不得崩溃，不得返回错误以外的结果。
    std::atomic<bool> stop{false};
    std::atomic<int> list_failures{0};
    std::thread mutator([&] {
        for (int index = 0; index < 300 && !stop.load(); ++index) {
            const fs::path file = root / "conc" / ("churn_" + std::to_string(index));
            WriteText(file, "x");
            std::error_code remove_ec;
            fs::remove(file, remove_ec);
        }
    });
    for (int index = 0; index < 300; ++index) {
        std::vector<FileEntry> churn_entries;
        const FileBrowserStatus status = browser.List("conc", &churn_entries);
        if (status != FileBrowserStatus::kOk) {
            ++list_failures;
        }
    }
    stop.store(true);
    mutator.join();
    Note("目录竞态 List：300 次，异常状态 %d 次（只允许 kOk）\n", list_failures.load());
    A_CHECK_EQ(list_failures.load(), 0);
    A_CHECK_EQ(CountPartFiles(root), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// 4c. 权限（root 时降权到 nobody）
// ---------------------------------------------------------------------------

struct PrivPaths {
    fs::path root;
    fs::path local_rw;
    fs::path local_ro;
};

int RunPrivilegeChecks(const PrivPaths& paths, int* checks_out) {
    const int checks_before = g_checks;
    const int failures_before = g_failures;
    const FileBrowser browser(paths.root.string());
    std::vector<FileEntry> entries;
    FileEntry entry;
    const std::string readable_local = (paths.local_rw / "readable.out").string();
    const std::string ro_local = (paths.local_ro / "ro.out").string();
    const std::string upload = (paths.local_rw / "upload.bin").string();

    A_CHECK_STATUS(browser.List("locked", &entries), FileBrowserStatus::kPermissionDenied);
    A_CHECK_STATUS(browser.Get("secret.bin", readable_local),
                   FileBrowserStatus::kPermissionDenied);
    A_CHECK_STATUS(browser.Put("ro/new.bin", upload), FileBrowserStatus::kPermissionDenied);
    A_CHECK_STATUS(browser.Get("readable.bin", ro_local),
                   FileBrowserStatus::kPermissionDenied);
    A_CHECK_STATUS(browser.List("", &entries), FileBrowserStatus::kOk);
    A_CHECK_STATUS(browser.Get("readable.bin", readable_local), FileBrowserStatus::kOk);
    A_CHECK_STATUS(browser.Put("rw/new.bin", upload), FileBrowserStatus::kOk);
    A_CHECK_STATUS(browser.Stat("secret.bin", &entry), FileBrowserStatus::kOk);
    A_CHECK_EQ(entry.size, static_cast<std::uint64_t>(6));
    A_CHECK_STATUS(browser.Stat("locked", &entry), FileBrowserStatus::kOk);
    A_CHECK_EQ(entry.type, FileEntryType::kDirectory);
    A_CHECK_STATUS(browser.List("rw", &entries), FileBrowserStatus::kOk);
    A_CHECK_STATUS(browser.List("missing", &entries), FileBrowserStatus::kNotFound);
    *checks_out = g_checks - checks_before;
    return g_failures - failures_before;
}

void RunPermissionScenario(const fs::path& base) {
    Section("场景 F：权限拒绝（chmod 000 / 只读根；root 时降权到 nobody）");
    const fs::path root = base / "root";
    const fs::path local_rw = base / "local_rw";
    const fs::path local_ro = base / "local_ro";
    std::error_code ec;
    fs::create_directories(root / "locked", ec);
    fs::create_directories(root / "ro", ec);
    fs::create_directories(root / "rw", ec);
    fs::create_directories(local_rw, ec);
    fs::create_directories(local_ro, ec);
    A_CHECK(!ec);
    A_CHECK(WriteText(root / "readable.bin", "read"));
    A_CHECK(WriteText(root / "secret.bin", "secret"));
    A_CHECK(WriteText(root / "locked" / "hidden.bin", "hidden"));
    A_CHECK(WriteText(root / "ro" / "kept.bin", "kept"));
    A_CHECK(WriteText(local_rw / "upload.bin", "upload"));
    A_CHECK_EQ(::chmod((root / "locked").c_str(), 0000), 0);
    A_CHECK_EQ(::chmod((root / "secret.bin").c_str(), 0000), 0);
    A_CHECK_EQ(::chmod((root / "ro").c_str(), 0555), 0);
    A_CHECK_EQ(::chmod((local_ro).c_str(), 0555), 0);
    A_CHECK_EQ(::chmod((local_rw).c_str(), 0777), 0);
    A_CHECK_EQ(::chmod((root / "rw").c_str(), 0777), 0);

    const PrivPaths paths{root, local_rw, local_ro};
    int child_checks = 0;
    int child_failures = 0;
    if (::geteuid() == 0) {
        int descriptors[2] = {-1, -1};
        A_CHECK(::pipe(descriptors) == 0);
        const pid_t pid = ::fork();
        A_CHECK(pid >= 0);
        if (pid == 0) {
            ::close(descriptors[0]);
            const bool dropped = ::setgroups(0, nullptr) == 0 && ::setgid(65534) == 0 &&
                                 ::setuid(65534) == 0;
            int child_result[2] = {0, 0};
            if (dropped) {
                int executed = 0;
                child_result[0] = RunPrivilegeChecks(paths, &executed);
                child_result[1] = executed;
            } else {
                child_result[0] = 99;
            }
            const ssize_t ignored =
                ::write(descriptors[1], child_result, sizeof(child_result));
            (void)ignored;
            ::close(descriptors[1]);
            std::fflush(stdout);
            std::fflush(stderr);
            ::_exit(child_result[0] == 0 ? 0 : 1);
        }
        ::close(descriptors[1]);
        int child_status = 0;
        A_CHECK(::waitpid(pid, &child_status, 0) == pid);
        int child_result[2] = {0, 0};
        const ssize_t got = ::read(descriptors[0], child_result, sizeof(child_result));
        ::close(descriptors[0]);
        A_CHECK_EQ(got, static_cast<ssize_t>(sizeof(child_result)));
        A_CHECK(WIFEXITED(child_status));
        A_CHECK_EQ(WEXITSTATUS(child_status), 0);
        child_failures = child_result[0];
        child_checks = child_result[1];
        Note("以 root 运行：子进程 setuid(65534) 后执行 %d 项检查，失败 %d\n", child_checks,
             child_failures);
    } else {
        int executed = 0;
        child_failures = RunPrivilegeChecks(paths, &executed);
        child_checks = executed;
        Note("以 uid=%d 运行：%d 项权限检查，失败 %d\n", static_cast<int>(::geteuid()),
             child_checks, child_failures);
    }
    A_CHECK_EQ(child_failures, 0);
    A_CHECK(child_checks >= 11);

    A_CHECK_EQ(::chmod((root / "locked").c_str(), 0755), 0);
    A_CHECK_EQ(::chmod((root / "secret.bin").c_str(), 0644), 0);
    A_CHECK_EQ(::chmod((root / "ro").c_str(), 0755), 0);
    A_CHECK_EQ(::chmod((local_ro).c_str(), 0755), 0);
    A_CHECK(!fs::exists(root / "ro" / "new.bin"));
    std::error_code remove_ec;
    fs::remove(local_rw / "readable.out", remove_ec);
    RemoveTree(base);
}

// ---------------------------------------------------------------------------
// 4d. 沙箱根不存在 / 只读 / 生命周期
// ---------------------------------------------------------------------------

void RunRootLifecycleScenario(const fs::path& base) {
    Section("场景 G：沙箱根不存在 / 只读 / 构造后删除");
    std::error_code ec;
    fs::create_directories(base, ec);
    A_CHECK(!ec);
    const fs::path missing = base / "missing-root";
    const std::string local = (base / "source.bin").string();
    A_CHECK(WriteText(fs::path(local), "source"));
    const FileBrowser invalid(missing.string());
    A_CHECK(!invalid.valid());
    std::vector<FileEntry> entries;
    FileEntry entry;
    A_CHECK_STATUS(invalid.List("", &entries), FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(invalid.Stat("a", &entry), FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(invalid.Get("a", local), FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(invalid.Put("a", local), FileBrowserStatus::kPathRejected);

    const fs::path file_root = base / "file-root";
    A_CHECK(WriteText(file_root, "not-a-dir"));
    const FileBrowser not_dir(file_root.string());
    A_CHECK(!not_dir.valid());

    const fs::path root = base / "root";
    fs::create_directories(root, ec);
    A_CHECK(!ec);
    A_CHECK(WriteText(root / "a.txt", "a"));
    {
        const FileBrowser browser(root.string());
        A_CHECK(browser.valid());
        fs::remove_all(root, ec);
        A_CHECK_STATUS(browser.List("", &entries), FileBrowserStatus::kNotFound);
        A_CHECK_STATUS(browser.List("a.txt", &entries), FileBrowserStatus::kNotFound);
        A_CHECK_STATUS(browser.Get("a.txt", local), FileBrowserStatus::kNotFound);
        A_CHECK_STATUS(browser.Put("new.txt", local), FileBrowserStatus::kNotFound);
        A_CHECK(!fs::exists(root));
    }
    Note("根不存在：构造无效 → kPathRejected；构造后删除 → kNotFound 且不被 Put 重建\n");
}

// ---------------------------------------------------------------------------
// 5. 边界 / 排序 / 特殊文件
// ---------------------------------------------------------------------------

void RunEdgeScenario(const fs::path& base) {
    Section("边界 H：排序、空文件、Unicode、NAME_MAX、路径长度、FIFO");
    const fs::path root = base / "root";
    std::error_code ec;
    fs::create_directories(root, ec);
    A_CHECK(!ec);

    // 排序：故意按逆序创建，断言输出严格升序且与期望一致。
    const std::vector<std::string> names = {"n19", "n03", "n11", "n00", "n07",
                                            "n15", "n02", "n18", "n06", "n10"};
    for (const std::string& name : names) {
        A_CHECK(WriteText(root / name, "x"));
    }
    A_CHECK(WriteText(root / "文件-ä.txt", "unicode"));
    const FileBrowser browser(root.string());
    std::vector<FileEntry> entries;
    A_CHECK_STATUS(browser.List("", &entries), FileBrowserStatus::kOk);
    A_CHECK_EQ(entries.size(), names.size() + 1);
    bool sorted = true;
    for (std::size_t index = 1; index < entries.size(); ++index) {
        if (!(entries[index - 1].name < entries[index].name)) {
            sorted = false;
        }
    }
    A_CHECK(sorted);
    if (!entries.empty()) {
        A_CHECK_EQ(entries.front().name, std::string("n00"));
        A_CHECK_EQ(entries.back().name, std::string("文件-ä.txt"));
    }

    // 空文件与 Unicode 往返。
    A_CHECK(WriteText(base / "empty.bin", ""));
    A_CHECK_STATUS(browser.Put("sub/empty.bin", (base / "empty.bin").string()),
                   FileBrowserStatus::kOk);
    A_CHECK_EQ(FileSize(root / "sub" / "empty.bin"), static_cast<std::uint64_t>(0));
    const fs::path unicode_target = base / "unicode.out";
    A_CHECK_STATUS(browser.Get("文件-ä.txt", unicode_target.string()), FileBrowserStatus::kOk);
    std::string text;
    A_CHECK(ReadText(unicode_target, &text));
    A_CHECK_EQ(text, std::string("unicode"));

    FileEntry entry;
    // NAME_MAX：255 字节组件不存在 → kNotFound；256 → kPathRejected。
    const std::string name_255(255, 'c');
    const std::string name_256(256, 'c');
    A_CHECK_STATUS(browser.Stat(name_255, &entry), FileBrowserStatus::kNotFound);
    A_CHECK_STATUS(browser.Stat(name_256, &entry), FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(browser.List(name_256, &entries), FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(browser.Get(name_256, (base / "n256.out").string()),
                   FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(browser.Put(name_256, (base / "empty.bin").string()),
                   FileBrowserStatus::kPathRejected);

    // 240 字节长名：临时名若拼接目标名会超过 NAME_MAX（255），本用例回归该缺陷。
    const std::string name_240(240, 'e');
    A_CHECK(WriteText(root / name_240, "long"));
    A_CHECK_STATUS(browser.Stat(name_240, &entry), FileBrowserStatus::kOk);
    const fs::path long_out = base / name_240;
    A_CHECK_STATUS(browser.Get(name_240, long_out.string()), FileBrowserStatus::kOk);
    A_CHECK_EQ(FileSize(long_out), static_cast<std::uint64_t>(4));
    A_CHECK_STATUS(browser.Put("long/" + name_240, long_out.string()),
                   FileBrowserStatus::kOk);
    A_CHECK_EQ(FileSize(root / "long" / name_240), static_cast<std::uint64_t>(4));

    // 路径长度边界：1024 字节合法（不存在 → kNotFound），1025 拒绝。
    std::string path_1024;
    for (int index = 0; index < 16; ++index) {
        if (index != 0) {
            path_1024.push_back('/');
        }
        path_1024.append(index == 15 ? 64 : 63, 'd');
    }
    A_CHECK_EQ(path_1024.size(), static_cast<std::size_t>(1024));
    A_CHECK_STATUS(browser.List(path_1024, &entries), FileBrowserStatus::kNotFound);
    std::string path_1025 = path_1024;
    path_1025.push_back('d');
    A_CHECK_EQ(path_1025.size(), static_cast<std::size_t>(1025));
    A_CHECK_STATUS(browser.List(path_1025, &entries), FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(browser.Get(path_1025, (base / "p1025.out").string()),
                   FileBrowserStatus::kPathRejected);

    // FIFO：List 报告 other；Get/List 目标是 FIFO → kNotRegular。
    A_CHECK(::mkfifo((root / "pipe").c_str(), 0644) == 0);
    A_CHECK_STATUS(browser.List("pipe", &entries), FileBrowserStatus::kNotRegular);
    A_CHECK_STATUS(browser.Get("pipe", (base / "pipe.out").string()),
                   FileBrowserStatus::kNotRegular);
    A_CHECK_STATUS(browser.Stat("pipe", &entry), FileBrowserStatus::kOk);
    A_CHECK_EQ(entry.type, FileEntryType::kOther);
    A_CHECK_STATUS(browser.List("", &entries), FileBrowserStatus::kOk);
    bool saw_fifo = false;
    for (const FileEntry& item : entries) {
        if (item.name == "pipe") {
            saw_fifo = true;
            A_CHECK_EQ(item.type, FileEntryType::kOther);
        }
    }
    A_CHECK(saw_fifo);

    // 空参数与空指针。
    A_CHECK_STATUS(browser.List("", nullptr), FileBrowserStatus::kInvalidArgument);
    A_CHECK_STATUS(browser.Stat("a", nullptr), FileBrowserStatus::kInvalidArgument);
    A_CHECK_STATUS(browser.Get("n00", ""), FileBrowserStatus::kInvalidArgument);
    A_CHECK_STATUS(browser.Put("x", ""), FileBrowserStatus::kInvalidArgument);
    A_CHECK_STATUS(browser.Get("", (base / "root.out").string()),
                   FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(browser.Put("", (base / "empty.bin").string()),
                   FileBrowserStatus::kPathRejected);
    A_CHECK_STATUS(browser.Stat("", &entry), FileBrowserStatus::kOk);
    A_CHECK_EQ(entry.type, FileEntryType::kDirectory);
    A_CHECK_EQ(CountPartFiles(root), static_cast<std::size_t>(0));
}

}  // namespace

int main(int argc, char** argv) {
    bool quick = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--quick") == 0) {
            quick = true;
        }
    }
    Note("audit_file_browser_stress：pid=%lld，uid=%d，quick=%d\n",
         static_cast<long long>(::getpid()), static_cast<int>(::geteuid()), quick ? 1 : 0);
    Note("基线 RSS %.1f MiB，fd %zu，逻辑 CPU %u\n",
         static_cast<double>(RssBytes()) / 1048576.0, CountFds(),
         static_cast<unsigned>(std::thread::hardware_concurrency()));

    const fs::path large_base = MakeTempBase("large");
    const fs::path bench_base = MakeTempBase("bench");
    const fs::path random_base = MakeTempBase("random");
    const fs::path escape_base = MakeTempBase("escape");
    const fs::path concurrency_base = MakeTempBase("concurrency");
    const fs::path edge_base = MakeTempBase("edge");
    A_CHECK(!large_base.empty() && !bench_base.empty() && !random_base.empty() &&
            !escape_base.empty() && !concurrency_base.empty() && !edge_base.empty());
    if (large_base.empty() || bench_base.empty() || random_base.empty() ||
        escape_base.empty() || concurrency_base.empty() || edge_base.empty()) {
        Note("临时目录创建失败，终止\n");
        return 1;
    }

    RunLargeFileScenario(large_base, quick);
    RunMicroBench(bench_base, quick);
    RunRandomPaths(random_base);
    RunEscapeScenario(escape_base);
    RunConcurrencyScenario(concurrency_base);
    RunEdgeScenario(edge_base);
    RunRootLifecycleScenario(large_base / "lifecycle");
    RunPermissionScenario(bench_base / "perm");

    RemoveTree(large_base);
    RemoveTree(bench_base);
    RemoveTree(random_base);
    RemoveTree(escape_base);
    RemoveTree(concurrency_base);
    RemoveTree(edge_base);

    Note("\n===== 审计结果：%d checks，%d failures =====\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
