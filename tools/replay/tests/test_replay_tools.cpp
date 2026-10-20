/**
 * @file test_replay_tools.cpp
 * @brief tools/replay 的独立单测：格式 v1/v2、select/timer/ctx 编码、分片、
 *        自动入口、stop_at_seq。
 *
 * 不依赖 go2cpp 运行时；由 tools/replay/CMakeLists.txt 的 ctest 注册。
 */

#include "replay/auto_entry.hpp"
#include "replay/format.hpp"
#include "replay/recorder.hpp"
#include "replay/replay_engine.hpp"
#include "replay/split.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(condition, message)                                            \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         (message));                                         \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

using namespace go2cpp::replay;

std::string TempPath(const char* name) {
    std::string path = "/tmp/g2c-replay-test-";
    path += name;
    path += ".g2cr";
    return path;
}

void TestV1Compatibility() {
    // 手工编码一条 v1 send 记录：payload 只有 decision/blocked/rsv/op/value。
    std::vector<std::uint8_t> payload;
    ByteWriter payload_writer(payload);
    payload_writer.U8(static_cast<std::uint8_t>(Decision::kOk));
    payload_writer.U8(0);
    payload_writer.U16(0);
    payload_writer.U32(7);
    payload_writer.I64(123);

    std::vector<std::uint8_t> record;
    ByteWriter writer(record);
    writer.U32(kRecordHeaderSize + static_cast<std::uint32_t>(payload.size()));
    writer.U16(static_cast<std::uint16_t>(EventType::kSend));
    writer.U16(0);
    writer.U64(3);
    writer.U64(0);
    writer.U32(1);
    writer.U32(0);
    writer.U32(0);
    std::uint32_t crc = Crc32(record.data(), record.size());
    crc = Crc32(payload.data(), payload.size(), crc);
    writer.U32(crc);
    record.insert(record.end(), payload.begin(), payload.end());

    Event event;
    std::size_t consumed = 0;
    std::string error;
    CHECK(DecodeEvent(record.data(), record.size(), &event, &consumed, &error),
          "v1 send decode");
    CHECK(consumed == record.size(), "v1 consumed size");
    CHECK(event.type == EventType::kSend, "v1 type");
    CHECK(event.op_index == 7, "v1 op_index");
    CHECK(event.value == 123, "v1 value");
    CHECK(event.data.empty(), "v1 data empty");
    CHECK(event.vtime_ns == 0, "v1 vtime zero");

    std::vector<std::uint8_t> header_bytes;
    FileHeader version1;
    version1.version = 1;
    EncodeFileHeader(version1, header_bytes);
    FileHeader decoded;
    CHECK(DecodeFileHeader(header_bytes.data(), header_bytes.size(), &decoded,
                           &error),
          "v1 file header accepted");
    CHECK(decoded.version == 1, "v1 version preserved");
}

void TestV2RoundTrip() {
    const std::string bytes = std::string("hello\0world", 11);
    Event send;
    send.type = EventType::kSend;
    send.seq = 11;
    send.vtime_ns = 123456;
    send.gid = 1;
    send.channel = 2;
    send.op_index = 3;
    send.value = static_cast<std::int64_t>(0xABCDEF);
    send.data = bytes;
    send.decision = Decision::kOk;
    send.blocked = 1;

    std::vector<std::uint8_t> record;
    EncodeEvent(send, record);
    Event decoded;
    std::size_t consumed = 0;
    std::string error;
    CHECK(DecodeEvent(record.data(), record.size(), &decoded, &consumed, &error),
          "v2 send decode");
    CHECK(consumed == record.size(), "v2 consumed size");
    CHECK(decoded.vtime_ns == send.vtime_ns, "v2 vtime roundtrip");
    CHECK(decoded.data == bytes, "v2 bytes roundtrip");
    CHECK(decoded.value == send.value, "v2 value roundtrip");

    Event select;
    select.type = EventType::kSelect;
    select.seq = 12;
    select.gid = 4;
    select.channel = 1;
    select.op_index = 9;
    select.chosen = 2;
    select.decision = Decision::kTimedOut;
    select.vtime_ns = 777;
    select.cases.push_back(SelectCaseRecord{0, SelectCaseKind::kRecv});
    select.cases.push_back(SelectCaseRecord{1, SelectCaseKind::kSend});
    select.cases.push_back(SelectCaseRecord{kNoChannel, SelectCaseKind::kTimeout});
    record.clear();
    EncodeEvent(select, record);
    CHECK(DecodeEvent(record.data(), record.size(), &decoded, &consumed, &error),
          "v2 select decode");
    CHECK(decoded.type == EventType::kSelect, "v2 select type");
    CHECK(decoded.chosen == 2, "v2 select chosen");
    CHECK(decoded.cases.size() == 3, "v2 select cases size");
    CHECK(decoded.cases[0].kind == SelectCaseKind::kRecv, "v2 select case 0");
    CHECK(decoded.cases[2].kind == SelectCaseKind::kTimeout, "v2 select case 2");

    Event timer;
    timer.type = EventType::kTimer;
    timer.gid = 2;
    timer.op_index = 5;
    timer.timer_kind = static_cast<std::uint8_t>(TimerKind::kSleep);
    timer.timer_ns = 100000000;
    timer.vtime_ns = 42;
    record.clear();
    EncodeEvent(timer, record);
    CHECK(DecodeEvent(record.data(), record.size(), &decoded, &consumed, &error),
          "v2 timer decode");
    CHECK(decoded.timer_ns == timer.timer_ns, "v2 timer duration");
    CHECK(decoded.vtime_ns == timer.vtime_ns, "v2 timer vtime");

    Event ctx;
    ctx.type = EventType::kCtx;
    ctx.ctx_id = 7;
    ctx.ctx_kind = static_cast<std::uint8_t>(ContextEventKind::kCancelled);
    ctx.op_index = 1;
    ctx.vtime_ns = 99;
    record.clear();
    EncodeEvent(ctx, record);
    CHECK(DecodeEvent(record.data(), record.size(), &decoded, &consumed, &error),
          "v2 ctx decode");
    CHECK(decoded.ctx_id == 7, "v2 ctx id");
    CHECK(decoded.ctx_kind ==
              static_cast<std::uint8_t>(ContextEventKind::kCancelled),
          "v2 ctx kind");

    CHECK(ValueKindName(ValueKind::kBytes) == std::string("bytes"),
          "value kind name");
    CHECK(std::strcmp(EventTypeName(EventType::kSelect), "select") == 0,
          "select name");
}

void TestCrcAndFnv() {
    CHECK(Crc32("123456789", 9) == 0xCBF43926U, "CRC32 check value");
    CHECK(Fnv1a64("", 0) == 1469598103934665603ULL, "FNV offset basis");
    const std::uint64_t first = Fnv1a64("a", 1);
    const std::uint64_t second = Fnv1a64("b", 1);
    CHECK(first != second, "FNV differs");
}

void WriteFixture(const std::string& path, std::vector<Event>* out_events) {
    RecordOptions options;
    options.path = path;
    options.demo_name = "fixture";
    options.gid_count = 2;
    options.channel_count = 1;
    Recorder recorder(options);
    CHECK(recorder.ok(), "fixture recorder ok");

    Event spawn;
    spawn.type = EventType::kSpawn;
    spawn.gid = 0;
    recorder.Write(spawn);
    Event park;
    park.type = EventType::kPark;
    park.gid = 0;
    park.channel = 0;
    park.reason = ParkReason::kRecv;
    park.op_index = 0;
    recorder.Write(park);
    Event wake;
    wake.type = EventType::kWake;
    wake.gid = 0;
    wake.channel = 0;
    wake.reason = ParkReason::kRecv;
    wake.op_index = 0;
    recorder.Write(wake);
    for (int index = 0; index < 6; ++index) {
        Event note;
        note.type = EventType::kNote;
        note.note = "fixture-" + std::to_string(index);
        recorder.Write(note);
    }
    recorder.Finalize("fixture");
    if (out_events != nullptr) {
        out_events->clear();
        *out_events = {};
    }
}

void TestSplitAndStopAt() {
    const std::string path = TempPath("split-source");
    WriteFixture(path, nullptr);

    SplitOptions options;
    options.out_dir = "/tmp/g2c-replay-test-split";
    options.prefix = "chunk";
    options.every_events = 3;
    const SplitResult result = SplitRecording(path, options);
    CHECK(result.ok, "split ok");
    CHECK(result.parts.size() >= 2, "split produces multiple parts");
    CHECK(result.source_events >= 4, "split source events");
    for (const auto& part : result.parts) {
        ReplayOptions replay_options;
        replay_options.keep_timeline = true;
        const ReplayReport report = ReplayFile(part.path, replay_options);
        CHECK(report.file_size > 0, "split part readable");
        CHECK(report.status != ReplayStatus::kFormatError,
              "split part format ok");
    }

    // 只重放到第一个 park 之后：前缀里没有 wake，应报告 partial 而不是 stuck。
    const ReplayReport partial = ReplayFile(path, [] {
        ReplayOptions replay_options;
        replay_options.stop_at_seq = 2;
        return replay_options;
    }());
    CHECK(partial.stopped_early, "stop_at marks prefix");
    CHECK(partial.status == ReplayStatus::kPartial, "stop_at status partial");
    CHECK(partial.checkpoints.empty(), "no checkpoints without option");

    ReplayOptions checkpoint_options;
    checkpoint_options.checkpoint_every_events = 2;
    const ReplayReport with_checkpoints = ReplayFile(path, checkpoint_options);
    CHECK(!with_checkpoints.checkpoints.empty(), "checkpoints collected");
    CHECK(with_checkpoints.checkpoints.front().seq <=
              with_checkpoints.checkpoints.back().seq,
          "checkpoints monotonic");

    std::remove(path.c_str());
    for (const auto& part : result.parts) {
        std::remove(part.path.c_str());
    }
}

void DummyEntry(std::uint32_t, ReplaySource&) {}

void TestAutoEntry() {
    CHECK(!HasFiberEntry(), "registry starts empty");
    CHECK(RegisterFiberEntry(&DummyEntry, "dummy"), "register dummy entry");
    CHECK(HasFiberEntry(), "registry has entry");
    CHECK(GetFiberEntry() == &DummyEntry, "registry entry pointer");
    CHECK(std::strcmp(FiberEntryName(), "dummy") == 0, "registry entry name");
    CHECK(!RegisterFiberEntry(&DummyEntry, "second"), "duplicate rejected");
    CHECK(std::strcmp(FiberEntryName(), "dummy") == 0, "first entry wins");
}

}  // namespace

int main() {
    TestV1Compatibility();
    TestV2RoundTrip();
    TestCrcAndFnv();
    TestSplitAndStopAt();
    TestAutoEntry();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all replay tool checks passed\n");
    return 0;
}
