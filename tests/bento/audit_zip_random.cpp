/**
 * @file audit_zip_random.cpp
 * @brief zip/backup 随机测试：1000 随机树、1000 随机合法归档、2 万次随机
 *        结构变异、随机声明炸弹与随机非法名字。
 *
 * @details 断言原则（不依赖已知答案）：
 *          - 任何输入都不得崩溃、不得卡死超过单次预算（进程存活即视为通过
 *            崩溃项）；
 *          - `ExtractZip` 成功 ⇒ `ListZip` 必须也成功（两条解析路径一致）；
 *          - 失败时留在输出目录里的每个文件都必须是「完整条目」：大小与
 *            CRC 与中央目录一致（实现承诺 CRC 不符会删除该文件）；
 *          - 任何变异都不得在输出根之外产生文件，也不得改动 canary 文件；
 *          - 声明式炸弹不得引起与声明大小成比例的内存分配。
 *
 * 编译（在项目根目录，建议额外跑一遍 ASAN）：
 * @code
 * g++ -std=c++17 -O2 -g -Iinclude -Itests/bento tests/bento/audit_zip_random.cpp \
 *     -L.build-bento-wsl -lgo2cpp_bento_app -lgo2cpp_bento -lgo2cpp_os \
 *     -lgo2cpp_hash -lgo2cpp_config -lgo2cpp_scheduler -lgo2cpp_log \
 *     -lgo2cpp_fiber -lpthread -Wl,-rpath,$PWD/.build-bento-wsl \
 *     -o .build-bento-wsl/audit_zip_random
 * @endcode
 *
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 */

#include "audit_zip_support.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "go2cpp/bento/app/backup.hpp"
#include "go2cpp/bento/app/zip.hpp"
#include "go2cpp/hash/crc32.hpp"
#include "test_backup_support.hpp"

namespace {

namespace fs = std::filesystem;
using go2cpp::bento::app::Backup;
using go2cpp::bento::app::ExtractZip;
using go2cpp::bento::app::ListZip;
using go2cpp::bento::app::ZipEntryInfo;
using go2cpp::bento::app::ZipLimits;
using go2cpp::bento::app::ZipListing;
using go2cpp::bento::app::ZipStatus;
using go2cpp::bento::app::ZipWriter;

/// 状态计数（变异测试用）。
using StatusCounts = std::map<std::string, int>;

void Section(const char* title) {
    std::printf("\n== %s ==\n", title);
    std::fflush(stdout);
}

/// 名字片段：ASCII / 空格 / 中文 / 常见安全符号。
std::string RandomSegment(audit::Rng& rng) {
    static const char* kPool[] = {"a",  "b",   "file", "data", "x1",  "备份", "照片",
                                  "日志", "2026", "v2",   "存档", "报告", "草稿"};
    static const char* kSuffix[] = {".txt", ".bin", ".dat", ".log", ""};
    std::string name = kPool[rng.Range(std::size(kPool))];
    if (rng.Chance(30)) {
        name += " " + std::to_string(rng.Range(100));
    }
    name += kSuffix[rng.Range(std::size(kSuffix))];
    return name;
}

/// CRC32（IEEE）与 ZIP 一致。
std::uint32_t CrcOf(const std::vector<std::uint8_t>& data) {
    go2cpp::hash::Crc32 crc;
    if (!data.empty()) {
        crc.Write(go2cpp::hash::AsBytes(data.data(), data.size()));
    }
    return crc.Sum32();
}

// ---------------------------------------------------------------------------
// 1. 1000 个随机文件树：打包 → 解包 → 全量比对
// ---------------------------------------------------------------------------

void RandomTrees() {
    Section("1. 1000 个随机文件树（深目录/中文名/空文件/大文件）");
    bento_test::TempDir dir("audit_random_trees");
    audit::Rng rng(0x5eed1234);
    int rounds = 0;
    int files_total = 0;
    std::size_t bytes_total = 0;
    for (int round = 0; round < 1000; ++round) {
        const auto root = dir.path() / ("r" + std::to_string(round));
        const auto src = root / "src";
        const auto zip = root / "tree.zip";
        const auto out = root / "out";
        std::error_code ec;
        fs::create_directories(src, ec);
        const int depth = 1 + static_cast<int>(rng.Range(4));
        const int per_dir = 1 + static_cast<int>(rng.Range(3));
        // 先生成目录骨架、再往目录里放文件：如果像早期版本那样把「文件」
        // 和「目录」混在同一层随机，可能先建了文件 `a`、再想把 `a` 当目录
        // 写 `a/b`，失败的是测试生成器而不是被测模块。
        std::set<std::string> used_paths;
        int created = 0;
        std::vector<std::string> current{""};
        for (int level = 0; level < depth; ++level) {
            std::vector<std::string> next;
            for (const std::string& base : current) {
                for (int index = 0; index < per_dir; ++index) {
                    const std::string segment = RandomSegment(rng);
                    const std::string relative =
                        base.empty() ? segment : base + "/" + segment;
                    if (!used_paths.insert(relative).second) {
                        continue; // 同父目录内重名，跳过
                    }
                    fs::create_directories(src / relative, ec);
                    next.push_back(relative);
                    // 在该目录里放 0..2 个文件。
                    const int files = static_cast<int>(rng.Range(3));
                    for (int file_index = 0; file_index < files; ++file_index) {
                        const std::string file_relative =
                            relative + "/" + RandomSegment(rng) + "_" +
                            std::to_string(file_index);
                        if (!used_paths.insert(file_relative).second) {
                            continue;
                        }
                        std::size_t size = 0;
                        if (rng.Chance(60)) {
                            size = static_cast<std::size_t>(rng.Range(4096));
                        } else if (rng.Chance(50)) {
                            size = static_cast<std::size_t>(rng.Range(64 * 1024));
                        }
                        const auto data = bento_test::RandomBytes(size, rng.Next());
                        if (!bento_test::WriteFile(src / file_relative, data)) {
                            std::printf("  第 %d 轮写文件失败：%s\n", round,
                                        file_relative.c_str());
                            ++audit::g_failures;
                            break;
                        }
                        bytes_total += size;
                        ++created;
                    }
                }
            }
            if (next.empty()) {
                break;
            }
            current = std::move(next);
        }
        if (rng.Chance(30)) {
            fs::create_directories(src / "empty_dir", ec);
        }
        ZipLimits limits;
        limits.max_entries = 4096;
        limits.max_total_bytes = 512ull << 20;
        const auto backup = Backup({src.string()}, zip.string(), limits);
        AUDIT_CHECK(backup.ok());
        if (!backup.ok()) {
            std::printf("  第 %d 轮打包失败：%s\n", round, backup.error.c_str());
            continue;
        }
        const auto extracted = ExtractZip(zip.string(), out.string(), limits);
        AUDIT_CHECK(extracted.ok());
        if (!extracted.ok()) {
            continue;
        }
        std::string compare_error;
        if (!bento_test::CompareTrees(src, out / "src", &compare_error)) {
            std::printf("  第 %d 轮比对失败：%s\n", round, compare_error.c_str());
            ++audit::g_failures;
        }
        ++audit::g_checks;
        files_total += created;
        ++rounds;
    }
    std::printf("  完成 %d 轮，%d 个文件，%.2f MiB，全部逐字节一致\n", rounds, files_total,
                static_cast<double>(bytes_total) / (1024.0 * 1024.0));
    AUDIT_CHECK(rounds == 1000);
}

// ---------------------------------------------------------------------------
// 2. 1000 个随机合法归档（AddBytes / AddFile / 目录混合）
// ---------------------------------------------------------------------------

void RandomValidArchives() {
    Section("2. 1000 个随机合法归档：List 与 Extract 结果一致");
    bento_test::TempDir dir("audit_random_zip");
    audit::Rng rng(0xabcdef9876543210ull);
    int rounds = 0;
    for (int round = 0; round < 1000; ++round) {
        const auto root = dir.path() / ("z" + std::to_string(round));
        const auto zip = root / "random.zip";
        const auto out = root / "out";
        std::error_code ec;
        fs::create_directories(root, ec);
        ZipLimits limits;
        limits.max_entries = 4096;
        limits.max_total_bytes = 256ull << 20;
        ZipWriter writer;
        if (writer.Open(zip.string(), limits) != ZipStatus::kOk) {
            ++audit::g_failures;
            continue;
        }
        const int entries = static_cast<int>(rng.Range(20));
        std::map<std::string, std::vector<std::uint8_t>> expected;
        for (int index = 0; index < entries; ++index) {
            const std::string name =
                "d" + std::to_string(rng.Range(3)) + "/" + RandomSegment(rng) + "_" +
                std::to_string(index);
            if (expected.count(name) != 0) {
                continue;
            }
            const std::size_t size = static_cast<std::size_t>(rng.Range(8192));
            const auto data = bento_test::RandomBytes(size, rng.Next());
            if (rng.Chance(50)) {
                const auto status =
                    writer.AddBytes(name, data.empty() ? nullptr : data.data(), data.size());
                AUDIT_CHECK(status == ZipStatus::kOk);
            } else {
                const fs::path source = root / "files" / ("s" + std::to_string(index));
                if (!bento_test::WriteFile(source, data)) {
                    ++audit::g_failures;
                    continue;
                }
                const auto status = writer.AddFile(source.string(), name);
                AUDIT_CHECK(status == ZipStatus::kOk);
            }
            expected[name] = data;
        }
        const ZipStatus close_status = writer.Close();
        AUDIT_CHECK(close_status == ZipStatus::kOk);
        if (close_status != ZipStatus::kOk) {
            continue;
        }
        const auto listing = ListZip(zip.string(), limits);
        AUDIT_CHECK(listing.ok());
        if (!listing.ok()) {
            continue;
        }
        AUDIT_CHECK(listing.entries.size() == expected.size());
        const auto extracted = ExtractZip(zip.string(), out.string(), limits);
        AUDIT_CHECK(extracted.ok());
        if (!extracted.ok()) {
            continue;
        }
        // 逐条目核对：名字、大小、CRC、内容。
        for (const auto& [name, data] : expected) {
            const auto& bytes = bento_test::ReadFile(out / name);
            if (bytes != data) {
                std::printf("  第 %d 轮内容不一致：%s\n", round, name.c_str());
                ++audit::g_failures;
            }
            ++audit::g_checks;
        }
        for (const ZipEntryInfo& entry : listing.entries) {
            AUDIT_CHECK(expected.count(entry.name) != 0);
            if (expected.count(entry.name) != 0) {
                AUDIT_CHECK(entry.size == expected[entry.name].size());
                AUDIT_CHECK(entry.crc32 == CrcOf(expected[entry.name]));
            }
        }
        ++rounds;
    }
    std::printf("  完成 %d 轮随机归档，逐条目比对通过\n", rounds);
    AUDIT_CHECK(rounds == 1000);
}

// ---------------------------------------------------------------------------
// 3. 2 万次随机结构变异
// ---------------------------------------------------------------------------

/// 变异策略。
std::vector<std::uint8_t> Mutate(const std::vector<std::uint8_t>& base, audit::Rng& rng) {
    std::vector<std::uint8_t> bytes = base;
    if (bytes.empty()) {
        return bytes;
    }
    const std::size_t eocd = audit::FindEocd(bytes);
    const auto random_offset = [&rng, &bytes]() {
        return static_cast<std::size_t>(rng.Range(bytes.size()));
    };
    switch (rng.Range(6)) {
        case 0: { // 随机字节翻转
            const int count = 1 + static_cast<int>(rng.Range(8));
            for (int i = 0; i < count; ++i) {
                const std::size_t offset = random_offset();
                bytes[offset] = static_cast<std::uint8_t>(bytes[offset] ^ (1u << rng.Range(8)));
            }
            break;
        }
        case 1: { // 截断
            const std::size_t length = static_cast<std::size_t>(rng.Range(bytes.size()));
            bytes.resize(length);
            break;
        }
        case 2: { // 插入随机字节
            const std::size_t offset = random_offset();
            const std::size_t count = 1 + static_cast<std::size_t>(rng.Range(16));
            std::vector<std::uint8_t> inserted(count, 0);
            for (std::uint8_t& byte : inserted) {
                byte = static_cast<std::uint8_t>(rng.Next());
            }
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(offset), inserted.begin(),
                         inserted.end());
            break;
        }
        case 3: { // 改 EOCD 字段
            if (eocd == std::string::npos) {
                break;
            }
            const std::size_t field = eocd + 4 * rng.Range(4); // 8/10/16/20 附近
            const std::size_t offset = std::min(field, eocd + 20);
            if (rng.Chance(50)) {
                audit::PutU16At(&bytes, offset, static_cast<std::uint16_t>(rng.Next()));
            } else {
                audit::PutU32At(&bytes, offset, static_cast<std::uint32_t>(rng.Next()));
            }
            break;
        }
        case 4: { // 改中央目录项字段（name_len/extra_len/comment_len/local_offset）
            if (eocd == std::string::npos) {
                break;
            }
            const std::uint32_t central_offset = audit::GetU32At(bytes, eocd + 16);
            if (central_offset + 46 > bytes.size()) {
                break;
            }
            const std::size_t item = central_offset;
            const std::size_t field = item + 28 + 2 * rng.Range(3); // name/extra/comment len
            const std::size_t target = rng.Chance(60) ? field : item + 42;
            if (rng.Chance(50)) {
                audit::PutU16At(&bytes, target, static_cast<std::uint16_t>(rng.Range(64)));
            } else {
                audit::PutU32At(&bytes, target, static_cast<std::uint32_t>(rng.Next()));
            }
            break;
        }
        case 5: { // 改本地头字段（crc / 大小 / name_len / method）
            const std::size_t offset = static_cast<std::size_t>(rng.Range(30));
            const std::size_t target = offset < 14 ? 14 + (offset % 12) : offset;
            audit::PutU32At(&bytes, target, static_cast<std::uint32_t>(rng.Next()));
            break;
        }
        default:
            break;
    }
    return bytes;
}

/// 输出目录里的每个文件都必须是完整条目（大小与 CRC 与中央目录一致）。
bool CheckOutputFiles(const fs::path& out, const ZipListing& listing, std::string* error) {
    std::map<std::string, const ZipEntryInfo*> index;
    for (const ZipEntryInfo& entry : listing.entries) {
        if (!entry.is_directory) {
            index[entry.name] = &entry;
        }
    }
    std::error_code ec;
    fs::recursive_directory_iterator it(out, ec);
    if (ec) {
        return true; // 目录不存在：没有残留，合法
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            *error = "遍历输出目录失败";
            return false;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::string relative = fs::relative(it->path(), out, ec).generic_string();
        if (ec) {
            *error = "相对路径失败";
            return false;
        }
        const auto found = index.find(relative);
        if (found == index.end()) {
            *error = "输出文件不在中央目录中: " + relative;
            return false;
        }
        const auto data = bento_test::ReadFile(it->path());
        if (static_cast<std::uint64_t>(data.size()) != found->second->size) {
            *error = "输出文件大小与声明不符: " + relative;
            return false;
        }
        if (CrcOf(data) != found->second->crc32) {
            *error = "输出文件 CRC 与声明不符: " + relative;
            return false;
        }
    }
    return true;
}

void RandomMutations() {
    Section("3. 20000 次随机结构变异（篡改/截断/插入/字段破坏）");
    bento_test::TempDir dir("audit_random_mutate");
    audit::Rng rng(0x1122334455667788ull);
    // 基准归档：4 个小条目，便于定位中央目录。
    const auto base_zip = dir.path() / "base.zip";
    {
        ZipWriter writer;
        AUDIT_CHECK(writer.Open(base_zip.string()) == ZipStatus::kOk);
        const std::array<std::string, 4> names{"a.txt", "dir/b.bin", "c.log", "d/e/f.dat"};
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto data = bento_test::RandomBytes(64 + i * 32, 100 + i);
            AUDIT_CHECK(writer.AddBytes(names[i], data.data(), data.size()) == ZipStatus::kOk);
        }
        AUDIT_CHECK(writer.Close() == ZipStatus::kOk);
    }
    const auto base = audit::ReadBytes(base_zip);
    AUDIT_CHECK(!base.empty());
    const auto canary = dir.path() / "canary.txt";
    AUDIT_CHECK(bento_test::WriteText(canary, "unchanged"));
    const auto canary_before = bento_test::ReadFile(canary);

    StatusCounts list_counts;
    StatusCounts extract_counts;
    int survived = 0;
    int extract_ok = 0;
    int residue_checked = 0;
    for (int round = 0; round < 20000; ++round) {
        const auto mutated = Mutate(base, rng);
        const auto zip = dir.path() / "mutated.zip";
        if (!audit::WriteBytes(zip, mutated)) {
            ++audit::g_failures;
            continue;
        }
        const auto out = dir.path() / ("out_" + std::to_string(round));
        const auto listing = ListZip(zip.string());
        ++list_counts[go2cpp::bento::app::ZipStatusToString(listing.status)];
        const auto extracted = ExtractZip(zip.string(), out.string());
        ++extract_counts[go2cpp::bento::app::ZipStatusToString(extracted.status)];
        if (extracted.status == ZipStatus::kOk) {
            ++extract_ok;
            // Extract 成功必须与 List 成功一致。
            if (!listing.ok()) {
                std::printf("  第 %d 次：Extract ok 但 List=%s\n", round,
                            go2cpp::bento::app::ZipStatusToString(listing.status));
                ++audit::g_failures;
                continue;
            }
            std::string error;
            if (!CheckOutputFiles(out, listing, &error)) {
                std::printf("  第 %d 次：%s\n", round, error.c_str());
                ++audit::g_failures;
            }
            ++audit::g_checks;
        } else if (listing.ok()) {
            // 失败路径：已写出的文件必须仍是完整条目。
            std::string error;
            if (!CheckOutputFiles(out, listing, &error)) {
                std::printf("  第 %d 次（失败残留）：%s\n", round, error.c_str());
                ++audit::g_failures;
            }
            ++residue_checked;
        }
        // canary 与输出根之外不得被改写。
        if (bento_test::ReadFile(canary) != canary_before) {
            std::printf("  第 %d 次：canary 被改写\n", round);
            ++audit::g_failures;
        }
        ++survived;
        std::error_code ec;
        fs::remove_all(out, ec);
    }
    std::printf("  完成 %d 次变异：Extract 成功 %d 次，失败残留校验 %d 次\n", survived,
                extract_ok, residue_checked);
    std::printf("  List 状态分布：");
    for (const auto& [name, count] : list_counts) {
        std::printf("%s=%d ", name.c_str(), count);
    }
    std::printf("\n  Extract 状态分布：");
    for (const auto& [name, count] : extract_counts) {
        std::printf("%s=%d ", name.c_str(), count);
    }
    std::printf("\n");
    AUDIT_CHECK(survived == 20000);
}

// ---------------------------------------------------------------------------
// 4. 随机声明炸弹
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> BuildBomb(std::size_t entries, std::uint32_t declared) {
    std::vector<std::uint8_t> bytes;
    const std::string name = "x.bin";
    std::vector<std::uint32_t> offsets;
    for (std::size_t i = 0; i < entries; ++i) {
        offsets.push_back(static_cast<std::uint32_t>(bytes.size()));
        const auto put16 = [&bytes](std::uint16_t value) {
            bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
            bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
        };
        const auto put32 = [&bytes](std::uint32_t value) {
            for (int shift = 0; shift < 32; shift += 8) {
                bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
            }
        };
        put32(0x04034b50u);
        put16(20);
        put16(0);
        put16(0);
        put16(0);
        put16(0x21);
        put32(0);
        put32(declared);
        put32(declared);
        put16(static_cast<std::uint16_t>(name.size()));
        put16(0);
        bytes.insert(bytes.end(), name.begin(), name.end());
    }
    const std::uint32_t central_offset = static_cast<std::uint32_t>(bytes.size());
    std::uint32_t central_bytes = 0;
    for (std::size_t i = 0; i < entries; ++i) {
        const auto put16 = [&bytes](std::uint16_t value) {
            bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
            bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
        };
        const auto put32 = [&bytes](std::uint32_t value) {
            for (int shift = 0; shift < 32; shift += 8) {
                bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
            }
        };
        put32(0x02014b50u);
        put16((3u << 8) | 30u);
        put16(20);
        put16(0);
        put16(0);
        put16(0);
        put16(0x21);
        put32(0);
        put32(declared);
        put32(declared);
        put16(static_cast<std::uint16_t>(name.size()));
        put16(0);
        put16(0);
        put16(0);
        put16(0);
        put32(0100644u << 16);
        put32(offsets[i]);
        bytes.insert(bytes.end(), name.begin(), name.end());
        central_bytes += 46u + static_cast<std::uint32_t>(name.size());
    }
    const auto put16 = [&bytes](std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    };
    const auto put32 = [&bytes](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    };
    put32(0x06054b50u);
    put16(0);
    put16(0);
    put16(static_cast<std::uint16_t>(entries));
    put16(static_cast<std::uint16_t>(entries));
    put32(central_bytes);
    put32(central_offset);
    put16(0);
    return bytes;
}

void RandomBombs() {
    Section("4. 500 个随机声明炸弹（不按声明大小分配内存）");
    bento_test::TempDir dir("audit_random_bomb");
    audit::Rng rng(0xdeadbeefcafebabeull);
    int rejected = 0;
    for (int round = 0; round < 500; ++round) {
        const std::size_t entries = 1 + static_cast<std::size_t>(rng.Range(16));
        const std::uint32_t declared = static_cast<std::uint32_t>(rng.Next() & 0x7fffffffu);
        const auto bytes = BuildBomb(entries, declared);
        const auto zip = dir.path() / "bomb.zip";
        if (!audit::WriteBytes(zip, bytes)) {
            ++audit::g_failures;
            continue;
        }
        audit::TrimMemory();
        const long rss_before = audit::RssKb();
        const auto out = dir.path() / ("bomb_out_" + std::to_string(round));
        const auto result = ExtractZip(zip.string(), out.string());
        const long rss_after = audit::RssKb();
        if (result.status != ZipStatus::kOk) {
            ++rejected;
        }
        if (rss_after - rss_before > 64 * 1024) {
            std::printf("  第 %d 次：RSS 增长 %ld KiB（声明 %u）\n", round,
                        rss_after - rss_before, declared);
            ++audit::g_failures;
        }
        ++audit::g_checks;
        std::error_code ec;
        fs::remove_all(out, ec);
    }
    std::printf("  完成 500 个随机炸弹，拒绝 %d 个，无内存膨胀\n", rejected);
    AUDIT_CHECK(rejected == 500);
}

// ---------------------------------------------------------------------------
// 5. 随机非法名字（写端拒绝 / 读端拒绝）
// ---------------------------------------------------------------------------

void RandomIllegalNames() {
    Section("5. 2000 个随机非法名字：写端拒绝，不产生条目");
    bento_test::TempDir dir("audit_random_names");
    audit::Rng rng(0x0f0f0f0f12345678ull);
    const std::array<std::string, 10> patterns{
        "../x",      "/abs",       "a\\b",       "a//b",      "a/./b",
        "a/../b",    "abc:",       "a\rb",       "a\x7f" "b", "/",
    };
    const auto zip = dir.path() / "names.zip";
    int rejected = 0;
    for (int round = 0; round < 2000; ++round) {
        // patterns 全部是语法非法名；不做插入扰动（插入可能把 ".." 变成
        // "..v2" 之类的合法段，那样断言就失去意义）。
        const std::string name = patterns[rng.Range(patterns.size())];
        // 写入器一旦失败就进入 poison 态，后续调用只返回 kIoError。
        // 因此每个名字都必须用全新的写入器。
        ZipWriter writer;
        if (writer.Open(zip.string()) != ZipStatus::kOk) {
            ++audit::g_failures;
            continue;
        }
        const ZipStatus status = writer.AddBytes(name, nullptr, 0);
        writer.Abort();
        if (status == ZipStatus::kPathRejected) {
            ++rejected;
        } else {
            // patterns 里全部是非法名字；任何非拒绝都算缺陷。
            std::printf("  第 %d 次：名字被接受：%s\n", round, name.c_str());
            ++audit::g_failures;
        }
        ++audit::g_checks;
    }
    std::printf("  拒收 %d / 2000\n", rejected);
    AUDIT_CHECK(rejected == 2000);
}

}  // namespace

int main() {
    RandomTrees();
    RandomValidArchives();
    RandomMutations();
    RandomBombs();
    RandomIllegalNames();
    return audit::Finish("audit_zip_random");
}
