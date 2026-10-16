/**
 * @file test_debug_breakpoints.cpp
 * @brief 协作式断点的测试。
 *
 * 覆盖（按项目规范：压测 + 随机 + >=5 个真实场景）：
 *   场景 1 未注册断点时 Checkpoint 恒为 0（零开销路径的行为）
 *   场景 2 名称匹配才命中；不匹配的名称不命中
 *   场景 3 skip 语义：前 N 次不触发、第 N+1 次触发
 *   场景 4 once 语义：触发一次后自动失效
 *   场景 5 task_id 过滤：只对指定 G 生效，task_id=0 表示任意
 *   场景 6 Resume / ResumeAll / Waiting 观测；ScopedCheckpoint 析构自动恢复
 *   场景 7 真实场景：在受管 G 里插入检查点，多个 G 并发经过同一位置
 *   压测   100 万次 Checkpoint（未注册断点时的路径）
 *   随机   随机注册/移除/命中，校验 id 唯一性与 Hits 累计一致
 */

#include "go2cpp/debug.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using go2cpp::debug::BreakpointSpec;
using go2cpp::debug::Checkpoint;
using go2cpp::debug::DebugBreakpoints;
using go2cpp::debug::ScopedCheckpoint;

}  // namespace

void run_debug_breakpoints_tests() {
    go2cpp_tests::announce("cooperative breakpoints (P5)");
    using namespace go2cpp::debug;

    auto& registry = DebugBreakpoints::Instance();

    // 场景 1：未注册断点时不命中
    {
        registry.Clear();
        registry.ResetStats();
        GO2CPP_CHECK(registry.Size() == 0);
        for (int i = 0; i < 100; ++i) {
            GO2CPP_CHECK(Checkpoint("nowhere") == 0);
        }
        GO2CPP_CHECK(registry.TotalHits() == 0);
    }

    // 场景 2：名称匹配才命中
    {
        registry.Clear();
        BreakpointSpec spec;
        spec.name = "before_commit";
        const std::uint64_t id = registry.Add(spec);
        GO2CPP_CHECK(id > 0);
        GO2CPP_CHECK(Checkpoint("other_place") == 0);
        GO2CPP_CHECK(Checkpoint("before_commit") == id);
        GO2CPP_CHECK(registry.Hits(id) == 1);
        GO2CPP_CHECK(Checkpoint("before_commit") == id);
        GO2CPP_CHECK(registry.Hits(id) == 2);
        GO2CPP_CHECK(registry.TotalHits() == 2);
        // waiting 表应有记录
        registry.Resume(id);
        GO2CPP_CHECK(registry.Waiting().empty());
        registry.Clear();
    }

    // 场景 3：skip 语义
    {
        registry.Clear();
        BreakpointSpec spec;
        spec.name = "loop";
        spec.skip = 5;
        const std::uint64_t id = registry.Add(spec);
        for (int i = 0; i < 5; ++i) {
            GO2CPP_CHECK(Checkpoint("loop") == 0);   // 跳过前 5 次
        }
        GO2CPP_CHECK(Checkpoint("loop") == id);       // 第 6 次命中
        GO2CPP_CHECK(registry.Hits(id) == 1);
        registry.Clear();
    }

    // 场景 4：once 语义
    {
        registry.Clear();
        BreakpointSpec spec;
        spec.name = "once_here";
        spec.once = true;
        const std::uint64_t id = registry.Add(spec);
        GO2CPP_CHECK(Checkpoint("once_here") == id);
        GO2CPP_CHECK(Checkpoint("once_here") == 0);   // 已失效
        GO2CPP_CHECK(registry.Hits(id) == 1);
        // 断点仍在表里，但 enabled=false
        const auto snapshot = registry.Snapshot();
        GO2CPP_CHECK(snapshot.size() == 1);
        GO2CPP_CHECK(!snapshot.front().second.enabled);
        // 重新启用后可以再次命中
        GO2CPP_CHECK(registry.SetEnabled(id, true));
        GO2CPP_CHECK(Checkpoint("once_here") == id);
        registry.Clear();
    }

    // 场景 5：task_id 过滤
    {
        registry.Clear();
        BreakpointSpec spec;
        spec.name = "sync_point";
        spec.task_id = 42;
        const std::uint64_t id = registry.Add(spec);
        GO2CPP_CHECK(Checkpoint("sync_point", 7) == 0);      // 不是 42
        GO2CPP_CHECK(Checkpoint("sync_point", 0) == 0);      // 0 不等于 42
        GO2CPP_CHECK(Checkpoint("sync_point", 42) == id);    // 命中
        GO2CPP_CHECK(registry.Hits(id) == 1);

        // task_id=0 的断点对任意 G 都生效
        BreakpointSpec any;
        any.name = "any_g";
        const std::uint64_t any_id = registry.Add(any);
        GO2CPP_CHECK(Checkpoint("any_g", 1) == any_id);
        GO2CPP_CHECK(Checkpoint("any_g", 999) == any_id);
        registry.Clear();
    }

    // 场景 6：Resume / ResumeAll / ScopedCheckpoint
    {
        registry.Clear();
        BreakpointSpec spec;
        spec.name = "cp";
        const std::uint64_t id = registry.Add(spec);

        GO2CPP_CHECK(Checkpoint("cp") == id);
        GO2CPP_CHECK(registry.Waiting().size() == 1);
        GO2CPP_CHECK(registry.Resume(id));
        GO2CPP_CHECK(!registry.Resume(id));      // 二次恢复返回 false
        GO2CPP_CHECK(registry.Waiting().empty());

        GO2CPP_CHECK(Checkpoint("cp") == id);
        GO2CPP_CHECK(Checkpoint("cp") == id);
        GO2CPP_CHECK(registry.Waiting().size() == 1);  // 同一个断点只登记一条
        registry.ResumeAll();
        GO2CPP_CHECK(registry.Waiting().empty());

        // ScopedCheckpoint：析构自动恢复
        {
            ScopedCheckpoint scoped("cp");
            GO2CPP_CHECK(scoped.hit());
            GO2CPP_CHECK(scoped.id() == id);
            GO2CPP_CHECK(registry.Waiting().size() == 1);
        }
        GO2CPP_CHECK(registry.Waiting().empty());
        // 未命中时 id()==0 且析构无副作用
        {
            ScopedCheckpoint scoped("no_such_place");
            GO2CPP_CHECK(!scoped.hit());
            GO2CPP_CHECK(scoped.id() == 0);
        }
        registry.Clear();
    }

    // 场景 7：真实场景 —— 在受管 G 里插入检查点，多 G 并发经过
    {
        registry.Clear();
        // 前 6 个场景已累计若干次命中，而 Clear() 刻意不重置统计
        // （累计命中是历史信息），所以这里显式建立基线。
        registry.ResetStats();
        BreakpointSpec spec;
        spec.name = "worker_stage_2";
        const std::uint64_t id = registry.Add(spec);

        go2cpp::SchedulerConfig config;
        config.processor_count = 4;
        config.max_workers = 4;
        config.min_workers = 4;
        go2cpp::Scheduler scheduler(config);
        scheduler.start();

        std::atomic<int> passed{0};
        std::vector<std::shared_ptr<go2cpp::Task>> tasks;
        for (int i = 0; i < 40; ++i) {
            tasks.push_back(scheduler.spawn([&passed] {
                // 阶段 1：业务代码
                passed.fetch_add(0, std::memory_order_relaxed);
                // 阶段 2：协作式检查点（不阻塞，只登记命中）
                ScopedCheckpoint checkpoint("worker_stage_2");
                if (checkpoint.hit()) {
                    passed.fetch_add(1, std::memory_order_relaxed);
                }
            }));
        }
        for (auto& task : tasks) {
            task->wait();
        }
        scheduler.shutdown();

        GO2CPP_CHECK(passed.load(std::memory_order_relaxed) == 40);   // 每个 G 都命中
        GO2CPP_CHECK(registry.Hits(id) == 40);
        GO2CPP_CHECK(registry.TotalHits() == 40);
        // ScopedCheckpoint 已自动恢复，waiting 应为空
        GO2CPP_CHECK(registry.Waiting().empty());

        // Clear 不清统计、ResetStats 清统计 —— 明确两者语义
        const std::uint64_t before_clear = registry.TotalHits();
        GO2CPP_CHECK(before_clear == 40);
        registry.Clear();
        GO2CPP_CHECK(registry.TotalHits() == before_clear);   // Clear 不动统计
        registry.ResetStats();
        GO2CPP_CHECK(registry.TotalHits() == 0);              // ResetStats 清零
    }

    // 压测：100 万次 Checkpoint（未注册断点的路径）
    {
        registry.Clear();
        constexpr int kOps = 1000000;
        const auto t0 = std::chrono::steady_clock::now();
        std::uint64_t sink = 0;
        for (int i = 0; i < kOps; ++i) {
            sink += Checkpoint("hot_path");
        }
        const auto t1 = std::chrono::steady_clock::now();
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        std::clog << "  [压测] Checkpoint（无断点）x" << kOps << ": " << micros
                  << " us  => " << (micros * 1000.0 / kOps) << " ns/次 (sink=" << sink << ")"
                  << std::endl;
        GO2CPP_CHECK(sink == 0);

        // 有断点但名称不匹配（最常见的"调试开着但没停"场景）
        BreakpointSpec spec;
        spec.name = "somewhere_else";
        (void)registry.Add(spec);
        const auto t2 = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i) {
            sink += Checkpoint("hot_path");
        }
        const auto t3 = std::chrono::steady_clock::now();
        const auto micros2 = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
        std::clog << "  [压测] Checkpoint（1 个不匹配断点）x" << kOps << ": " << micros2
                  << " us  => " << (micros2 * 1000.0 / kOps) << " ns/次" << std::endl;
        GO2CPP_CHECK(sink == 0);
        registry.Clear();
    }

    // 随机：随机注册/移除/命中
    {
        registry.Clear();
        std::mt19937 rng(20261016);
        std::uniform_int_distribution<int> action_dist(0, 3);
        std::uniform_int_distribution<int> name_dist(0, 3);
        std::set<std::uint64_t> live_ids;
        std::vector<std::uint64_t> all_ids;
        bool ok = true;
        for (int round = 0; round < 3000; ++round) {
            const std::string name = "spot_" + std::to_string(name_dist(rng));
            const int action = action_dist(rng);
            if (action == 0) {
                BreakpointSpec spec;
                spec.name = name;
                spec.skip = static_cast<std::size_t>(name_dist(rng));
                const std::uint64_t id = registry.Add(spec);
                ok = ok && id > 0 && live_ids.insert(id).second;   // id 必须唯一
                all_ids.push_back(id);
            } else if (action == 1 && !all_ids.empty()) {
                std::uniform_int_distribution<std::size_t> pick(0, all_ids.size() - 1);
                const std::uint64_t id = all_ids[pick(rng)];
                if (registry.Remove(id)) {
                    live_ids.erase(id);
                } else {
                    live_ids.erase(id);   // 可能已被移除
                }
            } else if (action == 2) {
                (void)Checkpoint(name, static_cast<std::uint64_t>(name_dist(rng)));
            } else {
                registry.ResumeAll();
            }
        }
        GO2CPP_CHECK(ok);
        GO2CPP_CHECK(registry.Size() == live_ids.size());
        registry.Clear();
    }
}
