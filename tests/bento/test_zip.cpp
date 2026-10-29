/**
 * @file test_zip.cpp
 * @brief MOD-31 store-only ZIP：CRC 向量、单文件/目录往返、1MB 数据、
 *        上限、非法名字、损坏归档、随机树与系统 unzip 交叉验证。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/bento/app/zip.hpp"
#include "test_backup_support.hpp"
#include "test_util.hpp"

namespace {

namespace fs = std::filesystem;
using go2cpp::bento::app::ExtractZip;
using go2cpp::bento::app::ListZip;
using go2cpp::bento::app::ZipLimits;
using go2cpp::bento::app::ZipStatus;
using go2cpp::bento::app::ZipWriter;

/// 等长改写归档内出现的名字（本地头与中央目录各一处）。
std::size_t RewriteName(std::vector<std::uint8_t>* bytes, std::string_view from,
                        std::string_view to) {
    BENTO_CHECK_EQ(from.size(), to.size());
    if (from.size() != to.size()) {
        return 0;
    }
    std::size_t replaced = 0;
    for (std::size_t i = 0; i + from.size() <= bytes->size();) {
        if (std::memcmp(bytes->data() + i, from.data(), from.size()) == 0) {
            std::memcpy(bytes->data() + i, to.data(), to.size());
            ++replaced;
            i += from.size();
        } else {
            ++i;
        }
    }
    return replaced;
}

/// CRC-32/IEEE 的标准测试向量：ASCII "123456789" -> 0xCBF43926。
void TestCrc32KnownVector() {
    bento_test::TempDir tmp("zip_crc");
    const auto zip = tmp / "vector.zip";
    const auto out = tmp / "out";
    const std::string text = "123456789";
    const std::vector<std::uint8_t> data(text.begin(), text.end());
    {
        ZipWriter writer;
        BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddBytes("vector.txt", data.data(), data.size()) == ZipStatus::kOk);
        BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    }
    const auto listing = ListZip(zip.string());
    BENTO_CHECK(listing.ok());
    BENTO_CHECK_EQ(listing.entries.size(), 1U);
    BENTO_CHECK_EQ(listing.entries[0].crc32, 0xCBF43926U);
    BENTO_CHECK_EQ(listing.entries[0].size, data.size());
    const auto result = ExtractZip(zip.string(), out.string());
    BENTO_CHECK(result.ok());
    BENTO_CHECK(bento_test::ReadFile(out / "vector.txt") == data);
}

/// 单文件（含 0..255 全字节）往返 + 中央目录字段。
void TestSingleFileRoundTrip() {
    bento_test::TempDir tmp("zip_single");
    const auto zip = tmp / "single.zip";
    const auto out = tmp / "out";
    std::vector<std::uint8_t> data(256 * 4);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i & 0xffu);
    }
    BENTO_CHECK(bento_test::WriteFile(tmp / "src" / "bytes.bin", data));
    {
        ZipWriter writer;
        BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddFile((tmp / "src" / "bytes.bin").string(), "bytes.bin") ==
                    ZipStatus::kOk);
        BENTO_CHECK(writer.Close() == ZipStatus::kOk);
        BENTO_CHECK_EQ(writer.stats().files, 1U);
        BENTO_CHECK_EQ(writer.stats().input_bytes, data.size());
        BENTO_CHECK_EQ(writer.stats().archive_bytes,
                       static_cast<std::uint64_t>(fs::file_size(zip)));
    }
    const auto listing = ListZip(zip.string());
    BENTO_CHECK(listing.ok());
    BENTO_CHECK_EQ(listing.entries.size(), 1U);
    BENTO_CHECK_EQ(listing.entries[0].name, std::string("bytes.bin"));
    BENTO_CHECK_EQ(listing.stats.files, 1U);
    BENTO_CHECK_EQ(listing.stats.archive_bytes,
                   static_cast<std::uint64_t>(fs::file_size(zip)));
    const auto result = ExtractZip(zip.string(), out.string());
    BENTO_CHECK(result.ok());
    BENTO_CHECK(bento_test::ReadFile(out / "bytes.bin") == data);
}

/// 0 字节文件：CRC 0、大小 0，仍应生成有效条目。
void TestEmptyFile() {
    bento_test::TempDir tmp("zip_empty_file");
    const auto zip = tmp / "empty.zip";
    const auto out = tmp / "out";
    {
        ZipWriter writer;
        BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddBytes("empty.bin", nullptr, 0) == ZipStatus::kOk);
        BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    }
    const auto listing = ListZip(zip.string());
    BENTO_CHECK(listing.ok());
    BENTO_CHECK_EQ(listing.entries.size(), 1U);
    BENTO_CHECK_EQ(listing.entries[0].size, 0U);
    BENTO_CHECK_EQ(listing.entries[0].crc32, 0U);
    const auto result = ExtractZip(zip.string(), out.string());
    BENTO_CHECK(result.ok());
    BENTO_CHECK(fs::exists(out / "empty.bin"));
    BENTO_CHECK_EQ(fs::file_size(out / "empty.bin"), 0U);
}

/// 多层目录、空目录、中文/空格名字与目录条目。
void TestDirectoryRecursive() {
    bento_test::TempDir tmp("zip_tree");
    const auto src = tmp / "src";
    const auto zip = tmp / "tree.zip";
    const auto out = tmp / "out";
    BENTO_CHECK(bento_test::WriteText(src / "a.txt", "alpha"));
    BENTO_CHECK(bento_test::WriteText(src / "nested" / "b.bin", "beta"));
    BENTO_CHECK(bento_test::WriteText(src / "nested" / "deep" / "c.bin", "gamma"));
    BENTO_CHECK(bento_test::WriteText(src / "中文 空格.txt", "unicode"));
    BENTO_CHECK(fs::create_directories(src / "empty"));
    {
        ZipWriter writer;
        BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddDirectory(src.string(), "root") == ZipStatus::kOk);
        BENTO_CHECK(writer.Close() == ZipStatus::kOk);
        BENTO_CHECK_EQ(writer.stats().directories, 4U); // root + empty + nested + deep
        BENTO_CHECK_EQ(writer.stats().files, 4U);
    }
    const auto listing = ListZip(zip.string());
    BENTO_CHECK(listing.ok());
    bool has_empty_directory = false;
    bool has_unicode = false;
    for (const auto& entry : listing.entries) {
        if (entry.name == "root/empty/" && entry.is_directory) {
            has_empty_directory = true;
        }
        if (entry.name == "root/中文 空格.txt") {
            has_unicode = true;
        }
    }
    BENTO_CHECK(has_empty_directory);
    BENTO_CHECK(has_unicode);
    const auto result = ExtractZip(zip.string(), out.string());
    BENTO_CHECK(result.ok());
    std::string error;
    BENTO_CHECK(bento_test::CompareTrees(src, out / "root", &error));
    if (!error.empty()) {
        std::fprintf(stderr, "[detail] %s\n", error.c_str());
    }
    BENTO_CHECK(fs::is_directory(out / "root" / "empty"));
}

/// 1MiB 目录 → zip → 解包逐字节比对，并用系统 unzip 交叉验证。
void TestOneMegabyteRoundTrip() {
    bento_test::TempDir tmp("zip_1m");
    const auto src = tmp / "src";
    const auto zip = tmp / "one_meg.zip";
    const auto out = tmp / "out";
    const auto data = bento_test::RandomBytes(1024 * 1024, 0xabcdef1234567890ull);
    BENTO_CHECK(bento_test::WriteFile(src / "big.bin", data));
    BENTO_CHECK(bento_test::WriteText(src / "readme.txt", "one megabyte backup"));
    {
        ZipWriter writer;
        BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddDirectory(src.string(), "src") == ZipStatus::kOk);
        BENTO_CHECK(writer.Close() == ZipStatus::kOk);
        BENTO_CHECK_EQ(writer.stats().input_bytes, 1024U * 1024U + 19U);
    }
    const auto result = ExtractZip(zip.string(), out.string());
    BENTO_CHECK(result.ok());
    std::string error;
    BENTO_CHECK(bento_test::CompareTrees(src, out / "src", &error));
    BENTO_CHECK(bento_test::ReadFile(out / "src" / "big.bin") == data);
    if (bento_test::HasUnzip()) {
        std::string unzip_error;
        BENTO_CHECK(bento_test::UnzipTest(zip, &unzip_error));
        if (!unzip_error.empty()) {
            std::fprintf(stderr, "[detail] unzip -t: %s\n", unzip_error.c_str());
        }
        bool unzip_ok = false;
        const auto unzipped = bento_test::UnzipRead(zip, "src/big.bin", &unzip_ok);
        BENTO_CHECK(unzip_ok);
        BENTO_CHECK(unzipped == data);
    } else {
        bento_test::Skip("unzip not available, internal reader only");
    }
}

/// 条目数上限在写入前生效，Abort 删除半成品。
void TestLimitsEntryCount() {
    bento_test::TempDir tmp("zip_limit_entries");
    const auto zip = tmp / "limit.zip";
    BENTO_CHECK(bento_test::WriteText(tmp / "a.txt", "a"));
    BENTO_CHECK(bento_test::WriteText(tmp / "b.txt", "b"));
    ZipLimits limits;
    limits.max_entries = 1;
    ZipWriter writer;
    BENTO_CHECK(writer.Open(zip.string(), limits) == ZipStatus::kOk);
    BENTO_CHECK(writer.AddFile((tmp / "a.txt").string(), "a.txt") == ZipStatus::kOk);
    BENTO_CHECK(writer.AddFile((tmp / "b.txt").string(), "b.txt") ==
                ZipStatus::kTooManyEntries);
    writer.Abort();
    BENTO_CHECK(!fs::exists(zip));
}

/// 单条目大小上限。
void TestLimitsEntryBytes() {
    bento_test::TempDir tmp("zip_limit_entry");
    const auto zip = tmp / "limit.zip";
    const auto data = bento_test::RandomBytes(4096, 7);
    BENTO_CHECK(bento_test::WriteFile(tmp / "large.bin", data));
    ZipLimits limits;
    limits.max_entry_bytes = 1024;
    ZipWriter writer;
    BENTO_CHECK(writer.Open(zip.string(), limits) == ZipStatus::kOk);
    BENTO_CHECK(writer.AddFile((tmp / "large.bin").string(), "large.bin") ==
                ZipStatus::kEntryTooLarge);
    writer.Abort();
}

/// 归档总量上限。
void TestLimitsTotalBytes() {
    bento_test::TempDir tmp("zip_limit_total");
    const auto zip = tmp / "limit.zip";
    const auto data = bento_test::RandomBytes(600, 11);
    BENTO_CHECK(bento_test::WriteFile(tmp / "a.bin", data));
    BENTO_CHECK(bento_test::WriteFile(tmp / "b.bin", data));
    ZipLimits limits;
    limits.max_total_bytes = 1024;
    ZipWriter writer;
    BENTO_CHECK(writer.Open(zip.string(), limits) == ZipStatus::kOk);
    BENTO_CHECK(writer.AddFile((tmp / "a.bin").string(), "a.bin") == ZipStatus::kOk);
    BENTO_CHECK(writer.AddFile((tmp / "b.bin").string(), "b.bin") ==
                ZipStatus::kArchiveTooLarge);
    writer.Abort();
}

/// 归档内名字的语法校验。
void TestInvalidEntryNames() {
    bento_test::TempDir tmp("zip_names");
    const auto zip = tmp / "names.zip";
    const std::vector<std::string> invalid{"../escape.txt", "/absolute.txt", "a\\b.txt",
                                           "a//b.txt", "a/../b.txt", "a/./b.txt", ""};
    for (const std::string& name : invalid) {
        ZipWriter writer;
        BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        const ZipStatus status = writer.AddBytes(name, nullptr, 0);
        BENTO_CHECK(status == ZipStatus::kPathRejected);
        writer.Abort();
    }
    ZipWriter writer;
    BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
    BENTO_CHECK(writer.AddBytes("ok.txt", nullptr, 0) == ZipStatus::kOk);
    BENTO_CHECK(writer.AddBytes("ok.txt", nullptr, 0) == ZipStatus::kInvalidArgument);
    writer.Abort();
    ZipWriter valid;
    BENTO_CHECK(valid.Open(zip.string()) == ZipStatus::kOk);
    BENTO_CHECK(valid.AddBytes("ok.txt", nullptr, 0) == ZipStatus::kOk);
    BENTO_CHECK(valid.Close() == ZipStatus::kOk);
}

/// 输入文件不存在。
void TestMissingInput() {
    bento_test::TempDir tmp("zip_missing");
    ZipWriter writer;
    BENTO_CHECK(writer.Open((tmp / "missing.zip").string()) == ZipStatus::kOk);
    BENTO_CHECK(writer.AddFile((tmp / "nope.bin").string(), "nope.bin") ==
                ZipStatus::kInputNotFound);
    writer.Abort();
}

/// 未打开/关闭后的状态。
void TestWriterState() {
    bento_test::TempDir tmp("zip_state");
    ZipWriter writer;
    BENTO_CHECK(writer.AddBytes("a.txt", nullptr, 0) == ZipStatus::kNotOpen);
    BENTO_CHECK(writer.Close() == ZipStatus::kNotOpen);
    BENTO_CHECK(writer.Open((tmp / "state.zip").string()) == ZipStatus::kOk);
    BENTO_CHECK(writer.AddBytes("a.txt", nullptr, 0) == ZipStatus::kOk);
    BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    BENTO_CHECK(writer.Close() == ZipStatus::kAlreadyClosed);
    BENTO_CHECK(writer.closed());
    BENTO_CHECK(!writer.opened());
}

/// 数据字节被篡改 → CRC 校验失败；EOCD 被破坏 → 归档不可解析。
void TestCorruptArchive() {
    bento_test::TempDir tmp("zip_corrupt");
    const auto zip = tmp / "good.zip";
    const auto data = bento_test::RandomBytes(1024, 33);
    {
        ZipWriter writer;
        BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddBytes("data.bin", data.data(), data.size()) == ZipStatus::kOk);
        BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    }
    auto bytes = bento_test::ReadFile(zip);
    BENTO_CHECK(!bytes.empty());
    // 单条目数据紧跟 30B 本地头 + 8B 名字。
    const std::size_t data_offset = 30 + std::string("data.bin").size();
    BENTO_CHECK(data_offset < bytes.size());
    bytes[data_offset] ^= 0xffu;
    const auto bad_crc = tmp / "bad_crc.zip";
    BENTO_CHECK(bento_test::WriteFile(bad_crc, bytes));
    const auto out = tmp / "out";
    const auto result = ExtractZip(bad_crc.string(), out.string());
    BENTO_CHECK(result.status == ZipStatus::kCorruptArchive);
    BENTO_CHECK(!fs::exists(out / "data.bin"));

    auto broken_eocd = bento_test::ReadFile(zip);
    BENTO_CHECK(broken_eocd.size() > 2);
    broken_eocd[broken_eocd.size() - 2] = 0xff;
    broken_eocd[broken_eocd.size() - 1] = 0xff;
    const auto bad_eocd = tmp / "bad_eocd.zip";
    BENTO_CHECK(bento_test::WriteFile(bad_eocd, broken_eocd));
    const auto listing = ListZip(bad_eocd.string());
    BENTO_CHECK(listing.status == ZipStatus::kCorruptArchive);

    auto truncated = bento_test::ReadFile(zip);
    truncated.resize(truncated.size() / 2);
    const auto bad_truncated = tmp / "truncated.zip";
    BENTO_CHECK(bento_test::WriteFile(bad_truncated, truncated));
    BENTO_CHECK(ListZip(bad_truncated.string()).status == ZipStatus::kCorruptArchive);
}

/// 符号链接跳过（不跟随、不进包）。
void TestSymlinkSkipped() {
    bento_test::TempDir tmp("zip_symlink");
    const auto src = tmp / "src";
    const auto zip = tmp / "symlink.zip";
    BENTO_CHECK(bento_test::WriteText(src / "target.txt", "target"));
    if (::symlink("target.txt", (src / "link.txt").c_str()) != 0) {
        bento_test::Skip("symlink not supported here");
        return;
    }
    BENTO_CHECK(::symlink(".", (src / "loop").c_str()) == 0);
    ZipWriter writer;
    BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
    BENTO_CHECK(writer.AddDirectory(src.string(), "src") == ZipStatus::kOk);
    BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    BENTO_CHECK_EQ(writer.stats().symlinks_skipped, 2U);
    BENTO_CHECK_EQ(writer.stats().files, 1U);
    const auto listing = ListZip(zip.string());
    BENTO_CHECK(listing.ok());
    for (const auto& entry : listing.entries) {
        BENTO_CHECK(entry.name != "src/link.txt");
        BENTO_CHECK(entry.name != "src/loop/");
    }
}

/// 中央目录顺序 = 写入顺序；统计正确。
void TestListOrder() {
    bento_test::TempDir tmp("zip_order");
    const auto zip = tmp / "order.zip";
    BENTO_CHECK(bento_test::WriteText(tmp / "first.txt", "1"));
    BENTO_CHECK(bento_test::WriteText(tmp / "second.txt", "2"));
    ZipWriter writer;
    BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
    BENTO_CHECK(writer.AddFile((tmp / "first.txt").string(), "first.txt") == ZipStatus::kOk);
    BENTO_CHECK(writer.AddFile((tmp / "second.txt").string(), "second.txt") == ZipStatus::kOk);
    BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    const auto listing = ListZip(zip.string());
    BENTO_CHECK(listing.ok());
    BENTO_CHECK_EQ(listing.entries.size(), 2U);
    BENTO_CHECK_EQ(listing.entries[0].name, std::string("first.txt"));
    BENTO_CHECK_EQ(listing.entries[1].name, std::string("second.txt"));
}

/// Abort 删除半成品。
void TestAbortRemovesPartial() {
    bento_test::TempDir tmp("zip_abort");
    const auto zip = tmp / "partial.zip";
    ZipWriter writer;
    BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
    BENTO_CHECK(writer.AddBytes("a.bin", nullptr, 0) == ZipStatus::kOk);
    BENTO_CHECK(fs::exists(zip));
    writer.Abort();
    BENTO_CHECK(!fs::exists(zip));
    BENTO_CHECK(!writer.opened());
}

/// 同一路径重复生成：旧条目不会残留。
void TestOverwriteExistingArchive() {
    bento_test::TempDir tmp("zip_overwrite");
    const auto zip = tmp / "same.zip";
    const auto out = tmp / "out";
    {
        ZipWriter first;
        BENTO_CHECK(first.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(first.AddBytes("one.txt", nullptr, 0) == ZipStatus::kOk);
        BENTO_CHECK(first.Close() == ZipStatus::kOk);
    }
    {
        ZipWriter second;
        BENTO_CHECK(second.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(second.AddBytes("two.txt", nullptr, 0) == ZipStatus::kOk);
        BENTO_CHECK(second.Close() == ZipStatus::kOk);
    }
    const auto listing = ListZip(zip.string());
    BENTO_CHECK(listing.ok());
    BENTO_CHECK_EQ(listing.entries.size(), 1U);
    BENTO_CHECK_EQ(listing.entries[0].name, std::string("two.txt"));
    BENTO_CHECK(ExtractZip(zip.string(), out.string()).ok());
    BENTO_CHECK(!fs::exists(out / "one.txt"));
}

/// 随机文件树 10 轮：打包 → 解包 → 全量比对。
void TestRandomTrees() {
    std::uint64_t state = 0x123456789abcdefull;
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (int round = 0; round < 10; ++round) {
        bento_test::TempDir tmp("zip_random_" + std::to_string(round));
        const auto src = tmp / "src";
        const auto zip = tmp / "random.zip";
        const auto out = tmp / "out";
        fs::create_directories(src);
        const int file_count = 1 + static_cast<int>(next() % 5);
        for (int i = 0; i < file_count; ++i) {
            const std::string relative = "d" + std::to_string(next() % 3) + "/f" +
                                         std::to_string(i) + ".bin";
            const std::size_t size = static_cast<std::size_t>(next() % 4096);
            BENTO_CHECK(bento_test::WriteFile(src / relative,
                                              bento_test::RandomBytes(size, next())));
        }
        if ((next() % 2) == 0) {
            fs::create_directories(src / "empty");
        }
        ZipWriter writer;
        BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddDirectory(src.string(), "src") == ZipStatus::kOk);
        BENTO_CHECK(writer.Close() == ZipStatus::kOk);
        const auto result = ExtractZip(zip.string(), out.string());
        BENTO_CHECK(result.ok());
        std::string error;
        BENTO_CHECK(bento_test::CompareTrees(src, out / "src", &error));
        if (!error.empty()) {
            std::fprintf(stderr, "[detail] round %d: %s\n", round, error.c_str());
        }
    }
}

/// 读取端上限：声明条目数超过 max_entries。
void TestReadLimits() {
    bento_test::TempDir tmp("zip_read_limits");
    const auto zip = tmp / "limits.zip";
    {
        ZipWriter writer;
        BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddBytes("a", nullptr, 0) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddBytes("b", nullptr, 0) == ZipStatus::kOk);
        BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    }
    ZipLimits limits;
    limits.max_entries = 1;
    BENTO_CHECK(ListZip(zip.string(), limits).status == ZipStatus::kTooManyEntries);
    // 0 字节条目不计入总量：1 字节上限仍可解压；非零条目的上限语义由
    // TestReadLimitsNonZero 覆盖。
    limits.max_entries = 16;
    limits.max_total_bytes = 1;
    limits.max_entry_bytes = 1;
    BENTO_CHECK(ExtractZip(zip.string(), (tmp / "out").string(), limits).ok());
    ZipLimits tiny;
    tiny.max_entry_bytes = 0;
    BENTO_CHECK(ListZip(zip.string(), tiny).status == ZipStatus::kInvalidArgument);
}

/// T-01：读取端 Zip Slip——等长改写归档内名字，List/Extract 都必须拒绝。
void TestReadZipSlipRejected() {
    bento_test::TempDir tmp("zip_slip");
    const auto zip = tmp / "slip.zip";
    const auto out = tmp / "out";
    const std::string original = "ab/c.t";
    const std::vector<std::uint8_t> data = {'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    {
        ZipWriter writer;
        BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddBytes(original, data.data(), data.size()) == ZipStatus::kOk);
        BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    }
    const auto bytes = bento_test::ReadFile(zip);
    BENTO_CHECK(!bytes.empty());
    const std::vector<std::string> evil = {"../x.t", "/tmp/x", "a\\b.tx"};
    for (const std::string& name : evil) {
        std::vector<std::uint8_t> mutated = bytes;
        BENTO_CHECK_EQ(RewriteName(&mutated, original, name), std::size_t{2});
        const auto evil_zip = tmp / "slip_evil.zip";
        BENTO_CHECK(bento_test::WriteFile(evil_zip, mutated));
        const auto listing = ListZip(evil_zip.string());
        BENTO_CHECK_EQ(listing.status, ZipStatus::kPathRejected);
        BENTO_CHECK(!listing.error.empty());
        const auto result = ExtractZip(evil_zip.string(), out.string());
        BENTO_CHECK_EQ(result.status, ZipStatus::kPathRejected);
        BENTO_CHECK(!result.error.empty());
        BENTO_CHECK(!fs::exists(tmp / "x.t"));
        BENTO_CHECK(!fs::exists(out / "x.t"));
        BENTO_CHECK(!fs::exists(out));
    }
}

/// T-02：读取端非零条目触发单条/总量上限，且在落盘前拒绝。
void TestReadLimitsNonZero() {
    bento_test::TempDir tmp("zip_read_limits_real");
    const auto zip = tmp / "real.zip";
    const auto out = tmp / "out";
    const auto small = bento_test::RandomBytes(600, 0x51);
    const auto large = bento_test::RandomBytes(2048, 0x52);
    {
        ZipWriter writer;
        BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddBytes("a.bin", small.data(), small.size()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddBytes("b.bin", small.data(), small.size()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddBytes("c.bin", large.data(), large.size()) == ZipStatus::kOk);
        BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    }
    ZipLimits total;
    total.max_total_bytes = 1024;
    BENTO_CHECK_EQ(ListZip(zip.string(), total).status, ZipStatus::kArchiveTooLarge);
    BENTO_CHECK_EQ(ExtractZip(zip.string(), out.string(), total).status,
                   ZipStatus::kArchiveTooLarge);
    BENTO_CHECK(!fs::exists(out));

    ZipLimits entry;
    entry.max_entry_bytes = 1024;
    BENTO_CHECK_EQ(ListZip(zip.string(), entry).status, ZipStatus::kEntryTooLarge);
    const auto extracted = ExtractZip(zip.string(), out.string(), entry);
    BENTO_CHECK_EQ(extracted.status, ZipStatus::kEntryTooLarge);
    BENTO_CHECK(!extracted.error.empty());
    BENTO_CHECK(!fs::exists(out));
}


/// T-03：条目数硬上限是 65534（0xFFFF 是 ZIP64 哨兵）。
/// 写 65535 条会生成一个连自己都读不回来的归档，所以必须在 Open 处拒绝。
void TestEntryCeiling() {
    bento_test::TempDir tmp("zip_ceiling");
    BENTO_CHECK_EQ(go2cpp::bento::app::kZipMaxEntriesCeiling, std::size_t{65534});
    ZipLimits too_many;
    too_many.max_entries = 65535;
    ZipWriter rejected;
    BENTO_CHECK_EQ(rejected.Open((tmp / "too_many.zip").string(), too_many),
                   ZipStatus::kInvalidArgument);
    BENTO_CHECK(!fs::exists(tmp / "too_many.zip"));
    ZipLimits maximal;
    maximal.max_entries = go2cpp::bento::app::kZipMaxEntriesCeiling;
    ZipWriter writer;
    BENTO_CHECK_EQ(writer.Open((tmp / "max.zip").string(), maximal), ZipStatus::kOk);
    writer.Abort();
}

/// T-04：输出目录里预置指向外部的符号链接，解包必须拒绝（ResolveOutputPath）。
void TestExtractSymlinkEscape() {
    bento_test::TempDir tmp("zip_symlink_escape");
    const auto zip = tmp / "link.zip";
    const std::vector<std::uint8_t> payload = {'p', 'w', 'n'};
    {
        ZipWriter writer;
        BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddBytes("link/pwn.txt", payload.data(), payload.size()) ==
                    ZipStatus::kOk);
        BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    }
    const auto outside = tmp / "outside";
    const auto out = tmp / "out";
    BENTO_CHECK(fs::create_directories(outside));
    BENTO_CHECK(fs::create_directories(out));
    if (::symlink(outside.c_str(), (out / "link").c_str()) != 0) {
        bento_test::Skip("symlink not supported here");
        return;
    }
    const auto result = ExtractZip(zip.string(), out.string());
    BENTO_CHECK_EQ(result.status, ZipStatus::kPathRejected);
    BENTO_CHECK(!result.error.empty());
    BENTO_CHECK(!fs::exists(outside / "pwn.txt"));
}

/// T-05：FIFO 等特殊文件跳过并计数，不阻塞、不进包。
void TestSpecialFileSkipped() {
    bento_test::TempDir tmp("zip_fifo");
    const auto src = tmp / "src";
    BENTO_CHECK(bento_test::WriteText(src / "normal.txt", "x"));
    if (::mkfifo((src / "pipe").c_str(), 0600) != 0) {
        bento_test::Skip("mkfifo not supported here");
        return;
    }
    ZipWriter writer;
    BENTO_CHECK(writer.Open((tmp / "fifo.zip").string()) == ZipStatus::kOk);
    BENTO_CHECK(writer.AddDirectory(src.string(), "src") == ZipStatus::kOk);
    BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    BENTO_CHECK_EQ(writer.stats().special_skipped, 1U);
    BENTO_CHECK_EQ(writer.stats().files, 1U);
    const auto listing = ListZip((tmp / "fifo.zip").string());
    BENTO_CHECK(listing.ok());
    for (const auto& entry : listing.entries) {
        BENTO_CHECK(entry.name != "src/pipe");
    }
}

/// T-06：EOCD 完好但条目数据区被删掉一段 → 解析拒绝，且输出目录不残留。
void TestTruncatedEntryData() {
    bento_test::TempDir tmp("zip_truncated_data");
    const auto zip = tmp / "data.zip";
    const auto data = bento_test::RandomBytes(4096, 9);
    {
        ZipWriter writer;
        BENTO_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        BENTO_CHECK(writer.AddBytes("data.bin", data.data(), data.size()) == ZipStatus::kOk);
        BENTO_CHECK(writer.Close() == ZipStatus::kOk);
    }
    auto bytes = bento_test::ReadFile(zip);
    BENTO_CHECK(!bytes.empty());
    const std::size_t header = 30 + std::string("data.bin").size();
    BENTO_CHECK(header + data.size() < bytes.size());
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(header),
                bytes.begin() + static_cast<std::ptrdiff_t>(header + data.size() / 2));
    const auto broken = tmp / "broken_data.zip";
    BENTO_CHECK(bento_test::WriteFile(broken, bytes));
    BENTO_CHECK(ListZip(broken.string()).status != ZipStatus::kOk);
    const auto out = tmp / "out";
    const auto result = ExtractZip(broken.string(), out.string());
    BENTO_CHECK(result.status != ZipStatus::kOk);
    BENTO_CHECK(!result.error.empty());
    BENTO_CHECK(!fs::exists(out));
}

} // namespace

int main() {
    TestCrc32KnownVector();
    TestSingleFileRoundTrip();
    TestEmptyFile();
    TestDirectoryRecursive();
    TestOneMegabyteRoundTrip();
    TestLimitsEntryCount();
    TestLimitsEntryBytes();
    TestLimitsTotalBytes();
    TestInvalidEntryNames();
    TestMissingInput();
    TestWriterState();
    TestCorruptArchive();
    TestSymlinkSkipped();
    TestListOrder();
    TestAbortRemovesPartial();
    TestOverwriteExistingArchive();
    TestRandomTrees();
    TestReadLimits();
    TestReadZipSlipRejected();
    TestReadLimitsNonZero();
    TestEntryCeiling();
    TestExtractSymlinkEscape();
    TestSpecialFileSkipped();
    TestTruncatedEntryData();
    return bento_test::Finish("test_zip");
}
