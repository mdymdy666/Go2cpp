/**
 * @file persistence_tests.cpp
 * @brief 持久化原型的随机/边界/故障注入测试（不接入顶部 ctest）。
 *
 * 覆盖：
 *   1. 定长编码器与 replay::DecodeEvent 的逐字段往返（含随机值）；
 *   2. buffered / mpsc / segmented 三种引擎的写→读回（计数、CRC、seq、checksum）；
 *   3. 多线程生产者（4/8 线程）的批间 seq 单调性与文件顺序；
 *   4. 段文件滚动 + seq 范围 / 时间范围定位读取；
 *   5. v4/v5 混合事件（v4 无 value/aux 字段，只校验计数/CRC/seq）；
 *   6. 故障注入：文件截断、记录 CRC 损坏、段大小与 manifest 不一致；
 *   7. 边界：0 条事件、1 条事件、interval fsync 的空闲触发；
 *   8. checksum 跨引擎一致性（同一事件流在 buffered/mpsc/segmented 下相同）。
 *
 * 运行：`persistence_tests`；全部通过返回 0，失败打印 FAIL 并返回 1。
 */

#include "persistence/encoder.hpp"
#include "persistence/reader.hpp"
#include "persistence/writer.hpp"
#include "replay/format.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using go2cpp::persistence::Engine;
using go2cpp::persistence::EventWriter;
using go2cpp::persistence::MixValue;
using go2cpp::persistence::Producer;
using go2cpp::persistence::ReadStats;
using go2cpp::persistence::SessionReader;
using go2cpp::persistence::SyncPolicy;
using go2cpp::persistence::WriterOptions;
using go2cpp::persistence::WriterStats;
using go2cpp::replay::Event;
using go2cpp::replay::EventType;

int g_failures = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                         #condition);                                       \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

std::string g_root = "/tmp/g2c-persistence-tests";

std::string PathFor(const std::string& name) {
    return g_root + "/" + name;
}

Event MakeEvent(std::uint64_t index, bool v5_only) {
    static constexpr EventType kV5Types[] = {
        EventType::kResRwReadLock,  EventType::kResRwWriteLock,
        EventType::kResRwUnlock,    EventType::kResCvWait,
        EventType::kResCvNotify,    EventType::kResCvNotifyAll,
        EventType::kResWgAdd,       EventType::kResWgDone,
    };
    static constexpr EventType kV4Types[] = {
        EventType::kResMutexLock,   EventType::kResMutexUnlock,
        EventType::kResPCWait,      EventType::kResPCNotify,
        EventType::kResChannelSend, EventType::kResChannelRecv,
    };
    Event event;
    if (v5_only) {
        event.type = kV5Types[index % 8U];
    } else {
        event.type = (index % 3U == 0)
                         ? kV4Types[index % 6U]
                         : kV5Types[index % 8U];
    }
    event.gid = static_cast<std::uint32_t>(index % 64U);
    event.channel = go2cpp::replay::kNoChannel;
    event.resource_id = (index % 1024U) + 1U;
    event.actor_gid = index % 64U;
    event.target_gid = (index * 7U) % 64U;
    event.resource_result = static_cast<std::uint32_t>(index % 3U);
    event.address = 0x7f0000000000ULL + (index % 1024U) * 64U;
    event.waiters = static_cast<std::uint32_t>(index % 8U);
    event.res_fd = -1;
    event.res_bytes = static_cast<std::uint32_t>(index % 512U);
    event.res_aux = index % 7U;
    event.value = static_cast<std::int64_t>(index);
    event.ts_ns = 1000000000ULL + index * 1000ULL;
    event.vtime_ns = index * 1000ULL;
    return event;
}

/// 与 Append 相同的 seq 基准不可预知（批预留），这里只按事件内容算 checksum；
/// MixValue 已刻意不含 seq，因此跨引擎可比较。
std::uint64_t ExpectedChecksum(std::uint64_t n, bool v5_only) {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        sum += MixValue(MakeEvent(i, v5_only));
    }
    return sum;
}

struct RoundTrip {
    WriterStats stats;
    ReadStats read;
    std::size_t segments{0};
};

bool WriteEvents(const std::string& path, Engine engine, SyncPolicy sync,
                 std::uint64_t n, std::uint32_t threads,
                 std::size_t batch_bytes, std::size_t segment_bytes,
                 bool v5_only, std::uint32_t interval_ms,
                 std::size_t sync_bytes, RoundTrip* result,
                 bool sleepy = false) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    WriterOptions options;
    options.path = path;
    options.engine = engine;
    options.sync = sync;
    options.batch_bytes = batch_bytes;
    options.segment_bytes = segment_bytes;
    options.interval_ms = interval_ms;
    options.sync_bytes = sync_bytes;
    std::string error;
    std::unique_ptr<EventWriter> writer = EventWriter::Create(options, &error);
    if (!writer) {
        std::fprintf(stderr, "Create 失败: %s\n", error.c_str());
        return false;
    }
    std::vector<Producer*> producers;
    for (std::uint32_t t = 0; t < threads; ++t) {
        producers.push_back(writer->NewProducer());
    }
    std::atomic<std::uint64_t> next{0};
    std::vector<std::thread> workers;
    std::vector<std::string> errors(threads);
    for (std::uint32_t t = 0; t < threads; ++t) {
        const std::uint64_t count =
            n / threads + (t < n % threads ? 1U : 0U);
        workers.emplace_back([&, t, count] {
            std::uint64_t local = 0;
            for (std::uint64_t i = 0; i < count; ++i) {
                const std::uint64_t index =
                    next.fetch_add(1, std::memory_order_relaxed);
                if (!writer->Append(producers[t], MakeEvent(index, v5_only))) {
                    errors[t] = "Append 失败";
                    return;
                }
                ++local;
                if (sleepy && local % 5000 == 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(25));
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    for (const auto& message : errors) {
        if (!message.empty()) {
            std::fprintf(stderr, "%s\n", message.c_str());
            return false;
        }
    }
    if (!writer->Finish(&error)) {
        std::fprintf(stderr, "Finish 失败: %s\n", error.c_str());
        return false;
    }
    result->stats = writer->Stats();
    result->segments = writer->Segments().size();
    std::unique_ptr<SessionReader> reader = SessionReader::Open(path, &error);
    if (!reader) {
        std::fprintf(stderr, "Open 失败: %s\n", error.c_str());
        return false;
    }
    result->read = reader->ScanAll();
    return true;
}

void TestEncoderRandom() {
    std::mt19937_64 rng(1234567);
    std::vector<std::uint8_t> buffer(256);
    for (int iteration = 0; iteration < 5000; ++iteration) {
        Event event = MakeEvent(rng() % 100000, true);
        event.seq = rng();
        event.flags = static_cast<std::uint16_t>(rng() % 8);
        const std::size_t size = go2cpp::persistence::EncodeFixed(
            event, buffer.data());
        CHECK(size == go2cpp::persistence::kResV5RecordSize);
        Event decoded;
        std::size_t consumed = 0;
        std::string error;
        CHECK(go2cpp::replay::DecodeEvent(buffer.data(), size, &decoded,
                                          &consumed, &error));
        CHECK(consumed == size);
        CHECK(decoded.type == event.type);
        CHECK(decoded.seq == event.seq);
        CHECK(decoded.ts_ns == event.ts_ns);
        CHECK(decoded.vtime_ns == event.vtime_ns);
        CHECK(decoded.gid == event.gid);
        CHECK(decoded.resource_id == event.resource_id);
        CHECK(decoded.actor_gid == event.actor_gid);
        CHECK(decoded.target_gid == event.target_gid);
        CHECK(decoded.resource_result == event.resource_result);
        CHECK(decoded.address == event.address);
        CHECK(decoded.waiters == event.waiters);
        CHECK(decoded.res_fd == event.res_fd);
        CHECK(decoded.res_bytes == event.res_bytes);
        CHECK(decoded.res_aux == event.res_aux);
        CHECK(decoded.value == event.value);
    }
    std::printf("[ok] encoder random roundtrip (5000)\n");
}

void TestEngineRoundTrip(Engine engine, const char* name, std::uint32_t threads,
                         SyncPolicy sync, std::size_t segment_bytes) {
    const std::uint64_t n = 20000;
    RoundTrip result;
    const std::string path = PathFor(std::string("roundtrip_") + name);
    CHECK(WriteEvents(path, engine, sync, n, threads, 64U * 1024U,
                      segment_bytes, true, 20, 256U * 1024U, &result));
    CHECK(result.stats.events == n);
    CHECK(result.read.records == n);
    CHECK(result.read.checksum == ExpectedChecksum(n, true));
    CHECK(result.read.checksum == result.stats.logical_checksum);
    CHECK(result.read.crc_ok);
    CHECK(result.read.seq_ok);
    CHECK(result.read.error.empty());
    if (engine == Engine::kSegmented) {
        CHECK(result.segments >= 2);
    }
    if (sync != SyncPolicy::kNone) {
        CHECK(result.stats.fsyncs > 0);
    }
    std::printf("[ok] %s roundtrip n=%llu threads=%u segments=%zu\n", name,
                static_cast<unsigned long long>(n), threads,
                result.segments);
}

void TestRangeRead() {
    const std::uint64_t n = 20000;
    RoundTrip result;
    const std::string path = PathFor("range");
    CHECK(WriteEvents(path, Engine::kSegmented, SyncPolicy::kNone, n, 1,
                      64U * 1024U, 256U * 1024U, true, 100, 0, &result));
    std::string error;
    std::unique_ptr<SessionReader> reader = SessionReader::Open(path, &error);
    CHECK(reader != nullptr);
    if (!reader) {
        return;
    }
    const ReadStats range = reader->ReadSeqRange(5001, 6000);
    CHECK(range.records == 1000);
    CHECK(range.crc_ok);
    CHECK(range.seq_ok);
    std::uint64_t expected = 0;
    for (std::uint64_t i = 5000; i < 6000; ++i) {
        expected += MixValue(MakeEvent(i, true));
    }
    CHECK(range.checksum == expected);
    // 时间范围：事件 i 的 ts = 1e9 + i*1000。
    const ReadStats by_time =
        reader->ReadTimeRange(1000000000ULL + 5000 * 1000ULL,
                              1000000000ULL + 5999 * 1000ULL);
    CHECK(by_time.records == 1000);
    CHECK(by_time.checksum == expected);
    // 全时间范围。
    const ReadStats all_time =
        reader->ReadTimeRange(0, UINT64_MAX);
    CHECK(all_time.records == n);
    CHECK(all_time.checksum == ExpectedChecksum(n, true));
    std::printf("[ok] segmented range read seq/time (%llu, %llu)\n",
                static_cast<unsigned long long>(range.records),
                static_cast<unsigned long long>(by_time.records));
}

void TestV4V5Mixed() {
    const std::uint64_t n = 9000;
    RoundTrip result;
    const std::string path = PathFor("mixed");
    CHECK(WriteEvents(path, Engine::kMpsc, SyncPolicy::kNone, n, 3,
                      128U * 1024U, 8U * 1024U * 1024U, false, 100, 0,
                      &result));
    CHECK(result.stats.events == n);
    CHECK(result.read.records == n);
    CHECK(result.read.crc_ok);
    CHECK(result.read.seq_ok);
    std::printf("[ok] v4/v5 mixed records=%llu\n",
                static_cast<unsigned long long>(result.read.records));
}

void TestTruncationAndCorruption() {
    const std::uint64_t n = 2000;
    RoundTrip result;
    const std::string path = PathFor("fault");
    CHECK(WriteEvents(path, Engine::kMpsc, SyncPolicy::kNone, n, 1,
                      64U * 1024U, 8U * 1024U * 1024U, true, 100, 0,
                      &result));
    struct stat st {};
    CHECK(::stat(path.c_str(), &st) == 0);
    CHECK(::truncate(path.c_str(), st.st_size - 50) == 0);
    std::string error;
    std::unique_ptr<SessionReader> reader = SessionReader::Open(path, &error);
    CHECK(reader != nullptr);
    if (reader) {
        const ReadStats truncated = reader->ScanAll();
        CHECK(!truncated.error.empty() || !truncated.crc_ok);
    }

    // 重新写一份，翻转 payload 中的一个字节，必须被逐条 CRC 拒绝。
    const std::string corrupt_path = PathFor("corrupt");
    CHECK(WriteEvents(corrupt_path, Engine::kMpsc, SyncPolicy::kNone, n, 1,
                      64U * 1024U, 8U * 1024U * 1024U, true, 100, 0,
                      &result));
    const int fd = ::open(corrupt_path.c_str(), O_RDWR);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const std::uint8_t bad = 0xFF;
        CHECK(::pwrite(fd, &bad, 1, 32 + 40 + 10) == 1);
        ::close(fd);
    }
    reader = SessionReader::Open(corrupt_path, &error);
    CHECK(reader != nullptr);
    if (reader) {
        const ReadStats corrupt = reader->ScanAll();
        CHECK(!corrupt.crc_ok);
    }
    std::printf("[ok] truncation/corruption detected\n");
}

void TestManifestValidation() {
    RoundTrip result;
    const std::string path = PathFor("manifest");
    CHECK(WriteEvents(path, Engine::kSegmented, SyncPolicy::kNone, 5000, 1,
                      64U * 1024U, 128U * 1024U, true, 100, 0, &result));
    std::string error;
    std::unique_ptr<SessionReader> reader = SessionReader::Open(path, &error);
    CHECK(reader != nullptr);
    // 追加 8 字节使段大小与 manifest 不一致，Open 必须失败。
    const std::string segment = path + ".seg-000001";
    const int fd = ::open(segment.c_str(), O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const char padding[8] = {};
        CHECK(::write(fd, padding, sizeof(padding)) == 8);
        ::close(fd);
    }
    reader = SessionReader::Open(path, &error);
    CHECK(reader == nullptr);
    CHECK(!error.empty());
    std::printf("[ok] manifest/segment size validation\n");
}

void TestEmptyAndSingle() {
    for (Engine engine : {Engine::kMpsc, Engine::kSegmented}) {
        RoundTrip result;
        const std::string path =
            PathFor(engine == Engine::kMpsc ? "empty_mpsc" : "empty_seg");
        CHECK(WriteEvents(path, engine, SyncPolicy::kNone, 0, 1, 64U * 1024U,
                          128U * 1024U, true, 100, 0, &result));
        CHECK(result.stats.events == 0);
        CHECK(result.read.records == 0);
        CHECK(result.read.crc_ok);
    }
    RoundTrip single;
    CHECK(WriteEvents(PathFor("single"), Engine::kMpsc, SyncPolicy::kBatch,
                      1, 1, 64U * 1024U, 8U * 1024U * 1024U, true, 100,
                      1024U, &single));
    CHECK(single.read.records == 1);
    CHECK(single.read.checksum == ExpectedChecksum(1, true));
    std::printf("[ok] empty/single event\n");
}

void TestIntervalFsync() {
    const std::uint64_t n = 40000;
    RoundTrip result;
    CHECK(WriteEvents(PathFor("interval"), Engine::kMpsc,
                      SyncPolicy::kInterval, n, 1, 64U * 1024U,
                      8U * 1024U * 1024U, true, 20, 0, &result, true));
    CHECK(result.stats.fsyncs >= 2);
    CHECK(result.stats.max_sync_gap_ns > 0);
    CHECK(result.stats.max_sync_gap_ns < 500000000ULL);
    CHECK(result.read.records == n);
    std::printf("[ok] interval fsync fsyncs=%llu max_gap=%.1fms\n",
                static_cast<unsigned long long>(result.stats.fsyncs),
                static_cast<double>(result.stats.max_sync_gap_ns) / 1e6);
}

void TestCrossEngineChecksum() {
    const std::uint64_t n = 12000;
    const std::uint64_t expected = ExpectedChecksum(n, true);
    for (Engine engine : {Engine::kBuffered, Engine::kMpsc,
                          Engine::kSegmented}) {
        RoundTrip result;
        const std::string name = std::string("cross_") +
                                 go2cpp::persistence::EngineName(engine);
        CHECK(WriteEvents(PathFor(name), engine, SyncPolicy::kNone, n, 2,
                          128U * 1024U, 512U * 1024U, true, 100, 0,
                          &result));
        CHECK(result.read.records == n);
        CHECK(result.read.checksum == expected);
        CHECK(result.stats.logical_checksum == expected);
    }
    std::printf("[ok] cross-engine checksum consistent\n");
}

}  // namespace

int main() {
    std::error_code ec;
    std::filesystem::remove_all(g_root, ec);
    std::filesystem::create_directories(g_root, ec);
    if (ec) {
        std::fprintf(stderr, "创建临时目录失败: %s\n", ec.message().c_str());
        return 2;
    }
    TestEncoderRandom();
    TestEngineRoundTrip(Engine::kBuffered, "buffered", 1, SyncPolicy::kNone,
                        8U * 1024U * 1024U);
    TestEngineRoundTrip(Engine::kMpsc, "mpsc", 4, SyncPolicy::kInterval,
                        8U * 1024U * 1024U);
    TestEngineRoundTrip(Engine::kSegmented, "segmented", 4, SyncPolicy::kNone,
                        512U * 1024U);
    TestRangeRead();
    TestV4V5Mixed();
    TestTruncationAndCorruption();
    TestManifestValidation();
    TestEmptyAndSingle();
    TestIntervalFsync();
    TestCrossEngineChecksum();
    std::filesystem::remove_all(g_root, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "失败断言: %d\n", g_failures);
        return 1;
    }
    std::printf("全部通过\n");
    return 0;
}
