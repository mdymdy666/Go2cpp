/**
 * @file test_debug_events.cpp
 * @brief 事件采集（EventBuffer / DebugEventCollector）的测试。
 *
 * 覆盖（按项目规范：压测 + 随机 + >=5 个真实场景）：
 *   场景 1 EventBuffer 的空/满语义与容量边界（含环形回绕）
 *   场景 2 满时丢弃并计数（dropped 增长、captured 不增长）
 *   场景 3 采集器接到真实调度器上：spawn 任务后被采集到 kTaskStarted 一类事件
 *   场景 4 小容量采集器在大量任务下必然丢弃（有界性）
 *   场景 5 Drain 语义：取走后 pending 归零；Peek 不改变内容
 *   场景 6 三种序列化输出可解析（json 用项目 json 解析回来）
 *   压测   100 万次 Push/Pop 往返
 *   随机   随机容量/事件数下 captured + dropped 与总推送量守恒
 */

#include "go2cpp/debug.hpp"
#include "go2cpp/json.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using go2cpp::SchedulerEvent;
using go2cpp::SchedulerEventType;
using go2cpp::debug::DebugEventCollector;
using go2cpp::debug::EventBuffer;

SchedulerEvent MakeEvent(go2cpp::GId task_id, SchedulerEventType type) {
    SchedulerEvent event;
    event.type = type;
    event.task_id = task_id;
    event.machine_id = 1;
    event.processor_id = 0;
    event.timestamp = std::chrono::steady_clock::now();
    return event;
}

bool Contains(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

}  // namespace

void run_debug_events_tests() {
    go2cpp_tests::announce("debug scheduler event capture (P2)");
    using namespace go2cpp::debug;

    // 场景 1：空/满/回绕语义
    {
        EventBuffer buffer(4);   // 实际可存 4 个
        GO2CPP_CHECK(buffer.Capacity() == 4);
        GO2CPP_CHECK(buffer.Size() == 0);
        SchedulerEvent out;
        GO2CPP_CHECK(!buffer.Pop(&out));      // 空 -> Pop 失败

        for (int i = 0; i < 4; ++i) {
            GO2CPP_CHECK(buffer.Push(MakeEvent(static_cast<go2cpp::GId>(i + 1),
                                               SchedulerEventType::kTaskStarted)));
        }
        GO2CPP_CHECK(buffer.Size() == 4);
        GO2CPP_CHECK(!buffer.Push(MakeEvent(99, SchedulerEventType::kTaskStarted)));  // 满
        // FIFO 顺序
        for (int i = 0; i < 4; ++i) {
            GO2CPP_CHECK(buffer.Pop(&out));
            GO2CPP_CHECK(out.task_id == static_cast<go2cpp::GId>(i + 1));
        }
        GO2CPP_CHECK(!buffer.Pop(&out));
        // 回绕：再写满一次仍正确
        for (int round = 0; round < 3; ++round) {
            for (int i = 0; i < 4; ++i) {
                GO2CPP_CHECK(buffer.Push(MakeEvent(static_cast<go2cpp::GId>(round * 10 + i),
                                                   SchedulerEventType::kTaskCompleted)));
            }
            GO2CPP_CHECK(buffer.Size() == 4);
            for (int i = 0; i < 4; ++i) {
                GO2CPP_CHECK(buffer.Pop(&out));
            }
        }
        GO2CPP_CHECK(buffer.Size() == 0);
    }

    // 场景 2：满时丢弃并计数
    {
        EventBuffer buffer(2);
        for (int i = 0; i < 10; ++i) {
            (void)buffer.Push(MakeEvent(static_cast<go2cpp::GId>(i), SchedulerEventType::kTaskStarted));
        }
        GO2CPP_CHECK(buffer.Pushed() == 2);      // 只有 2 个入队成功
        GO2CPP_CHECK(buffer.Dropped() == 8);     // 其余 8 个被丢弃
        GO2CPP_CHECK(buffer.Size() == 2);
        GO2CPP_CHECK(buffer.Pushed() + buffer.Dropped() == 10);   // 守恒
    }

    // 场景 3：接真实调度器，能采到事件
    {
        go2cpp::SchedulerConfig config;
        config.processor_count = 2;
        config.max_workers = 2;
        config.min_workers = 2;
        go2cpp::Scheduler scheduler(config);
        auto collector = std::make_shared<DebugEventCollector>(4096);
        scheduler.set_observer(collector);
        scheduler.start();

        std::vector<std::shared_ptr<go2cpp::Task>> tasks;
        for (int i = 0; i < 50; ++i) {
            tasks.push_back(scheduler.spawn([] {}));
        }
        for (auto& task : tasks) {
            task->wait();
        }
        scheduler.shutdown();

        const auto events = collector->Peek();
        GO2CPP_CHECK(collector->Captured() > 0);
        GO2CPP_CHECK(!events.empty());
        // 至少应看到任务级事件
        bool saw_task_event = false;
        bool saw_worker_event = false;
        for (const auto& event : events) {
            if (event.type == SchedulerEventType::kTaskStarted ||
                event.type == SchedulerEventType::kTaskCompleted ||
                event.type == SchedulerEventType::kTaskFailed) {
                saw_task_event = true;
            }
            if (event.type == SchedulerEventType::kWorkerStarted ||
                event.type == SchedulerEventType::kWorkerStopped) {
                saw_worker_event = true;
            }
        }
        GO2CPP_CHECK(saw_task_event);
        GO2CPP_CHECK(saw_worker_event);
        // 事件类型名映射对已知值必须给出具体名字
        for (const auto& event : events) {
            GO2CPP_CHECK(std::string(EventTypeName(event.type)) != "Unknown");
        }
    }

    // 场景 4：小容量采集器在大负载下必然丢弃（证明有界性，不假装无损）
    {
        go2cpp::SchedulerConfig config;
        config.processor_count = 4;
        config.max_workers = 4;
        config.min_workers = 4;
        go2cpp::Scheduler scheduler(config);
        auto collector = std::make_shared<DebugEventCollector>(8);   // 极小容量
        scheduler.set_observer(collector);
        scheduler.start();
        std::vector<std::shared_ptr<go2cpp::Task>> tasks;
        for (int i = 0; i < 300; ++i) {
            tasks.push_back(scheduler.spawn([] {}));
        }
        for (auto& task : tasks) {
            task->wait();
        }
        scheduler.shutdown();
        GO2CPP_CHECK(collector->Captured() + collector->Dropped() > 0);
        GO2CPP_CHECK(collector->Dropped() > 0);       // 一定丢（容量只有 8）
        GO2CPP_CHECK(collector->Pending() <= collector->Capacity());
    }

    // 场景 5：Drain 与 Peek 语义
    {
        DebugEventCollector collector(64);
        for (int i = 0; i < 10; ++i) {
            collector.OnEvent(MakeEvent(static_cast<go2cpp::GId>(i), SchedulerEventType::kTaskStarted));
        }
        GO2CPP_CHECK(collector.Pending() == 10);
        const auto peeked = collector.Peek();
        GO2CPP_CHECK(peeked.size() == 10);
        GO2CPP_CHECK(collector.Pending() == 10);     // Peek 不消耗
        const auto taken = collector.Drain(4);
        GO2CPP_CHECK(taken.size() == 4);
        GO2CPP_CHECK(collector.Pending() == 6);
        const auto rest = collector.Drain();
        GO2CPP_CHECK(rest.size() == 6);
        GO2CPP_CHECK(collector.Pending() == 0);
        GO2CPP_CHECK(collector.Drain().empty());
        // 空采集器的序列化也要可用
        collector.Clear();
        GO2CPP_CHECK(Contains(collector.DumpText(), "(no events captured)"));
    }

    // 场景 6：三种序列化
    {
        DebugEventCollector collector(128);
        for (int i = 0; i < 5; ++i) {
            collector.OnEvent(MakeEvent(static_cast<go2cpp::GId>(i + 1), SchedulerEventType::kTaskStarted));
        }
        const std::string text = collector.DumpText();
        GO2CPP_CHECK(Contains(text, "[events]"));
        GO2CPP_CHECK(Contains(text, "capacity="));
        GO2CPP_CHECK(Contains(text, "silence != no blocking"));
        GO2CPP_CHECK(Contains(text, "NOT cross-machine comparable"));
        GO2CPP_CHECK(Contains(text, "kTaskStarted"));

        const std::string ini = collector.DumpIni();
        GO2CPP_CHECK(Contains(ini, "[events]"));
        GO2CPP_CHECK(Contains(ini, "[event.0]"));
        GO2CPP_CHECK(Contains(ini, "snapshot_consistency=per_call"));

        const std::string json = collector.DumpJson();
        GO2CPP_CHECK(Contains(json, "\"schema\": \"go2cpp.dump.events/1\""));
        bool parsed = false;
        try {
            const auto value = go2cpp::Json::parse(json);
            parsed = value.contains("events") && value.contains("dropped") &&
                     value["events"].is_array() && value["events"].size() == 5;
            if (parsed) {
                parsed = value["events"][0]["type"].get<std::string>() == "kTaskStarted";
            }
        } catch (...) {
            parsed = false;
        }
        GO2CPP_CHECK(parsed);
    }

    // 压测：100 万次 Push/Pop 往返
    {
        EventBuffer buffer(1024);
        constexpr int kOps = 1000000;
        const auto t0 = std::chrono::steady_clock::now();
        std::uint64_t checksum = 0;
        SchedulerEvent out;
        for (int i = 0; i < kOps; ++i) {
            (void)buffer.Push(MakeEvent(static_cast<go2cpp::GId>(i), SchedulerEventType::kTaskStarted));
            if (buffer.Pop(&out)) {
                checksum += out.task_id;
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        std::clog << "  [压测] EventBuffer Push/Pop x" << kOps << ": "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
                  << " ms (checksum=" << checksum << ")" << std::endl;
        GO2CPP_CHECK(checksum > 0);
    }

    // 随机：captured + dropped 与推送总量守恒
    {
        std::mt19937 rng(20261016);
        std::uniform_int_distribution<std::size_t> cap_dist(1, 64);
        std::uniform_int_distribution<std::size_t> count_dist(0, 500);
        bool ok = true;
        for (int round = 0; round < 200; ++round) {
            const std::size_t capacity = cap_dist(rng);
            const std::size_t count = count_dist(rng);
            EventBuffer buffer(capacity);
            for (std::size_t i = 0; i < count; ++i) {
                (void)buffer.Push(MakeEvent(static_cast<go2cpp::GId>(i + 1),
                                            SchedulerEventType::kTaskStarted));
            }
            ok = ok && (buffer.Pushed() + buffer.Dropped() == count);
            ok = ok && buffer.Size() <= buffer.Capacity();
            ok = ok && buffer.Size() == buffer.Pushed();
        }
        GO2CPP_CHECK(ok);
    }
}
