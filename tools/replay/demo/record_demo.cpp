/**
 * @file record_demo.cpp
 * @brief 录制端 demo：3 个 producer + 1 个 consumer 在无缓冲 channel 上跑 96 次交接。
 *
 * 用法（无参数解析；路径取 argv[1]，缺省 `replay-demo.g2cr`）：
 * ```
 * ./go2cpp_replay_record /tmp/fanin.g2cr
 * ```
 *
 * 环境变量（故障注入/超时，均非必需）：
 * - `GO2CPP_REPLAY_DROP_WAKE=N`：丢弃第 N 次唤醒 → 进程真实卡死；
 * - `GO2CPP_REPLAY_WATCHDOG_SEC=S`：看门狗秒数（默认 5）。
 *
 * 卡死时看门狗会补写一条诊断 note + `kEnd` 后 `_Exit(0)`，因此记录文件
 * 仍然可读；重放端会看到「有 park、没有对应 wake」并判定 stuck。
 */

#include "go2cpp/scheduler.hpp"

#include "demo/scenario.hpp"
#include "replay/recorder.hpp"
#include "replay/sync_channel.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_done{false};

std::uint32_t EnvU32(const char* name, std::uint32_t fallback) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return fallback;
    }
    return static_cast<std::uint32_t>(std::strtoul(raw, nullptr, 10));
}

std::uint64_t FileSize(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return 0;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fclose(file);
    return size < 0 ? 0U : static_cast<std::uint64_t>(size);
}

/// 看门狗：进程卡死时补写诊断并强制收尾，保证记录文件不是半截不可读。
void StartWatchdog(go2cpp::replay::Recorder* recorder, int seconds) {
    std::thread([recorder, seconds] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (!g_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                recorder->Note(0, go2cpp::replay::kNoChannel,
                               "watchdog: no progress within " +
                                   std::to_string(seconds) +
                                   "s; process is stuck (likely a lost wake)",
                               go2cpp::replay::kFlagInjected);
                recorder->Finalize("watchdog-timeout");
                std::fflush(nullptr);
                std::_Exit(0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }).detach();
}

}  // namespace

int main(int argc, char** argv) {
    using namespace go2cpp;
    using namespace go2cpp::replay;

    const char* path = argc > 1 ? argv[1] : "replay-demo.g2cr";
    const std::uint32_t drop_wake_at = EnvU32("GO2CPP_REPLAY_DROP_WAKE", 0);
    const std::uint32_t watchdog_seconds = EnvU32("GO2CPP_REPLAY_WATCHDOG_SEC", 5);

    RecordOptions options;
    options.path = path;
    options.demo_name = "fanin-3x32";
    options.gid_count = demo::kFiberCount;
    options.channel_count = 1;
    options.drop_wake_at = drop_wake_at;

    Recorder recorder(options);
    if (!recorder.ok()) {
        std::fprintf(stderr, "[record] %s\n", recorder.error().c_str());
        return 1;
    }
    StartWatchdog(&recorder, static_cast<int>(watchdog_seconds));

    demo::Result result;
    {
        Scheduler scheduler(2);
        scheduler.start();
        SyncChannel<std::uint64_t> channel(demo::kChannelId, recorder);

        std::vector<std::shared_ptr<Task>> tasks;
        for (std::uint32_t gid = 0; gid < demo::kProducers; ++gid) {
            tasks.push_back(scheduler.spawn([&recorder, &channel, gid] {
                ScopedFiberGid guard(gid);
                Event spawn;
                spawn.type = EventType::kSpawn;
                spawn.gid = gid;
                recorder.Write(spawn);

                const std::uint64_t sent = demo::Produce(gid, channel);

                Event exit;
                exit.type = EventType::kExit;
                exit.gid = gid;
                exit.value = static_cast<std::int64_t>(sent);
                exit.aux64 = 0;
                recorder.Write(exit);
            }));
        }
        tasks.push_back(scheduler.spawn([&recorder, &channel, &result] {
            ScopedFiberGid guard(demo::kConsumerGid);
            Event spawn;
            spawn.type = EventType::kSpawn;
            spawn.gid = demo::kConsumerGid;
            recorder.Write(spawn);

            const std::uint64_t received = demo::Consume(channel, &result);

            Event exit;
            exit.type = EventType::kExit;
            exit.gid = demo::kConsumerGid;
            exit.value = 0;
            exit.aux64 = received;
            recorder.Write(exit);
        }));

        for (auto& task : tasks) {
            (void)task->wait();
        }
        scheduler.shutdown();
    }

    g_done.store(true, std::memory_order_release);
    recorder.Finalize("completed");

    const auto received = result.Snapshot();

    std::printf("[record] path=%s\n", path);
    std::printf("[record] file_bytes=%llu events=%llu\n",
                static_cast<unsigned long long>(FileSize(path)),
                static_cast<unsigned long long>(recorder.Records()));
    std::printf("[record] consumer received=%zu checksum=%llu\n", received.size(),
                static_cast<unsigned long long>(result.Checksum()));
    if (drop_wake_at != 0) {
        std::printf("[record] fault-injection: drop wake #%u was requested\n",
                    drop_wake_at);
    }
    // 落盘失败（磁盘满 / 坏盘 / /dev/full）不能被当成成功：Finalize() 之后的
    // error() 包含 short write 与 flush 失败，必须转成非零退出码并打印原因。
    const std::string write_error = recorder.error();
    if (!write_error.empty()) {
        std::fprintf(stderr, "[record] 写入记录文件失败（%s）：%s\n", path,
                     write_error.c_str());
        return 2;
    }
    return received.size() == demo::kTotalMessages ? 0 : 1;
}
