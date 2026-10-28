/**
 * @file test_backup.cpp
 * @brief MOD-31 Backup：1MiB 目录打包/解包比对、多路径、输出自排除、
 *        空目录、失败保留旧包、重名、符号链接、上限与 16MiB 吞吐。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/app/backup.hpp"
#include "go2cpp/bento/app/zip.hpp"
#include "test_backup_support.hpp"
#include "test_util.hpp"

namespace {

namespace fs = std::filesystem;
using go2cpp::bento::app::Backup;
using go2cpp::bento::app::BackupStatus;
using go2cpp::bento::app::ExtractZip;
using go2cpp::bento::app::ListZip;
using go2cpp::bento::app::ZipLimits;

/// 构造一个约 1MiB 的目录树。
fs::path MakeOneMegabyteTree(const fs::path& root) {
    const auto source = root / "source";
    BENTO_CHECK(bento_test::WriteFile(source / "a.bin", bento_test::RandomBytes(256 * 1024, 1)));
    BENTO_CHECK(bento_test::WriteFile(source / "sub" / "b.bin",
                                      bento_test::RandomBytes(256 * 1024, 2)));
    BENTO_CHECK(bento_test::WriteFile(source / "sub" / "deep" / "c.bin",
                                      bento_test::RandomBytes(256 * 1024, 3)));
    BENTO_CHECK(bento_test::WriteFile(source / "sub" / "deep" / "d.bin",
                                      bento_test::RandomBytes(256 * 1024, 4)));
    BENTO_CHECK(bento_test::WriteText(source / "notes.txt", "mod31 backup"));
    return source;
}

/// 1MiB 目录：Backup → Extract → 逐字节比对，并用系统 unzip 交叉验证。
void TestBackupOneMegabyteTree() {
    bento_test::TempDir tmp("backup_1m");
    const fs::path source = MakeOneMegabyteTree(tmp.path());
    const auto zip = tmp / "backup.zip";
    const auto out = tmp / "out";
    const auto result = Backup({source.string()}, zip.string());
    BENTO_CHECK(result.ok());
    BENTO_CHECK_EQ(result.status, BackupStatus::kOk);
    BENTO_CHECK_EQ(result.stats.files, 5U);
    BENTO_CHECK_EQ(result.stats.directories, 3U); // source + sub + deep
    BENTO_CHECK_EQ(result.stats.input_bytes, 1024U * 1024U + 12U);
    BENTO_CHECK_EQ(result.stats.archive_bytes,
                   static_cast<std::uint64_t>(fs::file_size(zip)));
    BENTO_CHECK_EQ(result.archive_path, zip.string());
    BENTO_CHECK(fs::exists(result.archive_path));
    BENTO_CHECK(result.error.empty());
    BENTO_CHECK(fs::exists(zip));
    BENTO_CHECK(!fs::exists(zip.string() + ".part"));
    const auto extracted = ExtractZip(zip.string(), out.string());
    BENTO_CHECK(extracted.ok());
    const std::string name = source.filename().string();
    std::string error;
    BENTO_CHECK(bento_test::CompareTrees(source, out / name, &error));
    if (!error.empty()) {
        std::fprintf(stderr, "[detail] %s\n", error.c_str());
    }
    if (bento_test::HasUnzip()) {
        std::string unzip_error;
        BENTO_CHECK(bento_test::UnzipTest(zip, &unzip_error));
        if (!unzip_error.empty()) {
            std::fprintf(stderr, "[detail] unzip -t: %s\n", unzip_error.c_str());
        }
    }
}

/// 多个输入路径：目录 + 独立文件，条目名取 basename。
void TestBackupMultiplePaths() {
    bento_test::TempDir tmp("backup_multi");
    const auto alpha = tmp / "alpha";
    BENTO_CHECK(bento_test::WriteText(alpha / "a.txt", "alpha"));
    const auto beta = tmp / "beta.txt";
    BENTO_CHECK(bento_test::WriteText(beta, "beta"));
    const auto zip = tmp / "multi.zip";
    const auto result = Backup({alpha.string(), beta.string()}, zip.string());
    BENTO_CHECK(result.ok());
    BENTO_CHECK_EQ(result.stats.files, 2U);
    const auto listing = ListZip(zip.string());
    BENTO_CHECK(listing.ok());
    bool has_alpha = false;
    bool has_beta = false;
    for (const auto& entry : listing.entries) {
        if (entry.name == "alpha/a.txt") {
            has_alpha = true;
        }
        if (entry.name == "beta.txt") {
            has_beta = true;
        }
    }
    BENTO_CHECK(has_alpha);
    BENTO_CHECK(has_beta);
    const auto out = tmp / "out";
    BENTO_CHECK(ExtractZip(zip.string(), out.string()).ok());
    BENTO_CHECK(bento_test::ReadFile(out / "alpha" / "a.txt") ==
                bento_test::ReadFile(alpha / "a.txt"));
    BENTO_CHECK(bento_test::ReadFile(out / "beta.txt") == bento_test::ReadFile(beta));
}

/// 输出 zip 位于输入目录内：不能把自己打进归档。
void TestBackupExcludesOutput() {
    bento_test::TempDir tmp("backup_self");
    const auto dir = tmp / "dir";
    BENTO_CHECK(bento_test::WriteText(dir / "keep.txt", "v1"));
    const auto zip = dir / "backup.zip";
    const auto first = Backup({dir.string()}, zip.string());
    BENTO_CHECK(first.ok());
    const std::uint64_t first_size = first.stats.archive_bytes;
    BENTO_CHECK(bento_test::WriteText(dir / "keep.txt", "v2"));
    const auto second = Backup({dir.string()}, zip.string());
    BENTO_CHECK(second.ok());
    BENTO_CHECK(second.stats.archive_bytes < first_size + 4096);
    const auto listing = ListZip(zip.string());
    BENTO_CHECK(listing.ok());
    for (const auto& entry : listing.entries) {
        BENTO_CHECK(entry.name != "dir/backup.zip");
        BENTO_CHECK(entry.name != "dir/backup.zip.part");
    }
    const auto out = tmp / "out";
    BENTO_CHECK(ExtractZip(zip.string(), out.string()).ok());
    BENTO_CHECK(!fs::exists(out / "dir" / "backup.zip"));
    BENTO_CHECK(bento_test::ReadFile(out / "dir" / "keep.txt") ==
                bento_test::ReadFile(dir / "keep.txt"));
}

/// 空目录仍生成有效归档与目录条目。
void TestBackupEmptyDirectory() {
    bento_test::TempDir tmp("backup_empty");
    const auto empty = tmp / "empty_dir";
    BENTO_CHECK(fs::create_directories(empty));
    const auto zip = tmp / "empty.zip";
    const auto result = Backup({empty.string()}, zip.string());
    BENTO_CHECK(result.ok());
    BENTO_CHECK_EQ(result.stats.files, 0U);
    BENTO_CHECK_EQ(result.stats.directories, 1U);
    const auto listing = ListZip(zip.string());
    BENTO_CHECK(listing.ok());
    BENTO_CHECK_EQ(listing.entries.size(), 1U);
    BENTO_CHECK(listing.entries[0].is_directory);
    const auto out = tmp / "out";
    BENTO_CHECK(ExtractZip(zip.string(), out.string()).ok());
    BENTO_CHECK(fs::is_directory(out / empty.filename()));
    BENTO_CHECK(fs::is_empty(out / empty.filename()));
}

/// 失败时旧包必须原样保留，且不留下 .part。
void TestBackupFailureKeepsOldArchive() {
    bento_test::TempDir tmp("backup_keep");
    const auto source = tmp / "src";
    BENTO_CHECK(bento_test::WriteText(source / "a.txt", "keep me"));
    const auto zip = tmp / "keep.zip";
    const auto first = Backup({source.string()}, zip.string());
    BENTO_CHECK(first.ok());
    const auto before = bento_test::ReadFile(zip);
    const auto missing = tmp / "does_not_exist";
    const auto failed = Backup({missing.string()}, zip.string());
    BENTO_CHECK(!failed.ok());
    BENTO_CHECK_EQ(failed.status, BackupStatus::kInputNotFound);
    BENTO_CHECK(!failed.error.empty());
    BENTO_CHECK(bento_test::ReadFile(zip) == before);
    BENTO_CHECK(!fs::exists(zip.string() + ".part"));
}

/// 两个不同目录下的同名文件：顶层名字冲突，明确失败。
void TestBackupDuplicateBasenames() {
    bento_test::TempDir tmp("backup_dup");
    const auto first = tmp / "a" / "same.txt";
    const auto second = tmp / "b" / "same.txt";
    BENTO_CHECK(bento_test::WriteText(first, "a"));
    BENTO_CHECK(bento_test::WriteText(second, "b"));
    const auto result = Backup({first.string(), second.string()}, (tmp / "dup.zip").string());
    BENTO_CHECK(!result.ok());
    BENTO_CHECK_EQ(result.status, BackupStatus::kInvalidArgument);
}

/// 符号链接被跳过并计数。
void TestBackupSymlinkSkipped() {
    bento_test::TempDir tmp("backup_symlink");
    const auto source = tmp / "src";
    BENTO_CHECK(bento_test::WriteText(source / "target.txt", "target"));
    if (::symlink("target.txt", (source / "link.txt").c_str()) != 0) {
        bento_test::Skip("symlink not supported here");
        return;
    }
    const auto zip = tmp / "symlink.zip";
    const auto result = Backup({source.string()}, zip.string());
    BENTO_CHECK(result.ok());
    BENTO_CHECK_EQ(result.stats.symlinks_skipped, 1U);
    BENTO_CHECK_EQ(result.stats.files, 1U);
    const auto listing = ListZip(zip.string());
    BENTO_CHECK(listing.ok());
    for (const auto& entry : listing.entries) {
        BENTO_CHECK(entry.name != source.filename().string() + "/link.txt");
    }
}

/// 上限透传：目录条目数超限 → kTooLarge。
void TestBackupLimits() {
    bento_test::TempDir tmp("backup_limits");
    const auto source = tmp / "src";
    BENTO_CHECK(bento_test::WriteText(source / "a.txt", "a"));
    BENTO_CHECK(bento_test::WriteText(source / "b.txt", "b"));
    ZipLimits limits;
    limits.max_entries = 1;
    const auto result = Backup({source.string()}, (tmp / "limit.zip").string(), limits);
    BENTO_CHECK(!result.ok());
    BENTO_CHECK_EQ(result.status, BackupStatus::kTooLarge);
    BENTO_CHECK(!fs::exists(tmp / "limit.zip"));
    BENTO_CHECK(!fs::exists(std::string(tmp / "limit.zip") + ".part"));
}

/// 输入不存在 / 输出是目录 / 输入输出相同。
void TestBackupInvalidArguments() {
    bento_test::TempDir tmp("backup_invalid");
    const auto missing = Backup({(tmp / "nope").string()}, (tmp / "x.zip").string());
    BENTO_CHECK_EQ(missing.status, BackupStatus::kInputNotFound);

    const auto directory = tmp / "outdir";
    BENTO_CHECK(fs::create_directories(directory));
    const auto output_is_dir = Backup({(tmp / "nope").string()}, directory.string());
    BENTO_CHECK_EQ(output_is_dir.status, BackupStatus::kInvalidArgument);

    const auto same = tmp / "same.zip";
    BENTO_CHECK(bento_test::WriteText(same, "not a zip"));
    const auto same_result = Backup({same.string()}, same.string());
    BENTO_CHECK_EQ(same_result.status, BackupStatus::kInvalidArgument);

    const auto empty_list = Backup({}, (tmp / "empty.zip").string());
    BENTO_CHECK_EQ(empty_list.status, BackupStatus::kInvalidArgument);
}

/// T-07：archive_path 精确回填；输出父目录不存在时自动创建。
void TestArchivePathAndNestedOutput() {
    bento_test::TempDir tmp("backup_archive_path");
    const auto source = tmp / "src";
    BENTO_CHECK(bento_test::WriteText(source / "only.txt", "nested output"));
    const fs::path zip = tmp / "nested" / "deep" / "backup.zip";
    const auto result = Backup({source.string()}, zip.string());
    BENTO_CHECK(result.ok());
    BENTO_CHECK_EQ(result.archive_path, zip.string());
    BENTO_CHECK(result.error.empty());
    BENTO_CHECK(fs::exists(result.archive_path));
    BENTO_CHECK_EQ(result.stats.files, 1U);
    BENTO_CHECK_EQ(result.stats.archive_bytes,
                   static_cast<std::uint64_t>(fs::file_size(zip)));
}

/// 16MiB 压测：打印耗时与吞吐（只做宽松下限断言，避免 CI 抖动误报）。
void TestBackupThroughput() {
    bento_test::TempDir tmp("backup_perf");
    const auto source = tmp / "perf";
    constexpr std::size_t kFileBytes = 4 * 1024 * 1024;
    constexpr int kFiles = 4;
    for (int i = 0; i < kFiles; ++i) {
        BENTO_CHECK(bento_test::WriteFile(source / ("part" + std::to_string(i) + ".bin"),
                                          bento_test::RandomBytes(kFileBytes, 100 + i)));
    }
    const auto zip = tmp / "perf.zip";
    const auto started = std::chrono::steady_clock::now();
    const auto result = Backup({source.string()}, zip.string());
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
    BENTO_CHECK(result.ok());
    const std::uint64_t expected = static_cast<std::uint64_t>(kFileBytes) * kFiles;
    BENTO_CHECK_EQ(result.stats.input_bytes, expected);
    const double seconds = elapsed.count() > 0.0 ? elapsed.count() : 0.001;
    const double mb_per_second = static_cast<double>(expected) / (1024.0 * 1024.0) / seconds;
    std::printf("[perf] backup 16MiB: %.3fs, %.1f MiB/s\n", elapsed.count(), mb_per_second);
    BENTO_CHECK(mb_per_second > 1.0);
}

/// D-06：临时包名唯一——预置的旧版固定 `.part` 哨兵不受影响。
void TestBackupUniqueTempName() {
    bento_test::TempDir tmp("backup_unique");
    const auto source = tmp / "src";
    BENTO_CHECK(bento_test::WriteText(source / "a.txt", "unique"));
    const auto zip = tmp / "unique.zip";
    const fs::path legacy = tmp / "unique.zip.part";
    BENTO_CHECK(bento_test::WriteText(legacy, "legacy-sentinel"));
    const auto result = Backup({source.string()}, zip.string());
    BENTO_CHECK(result.ok());
    const auto legacy_bytes = bento_test::ReadFile(legacy);
    BENTO_CHECK_EQ(std::string(legacy_bytes.begin(), legacy_bytes.end()),
                   std::string("legacy-sentinel"));
}

/// D-06：同一输出路径的并发 Backup 各用唯一临时包，全部成功且最终包有效。
void TestBackupConcurrentSameTarget() {
    bento_test::TempDir tmp("backup_concurrent");
    const auto source = tmp / "src";
    const auto payload = bento_test::RandomBytes(1024 * 1024, 7);
    BENTO_CHECK(bento_test::WriteFile(source / "a.bin", payload));
    const auto zip = tmp / "concurrent.zip";

    constexpr int kThreads = 4;
    std::vector<BackupStatus> statuses(kThreads, BackupStatus::kInvalidArgument);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&, index]() {
            statuses[static_cast<std::size_t>(index)] =
                Backup({source.string()}, zip.string()).status;
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (const BackupStatus status : statuses) {
        BENTO_CHECK_EQ(status, BackupStatus::kOk);
    }
    const auto listing = ListZip(zip.string());
    BENTO_CHECK(listing.ok());
    const auto out = tmp / "out";
    BENTO_CHECK(ExtractZip(zip.string(), out.string()).ok());
    BENTO_CHECK(bento_test::ReadFile(out / "src" / "a.bin") ==
                bento_test::ReadFile(source / "a.bin"));
    for (const auto& entry : fs::directory_iterator(tmp.path())) {
        BENTO_CHECK(entry.path().filename().string().find(".part.") == std::string::npos);
    }
}

} // namespace

int main() {
    TestBackupOneMegabyteTree();
    TestBackupMultiplePaths();
    TestBackupExcludesOutput();
    TestBackupEmptyDirectory();
    TestBackupFailureKeepsOldArchive();
    TestBackupDuplicateBasenames();
    TestBackupSymlinkSkipped();
    TestBackupLimits();
    TestBackupInvalidArguments();
    TestArchivePathAndNestedOutput();
    TestBackupThroughput();
    TestBackupUniqueTempName();
    TestBackupConcurrentSameTarget();
    return bento_test::Finish("test_backup");
}
