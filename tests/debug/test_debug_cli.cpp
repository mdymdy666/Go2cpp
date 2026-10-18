/**
 * @file test_debug_cli.cpp
 * @brief dump CLI 解析、文件 sink 与端到端的测试。
 *
 * 覆盖（按项目规范：压测 + 随机 + >=5 个真实场景）：
 *   场景 1 三种 --format 解析正确
 *   场景 2 --output / -o / 默认文件名的行为
 *   场景 3 非法选项与 --help 返回明确错误（不静默通过）
 *   场景 4 原子写入：目标存在、临时文件不残留；append 生效
 *   场景 5 端到端：解析 -> RunDump -> 文件内容可读且含关键字段
 *   压测   10000 次参数解析
 *   随机   随机选项组合解析，valid 与 error 互斥
 */

#include "go2cpp/debug.hpp"
#include "go2cpp/json.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdio>
#include <unistd.h>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool Contains(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool FileExists(const std::string& path) {
    std::ifstream in(path);
    return in.good();
}

struct Fixture {
    explicit Fixture(std::size_t workers) {
        go2cpp::SchedulerConfig config;
        config.processor_count = workers;
        config.max_workers = workers;
        config.min_workers = workers;
        scheduler = std::make_unique<go2cpp::Scheduler>(config);
        scheduler->start();
        auto warm = scheduler->spawn([] {});
        warm->wait();
    }
    ~Fixture() {
        if (scheduler) scheduler->shutdown();
    }
    std::unique_ptr<go2cpp::Scheduler> scheduler;
};

std::string TempPath(const std::string& tag) {
    return "/tmp/go2cpp-debug-cli-" + tag + "-" +
           std::to_string(::getpid()) + ".txt";
}

}  // namespace

void run_debug_cli_tests() {
    go2cpp_tests::announce("debug dump cli and file sink");
    using namespace go2cpp;
    using namespace go2cpp::debug;

    // 场景 1：三种格式解析
    {
        const auto text = ParseDumpCommand({"dump", "scheduler", "--format=text"});
        const auto ini = ParseDumpCommand({"dump", "scheduler", "--format=ini"});
        const auto json = ParseDumpCommand({"dump", "scheduler", "--format=json"});
        GO2CPP_CHECK(text.valid && text.format == DumpFormat::kText);
        GO2CPP_CHECK(ini.valid && ini.format == DumpFormat::kIni);
        GO2CPP_CHECK(json.valid && json.format == DumpFormat::kJson);
        // 不带 dump/scheduler 前缀也能解析（宽松入口）
        const auto bare = ParseDumpCommand({"--format=json"});
        GO2CPP_CHECK(bare.valid && bare.format == DumpFormat::kJson);
    }

    // 场景 2：--output / -o / 位置参数 name
    {
        const auto with_eq = ParseDumpCommand({"dump", "scheduler", "--output=/tmp/a.txt"});
        const auto with_short = ParseDumpCommand({"dump", "scheduler", "-o", "/tmp/b.txt"});
        const auto named = ParseDumpCommand({"dump", "scheduler", "my_scheduler"});
        GO2CPP_CHECK(with_eq.valid && with_eq.output == "/tmp/a.txt");
        GO2CPP_CHECK(with_short.valid && with_short.output == "/tmp/b.txt");
        GO2CPP_CHECK(named.valid && named.name == "my_scheduler");
        GO2CPP_CHECK(!DefaultDumpFileName().empty());
    }

    // 场景 3：非法选项与 --help 必须报错（不静默通过）
    {
        const auto bad_format = ParseDumpCommand({"dump", "scheduler", "--format=xml"});
        const auto bad_opt = ParseDumpCommand({"dump", "scheduler", "--nope"});
        const auto help = ParseDumpCommand({"dump", "scheduler", "--help"});
        GO2CPP_CHECK(!bad_format.valid && Contains(bad_format.error, "unknown format"));
        GO2CPP_CHECK(!bad_opt.valid && Contains(bad_opt.error, "unknown option"));
        GO2CPP_CHECK(!help.valid && Contains(help.error, "usage"));
        // 合法命令不得带 error
        const auto good = ParseDumpCommand({"dump", "scheduler", "--format=ini"});
        GO2CPP_CHECK(good.valid && good.error.empty());
    }

    // 场景 4：原子写入 + 临时文件不残留 + append
    {
        const std::string path = TempPath("atomic");
        std::remove(path.c_str());
        std::string error;
        GO2CPP_CHECK(WriteDumpFile(path, "first\n", false, &error));
        GO2CPP_CHECK(error.empty());
        GO2CPP_CHECK(FileExists(path));
        GO2CPP_CHECK(!FileExists(path + ".tmp"));   // 临时文件已 rename
        GO2CPP_CHECK(ReadFile(path) == "first\n");
        // append 模式
        GO2CPP_CHECK(WriteDumpFile(path, "second\n", true, &error));
        GO2CPP_CHECK(ReadFile(path) == "first\nsecond\n");
        // 覆盖模式
        GO2CPP_CHECK(WriteDumpFile(path, "third\n", false, &error));
        GO2CPP_CHECK(ReadFile(path) == "third\n");
        // 失败路径：目录不存在
        std::string err2;
        GO2CPP_CHECK(!WriteDumpFile("/tmp/no-such-dir-xyz/f.txt", "x", false, &err2));
        GO2CPP_CHECK(!err2.empty());
        std::remove(path.c_str());
    }

    // 场景 5：端到端（解析 -> RunDump -> 文件）
    {
        Fixture fixture(1);
        const std::string path = TempPath("e2e_ini");
        std::remove(path.c_str());
        const auto command = ParseDumpCommand({"dump", "scheduler", "--format=ini",
                                               "--output=" + path, "e2e"});
        GO2CPP_CHECK(command.valid);
        std::string text;
        std::string error;
        GO2CPP_CHECK(RunDump(command, *fixture.scheduler, &text, &error));
        GO2CPP_CHECK(error.empty());
        GO2CPP_CHECK(!text.empty());
        const std::string on_disk = ReadFile(path);
        GO2CPP_CHECK(on_disk == text);              // 文件内容与返回值一致
        GO2CPP_CHECK(Contains(on_disk, "[scheduler]"));
        GO2CPP_CHECK(Contains(on_disk, "snapshot_consistency=per_call"));
        std::remove(path.c_str());

        // json 走 stdout 路径也要能拿到文本
        const auto json_command = ParseDumpCommand({"dump", "scheduler", "--format=json"});
        std::string json_text;
        GO2CPP_CHECK(RunDump(json_command, *fixture.scheduler, &json_text, &error));
        bool parsed = false;
        try {
            const auto value = go2cpp::Json::parse(json_text);
            parsed = value.contains("scheduler");
        } catch (...) {
            parsed = false;
        }
        GO2CPP_CHECK(parsed);

        // RunDumpCli 的返回码
        const char* argv_ok[] = {"go2cpp", "dump", "scheduler", "--format=text"};
        GO2CPP_CHECK(RunDumpCli(4, argv_ok, *fixture.scheduler, &error) == 0);
        const char* argv_bad[] = {"go2cpp", "dump", "scheduler", "--bogus"};
        GO2CPP_CHECK(RunDumpCli(4, argv_bad, *fixture.scheduler, &error) == 1);
    }

    // 压测：10000 次解析
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::size_t valid_count = 0;
        for (int i = 0; i < 10000; ++i) {
            const auto command = ParseDumpCommand({"dump", "scheduler", "--format=json",
                                                   "--output=/tmp/x.txt"});
            valid_count += command.valid ? 1 : 0;
        }
        const auto t1 = std::chrono::steady_clock::now();
        std::clog << "  [压测] ParseDumpCommand x10000: "
                  << std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
                  << " us" << std::endl;
        GO2CPP_CHECK(valid_count == 10000);
    }

    // 随机：随机选项组合，valid 与 error 必须互斥
    {
        std::mt19937 rng(20261016);
        const std::vector<std::string> pool = {
            "--format=text", "--format=ini", "--format=json", "--format=xml",
            "--output=/tmp/r.txt", "-o", "/tmp/r2.txt", "--append", "--truncate",
            "--summary", "--processors", "--machines", "--metrics",
            "--no-processors", "--no-machines", "--no-metrics", "--bogus", "name"};
        std::uniform_int_distribution<std::size_t> count_dist(0, 4);
        std::uniform_int_distribution<std::size_t> pick_dist(0, pool.size() - 1);
        bool consistent = true;
        for (int round = 0; round < 500; ++round) {
            std::vector<std::string> args{"dump", "scheduler"};
            const auto count = count_dist(rng);
            for (std::size_t i = 0; i < count; ++i) {
                args.push_back(pool[pick_dist(rng)]);
            }
            const auto command = ParseDumpCommand(args);
            // 合法 => error 空；非法 => error 非空。两者必须互斥。
            if (command.valid == !command.error.empty()) {
                consistent = false;
            }
        }
        GO2CPP_CHECK(consistent);
    }

    // dump events 集成（第 13 轮补充）
    {
        Fixture fixture(1);
        DebugEventCollector collector(256);
        for (int i = 0; i < 7; ++i) {
            SchedulerEvent event;
            event.type = SchedulerEventType::kTaskStarted;
            event.task_id = static_cast<GId>(i + 1);
            event.machine_id = 1;
            event.timestamp = std::chrono::steady_clock::now();
            collector.OnEvent(event);
        }
        GO2CPP_CHECK(collector.Pending() == 7);

        // 解析：dump events 必须把 target 定为 kEvents，且不影响默认的 scheduler
        const auto events_text = ParseDumpCommand({"dump", "events", "--format=text"});
        const auto events_json = ParseDumpCommand({"dump", "events", "--format=json"});
        const auto default_target = ParseDumpCommand({"dump", "--format=ini"});
        GO2CPP_CHECK(events_text.valid && events_text.target == DumpTarget::kEvents);
        GO2CPP_CHECK(events_json.valid && events_json.target == DumpTarget::kEvents);
        GO2CPP_CHECK(default_target.valid && default_target.target == DumpTarget::kScheduler);
        // dump scheduler 仍然显式可写
        const auto sched = ParseDumpCommand({"dump", "scheduler", "--format=json"});
        GO2CPP_CHECK(sched.valid && sched.target == DumpTarget::kScheduler);
        // 非法 target 必须报错（不静默当成 scheduler）
        const auto bad = ParseDumpCommand({"dump", "bogus", "--format=text"});
        GO2CPP_CHECK(!bad.valid && Contains(bad.error, "unknown target"));

        // 端到端：写到文件并与返回值一致
        const std::string path = TempPath("e2e_events_ini");
        std::remove(path.c_str());
        auto cmd = ParseDumpCommand({"dump", "events", "--format=ini", "--output=" + path});
        std::string text;
        std::string error;
        GO2CPP_CHECK(RunEventsDump(cmd, collector, &text, &error));
        GO2CPP_CHECK(error.empty());
        GO2CPP_CHECK(Contains(text, "[events]"));
        GO2CPP_CHECK(Contains(text, "[event.0]"));
        GO2CPP_CHECK(ReadFile(path) == text);
        std::remove(path.c_str());
        // Peek 语义：dump 不应消耗事件
        GO2CPP_CHECK(collector.Pending() == 7);

        // json 产物可解析
        auto json_cmd = ParseDumpCommand({"dump", "events", "--format=json"});
        std::string json_text;
        GO2CPP_CHECK(RunEventsDump(json_cmd, collector, &json_text, &error));
        bool parsed = false;
        try {
            const auto value = go2cpp::Json::parse(json_text);
            parsed = value.contains("events") && value["events"].size() == 7;
        } catch (...) {
            parsed = false;
        }
        GO2CPP_CHECK(parsed);

        // 统一入口 RunDump 也应支持 events（带 collector 重载）
        std::string unified;
        GO2CPP_CHECK(RunDump(events_text, *fixture.scheduler, collector, &unified, &error));
        GO2CPP_CHECK(Contains(unified, "[events]"));
    }

    // --follow 解析与端到端（第 20 轮）
    {
        Fixture fixture(1);
        const auto bare = ParseDumpCommand({"dump", "events", "--follow"});
        GO2CPP_CHECK(bare.valid && bare.follow && bare.follow_batches == 0);

        const auto limited = ParseDumpCommand({"dump", "events", "--follow=7"});
        GO2CPP_CHECK(limited.valid && limited.follow && limited.follow_batches == 7);

        const auto interval = ParseDumpCommand({"dump", "events", "--follow=2",
                                                "--follow-interval=3"});
        GO2CPP_CHECK(interval.valid && interval.follow_interval.count() == 3);

        // 非法值必须报错，不能静默当成不跟随
        const auto zero = ParseDumpCommand({"dump", "events", "--follow=0"});
        const auto junk = ParseDumpCommand({"dump", "events", "--follow=abc"});
        const auto negative = ParseDumpCommand({"dump", "events", "--follow-interval=-1"});
        GO2CPP_CHECK(!zero.valid && !zero.error.empty());
        GO2CPP_CHECK(!junk.valid && Contains(junk.error, "不是整数"));
        GO2CPP_CHECK(!negative.valid && Contains(negative.error, "不能为负"));
        const auto bad_timeout = ParseDumpCommand({"dump", "events", "--follow-timeout=-5"});
        GO2CPP_CHECK(!bad_timeout.valid &&
                     Contains(bad_timeout.error, "不能为负"));

        // 不带 follow 时行为不变
        const auto plain = ParseDumpCommand({"dump", "events", "--format=json"});
        GO2CPP_CHECK(plain.valid && !plain.follow);

        // 端到端：预先放 3 个事件，follow 上限 1 批，应报告 followed 1 batches
        DebugEventCollector collector(64);
        for (int i = 0; i < 3; ++i) {
            SchedulerEvent event;
            event.type = SchedulerEventType::kTaskCompleted;
            event.task_id = static_cast<GId>(i + 1);
            event.timestamp = std::chrono::steady_clock::now();
            collector.OnEvent(event);
        }
        auto follow_cmd = ParseDumpCommand({"dump", "events", "--follow=1",
                                            "--follow-interval=0",
                                            "--follow-timeout=200"});
        GO2CPP_CHECK(follow_cmd.valid);
        std::string note;
        std::string error;
        GO2CPP_CHECK(RunEventsDump(follow_cmd, collector, &note, &error));
        GO2CPP_CHECK(Contains(note, "followed 1 batches"));
        GO2CPP_CHECK(collector.Pending() == 0);   // 跟随会消耗事件
        (void)fixture;
    }
}
