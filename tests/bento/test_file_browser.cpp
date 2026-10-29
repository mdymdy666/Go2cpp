/**
 * @file test_file_browser.cpp
 * @brief MOD-30 文件浏览测试：列目录、Stat、Get/Put、1MB 往返、路径与
 *        符号链接逃逸、不存在、权限、大小上限、条目上限与参数校验。
 * @details 覆盖 19 组场景：
 *          1. 构造（有效/无效根目录）与列根目录（排序、类型、隐藏项）；
 *          2. Stat（根、文件、符号链接、不存在、空指针）；
 *          3. Get（内容、覆盖语义、类型错误、不存在、空参数）；
 *          4. Put（自动建父目录、内容、覆盖语义、源错误）；
 *          5. 路径逃逸（`..`、绝对、反斜杠、盘符、空段、超长）；
 *          6. 符号链接逃逸（默认拒绝；follow=true 时仍以真实路径判据）；
 *          7. 1MB 文件 Get/Put 往返；
 *          8. 大小上限（Get/Put 均拒绝）；
 *          9. 权限拒绝（EACCES/只读根；root 时临时降权到 nobody，不 SKIP）；
 *          10. 目录条目上限；
 *          11. follow_symlinks=true 的根内链接；
 *          12. 空指针/空路径/非法选项；
 *          13. 1200 个强制非法 + 300 个合法随机路径（含 NUL/尾斜杠/超长段）；
 *          14. D-05 并发发布竞争（8MiB 源 + 抢占目标）；
 *          15. List 排序（逆序创建，验证不依赖 readdir 顺序）；
 *          16. 空文件 / Unicode / NAME_MAX / 路径长度 / NUL 参数；
 *          17. 根目录构造后被删除、替换为符号链接或普通文件（fail-closed）；
 *          18. follow_symlinks 与覆盖语义（不写穿链接目标）；
 *          19. 8 线程并发 Get/Put + 同一目标 overwrite=false 竞争。
 * @note 运行期约 1–2 秒；1 GiB / 10 万次压测在
 *       `tests/bento/audit_file_browser_stress.cpp` 独立审计程序中执行。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "go2cpp/bento/app/file_browser.hpp"
#include "test_util.hpp"

namespace {

namespace fs = std::filesystem;

using go2cpp::bento::app::FileBrowser;
using go2cpp::bento::app::FileBrowserOptions;
using go2cpp::bento::app::FileBrowserStatus;
using go2cpp::bento::app::FileEntry;
using go2cpp::bento::app::FileEntryType;

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

/// 创建唯一临时目录；失败返回空路径。
fs::path MakeTempDir(const char* tag) {
    const fs::path base = fs::temp_directory_path();
    for (int index = 0; index < 100; ++index) {
        const fs::path dir =
            base / (std::string("bento_browser_") + tag + "_" + std::to_string(::getpid()) + "_" +
                    std::to_string(index));
        std::error_code ec;
        if (fs::create_directories(dir, ec) && !ec) {
            return dir;
        }
    }
    return {};
}

/// 递归删除；先尽力恢复权限，避免 chmod 000 的目录挡住清理。
void RemoveTree(const fs::path& dir) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        std::error_code chmod_ec;
        fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, chmod_ec);
    }
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(dir, ec);
}

/// 写文件（父目录必须已存在）。
bool WriteFile(const fs::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(output);
}

/// 写二进制文件。
bool WriteFile(const fs::path& path, const std::vector<std::uint8_t>& content) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        return false;
    }
    if (!content.empty()) {
        output.write(reinterpret_cast<const char*>(content.data()),
                     static_cast<std::streamsize>(content.size()));
    }
    return static_cast<bool>(output);
}

/// 读整个文件。
bool ReadFileBytes(const fs::path& path, std::vector<std::uint8_t>* content) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    content->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

/// 读整个文本文件。
bool ReadFileText(const fs::path& path, std::string* content) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    content->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

/// 统计目录下名字含 `go2cpp-part` 的残留临时文件数。
std::size_t CountPartFiles(const fs::path& dir) {
    std::error_code ec;
    std::size_t count = 0;
    fs::directory_iterator iterator(dir, ec);
    const fs::directory_iterator end;
    while (iterator != end && !ec) {
        if (iterator->path().filename().string().find("go2cpp-part") != std::string::npos) {
            ++count;
        }
        iterator.increment(ec);
    }
    return count;
}

/// 生成确定性数据。
std::vector<std::uint8_t> MakeData(std::size_t size, std::uint32_t seed) {
    std::vector<std::uint8_t> data(size);
    std::uint32_t state = seed | 1u;
    for (std::uint8_t& byte : data) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return data;
}

/**
 * @brief 构造标准测试根目录。
 *
 * ```
 * base/
 *   root/
 *     a.txt        ("alpha")
 *     .hidden      ("h")
 *     docs/        空目录
 *     link_in   -> a.txt
 *     link_out  -> base/outside/secret.txt
 *     link_dir  -> base/outside
 *   outside/
 *     secret.txt   ("outside-secret")
 * ```
 */
bool BuildFixture(const fs::path& base, fs::path* root) {
    *root = base / "root";
    const fs::path outside = base / "outside";
    std::error_code ec;
    fs::create_directories(*root, ec);
    if (ec) {
        return false;
    }
    fs::create_directories(*root / "docs", ec);
    if (ec) {
        return false;
    }
    fs::create_directories(outside, ec);
    if (ec) {
        return false;
    }
    if (!WriteFile(*root / "a.txt", "alpha") || !WriteFile(*root / ".hidden", "h") ||
        !WriteFile(outside / "secret.txt", "outside-secret")) {
        return false;
    }
    if (::symlink("a.txt", (*root / "link_in").c_str()) != 0 ||
        ::symlink((outside / "secret.txt").c_str(), (*root / "link_out").c_str()) != 0 ||
        ::symlink(outside.c_str(), (*root / "link_dir").c_str()) != 0) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 场景
// ---------------------------------------------------------------------------

/// 场景 1：构造、列根目录、隐藏项与错误类型。
void TestListRoot() {
    const fs::path base = MakeTempDir("list");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }

    const FileBrowser browser(root.string());
    BENTO_CHECK(browser.valid());
    BENTO_CHECK_EQ(browser.root(), fs::weakly_canonical(root).string());

    std::vector<FileEntry> entries;
    BENTO_CHECK_EQ(browser.List("", &entries), FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(entries.size(), static_cast<std::size_t>(6));
    if (entries.size() == 6) {
        BENTO_CHECK_EQ(entries[0].name, std::string(".hidden"));
        BENTO_CHECK_EQ(entries[0].type, FileEntryType::kFile);
        BENTO_CHECK_EQ(entries[0].size, static_cast<std::uint64_t>(1));
        BENTO_CHECK_EQ(entries[1].name, std::string("a.txt"));
        BENTO_CHECK_EQ(entries[1].size, static_cast<std::uint64_t>(5));
        BENTO_CHECK(entries[1].mtime_ms > 0);
        BENTO_CHECK_EQ(entries[2].name, std::string("docs"));
        BENTO_CHECK_EQ(entries[2].type, FileEntryType::kDirectory);
        BENTO_CHECK_EQ(entries[3].name, std::string("link_dir"));
        BENTO_CHECK_EQ(entries[3].type, FileEntryType::kSymlink);
        BENTO_CHECK_EQ(entries[4].name, std::string("link_in"));
        BENTO_CHECK_EQ(entries[4].type, FileEntryType::kSymlink);
        BENTO_CHECK_EQ(entries[5].name, std::string("link_out"));
        BENTO_CHECK_EQ(entries[5].type, FileEntryType::kSymlink);
    }

    std::vector<FileEntry> no_hidden;
    FileBrowserOptions no_hidden_options;
    no_hidden_options.include_hidden = false;
    BENTO_CHECK_EQ(FileBrowser(root.string(), no_hidden_options).List("", &no_hidden),
                   FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(no_hidden.size(), static_cast<std::size_t>(5));
    BENTO_CHECK(std::none_of(no_hidden.begin(), no_hidden.end(), [](const FileEntry& entry) {
        return entry.name == ".hidden";
    }));

    std::vector<FileEntry> docs;
    BENTO_CHECK_EQ(browser.List("docs", &docs), FileBrowserStatus::kOk);
    BENTO_CHECK(docs.empty());

    BENTO_CHECK_EQ(browser.List("a.txt", &docs), FileBrowserStatus::kNotRegular);
    BENTO_CHECK_EQ(browser.List("nope", &docs), FileBrowserStatus::kNotFound);
    BENTO_CHECK_EQ(browser.List("", nullptr), FileBrowserStatus::kInvalidArgument);

    const FileBrowser missing((base / "missing").string());
    BENTO_CHECK(!missing.valid());
    BENTO_CHECK_EQ(missing.List("", &docs), FileBrowserStatus::kPathRejected);
    const FileBrowser file_root((root / "a.txt").string());
    BENTO_CHECK(!file_root.valid());

    RemoveTree(base);
}

/// 场景 2：Stat。
void TestStat() {
    const fs::path base = MakeTempDir("stat");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    const FileBrowser browser(root.string());

    FileEntry entry;
    BENTO_CHECK_EQ(browser.Stat("", &entry), FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(entry.type, FileEntryType::kDirectory);
    BENTO_CHECK_EQ(entry.name, std::string("root"));

    BENTO_CHECK_EQ(browser.Stat("a.txt", &entry), FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(entry.name, std::string("a.txt"));
    BENTO_CHECK_EQ(entry.size, static_cast<std::uint64_t>(5));
    BENTO_CHECK_EQ(entry.type, FileEntryType::kFile);
    BENTO_CHECK(entry.mtime_ms > 0);

    BENTO_CHECK_EQ(browser.Stat("link_in", &entry), FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(entry.type, FileEntryType::kSymlink);

    BENTO_CHECK_EQ(browser.Stat("nope", &entry), FileBrowserStatus::kNotFound);
    BENTO_CHECK_EQ(browser.Stat("a.txt", nullptr), FileBrowserStatus::kInvalidArgument);

    RemoveTree(base);
}

/// 场景 3：Get 的内容、覆盖语义与错误。
void TestGet() {
    const fs::path base = MakeTempDir("get");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    const FileBrowser browser(root.string());
    const fs::path local_dir = base / "local";
    std::error_code ec;
    fs::create_directories(local_dir, ec);
    BENTO_CHECK(!ec);
    const fs::path local = local_dir / "a.copy";

    BENTO_CHECK_EQ(browser.Get("a.txt", local.string()), FileBrowserStatus::kOk);
    std::string text;
    BENTO_CHECK(ReadFileText(local, &text));
    BENTO_CHECK_EQ(text, std::string("alpha"));
    BENTO_CHECK_EQ(CountPartFiles(local_dir), static_cast<std::size_t>(0));

    BENTO_CHECK_EQ(browser.Get("a.txt", local.string()), FileBrowserStatus::kAlreadyExists);

    FileBrowserOptions overwrite;
    overwrite.overwrite = true;
    BENTO_CHECK_EQ(FileBrowser(root.string(), overwrite).Get("a.txt", local.string()),
                   FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(CountPartFiles(local_dir), static_cast<std::size_t>(0));

    BENTO_CHECK_EQ(browser.Get("docs", (local_dir / "dir.copy").string()),
                   FileBrowserStatus::kNotRegular);
    BENTO_CHECK_EQ(browser.Get("nope", (local_dir / "none.copy").string()),
                   FileBrowserStatus::kNotFound);
    BENTO_CHECK_EQ(browser.Get("", (local_dir / "root.copy").string()),
                   FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Get("a.txt", ""), FileBrowserStatus::kInvalidArgument);
    BENTO_CHECK_EQ(browser.Get("a.txt", (local_dir / "missing" / "x").string()),
                   FileBrowserStatus::kNotFound);

    RemoveTree(base);
}

/// 场景 4：Put 的自动建目录、内容与覆盖语义。
void TestPut() {
    const fs::path base = MakeTempDir("put");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    const FileBrowser browser(root.string());
    const fs::path local_dir = base / "local";
    const fs::path local = local_dir / "upload.bin";
    std::error_code ec;
    fs::create_directories(local_dir, ec);
    BENTO_CHECK(!ec);
    BENTO_CHECK(WriteFile(local, "upload-data"));

    const go2cpp::bento::app::FileBrowserStatus first_put =
        browser.Put("upload/new.txt", local.string());
    BENTO_CHECK_EQ(first_put, FileBrowserStatus::kOk);
    std::string text;
    BENTO_CHECK(ReadFileText(root / "upload" / "new.txt", &text));
    BENTO_CHECK_EQ(text, std::string("upload-data"));
    BENTO_CHECK_EQ(CountPartFiles(root / "upload"), static_cast<std::size_t>(0));

    BENTO_CHECK_EQ(browser.Put("upload/new.txt", local.string()), FileBrowserStatus::kAlreadyExists);
    BENTO_CHECK(WriteFile(local, "updated"));
    FileBrowserOptions overwrite;
    overwrite.overwrite = true;
    BENTO_CHECK_EQ(FileBrowser(root.string(), overwrite).Put("upload/new.txt", local.string()),
                   FileBrowserStatus::kOk);
    BENTO_CHECK(ReadFileText(root / "upload" / "new.txt", &text));
    BENTO_CHECK_EQ(text, std::string("updated"));

    BENTO_CHECK_EQ(browser.Put("upload/x.txt", (local_dir / "missing.bin").string()),
                   FileBrowserStatus::kNotFound);
    BENTO_CHECK_EQ(browser.Put("upload/x.txt", local_dir.string()), FileBrowserStatus::kNotRegular);
    BENTO_CHECK_EQ(browser.Put("", local.string()), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Put("a.txt/..", local.string()), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Put("upload/x.txt", ""), FileBrowserStatus::kInvalidArgument);

    RemoveTree(base);
}

/// 场景 5：路径逃逸拒绝（List/Get/Put 三个入口）。
void TestPathEscape() {
    const fs::path base = MakeTempDir("escape");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    const FileBrowser browser(root.string());
    const std::string local = (base / "local.bin").string();
    BENTO_CHECK(WriteFile(fs::path(local), "x"));

    const std::vector<std::string> paths = {
        "..",            "../x",        "../outside/secret.txt", "/etc/passwd",
        "a/../b",        "a/../../b",   "a//b",                  "a\\b",
        "C:/windows",    "docs/..",     "./a.txt",               "a/./b",
        std::string(2000, 'x')};
    for (const std::string& path : paths) {
        std::vector<FileEntry> entries;
        BENTO_CHECK_EQ(browser.List(path, &entries), FileBrowserStatus::kPathRejected);
        BENTO_CHECK_EQ(browser.Get(path, local), FileBrowserStatus::kPathRejected);
        BENTO_CHECK_EQ(browser.Put(path, local), FileBrowserStatus::kPathRejected);
    }

    // 路径长度上限：默认 1024，这里用很小的配置再验证一次。
    FileBrowserOptions short_path;
    short_path.max_path_bytes = 3;
    const FileBrowser short_browser(root.string(), short_path);
    std::vector<FileEntry> entries;
    BENTO_CHECK_EQ(short_browser.List("docs", &entries), FileBrowserStatus::kPathRejected);

    RemoveTree(base);
}

/// 场景 6：符号链接逃逸默认拒绝；follow=true 仍以真实路径为判据。
void TestSymlinkEscape() {
    const fs::path base = MakeTempDir("symlink");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    const FileBrowser browser(root.string());
    const std::string local = (base / "link.out").string();
    BENTO_CHECK(WriteFile(fs::path(local), "x"));
    std::vector<FileEntry> entries;

    BENTO_CHECK_EQ(browser.List("link_dir", &entries), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.List("link_dir/secret.txt", &entries), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Get("link_out", local), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Get("link_in", local), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Put("link_dir/x.txt", local), FileBrowserStatus::kPathRejected);

    FileBrowserOptions follow;
    follow.follow_symlinks = true;
    const FileBrowser follower(root.string(), follow);
    BENTO_CHECK_EQ(follower.Get("link_out", local), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(follower.List("link_dir", &entries), FileBrowserStatus::kPathRejected);
    const std::string follow_local = (base / "link.in").string();
    BENTO_CHECK_EQ(follower.Get("link_in", follow_local), FileBrowserStatus::kOk);
    std::string text;
    BENTO_CHECK(ReadFileText(fs::path(follow_local), &text));
    BENTO_CHECK_EQ(text, std::string("alpha"));

    RemoveTree(base);
}

/// 场景 7：1MB 文件 Get/Put 往返。
void TestMegabyteRoundTrip() {
    const fs::path base = MakeTempDir("1mb");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    const std::vector<std::uint8_t> payload = MakeData(1024 * 1024, 0xabcdef01u);
    BENTO_CHECK(WriteFile(root / "data.bin", payload));
    const fs::path local = base / "data.copy";
    const FileBrowser browser(root.string());

    BENTO_CHECK_EQ(browser.Get("data.bin", local.string()), FileBrowserStatus::kOk);
    std::vector<std::uint8_t> actual;
    BENTO_CHECK(ReadFileBytes(local, &actual));
    BENTO_CHECK(actual == payload);
    BENTO_CHECK_EQ(fs::file_size(local), static_cast<std::uint64_t>(payload.size()));

    BENTO_CHECK_EQ(browser.Put("archive/data.copy.bin", local.string()), FileBrowserStatus::kOk);
    std::vector<std::uint8_t> uploaded;
    BENTO_CHECK(ReadFileBytes(root / "archive" / "data.copy.bin", &uploaded));
    BENTO_CHECK(uploaded == payload);
    BENTO_CHECK_EQ(CountPartFiles(root / "archive"), static_cast<std::size_t>(0));

    RemoveTree(base);
}

/// 场景 8：大小上限（Get/Put 都拒绝）。
void TestSizeLimit() {
    const fs::path base = MakeTempDir("toolarge");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    FileBrowserOptions options;
    options.max_file_bytes = 4;
    const FileBrowser browser(root.string(), options);
    const fs::path local = base / "out.bin";

    std::vector<FileEntry> entries;
    BENTO_CHECK_EQ(browser.List("", &entries), FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(browser.Get("a.txt", local.string()), FileBrowserStatus::kTooLarge);
    BENTO_CHECK(WriteFile(local, "12345"));
    BENTO_CHECK_EQ(browser.Put("upload.bin", local.string()), FileBrowserStatus::kTooLarge);
    BENTO_CHECK_EQ(CountPartFiles(base), static_cast<std::size_t>(0));

    // 恰好等于上限可以成功。
    BENTO_CHECK(WriteFile(local, "1234"));
    BENTO_CHECK_EQ(browser.Put("upload.bin", local.string()), FileBrowserStatus::kOk);

    RemoveTree(base);
}

/// 场景 9：权限拒绝（root 时临时降权到 nobody，不再 SKIP）。
void TestPermissionDenied() {
    const fs::path base = MakeTempDir("perm");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    std::error_code ec;
    const fs::path local_dir = base / "local";
    const fs::path local_ro = base / "local_ro";
    fs::create_directories(root / "locked", ec);
    fs::create_directories(root / "readonly", ec);
    fs::create_directories(local_dir, ec);
    fs::create_directories(local_ro, ec);
    BENTO_CHECK(!ec);
    BENTO_CHECK(WriteFile(root / "secret.txt", "secret"));
    BENTO_CHECK(WriteFile(root / "readable.txt", "read"));
    BENTO_CHECK(WriteFile(local_dir / "upload.bin", "upload"));
    BENTO_CHECK(::chmod((root / "locked").c_str(), 0000) == 0);
    BENTO_CHECK(::chmod((root / "secret.txt").c_str(), 0000) == 0);
    BENTO_CHECK(::chmod((root / "readonly").c_str(), 0555) == 0);
    BENTO_CHECK(::chmod(local_dir.c_str(), 0777) == 0);
    BENTO_CHECK(::chmod(local_ro.c_str(), 0555) == 0);

    // root 的 DAC 绕过会让 chmod 000 失效；临时把有效 uid/gid 降到 nobody，
    // 让同一组断言在 CI 容器（root）里也真实执行。结束时恢复。
    const bool dropped = ::geteuid() == 0;
    if (dropped) {
        BENTO_CHECK(::setegid(65534) == 0);
        BENTO_CHECK(::seteuid(65534) == 0);
    }

    const FileBrowser browser(root.string());
    std::vector<FileEntry> entries;
    FileEntry entry;
    BENTO_CHECK_EQ(browser.List("locked", &entries), FileBrowserStatus::kPermissionDenied);
    BENTO_CHECK_EQ(browser.Get("secret.txt", (local_dir / "secret.copy").string()),
                   FileBrowserStatus::kPermissionDenied);
    BENTO_CHECK_EQ(browser.Put("readonly/new.txt", (local_dir / "upload.bin").string()),
                   FileBrowserStatus::kPermissionDenied);
    BENTO_CHECK_EQ(browser.Get("readable.txt", (local_ro / "readable.copy").string()),
                   FileBrowserStatus::kPermissionDenied);
    BENTO_CHECK_EQ(browser.List("", &entries), FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(browser.Get("readable.txt", (local_dir / "readable.copy").string()),
                   FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(browser.Stat("secret.txt", &entry), FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(entry.size, static_cast<std::uint64_t>(6));
    BENTO_CHECK_EQ(browser.List("nope", &entries), FileBrowserStatus::kNotFound);

    if (dropped) {
        BENTO_CHECK(::seteuid(0) == 0);
        BENTO_CHECK(::setegid(0) == 0);
    }
    BENTO_CHECK_EQ(::chmod((root / "locked").c_str(), 0755), 0);
    BENTO_CHECK_EQ(::chmod((root / "secret.txt").c_str(), 0644), 0);
    BENTO_CHECK_EQ(::chmod((root / "readonly").c_str(), 0755), 0);
    BENTO_CHECK_EQ(::chmod(local_ro.c_str(), 0755), 0);
    BENTO_CHECK(!fs::exists(root / "readonly" / "new.txt"));
    RemoveTree(base);
}

/// 场景 10：目录条目上限。
void TestEntryLimit() {
    const fs::path base = MakeTempDir("entries");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    FileBrowserOptions options;
    options.max_entries = 2;
    const FileBrowser browser(root.string(), options);
    std::vector<FileEntry> entries;
    BENTO_CHECK_EQ(browser.List("", &entries), FileBrowserStatus::kTooManyEntries);
    BENTO_CHECK_EQ(entries.size(), static_cast<std::size_t>(2));
    RemoveTree(base);
}

/// 场景 11/12：非法选项、空参数与根目录语义。
void TestInvalidOptionsAndArguments() {
    const fs::path base = MakeTempDir("invalid");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }

    FileBrowserOptions zero_bytes;
    zero_bytes.max_file_bytes = 0;
    BENTO_CHECK(!FileBrowser(root.string(), zero_bytes).valid());

    FileBrowserOptions zero_path;
    zero_path.max_path_bytes = 0;
    BENTO_CHECK(!FileBrowser(root.string(), zero_path).valid());

    FileBrowserOptions huge_path;
    huge_path.max_path_bytes = 1ull << 20;
    BENTO_CHECK(!FileBrowser(root.string(), huge_path).valid());

    FileBrowserOptions zero_entries;
    zero_entries.max_entries = 0;
    BENTO_CHECK(!FileBrowser(root.string(), zero_entries).valid());

    const FileBrowser browser(root.string());
    std::vector<FileEntry> entries;
    BENTO_CHECK_EQ(browser.List("", &entries), FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(browser.Get("", ""), FileBrowserStatus::kInvalidArgument);
    BENTO_CHECK_EQ(browser.Put("a.txt", ""), FileBrowserStatus::kInvalidArgument);
    BENTO_CHECK_EQ(browser.Get("a.txt", ""), FileBrowserStatus::kInvalidArgument);

    RemoveTree(base);
}

/// 场景 13：随机路径 fuzz（1200 个强制非法 + 300 个合法组合）。
void TestRandomPathEscapes() {
    const fs::path base = MakeTempDir("fuzz");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    const FileBrowser browser(root.string());
    const fs::path local_path = base / "local.bin";
    BENTO_CHECK(WriteFile(local_path, "x"));
    const std::string local = local_path.string();

    std::mt19937 rng(20261028u);
    const std::string segments[] = {"docs", "nope", "sub", "sub2"};
    const auto make_valid = [&rng, &segments]() {
        std::string path;
        const int parts = 1 + static_cast<int>(rng() % 3);
        for (int index = 0; index < parts; ++index) {
            if (index != 0) {
                path.push_back('/');
            }
            path += segments[rng() % 4];
        }
        return path;
    };

    for (int iteration = 0; iteration < 1200; ++iteration) {
        std::string path;
        switch (rng() % 8) {
            case 0:
                path = "/" + make_valid();
                break;
            case 1:
                path = make_valid() + "/../" + make_valid();
                break;
            case 2:
                path = make_valid();
                path += "//";
                path += make_valid();
                break;
            case 3:
                path = make_valid() + "\\" + make_valid();
                break;
            case 4:
                path = make_valid() + ":x";
                break;
            case 5:
                path = make_valid() + "/";
                break;
            case 6:
                path = make_valid();
                path.push_back('\0');
                path += "x";
                break;
            default:
                path.assign(300, 'x');
                break;
        }
        std::vector<FileEntry> entries;
        FileEntry entry;
        BENTO_CHECK_EQ(browser.List(path, &entries), FileBrowserStatus::kPathRejected);
        BENTO_CHECK_EQ(browser.Stat(path, &entry), FileBrowserStatus::kPathRejected);
        BENTO_CHECK_EQ(browser.Get(path, local), FileBrowserStatus::kPathRejected);
        BENTO_CHECK_EQ(browser.Put(path, local), FileBrowserStatus::kPathRejected);
    }

    for (int iteration = 0; iteration < 300; ++iteration) {
        std::vector<FileEntry> entries;
        FileEntry entry;
        const std::string path = make_valid();
        const FileBrowserStatus list_status = browser.List(path, &entries);
        const FileBrowserStatus stat_status = browser.Stat(path, &entry);
        BENTO_CHECK(list_status == FileBrowserStatus::kOk ||
                    list_status == FileBrowserStatus::kNotFound);
        BENTO_CHECK(stat_status == FileBrowserStatus::kOk ||
                    stat_status == FileBrowserStatus::kNotFound);
    }

    RemoveTree(base);
}

/// D-05：overwrite=false 时，「检查目标不存在」之后被并发创建的目标不能被覆盖。
///
/// 旧实现先 lstat 再 rename：竞态创建的目标会被 rename 覆盖；新实现用
/// link() 原子占位，目标已存在时以 EEXIST 失败（kAlreadyExists）。为了稳定
/// 命中窗口，用 8 MiB 源文件把复制时间拉长，竞态线程一旦发现临时文件出现
/// 就用 O_EXCL 抢先创建目标。
void TestGetNoOverwriteRace() {
    const fs::path base = MakeTempDir("get_race");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    const FileBrowser browser(root.string());
    const fs::path local_dir = base / "local";
    std::error_code ec;
    fs::create_directories(local_dir, ec);
    BENTO_CHECK(!ec);

    const std::vector<std::uint8_t> big = MakeData(8 * 1024 * 1024, 0x5a5a1234u);
    BENTO_CHECK(WriteFile(root / "big.bin", big));
    const fs::path target = local_dir / "race.bin";

    bool saw_already_exists = false;
    for (int attempt = 0; attempt < 12 && !saw_already_exists; ++attempt) {
        std::error_code remove_ec;
        fs::remove(target, remove_ec);
        std::atomic<bool> stop{false};
        std::thread racer([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                std::error_code scan_ec;
                bool temp_seen = false;
                for (fs::directory_iterator it(local_dir, scan_ec), end;
                     it != end && !scan_ec; it.increment(scan_ec)) {
                    if (it->path().filename().string().find("go2cpp-part") !=
                        std::string::npos) {
                        temp_seen = true;
                        break;
                    }
                }
                if (!temp_seen) {
                    continue;
                }
                // 原子抢占：O_EXCL 成功 = 竞态者在发布前创建了目标。
                const int fd =
                    ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
                if (fd >= 0) {
                    const char kRacerText[] = "racer";
                    const ssize_t written =
                        ::write(fd, kRacerText, sizeof(kRacerText) - 1);
                    BENTO_CHECK_EQ(written, static_cast<ssize_t>(sizeof(kRacerText) - 1));
                    ::close(fd);
                }
                break;
            }
        });
        const FileBrowserStatus status = browser.Get("big.bin", target.string());
        stop.store(true);
        racer.join();
        if (status == FileBrowserStatus::kAlreadyExists) {
            saw_already_exists = true;
            std::string content;
            BENTO_CHECK(ReadFileText(target, &content));
            BENTO_CHECK_EQ(content, std::string("racer"));
        } else {
            // 本端先发布成功也是合法结果（目标内容是源文件）。
            BENTO_CHECK_EQ(status, FileBrowserStatus::kOk);
            BENTO_CHECK_EQ(fs::file_size(target), static_cast<std::uint64_t>(big.size()));
        }
    }
    BENTO_CHECK(saw_already_exists);
    BENTO_CHECK_EQ(CountPartFiles(local_dir), static_cast<std::size_t>(0));
    RemoveTree(base);
}

/// 场景 14：List 排序（故意逆序创建，不依赖 readdir 的返回顺序）。
void TestListSortedOrder() {
    const fs::path base = MakeTempDir("sort");
    BENTO_CHECK(!base.empty());
    const fs::path root = base / "root";
    std::error_code ec;
    fs::create_directories(root, ec);
    BENTO_CHECK(!ec);
    if (base.empty() || ec) {
        return;
    }
    std::vector<std::string> expected;
    for (int value = 23; value >= 0; --value) {
        char name[16];
        std::snprintf(name, sizeof(name), "n%02d", value);
        expected.emplace_back(name);
        BENTO_CHECK(WriteFile(root / name, "x"));
    }
    BENTO_CHECK(WriteFile(root / "文件-数据.txt", "unicode"));
    expected.emplace_back("文件-数据.txt");
    std::sort(expected.begin(), expected.end());

    const FileBrowser browser(root.string());
    std::vector<FileEntry> entries;
    BENTO_CHECK_EQ(browser.List("", &entries), FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(entries.size(), expected.size());
    std::vector<std::string> actual;
    actual.reserve(entries.size());
    for (const FileEntry& entry : entries) {
        actual.push_back(entry.name);
    }
    BENTO_CHECK(actual == expected);
    BENTO_CHECK(std::is_sorted(entries.begin(), entries.end(),
                               [](const FileEntry& left, const FileEntry& right) {
                                   return left.name < right.name;
                               }));
    RemoveTree(base);
}

/// 场景 15：空文件 / Unicode / NAME_MAX / 路径长度 / NUL 参数。
void TestEmptyUnicodeAndLimits() {
    const fs::path base = MakeTempDir("edge");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    const FileBrowser browser(root.string());
    std::vector<FileEntry> entries;
    FileEntry entry;

    // 0 字节文件往返。
    BENTO_CHECK(WriteFile(base / "empty.bin", ""));
    BENTO_CHECK_EQ(browser.Put("sub/empty.bin", (base / "empty.bin").string()),
                   FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(fs::file_size(root / "sub" / "empty.bin"),
                   static_cast<std::uint64_t>(0));
    BENTO_CHECK_EQ(browser.Get("sub/empty.bin", (base / "empty.out").string()),
                   FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(fs::file_size(base / "empty.out"), static_cast<std::uint64_t>(0));

    // Unicode 名称。
    BENTO_CHECK(WriteFile(root / "数据-ä.txt", "unicode"));
    BENTO_CHECK_EQ(browser.List("", &entries), FileBrowserStatus::kOk);
    BENTO_CHECK(std::any_of(entries.begin(), entries.end(), [](const FileEntry& item) {
        return item.name == "数据-ä.txt";
    }));
    BENTO_CHECK_EQ(browser.Get("数据-ä.txt", (base / "unicode.out").string()),
                   FileBrowserStatus::kOk);
    std::string text;
    BENTO_CHECK(ReadFileText(base / "unicode.out", &text));
    BENTO_CHECK_EQ(text, std::string("unicode"));

    // NAME_MAX：255 不存在 → not_found；256 → path_rejected（不再 io_error）。
    const std::string name_255(255, 'c');
    const std::string name_256(256, 'c');
    BENTO_CHECK_EQ(browser.Stat(name_255, &entry), FileBrowserStatus::kNotFound);
    BENTO_CHECK_EQ(browser.Stat(name_256, &entry), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.List(name_256, &entries), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Get(name_256, (base / "name256.out").string()),
                   FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Put(name_256, (base / "empty.bin").string()),
                   FileBrowserStatus::kPathRejected);

    // 240 字节长名：临时名不拼接目标名，Put/Get 都不会撞 NAME_MAX。
    const std::string long_name(240, 'l');
    BENTO_CHECK(WriteFile(base / "long-source.bin", "long-name"));
    BENTO_CHECK_EQ(browser.Put(long_name, (base / "long-source.bin").string()),
                   FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(browser.Get(long_name, (base / long_name).string()),
                   FileBrowserStatus::kOk);
    std::string long_text;
    BENTO_CHECK(ReadFileText(base / long_name, &long_text));
    BENTO_CHECK_EQ(long_text, std::string("long-name"));

    // 路径长度边界：1024 合法、1025 拒绝。
    std::string path_1024;
    for (int index = 0; index < 16; ++index) {
        if (index != 0) {
            path_1024.push_back('/');
        }
        path_1024.append(index == 15 ? 64 : 63, 'd');
    }
    BENTO_CHECK_EQ(path_1024.size(), static_cast<std::size_t>(1024));
    BENTO_CHECK_EQ(browser.List(path_1024, &entries), FileBrowserStatus::kNotFound);
    std::string path_1025 = path_1024;
    path_1025.push_back('d');
    BENTO_CHECK_EQ(path_1025.size(), static_cast<std::size_t>(1025));
    BENTO_CHECK_EQ(browser.List(path_1025, &entries), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Get(path_1025, (base / "p1025.out").string()),
                   FileBrowserStatus::kPathRejected);

    // rel 中的 NUL 按控制字符拒绝。
    const std::string nul_rel("a\0b", 3);
    BENTO_CHECK_EQ(browser.List(nul_rel, &entries), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Stat(nul_rel, &entry), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Get(nul_rel, (base / "nul.out").string()),
                   FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Put(nul_rel, (base / "empty.bin").string()),
                   FileBrowserStatus::kPathRejected);

    // local_path 中的 NUL：不能静默截断后写盘。
    std::string nul_local = (base / "nul-target").string();
    nul_local.push_back('\0');
    nul_local += "evil";
    BENTO_CHECK_EQ(browser.Get("a.txt", nul_local), FileBrowserStatus::kInvalidArgument);
    BENTO_CHECK_EQ(browser.Put("nul-upload.bin", nul_local),
                   FileBrowserStatus::kInvalidArgument);
    BENTO_CHECK(!fs::exists(base / "nul-target"));
    BENTO_CHECK(!fs::exists(root / "nul-upload.bin"));

    RemoveTree(base);
}

/// 场景 16：根目录构造后被删除/替换（fail-closed，不逃逸）。
void TestRootLifecycle() {
    const fs::path base = MakeTempDir("rootlife");
    BENTO_CHECK(!base.empty());
    const fs::path root = base / "root";
    const fs::path outside = base / "outside";
    std::error_code ec;
    fs::create_directories(root, ec);
    fs::create_directories(outside, ec);
    BENTO_CHECK(!ec);
    if (base.empty() || ec) {
        return;
    }
    BENTO_CHECK(WriteFile(root / "a.txt", "alpha"));
    BENTO_CHECK(WriteFile(outside / "leak.txt", "outside"));
    BENTO_CHECK(WriteFile(base / "upload.bin", "upload"));
    const FileBrowser browser(root.string());
    std::vector<FileEntry> entries;
    FileEntry entry;
    const std::string local = (base / "out.bin").string();

    // 删除根目录：不得复活，也不得把操作落到别处。
    fs::remove_all(root, ec);
    BENTO_CHECK(!ec);
    BENTO_CHECK_EQ(browser.List("", &entries), FileBrowserStatus::kNotFound);
    BENTO_CHECK_EQ(browser.List("a.txt", &entries), FileBrowserStatus::kNotFound);
    BENTO_CHECK_EQ(browser.Stat("", &entry), FileBrowserStatus::kNotFound);
    BENTO_CHECK_EQ(browser.Get("a.txt", local), FileBrowserStatus::kNotFound);
    BENTO_CHECK_EQ(browser.Put("resurrect.txt", (base / "upload.bin").string()),
                   FileBrowserStatus::kNotFound);
    BENTO_CHECK(!fs::exists(root));

    // 替换成指向外部的符号链接：List("") 不得列出外部目录。
    BENTO_CHECK(::symlink(outside.c_str(), root.c_str()) == 0);
    BENTO_CHECK_EQ(browser.List("", &entries), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.List("leak.txt", &entries), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Stat("", &entry), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Get("leak.txt", local), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(browser.Put("pwn.txt", (base / "upload.bin").string()),
                   FileBrowserStatus::kPathRejected);
    BENTO_CHECK(!fs::exists(outside / "pwn.txt"));
    std::string outside_text;
    BENTO_CHECK(ReadFileText(outside / "leak.txt", &outside_text));
    BENTO_CHECK_EQ(outside_text, std::string("outside"));

    // 替换成普通文件：根不再是目录。
    fs::remove(root, ec);
    BENTO_CHECK(WriteFile(root, "not-a-dir"));
    BENTO_CHECK_EQ(browser.List("", &entries), FileBrowserStatus::kNotFound);
    BENTO_CHECK_EQ(browser.Get("a.txt", local), FileBrowserStatus::kNotFound);
    RemoveTree(base);
}

/// 场景 17：follow_symlinks 与覆盖语义（不写穿链接目标）。
void TestSymlinkFollowAndOverwrite() {
    const fs::path base = MakeTempDir("follow");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    const fs::path outside = base / "outside";
    BENTO_CHECK(WriteFile(outside / "secret.txt", "outside-secret"));
    FileBrowserOptions follow_options;
    follow_options.follow_symlinks = true;
    const FileBrowser follower(root.string(), follow_options);
    std::vector<FileEntry> entries;
    FileEntry entry;
    const std::string local = (base / "follow.out").string();

    BENTO_CHECK_EQ(follower.List("link_in", &entries), FileBrowserStatus::kNotRegular);
    BENTO_CHECK_EQ(follower.Stat("link_in", &entry), FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(entry.type, FileEntryType::kFile);
    BENTO_CHECK_EQ(follower.Get("link_in", local), FileBrowserStatus::kOk);
    std::string text;
    BENTO_CHECK(ReadFileText(fs::path(local), &text));
    BENTO_CHECK_EQ(text, std::string("alpha"));
    BENTO_CHECK_EQ(follower.Stat("link_out", &entry), FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(follower.List("link_dir", &entries), FileBrowserStatus::kPathRejected);

    // 根内目录链接：follow=true 放行，默认拒绝。
    std::error_code ec;
    fs::create_directories(root / "real_dir", ec);
    BENTO_CHECK(!ec);
    BENTO_CHECK(WriteFile(root / "real_dir" / "inner.txt", "inner"));
    BENTO_CHECK(::symlink("real_dir", (root / "alias_dir").c_str()) == 0);
    BENTO_CHECK_EQ(follower.List("alias_dir", &entries), FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(FileBrowser(root.string()).List("alias_dir", &entries),
                   FileBrowserStatus::kPathRejected);
    BENTO_CHECK_EQ(follower.Put("alias_dir/put.txt", (root / "a.txt").string()),
                   FileBrowserStatus::kOk);
    BENTO_CHECK(ReadFileText(root / "real_dir" / "put.txt", &text));
    BENTO_CHECK_EQ(text, std::string("alpha"));

    // overwrite=true 的 Put 不能写穿符号链接目标。
    BENTO_CHECK(::symlink((outside / "secret.txt").c_str(),
                          (root / "evil_link").c_str()) == 0);
    FileBrowserOptions overwrite_options;
    overwrite_options.overwrite = true;
    const FileBrowser overwriter(root.string(), overwrite_options);
    BENTO_CHECK_EQ(overwriter.Put("evil_link", (root / "a.txt").string()),
                   FileBrowserStatus::kPathRejected);
    BENTO_CHECK(fs::is_symlink(root / "evil_link"));
    BENTO_CHECK(ReadFileText(outside / "secret.txt", &text));
    BENTO_CHECK_EQ(text, std::string("outside-secret"));

    // overwrite=true 的 Get 到本地符号链接：替换链接本身，不写穿。
    const fs::path victim = base / "victim.txt";
    BENTO_CHECK(WriteFile(victim, "victim"));
    BENTO_CHECK(::symlink(victim.c_str(), (base / "local_link").c_str()) == 0);
    BENTO_CHECK_EQ(overwriter.Get("a.txt", (base / "local_link").string()),
                   FileBrowserStatus::kOk);
    BENTO_CHECK(!fs::is_symlink(base / "local_link"));
    BENTO_CHECK(ReadFileText(victim, &text));
    BENTO_CHECK_EQ(text, std::string("victim"));
    BENTO_CHECK(ReadFileText(base / "local_link", &text));
    BENTO_CHECK_EQ(text, std::string("alpha"));

    RemoveTree(base);
}

/// 场景 18：8 线程并发 Get/Put 与同一目标发布竞争。
void TestConcurrentGetPut() {
    const fs::path base = MakeTempDir("concurrent");
    BENTO_CHECK(!base.empty());
    const fs::path root = base / "root";
    std::error_code ec;
    fs::create_directories(root / "conc", ec);
    BENTO_CHECK(!ec);
    if (base.empty() || ec) {
        return;
    }
    const FileBrowser browser(root.string());
    constexpr int kThreads = 8;
    constexpr int kIterations = 40;
    std::atomic<int> failures{0};
    std::atomic<int> completed{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&, thread_index] {
            const std::string payload =
                "thread-" + std::to_string(thread_index) + "-" + std::string(4096, 'c');
            const fs::path source = base / ("src_" + std::to_string(thread_index));
            if (!WriteFile(source, payload)) {
                ++failures;
                return;
            }
            const fs::path download = base / ("dst_" + std::to_string(thread_index));
            FileBrowserOptions overwrite_options;
            overwrite_options.overwrite = true;
            const FileBrowser overwrite_browser(root.string(), overwrite_options);
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                const std::string name = "conc/t" + std::to_string(thread_index) + "_" +
                                         std::to_string(iteration) + ".bin";
                if (overwrite_browser.Put(name, source.string()) != FileBrowserStatus::kOk ||
                    overwrite_browser.Get(name, download.string()) != FileBrowserStatus::kOk) {
                    ++failures;
                    continue;
                }
                std::string actual;
                if (!ReadFileText(download, &actual) || actual != payload) {
                    ++failures;
                }
                ++completed;
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    BENTO_CHECK_EQ(failures.load(), 0);
    BENTO_CHECK_EQ(completed.load(), kThreads * kIterations);
    BENTO_CHECK_EQ(CountPartFiles(root), static_cast<std::size_t>(0));

    // 同一目标 + overwrite=false：每轮恰好一个成功。
    const fs::path same_source = base / "same.bin";
    BENTO_CHECK(WriteFile(same_source, "same-target"));
    constexpr int kRounds = 20;
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
            for (int round = 0; round < kRounds; ++round) {
                const FileBrowserStatus status = browser.Put(
                    "conc/race_" + std::to_string(round) + ".bin", same_source.string());
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
    BENTO_CHECK_EQ(ok_count.load(), kRounds);
    BENTO_CHECK_EQ(exists_count.load(), kRounds * (kThreads - 1));
    BENTO_CHECK_EQ(other_count.load(), 0);
    BENTO_CHECK_EQ(CountPartFiles(root), static_cast<std::size_t>(0));
    RemoveTree(base);
}

/// 场景 19：FIFO 等特殊文件。
void TestSpecialFiles() {
    const fs::path base = MakeTempDir("special");
    BENTO_CHECK(!base.empty());
    const fs::path root = base / "root";
    std::error_code ec;
    fs::create_directories(root, ec);
    BENTO_CHECK(!ec);
    if (base.empty() || ec) {
        return;
    }
    BENTO_CHECK(::mkfifo((root / "pipe").c_str(), 0644) == 0);
    const FileBrowser browser(root.string());
    std::vector<FileEntry> entries;
    FileEntry entry;
    BENTO_CHECK_EQ(browser.List("", &entries), FileBrowserStatus::kOk);
    bool saw_pipe = false;
    for (const FileEntry& item : entries) {
        if (item.name == "pipe") {
            saw_pipe = true;
            BENTO_CHECK_EQ(item.type, FileEntryType::kOther);
        }
    }
    BENTO_CHECK(saw_pipe);
    BENTO_CHECK_EQ(browser.List("pipe", &entries), FileBrowserStatus::kNotRegular);
    BENTO_CHECK_EQ(browser.Get("pipe", (base / "pipe.out").string()),
                   FileBrowserStatus::kNotRegular);
    BENTO_CHECK_EQ(browser.Stat("pipe", &entry), FileBrowserStatus::kOk);
    BENTO_CHECK_EQ(entry.type, FileEntryType::kOther);
    RemoveTree(base);
}

}  // namespace

int main() {
    TestListRoot();
    TestStat();
    TestGet();
    TestPut();
    TestPathEscape();
    TestSymlinkEscape();
    TestMegabyteRoundTrip();
    TestSizeLimit();
    TestPermissionDenied();
    TestEntryLimit();
    TestInvalidOptionsAndArguments();
    TestRandomPathEscapes();
    TestGetNoOverwriteRace();
    TestListSortedOrder();
    TestEmptyUnicodeAndLimits();
    TestRootLifecycle();
    TestSymlinkFollowAndOverwrite();
    TestConcurrentGetPut();
    TestSpecialFiles();
    return bento_test::Finish("test_file_browser");
}
