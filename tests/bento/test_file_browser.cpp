/**
 * @file test_file_browser.cpp
 * @brief MOD-30 文件浏览测试：列目录、Stat、Get/Put、1MB 往返、路径与
 *        符号链接逃逸、不存在、权限、大小上限、条目上限与参数校验。
 * @details 覆盖 12 组场景：
 *          1. 构造（有效/无效根目录）与列根目录（排序、类型、隐藏项）；
 *          2. Stat（根、文件、符号链接、不存在、空指针）；
 *          3. Get（内容、覆盖语义、类型错误、不存在、空参数）；
 *          4. Put（自动建父目录、内容、覆盖语义、源错误）；
 *          5. 路径逃逸（`..`、绝对、反斜杠、盘符、空段、超长）；
 *          6. 符号链接逃逸（默认拒绝；follow=true 时仍以真实路径判据）；
 *          7. 1MB 文件 Get/Put 往返；
 *          8. 大小上限（Get/Put 均拒绝）；
 *          9. 权限拒绝（EACCES；root 用户跳过）；
 *          10. 目录条目上限；
 *          11. follow_symlinks=true 的根内链接；
 *          12. 空指针/空路径/非法选项。
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

/// 场景 9：权限拒绝（root 用户跳过）。
void TestPermissionDenied() {
    if (::geteuid() == 0) {
        bento_test::Skip("以 root 运行，chmod 000 不能阻止访问，跳过权限用例");
        return;
    }
    const fs::path base = MakeTempDir("perm");
    BENTO_CHECK(!base.empty());
    fs::path root;
    BENTO_CHECK(BuildFixture(base, &root));
    if (base.empty() || root.empty()) {
        return;
    }
    std::error_code ec;
    fs::create_directories(root / "locked", ec);
    BENTO_CHECK(!ec);
    BENTO_CHECK(WriteFile(root / "secret.txt", "secret"));
    BENTO_CHECK(::chmod((root / "locked").c_str(), 0000) == 0);
    BENTO_CHECK(::chmod((root / "secret.txt").c_str(), 0000) == 0);

    const FileBrowser browser(root.string());
    std::vector<FileEntry> entries;
    BENTO_CHECK_EQ(browser.List("locked", &entries), FileBrowserStatus::kPermissionDenied);
    BENTO_CHECK_EQ(browser.Get("secret.txt", (base / "secret.copy").string()),
                   FileBrowserStatus::kPermissionDenied);

    BENTO_CHECK(::chmod((root / "locked").c_str(), 0755) == 0);
    BENTO_CHECK(::chmod((root / "secret.txt").c_str(), 0644) == 0);
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

/// 场景 13：随机路径 fuzz（500 个强制非法 + 200 个合法组合）。
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

    for (int iteration = 0; iteration < 500; ++iteration) {
        std::string path;
        switch (rng() % 5) {
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
            default:
                path = make_valid() + ":x";
                break;
        }
        std::vector<FileEntry> entries;
        BENTO_CHECK_EQ(browser.List(path, &entries), FileBrowserStatus::kPathRejected);
        BENTO_CHECK_EQ(browser.Get(path, local), FileBrowserStatus::kPathRejected);
        BENTO_CHECK_EQ(browser.Put(path, local), FileBrowserStatus::kPathRejected);
    }

    for (int iteration = 0; iteration < 200; ++iteration) {
        std::vector<FileEntry> entries;
        const FileBrowserStatus status = browser.List(make_valid(), &entries);
        BENTO_CHECK(status == FileBrowserStatus::kOk || status == FileBrowserStatus::kNotFound);
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
    return bento_test::Finish("test_file_browser");
}
