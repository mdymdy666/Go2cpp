/**
 * @file test_resources_v5.cpp
 * @brief G2CRPLY v5 资源事件（RWMutex / ConditionVariable / WaitGroup / IO）测试。
 *
 * 覆盖：
 *   场景 1  v5 事件编解码往返（13 种类型，v4 基础字段 + v5 追加字段）；
 *   场景 2a RWMutex：读/写/解锁序列与读锁排队；
 *   场景 2b ConditionVariable：wait / notify / notify_all 与唤醒计数；
 *   场景 2c WaitGroup：add / done / wait 计数；
 *   场景 2d IO：read / write / wait / wake 与 fd / bytes；
 *   场景 3  混合场景（v4 Mutex/PC/Channel + v5 四族混排）：序列、计数、
 *           checksum 一致且两次回放确定；
 *   场景 4  v1 / v3 / v4 旧文件仍可读（手工按各版本布局编码）；
 *   场景 5  非法序列：无 lock 的 unlock 计入 resource_order_anomalies；
 *   场景 6  真实原语录制（仅 GO2CPP_DEBUG_RESOURCES=ON）：RWMutex + CV +
 *           WG + IO 走真实录制点 -> 落盘 -> 回放，断言各计数与两次回放
 *           checksum 一致。
 */

#include "go2cpp/debug/debug_resources.hpp"
#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/concurrency/rwmutex.hpp"
#include "go2cpp/channel.hpp"
#include "go2cpp/sync.hpp"
#include "go2cpp/io.hpp"
#include "go2cpp/scheduler.hpp"
#include "replay/format.hpp"
#include "replay/recorder.hpp"
#include "replay/replay_engine.hpp"
#include "test_support.hpp"

#include <unistd.h>
#include <sys/syscall.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace go2cpp::replay;

std::string TempPath(const char* tag) {
    return std::string("/tmp/go2cpp_res_v5_") + tag + "_" +
           std::to_string(static_cast<long long>(::getpid())) + ".g2cr";
}

void RemoveFile(const std::string& path) {
    std::remove(path.c_str());
}

/// 构造一条 v5 资源事件（11 参数版本的便捷包装）。
Event MakeV5(EventType type, std::uint64_t resource_id, std::uint64_t actor_gid,
             std::uint64_t target_gid, std::uint32_t result,
             std::uint32_t waiters, std::int32_t fd, std::uint32_t bytes,
             std::uint64_t aux, std::int64_t value) {
    return MakeResourceEvent(type, resource_id, actor_gid, target_gid, result,
                             type == EventType::kResIoRead ||
                                     type == EventType::kResIoWrite ||
                                     type == EventType::kResIoWait ||
                                     type == EventType::kResIoWake
                                 ? 0x2000U + static_cast<std::uint64_t>(fd)
                                 : 0x1000U + resource_id,
                             waiters, fd, bytes, aux, value);
}

/// 按指定文件版本手工追加一条记录（用于构造 v1/v3/v4 旧文件）。
void AppendLegacyRecord(std::vector<std::uint8_t>* out, EventType type,
                        std::uint64_t seq, std::uint32_t gid,
                        std::uint32_t channel,
                        const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> record;
    ByteWriter writer(record);
    writer.U32(static_cast<std::uint32_t>(kRecordHeaderSize + payload.size()));
    writer.U16(static_cast<std::uint16_t>(type));
    writer.U16(0);
    writer.U64(seq);
    writer.U64(seq);
    writer.U32(gid);
    writer.U32(channel);
    writer.U32(0);
    std::uint32_t crc = Crc32(record.data(), record.size());
    crc = Crc32(payload.data(), payload.size(), crc);
    writer.U32(crc);
    record.insert(record.end(), payload.begin(), payload.end());
    out->insert(out->end(), record.begin(), record.end());
}

/// 写一个旧版本文件：version + 若干记录 + kEnd（滚动 CRC 正确）。
void WriteLegacyFile(const std::string& path, std::uint16_t version,
                     const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> buffer;
    FileHeader header;
    header.version = version;
    EncodeFileHeader(header, buffer);
    buffer.insert(buffer.end(), body.begin(), body.end());
    const std::uint32_t rolling = Crc32(buffer.data() + kFileHeaderSize,
                                        buffer.size() - kFileHeaderSize);
    std::vector<std::uint8_t> end_payload;
    {
        ByteWriter writer(end_payload);
        writer.U64(1);            // records
        writer.U64(0);            // dropped
        writer.U32(rolling);      // 整文件滚动 CRC
        writer.U32(0);
    }
    AppendLegacyRecord(&buffer, EventType::kEnd, body.size() + 1U, 0, kNoChannel,
                       end_payload);
    std::FILE* file = std::fopen(path.c_str(), "wb");
    GO2CPP_CHECK(file != nullptr);
    if (file == nullptr) {
        return;
    }
    GO2CPP_CHECK(std::fwrite(buffer.data(), 1, buffer.size(), file) ==
                 buffer.size());
    std::fclose(file);
}

/**
 * @brief 场景 1：13 种 v5 事件编解码往返。
 */
void TestV5RoundTrip() {
    const EventType types[] = {
        EventType::kResRwReadLock,  EventType::kResRwWriteLock,
        EventType::kResRwUnlock,    EventType::kResCvWait,
        EventType::kResCvNotify,    EventType::kResCvNotifyAll,
        EventType::kResWgAdd,       EventType::kResWgDone,
        EventType::kResWgWait,      EventType::kResIoRead,
        EventType::kResIoWrite,     EventType::kResIoWait,
        EventType::kResIoWake};
    std::uint64_t resource = 1;
    for (const EventType type : types) {
        Event event = MakeV5(type, resource, 11, 13, 2, 3, 9, 128, 0x5, -4);
        event.seq = 77;
        event.vtime_ns = 4321;
        std::vector<std::uint8_t> buffer;
        EncodeEvent(event, buffer);
        GO2CPP_CHECK(buffer.size() == kRecordHeaderSize + 72U);
        Event decoded;
        std::size_t consumed = 0;
        std::string error;
        GO2CPP_CHECK(DecodeEvent(buffer.data(), buffer.size(), &decoded,
                                 &consumed, &error));
        GO2CPP_CHECK(error.empty());
        GO2CPP_CHECK(consumed == buffer.size());
        GO2CPP_CHECK(decoded.type == type);
        GO2CPP_CHECK(decoded.seq == 77U);
        GO2CPP_CHECK(decoded.resource_id == resource);
        GO2CPP_CHECK(decoded.actor_gid == 11U);
        GO2CPP_CHECK(decoded.target_gid == 13U);
        GO2CPP_CHECK(decoded.resource_result == 2U);
        GO2CPP_CHECK(decoded.waiters == 3U);
        GO2CPP_CHECK(decoded.res_fd == 9);
        GO2CPP_CHECK(decoded.res_bytes == 128U);
        GO2CPP_CHECK(decoded.res_aux == 0x5U);
        GO2CPP_CHECK(decoded.value == -4);
        GO2CPP_CHECK(decoded.vtime_ns == 4321U);
        GO2CPP_CHECK(IsResourceEventV5(type));
        ++resource;
    }
    GO2CPP_CHECK(std::string(EventTypeName(EventType::kResRwReadLock)) ==
                 "res-rw-read-lock");
    GO2CPP_CHECK(std::string(EventTypeName(EventType::kResIoWake)) ==
                 "res-io-wake");
    GO2CPP_CHECK(IsResourceEventV5(EventType::kResIoWake));
    GO2CPP_CHECK(!IsResourceEventV5(EventType::kResMutexLock));
    GO2CPP_CHECK(IsResourceEventType(EventType::kResMutexLock));
    GO2CPP_CHECK(IsResourceEventType(EventType::kResWgAdd));
    GO2CPP_CHECK(!IsResourceEventType(EventType::kNote));
}

/// 写一段事件到临时文件并回放，返回报告。
ReplayReport RecordAndReplay(const std::string& path,
                             const std::vector<Event>& events) {
    {
        RecordOptions options;
        options.path = path;
        options.demo_name = "resources-v5-test";
        options.gid_count = 8;
        Recorder recorder(options);
        GO2CPP_CHECK(recorder.ok());
        for (const auto& event : events) {
            GO2CPP_CHECK(recorder.Write(event));
        }
        recorder.Finalize("test");
    }
    ReplayOptions replay_options;
    replay_options.keep_timeline = true;
    return ReplayFile(path, replay_options);
}

/// 场景 2a：RWMutex 读/写/解锁。
void TestRwMutexScenario() {
    const std::string path = TempPath("rw");
    std::vector<Event> events;
    events.push_back(MakeV5(EventType::kResRwReadLock, 100, 1, 0, 0, 0, -1, 0,
                            static_cast<std::uint64_t>(ResourceAuxFlags::kRead),
                            0));
    events.push_back(MakeV5(EventType::kResRwWriteLock, 100, 2, 0, 1, 1, -1, 0,
                            static_cast<std::uint64_t>(ResourceAuxFlags::kWrite),
                            0));
    events.push_back(MakeV5(EventType::kResRwUnlock, 100, 1, 2, 2, 0, -1, 0,
                            static_cast<std::uint64_t>(ResourceAuxFlags::kRead),
                            0));
    events.push_back(MakeV5(EventType::kResRwWriteLock, 100, 2, 0, 0, 0, -1, 0,
                            static_cast<std::uint64_t>(ResourceAuxFlags::kWrite),
                            0));
    events.push_back(MakeV5(EventType::kResRwUnlock, 100, 2, 0, 0, 0, -1, 0,
                            static_cast<std::uint64_t>(ResourceAuxFlags::kWrite),
                            0));
    const ReplayReport report = RecordAndReplay(path, events);
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.stats.res_rw_reads == 1U);
    GO2CPP_CHECK(report.stats.res_rw_writes == 1U);
    GO2CPP_CHECK(report.stats.res_rw_unlocks == 2U);
    GO2CPP_CHECK(report.stats.res_rw_write_blocks == 1U);
    GO2CPP_CHECK(report.stats.res_rw_read_blocks == 0U);
    GO2CPP_CHECK(report.stats.resource_order_anomalies == 0U);
    GO2CPP_CHECK(report.stats.resource_events == 5U);
    GO2CPP_CHECK(report.resources.size() == 5U);
    GO2CPP_CHECK(report.resources[1].waiters == 1U);
    GO2CPP_CHECK(report.resources[2].target_gid == 2U);
    RemoveFile(path);
}

/// 场景 2b：ConditionVariable wait / notify / notify_all。
void TestConditionVariableScenario() {
    const std::string path = TempPath("cv");
    std::vector<Event> events;
    events.push_back(MakeV5(EventType::kResCvWait, 200, 1, 0, 1, 1, -1, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResCvWait, 200, 2, 0, 1, 2, -1, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResCvNotify, 200, 3, 2, 2, 0, -1, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResCvNotifyAll, 200, 3, 1, 2, 2, -1, 0,
                            static_cast<std::uint64_t>(
                                ResourceAuxFlags::kNotifyAll),
                            2));
    const ReplayReport report = RecordAndReplay(path, events);
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.stats.res_cv_waits == 2U);
    GO2CPP_CHECK(report.stats.res_cv_notifies == 1U);
    GO2CPP_CHECK(report.stats.res_cv_notify_alls == 1U);
    GO2CPP_CHECK(report.stats.res_cv_woken == 3U);
    GO2CPP_CHECK(report.resources[2].target_gid == 2U);
    GO2CPP_CHECK(report.resources[3].value == 2);
    RemoveFile(path);
}

/// 场景 2c：WaitGroup add / done / wait。
void TestWaitGroupScenario() {
    const std::string path = TempPath("wg");
    std::vector<Event> events;
    events.push_back(MakeV5(EventType::kResWgAdd, 300, 1, 0, 0, 0, -1, 0, 2, 2));
    events.push_back(MakeV5(EventType::kResWgDone, 300, 2, 0, 0, 0, -1, 0,
                            0xFFFFFFFFULL, 1));
    events.push_back(MakeV5(EventType::kResWgWait, 300, 3, 0, 1, 1, -1, 0, 0, 1));
    events.push_back(MakeV5(EventType::kResWgDone, 300, 4, 3, 2, 1, -1, 0,
                            0xFFFFFFFFULL, 0));
    const ReplayReport report = RecordAndReplay(path, events);
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.stats.res_wg_adds == 1U);
    GO2CPP_CHECK(report.stats.res_wg_dones == 2U);
    GO2CPP_CHECK(report.stats.res_wg_waits == 1U);
    GO2CPP_CHECK(report.resources[0].value == 2);
    GO2CPP_CHECK(report.resources[2].waiters == 1U);
    RemoveFile(path);
}

/// 场景 2d：IO read / write / wait / wake。
void TestIoScenario() {
    const std::string path = TempPath("io");
    std::vector<Event> events;
    events.push_back(MakeV5(EventType::kResIoRead, 400, 1, 0, 0, 0, 5, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResIoWait, 400, 1, 0, 1, 1, 5, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResIoWake, 400, 0, 1, 0, 0, 5, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResIoWrite, 400, 2, 0, 0, 0, 6, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResIoRead, 400, 3, 0, 3, 0, 7, 0, 0, 0));
    const ReplayReport report = RecordAndReplay(path, events);
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.stats.res_io_reads == 2U);
    GO2CPP_CHECK(report.stats.res_io_writes == 1U);
    GO2CPP_CHECK(report.stats.res_io_waits == 1U);
    GO2CPP_CHECK(report.stats.res_io_wakes == 1U);
    GO2CPP_CHECK(report.stats.res_io_bytes == 0U);
    for (const auto& entry : report.resources) {
        GO2CPP_CHECK(entry.fd == 5 || entry.fd == 6 || entry.fd == 7);
    }
    GO2CPP_CHECK(report.resources[0].fd == 5);
    GO2CPP_CHECK(report.resources[2].target_gid == 1U);
    RemoveFile(path);
}

/// 场景 3：v4 + v5 混合场景，序列 / 计数 / checksum 一致且可重复。
void TestMixedScenario() {
    const std::string path = TempPath("mixed");
    std::vector<Event> events;
    // v4：Mutex 加锁/解锁、PC 等待/通知、channel 收发各一组。
    events.push_back(MakeResourceEvent(EventType::kResMutexLock, 500, 1, 0, 0,
                                       0x5000, 0));
    events.push_back(MakeResourceEvent(EventType::kResMutexUnlock, 500, 1, 2, 2,
                                       0x5000, 0));
    events.push_back(MakeResourceEvent(EventType::kResPCWait, 501, 2, 0, 1,
                                       0x5001, 1));
    events.push_back(MakeResourceEvent(EventType::kResPCNotify, 501, 1, 2, 2,
                                       0x5001, 0));
    events.push_back(MakeResourceEvent(EventType::kResChannelSend, 502, 3, 0, 0,
                                       0x5002, 0));
    events.push_back(MakeResourceEvent(EventType::kResChannelRecv, 502, 4, 0, 0,
                                       0x5002, 0));
    // v5：四族各若干条，混排在不同 seq 位置。
    events.push_back(MakeV5(EventType::kResRwReadLock, 600, 5, 0, 0, 0, -1, 0,
                            static_cast<std::uint64_t>(ResourceAuxFlags::kRead),
                            0));
    events.push_back(MakeV5(EventType::kResCvWait, 601, 6, 0, 1, 1, -1, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResWgAdd, 602, 7, 0, 0, 0, -1, 0, 1, 1));
    events.push_back(MakeV5(EventType::kResIoRead, 603, 8, 0, 0, 0, 11, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResIoWait, 603, 8, 0, 1, 1, 11, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResIoWake, 603, 0, 8, 0, 0, 11, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResRwUnlock, 600, 5, 0, 0, 0, -1, 0,
                            static_cast<std::uint64_t>(ResourceAuxFlags::kRead),
                            0));
    events.push_back(MakeV5(EventType::kResCvNotify, 601, 9, 6, 2, 0, -1, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResWgDone, 602, 10, 0, 2, 0, -1, 0,
                            0xFFFFFFFFULL, 0));

    const ReplayReport first = RecordAndReplay(path, events);
    GO2CPP_CHECK(first.status == ReplayStatus::kOk);
    GO2CPP_CHECK(first.complete);
    GO2CPP_CHECK(first.stats.resource_events == events.size());
    GO2CPP_CHECK(first.stats.res_mutex_locks == 1U);
    GO2CPP_CHECK(first.stats.res_pc_waits == 1U);
    GO2CPP_CHECK(first.stats.res_channel_sends == 1U);
    GO2CPP_CHECK(first.stats.res_rw_reads == 1U);
    GO2CPP_CHECK(first.stats.res_rw_unlocks == 1U);
    GO2CPP_CHECK(first.stats.res_cv_waits == 1U);
    GO2CPP_CHECK(first.stats.res_cv_notifies == 1U);
    GO2CPP_CHECK(first.stats.res_wg_adds == 1U);
    GO2CPP_CHECK(first.stats.res_wg_dones == 1U);
    GO2CPP_CHECK(first.stats.res_io_reads == 1U);
    GO2CPP_CHECK(first.stats.res_io_waits == 1U);
    GO2CPP_CHECK(first.stats.res_io_wakes == 1U);
    GO2CPP_CHECK(first.stats.resource_order_anomalies == 0U);
    GO2CPP_CHECK(first.stats.resource_checksum ==
                 first.stats.replayed_resource_checksum);
    GO2CPP_CHECK(first.stats.res_v5_checksum != 1469598103934665603ULL);
    // 时间线 seq 严格递增，类型序列与写入顺序一致。
    for (std::size_t index = 0; index < first.resources.size(); ++index) {
        GO2CPP_CHECK(first.resources[index].type == events[index].type);
        if (index != 0) {
            GO2CPP_CHECK(first.resources[index].seq >
                         first.resources[index - 1].seq);
        }
    }
    // 二次回放（同一文件）必须逐位一致。
    const ReplayReport second = ReplayFile(path);
    GO2CPP_CHECK(second.stats.resource_checksum ==
                 first.stats.resource_checksum);
    GO2CPP_CHECK(second.stats.res_v5_checksum == first.stats.res_v5_checksum);
    GO2CPP_CHECK(second.stats.res_io_reads == first.stats.res_io_reads);
    GO2CPP_CHECK(second.stats.res_rw_reads == first.stats.res_rw_reads);
    RemoveFile(path);
}

/// 场景 4：v1 / v3 / v4 旧文件仍可读。
void TestLegacyCompatibility() {
    // v1：kMeta（v1 布局）+ kPark（8 字节 payload）+ kEnd。
    {
        const std::string path = TempPath("v1");
        std::vector<std::uint8_t> body;
        {
            std::vector<std::uint8_t> payload;
            ByteWriter writer(payload);
            writer.U32(2);   // n_gids
            writer.U32(1);   // n_channels
            writer.U32(0);   // value_kind
            writer.U32(0);   // reserved
            writer.Str("legacy-v1");
            AppendLegacyRecord(&body, EventType::kMeta, 1, 0, kNoChannel,
                               payload);
        }
        {
            std::vector<std::uint8_t> payload;
            ByteWriter writer(payload);
            writer.U8(static_cast<std::uint8_t>(ParkReason::kRecv));
            writer.U8(0);
            writer.U16(0);
            writer.U32(0);
            AppendLegacyRecord(&body, EventType::kPark, 2, 1, kNoChannel,
                               payload);
        }
        WriteLegacyFile(path, 1, body);
        std::vector<Event> events;
        std::string error;
        GO2CPP_CHECK(LoadRecordingEvents(path, &events, &error));
        GO2CPP_CHECK(error.empty());
        GO2CPP_CHECK(events.size() == 2U);
        GO2CPP_CHECK(events[0].type == EventType::kMeta);
        GO2CPP_CHECK(events[0].note == "legacy-v1");
        GO2CPP_CHECK(events[1].type == EventType::kPark);
        GO2CPP_CHECK(events[1].reason == ParkReason::kRecv);
        // ReplayFile 额外校验整文件 CRC：v1 文件的滚动 CRC 必须能通过。
        const ReplayReport report = ReplayFile(path);
        GO2CPP_CHECK(report.complete);
        GO2CPP_CHECK(report.stats.resource_events == 0U);
        GO2CPP_CHECK(report.stats.res_v5_checksum == 1469598103934665603ULL);
        RemoveFile(path);
    }
    // v3：kMeta + kTimer（无资源事件）+ kEnd。
    {
        const std::string path = TempPath("v3");
        std::vector<std::uint8_t> body;
        {
            std::vector<std::uint8_t> payload;
            ByteWriter writer(payload);
            writer.U32(1);
            writer.U32(0);
            writer.U32(0);
            writer.U32(0);
            writer.Str("legacy-v3");
            AppendLegacyRecord(&body, EventType::kMeta, 1, 0, kNoChannel,
                               payload);
        }
        {
            Event timer = MakeTimerEvent(0, TimerKind::kSleep, 1000, 0, "sleep");
            timer.seq = 2;
            std::vector<std::uint8_t> encoded;
            EncodeEvent(timer, encoded);
            // EncodeEvent 已算好单条 CRC，直接放进 body（seq 保持单调）。
            body.insert(body.end(), encoded.begin(), encoded.end());
        }
        WriteLegacyFile(path, 3, body);
        std::vector<Event> events;
        std::string error;
        GO2CPP_CHECK(LoadRecordingEvents(path, &events, &error));
        GO2CPP_CHECK(error.empty());
        GO2CPP_CHECK(events.size() == 2U);
        GO2CPP_CHECK(events[0].type == EventType::kMeta);
        GO2CPP_CHECK(events[1].type == EventType::kTimer);
        GO2CPP_CHECK(events[1].timer_ns == 1000U);
        const ReplayReport report = ReplayFile(path);
        GO2CPP_CHECK(report.complete);
        GO2CPP_CHECK(report.stats.resource_events == 0U);
        RemoveFile(path);
    }
    // v4：6 种 v4 资源事件，payload 48 字节，读端按版本接受。
    {
        const std::string path = TempPath("v4");
        std::vector<Event> events;
        events.push_back(MakeResourceEvent(EventType::kResMutexLock, 900, 1, 0,
                                           0, 0x9000, 0));
        events.push_back(MakeResourceEvent(EventType::kResMutexUnlock, 900, 1, 2,
                                           2, 0x9000, 0));
        events.push_back(MakeResourceEvent(EventType::kResPCWait, 901, 2, 0, 1,
                                           0x9001, 1));
        events.push_back(MakeResourceEvent(EventType::kResPCNotify, 901, 1, 2, 2,
                                           0x9001, 0));
        events.push_back(MakeResourceEvent(EventType::kResChannelSend, 902, 3, 0,
                                           0, 0x9002, 0));
        events.push_back(MakeResourceEvent(EventType::kResChannelRecv, 902, 4, 0,
                                           0, 0x9002, 0));
        {
            RecordOptions options;
            options.path = path;
            options.demo_name = "legacy-v4";
            options.gid_count = 4;
            Recorder recorder(options);
            GO2CPP_CHECK(recorder.ok());
            for (const auto& event : events) {
                GO2CPP_CHECK(recorder.Write(event));
            }
            recorder.Finalize("test");
        }
        const ReplayReport report = ReplayFile(path);
        GO2CPP_CHECK(report.status == ReplayStatus::kOk);
        GO2CPP_CHECK(report.stats.resource_events == 6U);
        GO2CPP_CHECK(report.stats.res_mutex_locks == 1U);
        GO2CPP_CHECK(report.stats.res_pc_notifies == 1U);
        GO2CPP_CHECK(report.stats.res_v5_checksum == 1469598103934665603ULL);
        for (const auto& entry : report.resources) {
            GO2CPP_CHECK(entry.fd == -1);
            GO2CPP_CHECK(entry.bytes == 0U);
        }
        RemoveFile(path);
    }
}

/// 场景 5：非法序列（无 lock 的 unlock）计入 order anomaly，不判损坏。
void TestOrderAnomaly() {
    const std::string path = TempPath("bad");
    std::vector<Event> events;
    events.push_back(MakeV5(EventType::kResRwUnlock, 700, 1, 0, 0, 0, -1, 0,
                            static_cast<std::uint64_t>(ResourceAuxFlags::kRead),
                            0));
    events.push_back(MakeV5(EventType::kResRwUnlock, 700, 2, 0, 0, 0, -1, 0,
                            static_cast<std::uint64_t>(ResourceAuxFlags::kWrite),
                            0));
    const ReplayReport report = RecordAndReplay(path, events);
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.stats.resource_order_anomalies == 2U);
    RemoveFile(path);
}

#if GO2CPP_DEBUG_RESOURCES
/// 场景 6：真实原语录制（RWMutex / CV / WG / IO）-> 回放。
void TestRealPrimitives() {
    const std::string path = TempPath("real");
    {
        RecordOptions options;
        options.path = path;
        options.demo_name = "resources-v5-real";
        options.gid_count = 32;
        Recorder recorder(options);
        GO2CPP_CHECK(recorder.ok());

        go2cpp::sync::RWMutex rw;
        go2cpp::sync::Mutex mutex;
        go2cpp::sync::ConditionVariable cv;
        go2cpp::sync::WaitGroup wg;
        auto rw_gate = go2cpp::MakeChannel<int>(1);
        std::atomic<bool> g1_locked{false};
        std::atomic<bool> g2_done{false};
        std::atomic<bool> cv_done{false};
        std::atomic<bool> wg_done{false};
        // Add 发生在主线程：WgAdd 事件由此产生（生产者在启动 G 之前登记）。
        wg.Add(1);

        go2cpp::SchedulerConfig config;
        config.processor_count = 2;
        config.max_workers = 2;
        go2cpp::Scheduler scheduler(config);
        scheduler.start();

        const auto wait_flag = [](const std::atomic<bool>& flag) {
            for (int attempt = 0; attempt < 2000 && !flag.load(); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return flag.load();
        };

        // G1：持读锁后 park 在 gate 上，主线程放行后才释放。
        scheduler.go([&] {
            rw.RLock();
            g1_locked.store(true, std::memory_order_release);
            (void)rw_gate->RecvFor(std::chrono::seconds(2));
            rw.RUnlock();
        });
        GO2CPP_CHECK(wait_flag(g1_locked));
        // G2：G1 仍持读锁 -> 写锁进入等待队列（kResRwWriteLock blocked）。
        scheduler.go([&] {
            rw.Lock();
            rw.Unlock();
            g2_done.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        // 放行 G1：RUnlock 把写锁交接给 G2。
        (void)rw_gate->Send(1);
        GO2CPP_CHECK(wait_flag(g2_done));

        // G3：CV 等待；G4 在它 park 之后通知（NotifyOne + NotifyAll）。
        scheduler.go([&] {
            mutex.Lock();
            (void)cv.WaitFor(mutex, std::chrono::milliseconds(500),
                             go2cpp::Background());
            mutex.Unlock();
            cv_done.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        scheduler.go([&] {
            mutex.Lock();
            cv.NotifyOne();
            cv.NotifyAll();
            mutex.Unlock();
        });
        GO2CPP_CHECK(wait_flag(cv_done));

        // G5：WaitGroup 等待；G6 完成计数 -> 归零唤醒。
        scheduler.go([&] {
            (void)wg.Wait();
            wg_done.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        scheduler.go([&] { wg.Done(); });
        GO2CPP_CHECK(wait_flag(wg_done));

        GO2CPP_CHECK(scheduler.shutdown_for(std::chrono::seconds(10)));

        // IO：独立 IOManager（自带调度器），真实 pipe 读等待。
        {
            go2cpp::SchedulerConfig io_config;
            io_config.processor_count = 1;
            io_config.max_workers = 1;
            go2cpp::IOManager manager(io_config);
            GO2CPP_CHECK(manager.Start());
            int fds[2]{-1, -1};
            GO2CPP_CHECK(::pipe(fds) == 0);
            std::atomic<bool> io_done{false};
            manager.Go([&] {
                const auto result = manager.WaitFor(
                    fds[0], go2cpp::IOEvent::kRead, std::chrono::seconds(2));
                (void)result;
                char byte = 0;
                (void)::syscall(SYS_read, fds[0], &byte, 1);
                io_done.store(true, std::memory_order_release);
            });
            const char byte = 'v';
            GO2CPP_CHECK(::syscall(SYS_write, fds[1], &byte, 1) == 1);
            for (int attempt = 0; attempt < 2000 && !io_done.load();
                 ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            GO2CPP_CHECK(io_done.load(std::memory_order_acquire));
            manager.Shutdown();
            ::close(fds[0]);
            ::close(fds[1]);
        }
        recorder.Finalize("test");
    }

    const ReplayReport report = ReplayFile(path);
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.complete);
    GO2CPP_CHECK(report.stats.resource_events > 0U);
    GO2CPP_CHECK(report.stats.res_rw_reads >= 1U);
    GO2CPP_CHECK(report.stats.res_rw_writes >= 1U);
    GO2CPP_CHECK(report.stats.res_rw_unlocks >= 2U);
    GO2CPP_CHECK(report.stats.res_cv_waits >= 1U);
    GO2CPP_CHECK(report.stats.res_cv_notifies +
                     report.stats.res_cv_notify_alls >=
                 1U);
    GO2CPP_CHECK(report.stats.res_wg_adds >= 1U);
    GO2CPP_CHECK(report.stats.res_wg_dones >= 1U);
    GO2CPP_CHECK(report.stats.res_wg_waits >= 1U);
    GO2CPP_CHECK(report.stats.res_io_reads >= 1U);
    GO2CPP_CHECK(report.stats.res_io_wakes >= 1U);
    // 真实录制的事件序列必须单调（sink 在文件锁内分配 seq）。
    for (std::size_t index = 1; index < report.resources.size(); ++index) {
        GO2CPP_CHECK(report.resources[index].seq >
                     report.resources[index - 1].seq);
    }
    // 回放是只读重演：两次结果逐位一致。
    const ReplayReport again = ReplayFile(path);
    GO2CPP_CHECK(again.stats.resource_checksum ==
                 report.stats.resource_checksum);
    GO2CPP_CHECK(again.stats.res_v5_checksum == report.stats.res_v5_checksum);
    GO2CPP_CHECK(again.stats.res_io_wakes == report.stats.res_io_wakes);
    RemoveFile(path);
}
#endif  // GO2CPP_DEBUG_RESOURCES

}  // namespace

void run_resources_v5_tests() {
    go2cpp_tests::announce("resources_v5");
    TestV5RoundTrip();
    TestRwMutexScenario();
    TestConditionVariableScenario();
    TestWaitGroupScenario();
    TestIoScenario();
    TestMixedScenario();
    TestLegacyCompatibility();
    TestOrderAnomaly();
#if GO2CPP_DEBUG_RESOURCES
    TestRealPrimitives();
#endif
}
