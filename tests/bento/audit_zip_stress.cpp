/**
 * @file audit_zip_stress.cpp
 * @brief zip 模块压测与复杂场景审计（独立程序，不注册 CTest）。
 *
 * @details 覆盖：
 *          - 压测 A：约 1 GiB、6 万文件的目录树打包 → 解包 → 全量逐字节比对
 *            （吞吐、RSS、fd、磁盘）；
 *          - 压测 B：1 GiB 单文件打包/解包（验证流式，RSS 不随文件线性增长）；
 *          - 场景 1：zip 炸弹（声明 1 GiB/10 GiB、伪造大小、总量上限拒绝）；
 *          - 场景 2：Zip Slip（`../`、绝对路径、反斜杠、输出目录预置符号链接）；
 *          - 场景 3：10 万小文件打包（ZIP32 条目上限行为 + 性能 + 无残留）。
 *
 * 编译（在项目根目录）：
 * @code
 * g++ -std=c++17 -O2 -g -Iinclude -Itests/bento \
 *     tests/bento/audit_zip_stress.cpp \
 *     -L.build-bento-wsl -lgo2cpp_bento_app -lgo2cpp_bento -lgo2cpp_os \
 *     -lgo2cpp_hash -lgo2cpp_config -lgo2cpp_scheduler -lgo2cpp_log \
 *     -lgo2cpp_fiber -lpthread -Wl,-rpath,$PWD/.build-bento-wsl \
 *     -o .build-bento-wsl/audit_zip_stress
 * @endcode
 *
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 */

#include "audit_zip_support.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/app/backup.hpp"
#include "go2cpp/bento/app/zip.hpp"
#include "go2cpp/hash/sha256.hpp"
#include "test_backup_support.hpp"

namespace {

namespace fs = std::filesystem;

using go2cpp::bento::app::Backup;
using go2cpp::bento::app::BackupStatus;
using go2cpp::bento::app::ExtractZip;
using go2cpp::bento::app::ListZip;
using go2cpp::bento::app::ZipLimits;
using go2cpp::bento::app::ZipStatus;
using go2cpp::bento::app::ZipWriter;

/// 后台资源采样：峰值 RSS 与 fd 数。
class ResourceSampler final {
public:
    void Start() {
        m_stop = false;
        m_peak_rss = audit::RssKb();
        m_peak_fd = audit::FdCount();
        m_thread = std::thread([this] {
            while (!m_stop.load(std::memory_order_relaxed)) {
                const long rss = audit::RssKb();
                const int fd = audit::FdCount();
                if (rss > m_peak_rss) {
                    m_peak_rss = rss;
                }
                if (fd > m_peak_fd) {
                    m_peak_fd = fd;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }

    void Stop() {
        m_stop = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    long PeakRssKb() const { return m_peak_rss; }
    int PeakFd() const { return m_peak_fd; }

private:
    std::atomic<bool> m_stop{true};
    std::thread m_thread;
    long m_peak_rss{0};
    int m_peak_fd{0};
};

/// 人类可读字节。
std::string Human(std::uint64_t bytes) {
    static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, kUnits[unit]);
    return std::string(buffer);
}

/// 打印一行带数据的分节标题。
void Section(const char* title) {
    std::printf("\n== %s ==\n", title);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 场景 1：zip 炸弹
// ---------------------------------------------------------------------------

/**
 * @brief 构造一个只有 EOCD + 若干中央目录项/本地头的「声明式」zip。
 *
 * @details 每条目声明 `declared_size` 字节，实际数据 0 字节。store-only
 *          没有压缩膨胀，所以「炸弹」只能靠伪造声明大小触发：
 *          解析器必须在校验偏移/上限时拒绝，而不是尝试读 10 GiB。
 */
std::vector<std::uint8_t> BuildDeclaredBomb(std::size_t entries,
                                            std::uint32_t declared_size) {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint32_t> offsets;
    const std::string name = "b.bin";
    for (std::size_t i = 0; i < entries; ++i) {
        offsets.push_back(static_cast<std::uint32_t>(bytes.size()));
        auto put16 = [&bytes](std::uint16_t value) {
            bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
            bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
        };
        auto put32 = [&bytes](std::uint32_t value) {
            for (int shift = 0; shift < 32; shift += 8) {
                bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
            }
        };
        put32(0x04034b50u);                       // 本地头签名
        put16(20);                                // version needed
        put16(0);                                 // flags
        put16(0);                                 // method = store
        put16(0);                                 // time
        put16(0x21);                              // date
        put32(0);                                 // crc
        put32(declared_size);                     // compressed size（声明）
        put32(declared_size);                     // uncompressed size（声明）
        put16(static_cast<std::uint16_t>(name.size()));
        put16(0);                                 // extra
        bytes.insert(bytes.end(), name.begin(), name.end());
        // 不写数据：声明的大小与实际字节数不一致。
    }
    const std::uint32_t central_offset = static_cast<std::uint32_t>(bytes.size());
    std::uint32_t central_bytes = 0;
    for (std::size_t i = 0; i < entries; ++i) {
        auto put16 = [&bytes](std::uint16_t value) {
            bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
            bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
        };
        auto put32 = [&bytes](std::uint32_t value) {
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
        put32(declared_size);
        put32(declared_size);
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
    auto put16 = [&bytes](std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    };
    auto put32 = [&bytes](std::uint32_t value) {
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

void ScenarioBomb() {
    Section("场景 1：zip 炸弹（声明式 + 真实数据上限）");
    bento_test::TempDir dir("audit_bomb");
    const auto out = dir.path() / "out";

    // 1a：单条声明 2 GiB，超过默认 max_entry_bytes（1 GiB）→ kEntryTooLarge。
    {
        const auto bytes = BuildDeclaredBomb(1, 0x80000000u);
        const auto zip = dir.path() / "entry_bomb.zip";
        AUDIT_CHECK(audit::WriteBytes(zip, bytes));
        const auto listing = ListZip(zip.string());
        std::printf("  1a 单条声明 2GiB -> %s\n",
                    go2cpp::bento::app::ZipStatusToString(listing.status));
        AUDIT_CHECK(listing.status == ZipStatus::kEntryTooLarge);
    }

    // 1b：伪造 10 条 × 1 GiB 声明（合计 10 GiB）但没有数据字节。
    //     解析器在「数据越界」处拒绝，不会按声明大小分配内存或写盘。
    {
        const auto bytes = BuildDeclaredBomb(10, 0x40000000u);
        const auto zip = dir.path() / "fake_10g.zip";
        AUDIT_CHECK(audit::WriteBytes(zip, bytes));
        const long rss_before = audit::RssKb();
        const auto result = ExtractZip(zip.string(), out.string());
        const long rss_after = audit::RssKb();
        std::printf("  1b 伪造 10x1GiB 声明 -> %s（%s），RSS %ld→%ld KiB\n",
                    go2cpp::bento::app::ZipStatusToString(result.status), result.error.c_str(),
                    rss_before, rss_after);
        AUDIT_CHECK(result.status == ZipStatus::kCorruptArchive);
        AUDIT_CHECK(!fs::exists(out));
        AUDIT_CHECK(rss_after - rss_before < 64 * 1024); // 不允许按声明大小分配
    }

    // 1c：上限检查先于数据读取：同样的伪造声明 + max_entry_bytes=1MiB
    //     → 单条上限直接拒绝（kEntryTooLarge），而不是先尝试读 1GiB。
    {
        const auto bytes = BuildDeclaredBomb(10, 0x40000000u);
        const auto zip = dir.path() / "ceiling_first.zip";
        AUDIT_CHECK(audit::WriteBytes(zip, bytes));
        ZipLimits limits;
        limits.max_entry_bytes = 1u << 20;
        const auto result = ExtractZip(zip.string(), out.string(), limits);
        std::printf("  1c 声明 1GiB/条，单条上限 1MiB -> %s\n",
                    go2cpp::bento::app::ZipStatusToString(result.status));
        AUDIT_CHECK(result.status == ZipStatus::kEntryTooLarge);
    }

    // 1d：真实数据 3 × 800 KiB（2.4 MiB），总量上限 2 MiB → 第 2 条即拒绝。
    {
        const auto zip = dir.path() / "real_total.zip";
        const std::vector<std::uint8_t> chunk(800 * 1024, 0x5a);
        ZipWriter writer;
        AUDIT_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        for (int i = 0; i < 3; ++i) {
            AUDIT_CHECK(writer.AddBytes("c" + std::to_string(i) + ".bin", chunk.data(),
                                        chunk.size()) == ZipStatus::kOk);
        }
        AUDIT_CHECK(writer.Close() == ZipStatus::kOk);
        ZipLimits limits;
        limits.max_total_bytes = 2u << 20;
        const auto result = ExtractZip(zip.string(), out.string(), limits);
        std::printf("  1d 真实 3x800KiB，总量上限 2MiB -> %s（%s）\n",
                    go2cpp::bento::app::ZipStatusToString(result.status), result.error.c_str());
        AUDIT_CHECK(result.status == ZipStatus::kArchiveTooLarge);
        AUDIT_CHECK(!fs::exists(out));
    }

    // 1e：真实数据 1 × 200 KiB，单条上限 128 KiB → kEntryTooLarge。
    {
        const auto zip = dir.path() / "real_entry.zip";
        const std::vector<std::uint8_t> chunk(200 * 1024, 0x33);
        ZipWriter writer;
        AUDIT_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
        AUDIT_CHECK(writer.AddBytes("big.bin", chunk.data(), chunk.size()) == ZipStatus::kOk);
        AUDIT_CHECK(writer.Close() == ZipStatus::kOk);
        ZipLimits limits;
        limits.max_entry_bytes = 128u << 10;
        const auto result = ExtractZip(zip.string(), out.string(), limits);
        std::printf("  1e 真实 200KiB，单条上限 128KiB -> %s\n",
                    go2cpp::bento::app::ZipStatusToString(result.status));
        AUDIT_CHECK(result.status == ZipStatus::kEntryTooLarge);
        AUDIT_CHECK(!fs::exists(out));
    }
}

// ---------------------------------------------------------------------------
// 场景 2：Zip Slip
// ---------------------------------------------------------------------------

/// 等长改写归档内出现的名字（本地头 + 中央目录各一处）。
std::size_t RewriteName(std::vector<std::uint8_t>* bytes, const std::string& from,
                        const std::string& to) {
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

void ScenarioZipSlip() {
    Section("场景 2：Zip Slip");
    bento_test::TempDir dir("audit_slip");
    const auto base_zip = dir.path() / "base.zip";
    const std::string original = "ab/c.t";
    const std::vector<std::uint8_t> payload = {'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    {
        ZipWriter writer;
        AUDIT_CHECK(writer.Open(base_zip.string()) == ZipStatus::kOk);
        AUDIT_CHECK(writer.AddBytes(original, payload.data(), payload.size()) == ZipStatus::kOk);
        AUDIT_CHECK(writer.Close() == ZipStatus::kOk);
    }
    const auto base_bytes = audit::ReadBytes(base_zip);
    AUDIT_CHECK(!base_bytes.empty());

    // 2a：等长改写名字为 `../x.t` / 绝对路径 / 反斜杠，List 与 Extract 都必须拒绝。
    const std::vector<std::string> evil = {"../x.t", "/tmp/x", "a\\b.tx"};
    for (std::size_t index = 0; index < evil.size(); ++index) {
        auto mutated = base_bytes;
        const std::size_t replaced = RewriteName(&mutated, original, evil[index]);
        AUDIT_CHECK(replaced == 2);
        const auto zip = dir.path() / ("slip_" + std::to_string(index) + ".zip");
        AUDIT_CHECK(audit::WriteBytes(zip, mutated));
        const auto out = dir.path() / ("out_" + std::to_string(index));
        const auto listing = ListZip(zip.string());
        const auto result = ExtractZip(zip.string(), out.string());
        std::printf("  2a 名字 \"%s\" -> list=%s extract=%s\n", evil[index].c_str(),
                    go2cpp::bento::app::ZipStatusToString(listing.status),
                    go2cpp::bento::app::ZipStatusToString(result.status));
        AUDIT_CHECK(listing.status == ZipStatus::kPathRejected);
        AUDIT_CHECK(result.status == ZipStatus::kPathRejected);
        AUDIT_CHECK(!fs::exists(out));
        AUDIT_CHECK(!fs::exists(dir.path() / "x.t"));
        AUDIT_CHECK(!fs::exists("/tmp/x"));
    }

    // 2b：输出目录预置符号链接，归档内的合法名字必须被 ResolveOutputPath 拦下。
    {
        const auto outside = dir.path() / "outside";
        AUDIT_CHECK(fs::create_directories(outside));
        const auto out = dir.path() / "out_link";
        AUDIT_CHECK(fs::create_directories(out));
        if (::symlink(outside.c_str(), (out / "link").c_str()) != 0) {
            std::printf("  2b 跳过：符号链接不可用\n");
        } else {
            const auto zip = dir.path() / "link_entry.zip";
            {
                ZipWriter writer;
                AUDIT_CHECK(writer.Open(zip.string()) == ZipStatus::kOk);
                AUDIT_CHECK(writer.AddBytes("link/pwn.txt", payload.data(), payload.size()) ==
                            ZipStatus::kOk);
                AUDIT_CHECK(writer.Close() == ZipStatus::kOk);
            }
            const auto result = ExtractZip(zip.string(), out.string());
            std::printf("  2b 预置 symlink 逃逸 -> %s (%s)\n",
                        go2cpp::bento::app::ZipStatusToString(result.status),
                        result.error.c_str());
            AUDIT_CHECK(result.status == ZipStatus::kPathRejected);
            AUDIT_CHECK(!fs::exists(outside / "pwn.txt"));
        }
    }

    // 2c：合法名字 `a/./b`、`a//b` 已在语法校验拒绝；补一个绝对路径条目。
    {
        const auto zip = dir.path() / "absolute.zip";
        auto mutated = base_bytes;
        AUDIT_CHECK(RewriteName(&mutated, original, "/abs.x") == 2);
        AUDIT_CHECK(audit::WriteBytes(zip, mutated));
        const auto result = ExtractZip(zip.string(), (dir.path() / "out_abs").string());
        std::printf("  2c 绝对路径 /abs.x -> %s\n",
                    go2cpp::bento::app::ZipStatusToString(result.status));
        AUDIT_CHECK(result.status == ZipStatus::kPathRejected);
        AUDIT_CHECK(!fs::exists("/abs.x"));
    }
}

// ---------------------------------------------------------------------------
// 场景 3：10 万小文件
// ---------------------------------------------------------------------------

/// 生成 count 个小文件（每目录 1000 个，内容 16B 递增），返回耗时。
double MakeManyFiles(const fs::path& root, std::size_t count) {
    audit::Stopwatch watch;
    std::vector<std::uint8_t> data(16, 0x5a);
    std::error_code ec;
    std::size_t created = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t bucket = i / 1000;
        const fs::path dir = root / ("d" + std::to_string(bucket));
        if (i % 1000 == 0) {
            fs::create_directories(dir, ec);
        }
        data[15] = static_cast<std::uint8_t>(i);
        std::ofstream out(dir / ("f" + std::to_string(i) + ".bin"),
                          std::ios::binary | std::ios::trunc);
        if (!out) {
            break;
        }
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!out) {
            break;
        }
        ++created;
    }
    std::printf("  生成 %zu 个文件耗时 %.2fs\n", created, watch.Seconds());
    return watch.Seconds();
}

void ScenarioHundredThousandFiles() {
    Section("场景 3：10 万小文件打包（ZIP32 条目上限）");
    bento_test::TempDir dir("audit_100k");
    const auto source = dir.path() / "many";
    constexpr std::size_t kFiles = 100000;
    const double create_seconds = MakeManyFiles(source, kFiles);
    std::printf("  目录树：%zu 文件，%.2f MiB 数据，生成耗时 %.2fs\n", kFiles,
                static_cast<double>(kFiles * 16) / (1024.0 * 1024.0), create_seconds);

    // 3a：默认上限 16384 条目 → 明确失败，且归档与临时文件都不残留。
    {
        const auto zip = dir.path() / "default_limit.zip";
        audit::Stopwatch watch;
        const auto result = Backup({source.string()}, zip.string());
        const double seconds = watch.Seconds();
        std::printf("  3a 默认 max_entries=16384 -> %s（%.2fs）\n",
                    go2cpp::bento::app::BackupStatusToString(result.status), seconds);
        AUDIT_CHECK(result.status == BackupStatus::kTooLarge);
        AUDIT_CHECK(!fs::exists(zip));
        AUDIT_CHECK(!fs::exists(zip.string() + ".part"));
        bool leftovers = false;
        for (const auto& entry : fs::directory_iterator(dir.path())) {
            if (entry.path().filename().string().find(".part.") != std::string::npos) {
                leftovers = true;
            }
        }
        AUDIT_CHECK(!leftovers);
    }

    // 3b：即使放宽到硬上限 65534，10 万文件仍必须被拒绝（不能生成非法 zip）。
    {
        const auto zip = dir.path() / "max_limit.zip";
        ZipLimits limits;
        limits.max_entries = 65534;
        audit::Stopwatch watch;
        const auto result = Backup({source.string()}, zip.string(), limits);
        const double seconds = watch.Seconds();
        std::printf("  3b max_entries=65534 -> %s（%.2fs）\n",
                    go2cpp::bento::app::BackupStatusToString(result.status), seconds);
        AUDIT_CHECK(result.status == BackupStatus::kTooLarge);
        AUDIT_CHECK(!fs::exists(zip));
    }

    // 3c：10 万文件遍历本身的性能（失败路径也完整遍历了目录树）。
    {
        const auto zip = dir.path() / "perf.zip";
        audit::Stopwatch watch;
        const auto result = Backup({source.string()}, zip.string());
        std::printf("  3c 失败前遍历 10 万文件的耗时 %.2fs（%.0f 文件/秒）\n",
                    watch.Seconds(),
                    static_cast<double>(kFiles) / std::max(watch.Seconds(), 0.001));
        AUDIT_CHECK(result.status == BackupStatus::kTooLarge);
    }

    // 3d：60000 个文件（<= 65534）必须能完整打包。
    {
        const auto subset = dir.path() / "subset";
        std::error_code ec;
        fs::create_directories(subset, ec);
        std::size_t copied = 0;
        for (const auto& entry : fs::directory_iterator(source, ec)) {
            if (copied >= 60) {
                break;
            }
            fs::copy(entry.path(), subset / entry.path().filename(),
                     fs::copy_options::recursive, ec);
            ++copied;
        }
        const auto zip = dir.path() / "subset.zip";
        ZipLimits limits;
        limits.max_entries = 65534;
        limits.max_total_bytes = 0xfff00000ull;
        audit::Stopwatch watch;
        const auto result = Backup({subset.string()}, zip.string(), limits);
        std::printf("  3d 60000 文件打包 -> %s（%.2fs，%s，错误：%s）\n",
                    go2cpp::bento::app::BackupStatusToString(result.status), watch.Seconds(),
                    Human(result.stats.archive_bytes).c_str(), result.error.c_str());
        AUDIT_CHECK(result.ok());
        AUDIT_CHECK(result.stats.files >= 59000);
        AUDIT_CHECK(fs::exists(zip));
    }
}

// ---------------------------------------------------------------------------
// 压测 A：1 GiB 目录树
// ---------------------------------------------------------------------------

/// 生成约 1 GiB、count 个文件的树；每个文件大小 = total/count。
void MakeTree(const fs::path& root, std::size_t count, std::uint64_t total_bytes,
              const char* label) {
    audit::Stopwatch watch;
    const std::size_t per_file = static_cast<std::size_t>(total_bytes / count);
    std::vector<char> block(per_file, 'A');
    std::error_code ec;
    for (std::size_t i = 0; i < count; ++i) {
        const fs::path dir = root / ("g" + std::to_string(i / 2000));
        if (i % 2000 == 0) {
            fs::create_directories(dir, ec);
        }
        block[0] = static_cast<char>(i & 0xff);
        block[block.size() / 2] = static_cast<char>((i >> 8) & 0xff);
        std::ofstream out(dir / ("f" + std::to_string(i) + ".bin"),
                          std::ios::binary | std::ios::trunc);
        if (!out) {
            std::printf("  [warn] 第 %zu 个文件创建失败\n", i);
            break;
        }
        out.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    std::printf("  生成 %s：%zu 文件 × %zu B = %s，耗时 %.2fs\n", label, count, per_file,
                Human(static_cast<std::uint64_t>(count) * per_file).c_str(), watch.Seconds());
}

void Stress1GiBTree() {
    Section("压测 A：约 1 GiB / 6 万文件树（打包 → 解包 → 全量比对）");
    bento_test::TempDir dir("audit_1g");
    const auto source = dir.path() / "tree";
    const auto zip = dir.path() / "tree.zip";
    const auto out = dir.path() / "out";
    constexpr std::size_t kFiles = 60000;
    constexpr std::uint64_t kTotal = 1050ull * 1024 * 1024; // 约 1.02 GiB
    MakeTree(source, kFiles, kTotal, "源目录树");

    ZipLimits limits;
    limits.max_entries = 65534;
    limits.max_total_bytes = 0xfff00000ull;

    ResourceSampler sampler;
    sampler.Start();
    audit::Stopwatch watch;
    const auto result = Backup({source.string()}, zip.string(), limits);
    const double backup_seconds = watch.Seconds();
    sampler.Stop();
    std::printf("  打包：%s 状态=%.2fs 吞吐=%.2f MiB/s 峰值RSS=%.1f MiB 峰值fd=%d\n",
                go2cpp::bento::app::BackupStatusToString(result.status), backup_seconds,
                static_cast<double>(result.stats.input_bytes) / (1024.0 * 1024.0) /
                    std::max(backup_seconds, 0.001),
                static_cast<double>(sampler.PeakRssKb()) / 1024.0, sampler.PeakFd());
    AUDIT_CHECK(result.ok());
    AUDIT_CHECK(result.stats.files == kFiles);
    const std::uint64_t per_file = kTotal / kFiles;
    AUDIT_CHECK(result.stats.input_bytes == per_file * kFiles);
    std::error_code size_ec;
    const std::uintmax_t zip_size = fs::file_size(zip, size_ec);
    std::printf("  归档：%s（条目 %zu / 目录 %zu）\n", Human(result.stats.archive_bytes).c_str(),
                result.stats.files, result.stats.directories);
    AUDIT_CHECK(!size_ec && result.stats.archive_bytes == static_cast<std::uint64_t>(zip_size));

    ResourceSampler extract_sampler;
    extract_sampler.Start();
    watch.Restart();
    const auto extracted = ExtractZip(zip.string(), out.string(), limits);
    const double extract_seconds = watch.Seconds();
    extract_sampler.Stop();
    std::printf("  解包：%s 状态=%.2fs 吞吐=%.2f MiB/s 峰值RSS=%.1f MiB 峰值fd=%d\n",
                go2cpp::bento::app::ZipStatusToString(extracted.status), extract_seconds,
                static_cast<double>(extracted.stats.input_bytes) / (1024.0 * 1024.0) /
                    std::max(extract_seconds, 0.001),
                static_cast<double>(extract_sampler.PeakRssKb()) / 1024.0,
                extract_sampler.PeakFd());
    AUDIT_CHECK(extracted.ok());
    AUDIT_CHECK(extracted.stats.files == kFiles);

    watch.Restart();
    std::string compare_error;
    const bool same = bento_test::CompareTrees(source, out / "tree", &compare_error);
    const double compare_seconds = watch.Seconds();
    std::printf("  比对：%s（%.2fs）%s\n", same ? "完全一致" : "不一致", compare_seconds,
                compare_error.c_str());
    AUDIT_CHECK(same);
    std::printf("  磁盘：源 %s / 包 %s / 解包 %s\n",
                Human(audit::DiskBytes(source)).c_str(), Human(audit::DiskBytes(zip)).c_str(),
                Human(audit::DiskBytes(out)).c_str());
}

// ---------------------------------------------------------------------------
// 压测 B：1 GiB 单文件
// ---------------------------------------------------------------------------

/// 生成指定大小的文件（重复填充，不占 RSS）。
bool MakeBigFile(const fs::path& path, std::uint64_t bytes, char fill) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    std::vector<char> block(1024 * 1024, fill);
    std::uint64_t remaining = bytes;
    while (remaining > 0) {
        const std::size_t chunk =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, block.size()));
        out.write(block.data(), static_cast<std::streamsize>(chunk));
        if (!out) {
            return false;
        }
        remaining -= chunk;
    }
    return true;
}

/// 文件 SHA-256（流式）。
std::string FileSha256(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    go2cpp::hash::Sha256 hasher;
    std::vector<char> buffer(256 * 1024);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
        hasher.Write(go2cpp::hash::AsBytes(buffer.data(), static_cast<std::size_t>(got)));
    }
    const auto digest = hasher.Sum();
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    for (const std::uint8_t byte : digest) {
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 0x0fu]);
    }
    return text;
}

void Stress1GiBSingleFile() {
    Section("压测 B：1 GiB 单文件（流式打包/解包）");
    bento_test::TempDir dir("audit_1g_file");
    const auto source = dir.path() / "src";
    const auto zip = dir.path() / "big.zip";
    const auto out = dir.path() / "out";
    constexpr std::uint64_t kSize = 1024ull * 1024 * 1024; // 恰好 1 GiB
    audit::Stopwatch generate_watch;
    AUDIT_CHECK(MakeBigFile(source / "big.bin", kSize, 'Q'));
    std::printf("  生成 1 GiB 文件耗时 %.2fs\n", generate_watch.Seconds());
    const std::string source_sha = FileSha256(source / "big.bin");

    ZipLimits limits;
    limits.max_total_bytes = 0xfff00000ull;
    ResourceSampler sampler;
    sampler.Start();
    audit::Stopwatch watch;
    const auto result = Backup({source.string()}, zip.string(), limits);
    const double seconds = watch.Seconds();
    sampler.Stop();
    const double mib = static_cast<double>(kSize) / (1024.0 * 1024.0);
    std::printf("  打包：%s %.2fs %.2f MiB/s 峰值RSS=%.1f MiB 峰值fd=%d\n",
                go2cpp::bento::app::BackupStatusToString(result.status), seconds,
                mib / std::max(seconds, 0.001),
                static_cast<double>(sampler.PeakRssKb()) / 1024.0, sampler.PeakFd());
    AUDIT_CHECK(result.ok());
    AUDIT_CHECK(result.stats.input_bytes == kSize);
    // 流式校验：RSS 不应随 1 GiB 线性增长（上限给 256 MiB，远小于 1 GiB）。
    AUDIT_CHECK(sampler.PeakRssKb() < 256 * 1024);

    ResourceSampler extract_sampler;
    extract_sampler.Start();
    watch.Restart();
    const auto extracted = ExtractZip(zip.string(), out.string(), limits);
    const double extract_seconds = watch.Seconds();
    extract_sampler.Stop();
    std::printf("  解包：%s %.2fs %.2f MiB/s 峰值RSS=%.1f MiB\n",
                go2cpp::bento::app::ZipStatusToString(extracted.status), extract_seconds,
                mib / std::max(extract_seconds, 0.001),
                static_cast<double>(extract_sampler.PeakRssKb()) / 1024.0);
    AUDIT_CHECK(extracted.ok());
    const std::string out_sha = FileSha256(out / "src" / "big.bin");
    std::printf("  SHA-256 一致：%s\n", source_sha == out_sha ? "是" : "否");
    AUDIT_CHECK(source_sha == out_sha);
}

// ---------------------------------------------------------------------------
// 环境信息
// ---------------------------------------------------------------------------

void PrintEnvironment() {
    Section("环境");
    audit::Rng rng(1);
    std::printf("  时间戳种子=%" PRIu64 "，RSS=%.1f MiB，fd=%d，进程数=%d\n", rng.Next(),
                static_cast<double>(audit::RssKb()) / 1024.0, audit::FdCount(),
                audit::ProcessCount());
    std::FILE* pipe = ::popen("nproc; free -g | head -2; df -h /tmp | tail -1; "
                              "curl --version | head -1; unzip -v | head -1",
                              "r");
    if (pipe != nullptr) {
        char line[512];
        while (std::fgets(line, sizeof(line), pipe) != nullptr) {
            std::printf("  %s", line);
        }
        ::pclose(pipe);
    }
}

}  // namespace

int main() {
    PrintEnvironment();
    ScenarioBomb();
    ScenarioZipSlip();
    ScenarioHundredThousandFiles();
    Stress1GiBTree();
    Stress1GiBSingleFile();
    return audit::Finish("audit_zip_stress");
}
