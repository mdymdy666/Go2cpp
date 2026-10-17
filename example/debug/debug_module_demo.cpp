/**
 * @file debug_module_demo.cpp
 * @brief Go2Cpp 调试模块的可运行示例（P0~P3 与 P5 的能力演示）。
 *
 * 覆盖：
 *   1. DebugScheduler 代理 + text/ini/json 三种调度器快照
 *   2. DebugEventCollector 采集调度事件并三种格式导出
 *   3. DebugMutex / DebugConditionVariable / DebugWaitGroup 的只读快照
 *   4. 协作式断点（DebugBreakpoints + Checkpoint + ScopedCheckpoint）
 *   5. dump CLI（ParseDumpCommand + RunDumpCli）写文件
 *
 * 编译：由 CMake 在 GO2CPP_BUILD_EXAMPLES 下登记（目标名 go2cpp_debug_module_demo），
 *       并且会**自动注册为 ctest 用例**，因此本程序必须快速结束且失败时返回非零。
 *
 * 运行：./build-quality/go2cpp_debug_module_demo
 */

#include "go2cpp/debug.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

/// G 状态的可读名（示例里只用于打印）。
const char* GStateLabel(go2cpp::GState state) {
    switch (state) {
        case go2cpp::GState::kNew: return "New";
        case go2cpp::GState::kRunnable: return "Runnable";
        case go2cpp::GState::kRunning: return "Running";
        case go2cpp::GState::kWaiting: return "Waiting";
        case go2cpp::GState::kDead: return "Dead";
        case go2cpp::GState::kCancelled: return "Cancelled";
        case go2cpp::GState::kFailed: return "Failed";
    }
    return "Unknown";
}

void Check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  [FAIL] %s\n", what);
        ++g_failures;
    }
}

void Section(const char* title) {
    std::printf("\n========== %s ==========\n", title);
}

}  // namespace

int main() {
    using namespace go2cpp;
    using namespace go2cpp::debug;

    // ---------- 1. 建调度器并挂上事件采集器 ----------
    Section("1. 启动调度器 + 事件采集");
    SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    config.min_workers = 2;
    Scheduler scheduler(config);
    auto collector = std::make_shared<DebugEventCollector>(4096);
    scheduler.set_observer(collector);
    scheduler.start();
    std::printf("  调度器已启动，观察者已挂载\n");

    // ---------- 2. 跑一批任务 ----------
    Section("2. 跑 20 个任务（同时演示协作式断点）");
    BreakpointSpec spec;
    spec.name = "demo_stage_2";
    const std::uint64_t breakpoint_id = DebugBreakpoints::Instance().Add(spec);
    std::printf("  已注册协作式断点 id=%llu name=%s\n",
                static_cast<unsigned long long>(breakpoint_id), spec.name.c_str());

    std::vector<std::shared_ptr<Task>> tasks;
    for (int i = 0; i < 20; ++i) {
        tasks.push_back(scheduler.spawn([] {
            // 阶段 1：业务代码
            std::this_thread::yield();
            // 阶段 2：协作式检查点（不阻塞，只登记命中）
            ScopedCheckpoint checkpoint("demo_stage_2");
            if (checkpoint.hit()) {
                // 真实场景里这里可以等待观测方 Resume
            }
        }));
    }
    for (auto& task : tasks) {
        task->wait();
    }
    std::printf("  20 个任务已完成；断点命中次数 = %llu\n",
                static_cast<unsigned long long>(DebugBreakpoints::Instance().Hits(breakpoint_id)));
    Check(DebugBreakpoints::Instance().Hits(breakpoint_id) == 20, "断点应命中 20 次");

    // ---------- 3. 调度器快照：三种格式 ----------
    Section("3. dump scheduler（text / ini / json）");
    DebugScheduler proxy(scheduler);
    const auto snapshot = proxy.Snapshot();
    std::printf("  processors=%zu workers=%zu runnable=%zu running=%s\n",
                snapshot.processor_count, snapshot.worker_count, snapshot.runnable_total,
                snapshot.running ? "true" : "false");
    std::printf("  --- text（前 8 行）---\n");
    {
        const std::string text = proxy.DumpText();
        std::size_t line = 0;
        for (std::size_t i = 0; i < text.size() && line < 8;) {
            const auto end = text.find('\n', i);
            const std::string row = text.substr(i, (end == std::string::npos ? text.size() : end) - i);
            std::printf("  | %s\n", row.c_str());
            i = (end == std::string::npos) ? text.size() : end + 1;
            ++line;
        }
    }
    std::printf("  --- ini（前 6 行）---\n");
    {
        const std::string ini = proxy.DumpIni();
        std::size_t line = 0;
        for (std::size_t i = 0; i < ini.size() && line < 6;) {
            const auto end = ini.find('\n', i);
            const std::string row = ini.substr(i, (end == std::string::npos ? ini.size() : end) - i);
            std::printf("  | %s\n", row.c_str());
            i = (end == std::string::npos) ? ini.size() : end + 1;
            ++line;
        }
    }
    {
        const std::string json = proxy.DumpJson();
        std::printf("  --- json（%zu 字节，前 2 行）---\n", json.size());
        std::printf("  | %s\n", json.substr(0, json.find('\n')).c_str());
        const auto second = json.find('\n');
        if (second != std::string::npos) {
            const auto third = json.find('\n', second + 1);
            std::printf("  | %s\n", json.substr(second + 1, third - second - 1).c_str());
        }
    }

    // ---------- 4. 事件 dump ----------
    Section("4. dump events");
    std::printf("  采集到 %llu 个事件（丢弃 %llu，待取 %zu）\n",
                static_cast<unsigned long long>(collector->Captured()),
                static_cast<unsigned long long>(collector->Dropped()),
                collector->Pending());
    Check(collector->Captured() > 0, "应采集到事件");
    {
        const std::string events = collector->DumpText(5);   // 只看前 5 条
        std::printf("  --- text（前 5 条）---\n");
        std::size_t line = 0;
        for (std::size_t i = 0; i < events.size() && line < 9;) {
            const auto end = events.find('\n', i);
            const std::string row = events.substr(i, (end == std::string::npos ? events.size() : end) - i);
            std::printf("  | %s\n", row.c_str());
            i = (end == std::string::npos) ? events.size() : end + 1;
            ++line;
        }
    }
    // dump 不应消耗事件
    const std::size_t pending_before = collector->Pending();
    (void)collector->DumpJson(3);
    Check(collector->Pending() == pending_before, "dump 不应消耗事件（只读快照）");

    // ---------- 5. 锁 / 条件变量 / WaitGroup 快照 ----------
    Section("5. 同步原语只读快照");
    {
        sync::Mutex mutex;
        DebugMutex mutex_probe(mutex);
        const auto idle = mutex_probe.Snapshot();
        std::printf("  Mutex（未持有）: held=%s waiters=%zu\n", idle.held ? "true" : "false",
                    idle.waiters);
        Check(!idle.held && idle.waiters == 0, "未持有时快照应为 held=false/waiters=0");
        if (mutex.TryLock()) {
            const auto locked = mutex_probe.Snapshot();
            std::printf("  Mutex（已持有）: held=%s\n", locked.held ? "true" : "false");
            Check(locked.held, "持有时 held 应为 true");
            mutex.Unlock();
        }
    }
    {
        sync::ConditionVariable cond;
        DebugConditionVariable cond_probe(cond);
        std::printf("  ConditionVariable: waiters=%zu\n", cond_probe.Waiters());
        Check(cond_probe.Waiters() == 0, "无条件变量等待者");
    }
    {
        sync::WaitGroup group;
        DebugWaitGroup group_probe(group);
        group.Add(3);
        std::printf("  WaitGroup: count=%lld waiters=%zu\n",
                    static_cast<long long>(group_probe.Count()), group_probe.Waiters());
        Check(group_probe.Count() == 3, "WaitGroup count 应为 3");
        group.Done();
        group.Done();
        group.Done();
        Check(group_probe.Count() == 0, "WaitGroup 归零");
    }

    // ---------- 6. dump CLI + 原子写文件 ----------
    Section("6. dump CLI（写文件）");
    {
        const std::string path = "/tmp/go2cpp-debug-demo-dump.ini";
        std::remove(path.c_str());
        // 必须先把拼好的参数存入**具名** std::string 再取 c_str()。
        // 早期版本直接写 ("--output=" + path).c_str()：临时 std::string 在整条语句
        // 结束后即析构，argv[4] 变成悬垂指针，运行时报
        // "basic_string: construction from null is not valid"。
        const std::string output_arg = "--output=" + path;
        const char* argv[] = {"go2cpp", "dump", "scheduler", "--format=ini",
                              output_arg.c_str()};
        std::string error;
        // argc 必须与数组元素个数一致。早期版本写了 6 而数组只有 5 个元素，
        // argv[5] 越界读到 nullptr，被构造成 std::string 后抛
        // basic_string: construction from null（gdb backtrace 指向 RunDumpCli）。
        // 这里直接用 sizeof 推导，从根上避免再数错。
        const int rc = RunDumpCli(static_cast<int>(sizeof(argv) / sizeof(argv[0])),
                                  argv, scheduler, &error);
        std::printf("  RunDumpCli 返回 %d%s%s\n", rc, error.empty() ? "" : " error=", error.c_str());
        Check(rc == 0, "RunDumpCli 应返回 0");

        const char* argv_events[] = {"go2cpp", "dump", "events", "--format=json"};
        const auto command = ParseDumpCommand({"dump", "events", "--format=json"});
        std::string events_json;
        const bool ok = RunEventsDump(command, *collector, &events_json, &error);
        std::printf("  dump events json 长度 = %zu\n", events_json.size());
        Check(ok && !events_json.empty(), "应能导出事件 json");
        (void)argv_events;
        std::remove(path.c_str());
    }

    // ---------- 7. 断点观测与清理 ----------
    Section("7. 断点观测");
    {
        const std::string dump = DebugBreakpoints::Instance().DumpText();
        std::size_t line = 0;
        for (std::size_t i = 0; i < dump.size() && line < 7;) {
            const auto end = dump.find('\n', i);
            const std::string row = dump.substr(i, (end == std::string::npos ? dump.size() : end) - i);
            std::printf("  | %s\n", row.c_str());
            i = (end == std::string::npos) ? dump.size() : end + 1;
            ++line;
        }
        Check(DebugBreakpoints::Instance().Waiting().empty(),
              "ScopedCheckpoint 应已自动恢复，waiting 为空");
        DebugBreakpoints::Instance().Clear();
    }


    // ---------- 8. 枚举全部 G（dump scheduler --include-fibers）----------
    Section("8. 枚举全部 G（fibers 段）");
    {
        // 制造一个长命阻塞 G，让它留在注册表里
        sync::Mutex gate;
        (void)gate.TryLock();
        auto blocked = scheduler.spawn([&gate] {
            if (gate.Lock()) {
                gate.Unlock();
            }
        });
        {
            DebugMutex probe(gate);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline && probe.Waiters() == 0) {
                std::this_thread::yield();
            }
        }

        DumpOptions with_fibers;
        with_fibers.include_fibers = true;
        const auto snap = proxy.SnapshotWith(with_fibers);
        std::printf("  枚举到 %zu 个存活 G：\n", snap.tasks.size());
        std::size_t shown = 0;
        for (const auto& t : snap.tasks) {
            if (shown++ >= 6) {
                break;
            }
            std::printf("    g=%-4llu state=%-9s queued=%-3s started=%-3s failed=%s\n",
                        static_cast<unsigned long long>(t.id), GStateLabel(t.state),
                        t.queued ? "yes" : "no", t.started ? "yes" : "no",
                        t.failed ? "yes" : "no");
        }
        Check(!snap.tasks.empty(), "应能枚举到存活 G");
        // 文本输出的 fibers 段
        const std::string text_with = proxy.DumpText(with_fibers);
        Check(text_with.find("fibers (all non-terminated G in registries)") != std::string::npos,
              "text 应含 fibers 段");
        // 默认（不要求）时 json 仍为 null
        Check(proxy.DumpJson().find("\"fibers\": null") != std::string::npos,
              "默认 json 的 fibers 应为 null");

        gate.Unlock();
        blocked->wait();
    }

    // ---------- 9. dump events --follow ----------
    Section("9. dump events --follow（跟随 2 批）");
    {
        auto follow_cmd = ParseDumpCommand({"dump", "events", "--follow=2",
                                            "--follow-interval=0", "--follow-timeout=300"});
        Check(follow_cmd.valid, "--follow 解析应成功");
        Check(follow_cmd.follow, "follow 标志应为真");
        Check(follow_cmd.follow_batches == 2, "批次上限应为 2");
        // 造几个事件再跟随
        for (int i = 0; i < 3; ++i) {
            SchedulerEvent event;
            event.type = SchedulerEventType::kTaskCompleted;
            event.task_id = static_cast<GId>(i + 1);
            event.timestamp = std::chrono::steady_clock::now();
            collector->OnEvent(event);
        }
        std::string note;
        std::string error;
        const bool ok = RunEventsDump(follow_cmd, *collector, &note, &error);
        Check(ok, "RunEventsDump(follow) 应成功");
        std::printf("  跟随结果: %s\n", note.c_str());
        Check(note.find("followed") != std::string::npos, "应报告跟随批数");
    }

    // ---------- 10. 信号驱动的跨进程 dump ----------
    Section("10. 跨进程 dump（SIGUSR1）");
    {
        std::string error;
        const bool installed = DebugSignalDumper::Install(scheduler, collector.get(), "/tmp", &error);
        Check(installed, "Install 应成功");
        std::printf("  已安装 SIGUSR1 处理器（signal=%d）；真实场景下用 `kill -USR1 <pid>` 触发\n",
                    DebugSignalDumper::SignalNumber());
        if (installed) {
            // 进程内直接 raise 模拟外部信号；处理器只置位，真正 dump 在 PollOnce 里做
            Check(std::raise(DebugSignalDumper::SignalNumber()) == 0, "raise 应成功");
            Check(DebugSignalDumper::Pending(), "应检测到待处理请求");
            std::string path;
            const auto produced = DebugSignalDumper::PollOnce(&path, &error);
            Check(produced == 1, "PollOnce 应执行一次 dump");
            std::printf("  已写出快照: %s\n", path.c_str());
            Check(path.find("go2cpp-signal-dump-") != std::string::npos, "文件名应含前缀");
            std::remove(path.c_str());
            std::remove((path + ".events.ini").c_str());
            DebugSignalDumper::Uninstall();
        }
    }

    // ---------- 收尾 ----------
    scheduler.shutdown();
    Section("结果");
    if (g_failures == 0) {
        std::printf("  所有演示步骤通过\n");
        return 0;
    }
    std::printf("  失败 %d 项\n", g_failures);
    return 1;
}
