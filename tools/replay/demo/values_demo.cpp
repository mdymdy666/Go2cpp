/**
 * @file values_demo.cpp
 * @brief 非 i64 值：`std::string` 与带 `Serialize/Deserialize` 钩子的结构体。
 *
 * 两个独立的无缓冲 channel：
 *   - `SyncChannel<std::string>`  → `ValueCodec<std::string>` 走 `data` 槽位；
 *   - `SyncChannel<Message>`     → 主模板检测 `Serialize()/Deserialize()`，
 *                                 结构体序列化成 bytes 后入 `data`。
 *
 * 录制端与重放端各自打印消息内容的 FNV 校验和；不一致即失败。
 *
 * 用法：
 * ```
 * ./go2cpp_replay_values record /tmp/values.g2cr
 * ./go2cpp_replay_values replay /tmp/values.g2cr
 * ```
 */

#include "go2cpp/scheduler.hpp"

#include "replay/auto_entry.hpp"
#include "replay/format.hpp"
#include "replay/recorder.hpp"
#include "replay/sync_channel.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace demo_values {

using go2cpp::replay::Decision;

inline constexpr std::uint32_t kProducerGid = 0;
inline constexpr std::uint32_t kConsumerGid = 1;
inline constexpr std::uint32_t kFiberCount = 2;
inline constexpr std::uint32_t kStringChannel = 0;
inline constexpr std::uint32_t kMessageChannel = 1;
inline constexpr std::uint32_t kStringCount = 8;
inline constexpr std::uint32_t kMessageCount = 8;

/// 带序列化钩子的结构体：主模板 ValueCodec<Message> 自动检测并使用。
struct Message {
    std::uint32_t id{0};
    std::string text;

    std::string Serialize() const {
        std::string bytes;
        bytes.push_back(static_cast<char>(id & 0xFFU));
        bytes.push_back(static_cast<char>((id >> 8U) & 0xFFU));
        bytes.push_back(static_cast<char>((id >> 16U) & 0xFFU));
        bytes.push_back(static_cast<char>((id >> 24U) & 0xFFU));
        bytes += text;
        return bytes;
    }

    static Message Deserialize(const std::string& bytes) {
        Message message;
        if (bytes.size() < 4U) {
            return message;
        }
        message.id = static_cast<std::uint32_t>(
                         static_cast<unsigned char>(bytes[0])) |
                     (static_cast<std::uint32_t>(
                          static_cast<unsigned char>(bytes[1]))
                      << 8U) |
                     (static_cast<std::uint32_t>(
                          static_cast<unsigned char>(bytes[2]))
                      << 16U) |
                     (static_cast<std::uint32_t>(
                          static_cast<unsigned char>(bytes[3]))
                      << 24U);
        message.text = bytes.substr(4);
        return message;
    }
};

struct Result {
    void PushString(std::string value) {
        std::lock_guard<std::mutex> lock(mutex);
        strings.push_back(std::move(value));
    }
    void PushMessage(Message value) {
        std::lock_guard<std::mutex> lock(mutex);
        messages.push_back(std::move(value));
    }
    std::size_t Count() {
        std::lock_guard<std::mutex> lock(mutex);
        return strings.size() + messages.size();
    }
    std::uint64_t Checksum() {
        std::lock_guard<std::mutex> lock(mutex);
        std::uint64_t hash = 1469598103934665603ULL;
        for (const auto& value : strings) {
            hash = go2cpp::replay::Fnv1a64(value.data(), value.size(), hash);
        }
        for (const auto& value : messages) {
            const std::string bytes = value.Serialize();
            hash = go2cpp::replay::Fnv1a64(bytes.data(), bytes.size(), hash);
        }
        return hash;
    }

    std::mutex mutex;
    std::vector<std::string> strings;
    std::vector<Message> messages;
};

inline Result& SharedResult() {
    static Result result;
    return result;
}

std::string MakeString(std::uint32_t gid, std::uint32_t index) {
    return "msg-" + std::to_string(gid) + "-" + std::to_string(index) +
           "-\xE4\xBD\xA0\xE5\xA5\xBD";  // 结尾是 UTF-8 "你好"
}

Message MakeMessage(std::uint32_t gid, std::uint32_t index) {
    Message message;
    message.id = gid * 1000U + index;
    message.text = MakeString(gid, index) + "-struct";
    return message;
}

/// 录制/重放共用的 producer 逻辑。
template <typename StringChannel, typename MessageChannel>
void Produce(StringChannel& strings, MessageChannel& messages) {
    for (std::uint32_t index = 0; index < kStringCount; ++index) {
        const std::string value = MakeString(kProducerGid, index);
        if (strings.Send(value) != Decision::kOk) {
            break;
        }
    }
    for (std::uint32_t index = 0; index < kMessageCount; ++index) {
        const Message value = MakeMessage(kProducerGid, index);
        if (messages.Send(value) != Decision::kOk) {
            break;
        }
    }
}

/// 录制/重放共用的 consumer 逻辑。
template <typename StringChannel, typename MessageChannel>
std::uint64_t Consume(StringChannel& strings, MessageChannel& messages,
                      Result* result) {
    std::uint64_t received = 0;
    for (std::uint32_t index = 0; index < kStringCount; ++index) {
        auto outcome = strings.Recv();
        if (!outcome.ok()) {
            break;
        }
        result->PushString(outcome.value);
        ++received;
    }
    for (std::uint32_t index = 0; index < kMessageCount; ++index) {
        auto outcome = messages.Recv();
        if (!outcome.ok()) {
            break;
        }
        result->PushMessage(outcome.value);
        ++received;
    }
    return received;
}

}  // namespace demo_values

namespace {

void ValuesEntry(std::uint32_t gid, go2cpp::replay::ReplaySource& source) {
    using namespace go2cpp::replay;
    using namespace demo_values;
    ScopedGid guard(gid);
    SyncChannel<std::string> strings(kStringChannel, source);
    SyncChannel<Message> messages(kMessageChannel, source);
    if (gid == kProducerGid) {
        Produce(strings, messages);
    } else {
        (void)Consume(strings, messages, &SharedResult());
    }
}

}  // namespace

RECORD_FIBER_ENTRY_NAMED("values-string-struct", ValuesEntry);

namespace {

int RunRecord(const char* path) {
    using namespace go2cpp;
    using namespace go2cpp::replay;
    using namespace demo_values;

    RecordOptions options;
    options.path = path;
    options.demo_name = "values-string-struct";
    options.gid_count = kFiberCount;
    options.channel_count = 2;
    options.value_kind = static_cast<std::int64_t>(ValueKind::kBytes);

    Recorder recorder(options);
    if (!recorder.ok()) {
        std::fprintf(stderr, "[values/record] %s\n", recorder.error().c_str());
        return 1;
    }

    std::uint64_t consumer_received = 0;
    {
        Scheduler scheduler(2);
        scheduler.start();
        SyncChannel<std::string> strings(kStringChannel, recorder);
        SyncChannel<Message> messages(kMessageChannel, recorder);

        std::vector<std::shared_ptr<Task>> tasks;
        tasks.push_back(scheduler.spawn([&] {
            ScopedFiberGid guard(kProducerGid);
            Event spawn;
            spawn.type = EventType::kSpawn;
            spawn.gid = kProducerGid;
            recorder.Write(spawn);
            Produce(strings, messages);
            Event exit;
            exit.type = EventType::kExit;
            exit.gid = kProducerGid;
            exit.value = static_cast<std::int64_t>(kStringCount + kMessageCount);
            exit.aux64 = 0;
            recorder.Write(exit);
        }));
        tasks.push_back(scheduler.spawn([&] {
            ScopedFiberGid guard(kConsumerGid);
            Event spawn;
            spawn.type = EventType::kSpawn;
            spawn.gid = kConsumerGid;
            recorder.Write(spawn);
            consumer_received = Consume(strings, messages, &SharedResult());
            Event exit;
            exit.type = EventType::kExit;
            exit.gid = kConsumerGid;
            exit.value = 0;
            exit.aux64 = consumer_received;
            recorder.Write(exit);
        }));
        for (auto& task : tasks) {
            (void)task->wait();
        }
        scheduler.shutdown();
    }
    recorder.Finalize("completed");

    std::printf("[values/record] path=%s\n", path);
    std::printf("[values/record] received=%llu checksum=%llu\n",
                static_cast<unsigned long long>(consumer_received),
                static_cast<unsigned long long>(SharedResult().Checksum()));
    const bool ok = consumer_received == kStringCount + kMessageCount;
    std::printf("[values/record] expected=%s\n", ok ? "MATCH" : "MISMATCH");
    return ok ? 0 : 1;
}

int RunReplay(const char* path) {
    using namespace go2cpp::replay;
    using namespace demo_values;

    ReplayOptions options;
    options.keep_timeline = true;  // 用记录里的原始 bytes 重算内容指纹
    const ReplayReport report = ReplayFileAuto(path, options);
    const std::size_t received = SharedResult().Count();

    // 记录侧的内容指纹：按 seq 顺序对每条成功接收的原始 bytes 做 FNV。
    std::uint64_t recorded_data_checksum = 1469598103934665603ULL;
    for (const auto& entry : report.timeline) {
        if (entry.type == go2cpp::replay::EventType::kRecv &&
            entry.decision == go2cpp::replay::Decision::kOk &&
            !entry.data.empty()) {
            recorded_data_checksum = go2cpp::replay::Fnv1a64(
                entry.data.data(), entry.data.size(), recorded_data_checksum);
        }
    }

    std::printf("%s", report.Summary().c_str());
    std::printf("[values/replay] received=%zu checksum=%llu recorded_data=%llu\n",
                received,
                static_cast<unsigned long long>(SharedResult().Checksum()),
                static_cast<unsigned long long>(recorded_data_checksum));
    std::printf("[values/replay] recorded messages=%llu bytes=%llu checksum=%llu "
                "replayed=%llu\n",
                static_cast<unsigned long long>(report.stats.messages),
                static_cast<unsigned long long>(report.stats.bytes_messages),
                static_cast<unsigned long long>(report.stats.message_checksum),
                static_cast<unsigned long long>(
                    report.stats.replayed_message_checksum));
    const bool consistent =
        report.status == ReplayStatus::kOk &&
        received == kStringCount + kMessageCount &&
        SharedResult().Checksum() == recorded_data_checksum &&
        report.stats.message_checksum == report.stats.replayed_message_checksum;
    std::printf("[values/replay] consistency=%s\n",
                consistent ? "MATCH" : "MISMATCH");
    if (!consistent) {
        std::printf("[values/replay] status=%s error=%s\n",
                    ReplayStatusName(report.status), report.error.c_str());
    }
    return consistent ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || (std::strcmp(argv[1], "record") != 0 &&
                     std::strcmp(argv[1], "replay") != 0)) {
        std::fprintf(stderr,
                     "usage: %s <record|replay> <recording.g2cr>\n", argv[0]);
        return 2;
    }
    if (std::strcmp(argv[1], "record") == 0) {
        return RunRecord(argv[2]);
    }
    return RunReplay(argv[2]);
}
