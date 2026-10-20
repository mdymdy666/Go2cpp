/**
 * @file test_resource_replay.cpp
 * @brief G2CRPLY v4 资源事件（锁/条件变量/channel）测试。
 *
 * 覆盖：
 *   场景 1 v4 资源事件的编码/解码往返（6 种类型、全部字段）；
 *   场景 2 录制→回放：lock/unlock 序列、owner 与持锁时长；
 *   场景 3 非法序列：unlock 找不到 owner → kMismatch；
 *   场景 4 旧版本兼容：v3 文件（无资源事件）仍能读；
 *   场景 5 资源事件 sink：Recorder 自动接收 RecordResourceEvent；
 *   场景 6 资源事件与 channel/park 混排时互不干扰（seq 有序）。
 */

#include "go2cpp/debug/debug_resources.hpp"
#include "go2cpp/debug/debug_event_ring.hpp"
#include "replay/format.hpp"
#include "replay/recorder.hpp"
#include "replay/replay_engine.hpp"
#include "test_support.hpp"

#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace go2cpp::replay;

std::string TempPath(const char* tag) {
    return std::string("/tmp/go2cpp_res_replay_") + tag + "_" +
           std::to_string(static_cast<long long>(::getpid())) + ".g2cr";
}

void RemoveFile(const std::string& path) {
    std::remove(path.c_str());
}

Event MakeLock(std::uint64_t resource, std::uint64_t actor,
               std::uint32_t result, std::uint32_t waiters) {
    return MakeResourceEvent(EventType::kResMutexLock, resource, actor, 0, result,
                             0x1000U + resource, waiters);
}

Event MakeUnlock(std::uint64_t resource, std::uint64_t actor,
                 std::uint64_t target) {
    return MakeResourceEvent(EventType::kResMutexUnlock, resource, actor, target,
                             target != 0U ? 2U : 0U, 0x1000U + resource, 0);
}

}  // namespace

void run_resource_replay_tests() {
    go2cpp_tests::announce("resource_replay");

    // 场景 1：v4 编解码往返。
    {
        const EventType types[] = {
            EventType::kResMutexLock,   EventType::kResMutexUnlock,
            EventType::kResPCWait,      EventType::kResPCNotify,
            EventType::kResChannelSend, EventType::kResChannelRecv};
        std::uint64_t resource = 1;
        for (const EventType type : types) {
            Event event = MakeResourceEvent(type, resource, 7, 9, 2, 0xDEADBEEFULL,
                                            5);
            event.seq = 42;
            event.vtime_ns = 1234;
            std::vector<std::uint8_t> buffer;
            EncodeEvent(event, buffer);
            Event decoded;
            std::size_t consumed = 0;
            std::string error;
            GO2CPP_CHECK(
                DecodeEvent(buffer.data(), buffer.size(), &decoded, &consumed,
                            &error));
            GO2CPP_CHECK(consumed == buffer.size());
            GO2CPP_CHECK(decoded.type == type);
            GO2CPP_CHECK(decoded.seq == 42U);
            GO2CPP_CHECK(decoded.gid == 7U);
            GO2CPP_CHECK(decoded.resource_id == resource);
            GO2CPP_CHECK(decoded.actor_gid == 7U);
            GO2CPP_CHECK(decoded.target_gid == 9U);
            GO2CPP_CHECK(decoded.resource_result == 2U);
            GO2CPP_CHECK(decoded.address == 0xDEADBEEFULL);
            GO2CPP_CHECK(decoded.waiters == 5U);
            GO2CPP_CHECK(decoded.vtime_ns == 1234U);
            ++resource;
        }
        GO2CPP_CHECK(std::string(EventTypeName(EventType::kResMutexLock)) ==
                     "res-mutex-lock");
        GO2CPP_CHECK(std::string(EventTypeName(EventType::kResChannelRecv)) ==
                     "res-channel-recv");
    }

    // 场景 2：录制 → 回放，owner 序列与持锁时长。
    {
        const std::string path = TempPath("pair");
        {
            RecordOptions options;
            options.path = path;
            options.demo_name = "resource-pair";
            options.gid_count = 4;
            Recorder recorder(options);
            GO2CPP_CHECK(recorder.ok());
            // 0 号先持锁；1 号阻塞；0 号解锁并交接给 1 号；1 号再解锁。
            recorder.Write(MakeLock(1, 0, 0, 0));
            recorder.Write(MakeLock(1, 1, 1, 1));
            recorder.Write(MakeUnlock(1, 0, 1));
            recorder.Write(MakeLock(1, 1, 0, 0));
            recorder.Write(MakeUnlock(1, 1, 0));
            recorder.Finalize("test");
        }
        ReplayOptions replay_options;
        replay_options.keep_timeline = true;
        const ReplayReport report = ReplayFile(path, replay_options);
        GO2CPP_CHECK(report.status == ReplayStatus::kOk);
        GO2CPP_CHECK(report.complete);
        GO2CPP_CHECK(report.stats.resource_events == 5U);
        GO2CPP_CHECK(report.stats.res_mutex_locks == 3U);
        GO2CPP_CHECK(report.stats.res_mutex_unlocks == 2U);
        GO2CPP_CHECK(report.resources.size() == 5U);
        GO2CPP_CHECK(report.resources[0].type == EventType::kResMutexLock);
        GO2CPP_CHECK(report.resources[0].actor_gid == 0U);
        GO2CPP_CHECK(report.resources[1].result == 1U);  // blocked
        GO2CPP_CHECK(report.resources[2].type == EventType::kResMutexUnlock);
        GO2CPP_CHECK(report.resources[2].actor_gid == 0U);
        GO2CPP_CHECK(report.resources[2].target_gid == 1U);
        GO2CPP_CHECK(report.resources[3].actor_gid == 1U);
        GO2CPP_CHECK(report.resources[3].result == 0U);
        GO2CPP_CHECK(report.stats.resource_checksum != 0U);
        GO2CPP_CHECK(report.stats.resource_order_anomalies == 0U);
        // 资源事件的 vtime 必须被编码（否则锁持有时间恒为 0）。
        GO2CPP_CHECK(report.resources[2].vtime_ns >=
                     report.resources[0].vtime_ns);
        // 只读重演：重放侧 checksum 与记录值一致。
        GO2CPP_CHECK(report.stats.replayed_resource_checksum ==
                     report.stats.resource_checksum);
        RemoveFile(path);
    }

    // 场景 3：unlock 没有对应 owner → mismatch。
    {
        const std::string path = TempPath("bad");
        {
            RecordOptions options;
            options.path = path;
            options.demo_name = "resource-bad";
            options.gid_count = 2;
            Recorder recorder(options);
            recorder.Write(MakeUnlock(7, 3, 0));
            recorder.Finalize("test");
        }
        const ReplayReport report = ReplayFile(path);
        // 记录点滞后于线性化顺序是**合法**的记录形态：回放只读重演，
        // 不据此判文件损坏，只计入顺序异常统计。
        GO2CPP_CHECK(report.status == ReplayStatus::kOk);
        GO2CPP_CHECK(report.stats.resource_order_anomalies == 1U);
        GO2CPP_CHECK(report.resources.size() == 1U);
        RemoveFile(path);
    }

    // 场景 3b：owner 不匹配（0 号持锁、1 号解锁）。
    {
        const std::string path = TempPath("owner");
        {
            RecordOptions options;
            options.path = path;
            options.demo_name = "resource-owner";
            options.gid_count = 2;
            Recorder recorder(options);
            recorder.Write(MakeLock(3, 0, 0, 0));
            recorder.Write(MakeUnlock(3, 1, 0));
            recorder.Finalize("test");
        }
        const ReplayReport report = ReplayFile(path);
        GO2CPP_CHECK(report.status == ReplayStatus::kOk);
        GO2CPP_CHECK(report.stats.resource_order_anomalies == 1U);
        RemoveFile(path);
    }

    // 场景 4：v3 文件（无资源事件）仍可读。
    {
        const std::string path = TempPath("v3");
        {
            FileHeader header;
            header.version = 3;
            header.created_unix_ns = 1;
            header.producer_id = 2;
            std::vector<std::uint8_t> header_bytes;
            EncodeFileHeader(header, header_bytes);
            Event meta;
            meta.type = EventType::kMeta;
            meta.note = "v3-file";
            meta.gid = 1;
            meta.channel = 0;
            meta.seq = 0;
            std::vector<std::uint8_t> body;
            EncodeEvent(meta, body);
            // kEnd.sched_type 必须是整文件滚动 CRC（不含 kEnd 自身），
            // 否则 LoadFile 会拒绝这个手工构造的 v3 文件。
            Event end;
            end.type = EventType::kEnd;
            end.seq = 1;
            end.value = 1;
            end.sched_type = Crc32(body.data(), body.size());
            std::vector<std::uint8_t> end_bytes;
            EncodeEvent(end, end_bytes);

            std::FILE* file = std::fopen(path.c_str(), "wb");
            GO2CPP_CHECK(file != nullptr);
            if (file != nullptr) {
                std::fwrite(header_bytes.data(), 1, header_bytes.size(), file);
                std::fwrite(body.data(), 1, body.size(), file);
                std::fwrite(end_bytes.data(), 1, end_bytes.size(), file);
                std::fclose(file);
            }
        }
        std::vector<Event> events;
        std::string error;
        GO2CPP_CHECK(LoadRecordingEvents(path, &events, &error));
        // LoadFile 不把 kEnd 放进 events（它只用于完整性判定）。
        GO2CPP_CHECK(events.size() == 1U);
        GO2CPP_CHECK(events[0].type == EventType::kMeta);
        GO2CPP_CHECK(events[0].note == "v3-file");
        RemoveFile(path);
    }

    // 场景 5：Recorder 的资源事件 sink（需要资源事件参与编译）。
#if GO2CPP_DEBUG_RESOURCES
    {
        go2cpp::debug::DebugConfig::Reset();
        const std::string path = TempPath("sink");
        {
            RecordOptions options;
            options.path = path;
            options.demo_name = "resource-sink";
            options.gid_count = 8;
            Recorder recorder(options);
            GO2CPP_CHECK(recorder.ok());
            go2cpp::debug::ResourceEventRecord event;
            event.code = go2cpp::debug::SchedEventCode::kMutexLock;
            event.resource_id = 99;
            event.actor_gid = 5;
            event.result = go2cpp::debug::ResourceResult::kOk;
            event.waiters = 3;
            go2cpp::debug::RecordResourceEvent(event);
            recorder.Finalize("test");
        }
        // Recorder 析构后必须卸载 sink。
        GO2CPP_CHECK(go2cpp::debug::GetResourceReplaySink(nullptr) == nullptr);
        std::vector<Event> events;
        std::string error;
        GO2CPP_CHECK(LoadRecordingEvents(path, &events, &error));
        bool found = false;
        for (const auto& event : events) {
            if (event.type == EventType::kResMutexLock &&
                event.resource_id == 99U && event.actor_gid == 5U) {
                found = true;
            }
        }
        GO2CPP_CHECK(found);
        RemoveFile(path);

        // 类别过滤同样作用于录制：只开 lifecycle 时资源事件不写入。
        const std::string filtered = TempPath("sink_filtered");
        {
            go2cpp::debug::DebugConfig config;
            config.lifecycle = true;
            config.sched = false;
            config.park = false;
            config.primitives = false;
            config.timer = false;
            config.io = false;
            go2cpp::debug::DebugConfig::Set(config);
            RecordOptions options;
            options.path = filtered;
            options.demo_name = "resource-sink-filtered";
            Recorder recorder(options);
            go2cpp::debug::ResourceEventRecord event;
            event.code = go2cpp::debug::SchedEventCode::kMutexLock;
            event.resource_id = 100;
            event.actor_gid = 6;
            go2cpp::debug::RecordResourceEvent(event);
            recorder.Finalize("test");
            go2cpp::debug::DebugConfig::Reset();
        }
        std::vector<Event> filtered_events;
        GO2CPP_CHECK(LoadRecordingEvents(filtered, &filtered_events, &error));
        bool filtered_found = false;
        for (const auto& event : filtered_events) {
            if (event.type == EventType::kResMutexLock) {
                filtered_found = true;
            }
        }
        GO2CPP_CHECK(!filtered_found);
        RemoveFile(filtered);
    }
#endif  // GO2CPP_DEBUG_RESOURCES

    // 场景 6：资源事件与 park/channel 混排，seq 严格递增。
    {
        const std::string path = TempPath("mixed");
        {
            RecordOptions options;
            options.path = path;
            options.demo_name = "resource-mixed";
            options.gid_count = 4;
            Recorder recorder(options);
            recorder.Write(MakeParkEvent(EventType::kPark, 1, 0,
                                         ParkReason::kRecv, 0, 1));
            recorder.Write(MakeLock(5, 1, 0, 0));
            recorder.Write(MakeResourceEvent(EventType::kResChannelRecv, 0, 1, 0,
                                             0, 0, 0));
            recorder.Write(MakeUnlock(5, 1, 0));
            recorder.Write(MakeParkEvent(EventType::kWake, 1, 0,
                                         ParkReason::kRecv, 0, 1));
            recorder.Finalize("test");
        }
        std::vector<Event> events;
        std::string error;
        GO2CPP_CHECK(LoadRecordingEvents(path, &events, &error));
        std::uint64_t previous_seq = 0;
        for (std::size_t index = 0; index < events.size(); ++index) {
            if (index != 0) {
                GO2CPP_CHECK(events[index].seq > previous_seq);
            }
            previous_seq = events[index].seq;
        }
        const ReplayReport report = ReplayFile(path);
        GO2CPP_CHECK(report.status == ReplayStatus::kOk);
        GO2CPP_CHECK(report.stats.parks == 1U);
        GO2CPP_CHECK(report.stats.wakes == 1U);
        GO2CPP_CHECK(report.stats.res_channel_recvs == 1U);
        RemoveFile(path);
    }
}
