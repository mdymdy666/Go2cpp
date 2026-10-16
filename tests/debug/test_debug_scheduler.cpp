/**
 * @file test_debug_scheduler.cpp
 * @brief DebugScheduler 代理与三种序列化的测试。
 *
 * 覆盖（按项目规范：压测 + 随机 + >=5 个真实场景）：
 *   场景 1 快照字段与 Scheduler 的 public 接口逐项一致
 *   场景 2 双时间戳（steady + wall）都存在且单调合理，并标注不可跨机对齐
 *   场景 3 text 输出包含关键字段与三条诚实声明
 *   场景 4 ini 输出的节与键可被逐行解析
 *   场景 5 json 输出可被项目 json 模块解析回来（结构校验）
 *   压测   2000 次 Snapshot + 200 次三种格式 Dump
 *   随机   随机 worker 数与任务数下，快照计数与实际一致
 */

#include "go2cpp/debug.hpp"
#include "go2cpp/json.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <chrono>
#include <random>
#include <string>
#include <vector>

namespace {

struct SchedulerFixture {
    explicit SchedulerFixture(std::size_t workers) {
        go2cpp::SchedulerConfig config;
        config.processor_count = workers;
        config.max_workers = workers;
        config.min_workers = workers;
        scheduler = std::make_unique<go2cpp::Scheduler>(config);
        scheduler->start();
    }
    ~SchedulerFixture() {
        if (scheduler) {
            scheduler->shutdown();
        }
    }
    std::unique_ptr<go2cpp::Scheduler> scheduler;
};

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

void run_debug_scheduler_tests() {
    go2cpp_tests::announce("debug scheduler proxy and serialization");
    using namespace go2cpp;
    using namespace go2cpp::debug;

    // 场景 1：快照字段与 Scheduler 的 public 接口逐项一致
    {
        SchedulerFixture fixture(2);
        DebugScheduler proxy(*fixture.scheduler);
        const auto snapshot = proxy.Snapshot();
        GO2CPP_CHECK(snapshot.processor_count == fixture.scheduler->processor_count());
        GO2CPP_CHECK(snapshot.worker_count == fixture.scheduler->worker_count());
        GO2CPP_CHECK(snapshot.runnable_total == fixture.scheduler->runnable_count());
        GO2CPP_CHECK(snapshot.runnable_global == fixture.scheduler->global_runnable_count());
        GO2CPP_CHECK(snapshot.running == fixture.scheduler->is_running());
        GO2CPP_CHECK(snapshot.sysmon_running == fixture.scheduler->sysmon_running());
        GO2CPP_CHECK(snapshot.processors.size() == snapshot.processor_count);
        GO2CPP_CHECK(snapshot.machines.size() >= snapshot.processor_count);
        GO2CPP_CHECK(!snapshot.per_call_consistent);  // 如实标注：非全局原子
    }

    // 场景 2：双时间戳存在且 wall 合理（2020 年之后）
    {
        SchedulerFixture fixture(1);
        DebugScheduler proxy(*fixture.scheduler);
        const auto snapshot = proxy.Snapshot();
        const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 snapshot.wall_at.time_since_epoch())
                                 .count();
        GO2CPP_CHECK(wall_ns > 1577836800LL * 1000000000LL);  // > 2020-01-01
        GO2CPP_CHECK(snapshot.steady_at.time_since_epoch().count() > 0);
    }

    // 场景 3：text 输出含关键字段与三条诚实声明
    {
        SchedulerFixture fixture(1);
        DebugScheduler proxy(*fixture.scheduler);
        const std::string text = proxy.DumpText();
        GO2CPP_CHECK(Contains(text, "[scheduler]"));
        GO2CPP_CHECK(Contains(text, "processors="));
        GO2CPP_CHECK(Contains(text, "workers="));
        GO2CPP_CHECK(Contains(text, "sysmon:"));
        GO2CPP_CHECK(Contains(text, "per-call consistent, NOT globally atomic"));
        GO2CPP_CHECK(Contains(text, "steady is NOT cross-machine comparable"));
        GO2CPP_CHECK(Contains(text, "long_syscalls is a COUNT"));
        GO2CPP_CHECK(Contains(text, "processor"));
        GO2CPP_CHECK(Contains(text, "machine"));
    }

    // 场景 4：ini 输出的节与键可逐行解析
    {
        SchedulerFixture fixture(1);
        // 先跑一个任务：worker 是**惰性启动**的，刚 start() 时 machines() 可能为空，
        // 那样 [machine.N] 本来就不该出现。先提交并等待一个任务，确保已有 worker。
        auto warmup = fixture.scheduler->spawn([] {});
        warmup->wait();
        DebugScheduler proxy(*fixture.scheduler);
        const std::string ini = proxy.DumpIni();
        const auto snapshot = proxy.Snapshot();
        std::clog << "  [诊断] processors=" << snapshot.processors.size()
                  << " machines=" << snapshot.machines.size();
        if (!snapshot.processors.empty()) {
            std::clog << " p0.id=" << snapshot.processors.front().id;
        }
        if (!snapshot.machines.empty()) {
            std::clog << " m0.id=" << snapshot.machines.front().id;
        }
        std::clog << std::endl;
        GO2CPP_CHECK(!snapshot.processors.empty());
        GO2CPP_CHECK(!snapshot.machines.empty());
        GO2CPP_CHECK(Contains(ini, "[scheduler]"));
        GO2CPP_CHECK(Contains(ini, "processor_count="));
        GO2CPP_CHECK(Contains(ini, "snapshot_consistency=per_call"));
        GO2CPP_CHECK(Contains(ini, "[scheduler.metrics]"));
        // 用**实际 id**而不是假设从 0 开始
        GO2CPP_CHECK(Contains(ini, "[processor." +
                                    std::to_string(snapshot.processors.front().id) + "]"));
        GO2CPP_CHECK(Contains(ini, "[machine." +
                                    std::to_string(snapshot.machines.front().id) + "]"));
        // 逐行解析：每个非空行要么是 [节]，要么是 key=value
        bool parse_ok = true;
        std::size_t line_count = 0;
        std::size_t i = 0;
        while (i < ini.size()) {
            const auto end = ini.find('\n', i);
            const std::string line = ini.substr(i, (end == std::string::npos ? ini.size() : end) - i);
            i = (end == std::string::npos) ? ini.size() : end + 1;
            if (line.empty()) {
                continue;
            }
            ++line_count;
            const bool section = line.front() == '[' && line.back() == ']';
            const bool kv = line.find('=') != std::string::npos;
            if (!section && !kv) {
                parse_ok = false;
            }
        }
        GO2CPP_CHECK(parse_ok);
        GO2CPP_CHECK(line_count > 10);
    }

    // 场景 5：json 输出可被项目 json 模块解析回来
    {
        SchedulerFixture fixture(2);
        DebugScheduler proxy(*fixture.scheduler);
        const std::string text = proxy.DumpJson();
        GO2CPP_CHECK(Contains(text, "\"schema\": \"go2cpp.dump.scheduler/1\""));
        GO2CPP_CHECK(Contains(text, "\"cross_machine_aligned\": false"));
        bool parsed = false;
        try {
            const auto value = go2cpp::Json::parse(text);
            parsed = value.contains("scheduler") && value.contains("processors") &&
                     value.contains("machines");
            if (parsed) {
                parsed = value["scheduler"]["processor_count"].get<std::size_t>() ==
                         fixture.scheduler->processor_count();
            }
        } catch (...) {
            parsed = false;
        }
        GO2CPP_CHECK(parsed);
    }

    // 压测：Snapshot 2000 次 + 三种 Dump 各 200 次
    {
        SchedulerFixture fixture(2);
        DebugScheduler proxy(*fixture.scheduler);
        constexpr int kSnapshotCount = 2000;
        const auto t0 = std::chrono::steady_clock::now();
        std::size_t sink = 0;
        for (int i = 0; i < kSnapshotCount; ++i) {
            const auto s = proxy.Snapshot();
            sink += s.processor_count + s.machines.size();
        }
        const auto t1 = std::chrono::steady_clock::now();
        constexpr int kDumpCount = 200;
        for (int i = 0; i < kDumpCount; ++i) {
            sink += proxy.DumpText().size() + proxy.DumpIni().size() + proxy.DumpJson().size();
        }
        const auto t2 = std::chrono::steady_clock::now();
        std::clog << "  [压测] Snapshot x" << kSnapshotCount << ": "
                  << std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
                  << " us; 三种 Dump x" << kDumpCount << ": "
                  << std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count()
                  << " us (sink=" << sink << ")" << std::endl;
        GO2CPP_CHECK(sink > 0);
    }

    // 随机：随机 worker 数/任务数下的快照一致性
    {
        std::mt19937 rng(20261016);
        std::uniform_int_distribution<int> worker_dist(1, 4);
        std::uniform_int_distribution<int> task_dist(0, 300);
        bool all_ok = true;
        for (int round = 0; round < 6; ++round) {
            const auto workers = static_cast<std::size_t>(worker_dist(rng));
            const auto tasks = static_cast<std::size_t>(task_dist(rng));
            SchedulerFixture fixture(workers);
            DebugScheduler proxy(*fixture.scheduler);
            std::vector<std::shared_ptr<Task>> handles;
            for (std::size_t i = 0; i < tasks; ++i) {
                handles.push_back(fixture.scheduler->spawn([] {}));
            }
            for (auto& handle : handles) {
                handle->wait();
            }
            const auto snapshot = proxy.Snapshot();
            all_ok = all_ok &&
                     snapshot.processor_count == fixture.scheduler->processor_count() &&
                     snapshot.worker_count == fixture.scheduler->worker_count() &&
                     snapshot.processors.size() == snapshot.processor_count;
        }
        GO2CPP_CHECK(all_ok);
    }
}
